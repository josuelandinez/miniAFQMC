////////////////////////////////////////////////////////////////////////////////
//// This file is distributed under the University of Illinois/NCSA Open Source
//// License.  See LICENSE file in top directory for details.
////
//// Copyright (c) 2016 Jeongnim Kim and QMCPACK developers.
////
//// File developed by:
////
//// File created by:
//////////////////////////////////////////////////////////////////////////////////

#ifndef  AFQMC_OPS_SHM_HPP 
#define  AFQMC_OPS_SHM_HPP 

#include "Configuration.h"
#include "Utilities/taskgroup.hpp"
#include "Numerics/ma_blas.hpp"
#include "Numerics/ma_operations.hpp"
#include "AFQMC/AFQMCInfo.hpp"
#include "AFQMC/energy.hpp"
#include "AFQMC/vHS.hpp"
#include "AFQMC/mixed_density_matrix.hpp"

namespace qmcplusplus
{

namespace shm
{

struct afqmc_sys: public AFQMCInfo
{

  public:

    afqmc_sys(TaskGroup& tg_):TG(tg_) 
    {
      nnodes = TG.getTotalNodes();
      nodeid = TG.getNodeID();
      ncores = TG.getTotalCores();
      coreid = TG.getCoreID();
      core_rank = TG.getCoreRank();
      ncores_per_TG = TG.getNCoresPerTG();  

      one = ComplexType(1.,0.);
      zero = ComplexType(0.,0.);
    }

    ~afqmc_sys() {}

    ComplexMatrix trialwfn_alpha;
    ComplexMatrix trialwfn_beta;
    TaskGroup& TG;

    void setup(int nmo_, int na) {
      NMO = nmo_;
      NAEA = NAEB = na;
      TWORK1.resize(extents[NAEA][NAEA]);
      TWORK2.resize(extents[NAEA][NMO]);
      TWORK3.resize(extents[NAEA][NMO]);
      IWORK1.resize(extents[2*NMO]);
      S0.resize(extents[NMO][NAEA]);    
      TWORKV1.resize(extents[NAEA*NAEA]); 
      DM.resize(extents[NMO][NMO]);      
      
      Gcloc.resize(extents[1][1]); // force resize later 
    } 

    template< class WSet, 
              class MatA, 
              class MatB 
            >
    void calculate_mixed_density_matrix(const WSet& W, MatA& W_data, MatB& G, bool compact=true)
    {
      int nwalk = W.shape()[0];
      assert(G.num_elements() >= 2*NAEA*NMO*nwalk);
      assert(W_data.shape()[0] >= nwalk);
      assert(W_data.shape()[1] >= 4);
      int N_ = compact?NAEA:NMO;
      boost::multi_array_ref<ComplexType,2> DMr(DM.data(), extents[N_][NMO]); 
      boost::multi_array_ref<ComplexType,4> G_4D(G.data(), extents[2][N_][NMO][nwalk]); 
      for(int n=0, cnt=0; n<nwalk; n++) {

        if(cnt%ncores_per_TG == core_rank) {
          W_data[n][2] = base::MixedDensityMatrix<ComplexType>(trialwfn_alpha,W[n][0],
                       DMr,IWORK1,TWORK1,TWORK2,TWORKV1,compact);
          G_4D[ indices[0][range_t(0,N_)][range_t(0,NMO)][n] ] = DMr;
        }
        cnt++;

        if(cnt%ncores_per_TG == core_rank) {
          W_data[n][3] = base::MixedDensityMatrix<ComplexType>(trialwfn_beta,W[n][1],
                       DMr,IWORK1,TWORK1,TWORK2,TWORKV1,compact);
          G_4D[ indices[1][range_t(0,N_)][range_t(0,NMO)][n] ] = DMr;
        }
        cnt++;

      }
      TG.local_barrier();  
    }

    template<class SpMat,
             class MatA,
             class MatB,
             class MatC
            >
    RealType calculate_energy(MatA& W_data, const MatB& G, const MatC& haj, const SpMat& V) 
    {
      assert(G.shape()[0] == 2*NAEA*NMO);
      if(G.shape()[1] != Gcloc.shape()[1] || Gcloc.shape()[0] != V.rows())
        Gcloc.resize(extents[V.rows()][G.shape()[1]]);  
      if(core_rank==0) {
        // zero 
        for(int n=0, ne=W_data.shape()[0]; n<ne; n++) W_data[n][0] = 0.;
      }
      TG.local_barrier();
      shm::calculate_energy(G,Gcloc,haj,V,locWlkVec);
      {
        boost::interprocess::scoped_lock<boost::interprocess::interprocess_mutex> lock(*((TG.getBuffer())->getMutex()));
        BLAS::axpy(W_data.shape()[0],one,locWlkVec.data(),1,W_data.origin(),W_data.strides()[0]);
      }
      TG.local_barrier();
      RealType eav = 0., wgt=0.;
      for(int n=0, nw=G.shape()[1]; n<nw; n++) {
        wgt += W_data[n][1].real();
        eav += W_data[n][0].real()*W_data[n][1].real();
      }
      return eav/wgt;
    }

    template<class WSet, class Mat>
    void calculate_overlaps(const WSet& W, Mat& W_data)
    {
      assert(W_data.shape()[0] >= W.shape()[0]);
      assert(W_data.shape()[1] >= 4);
      for(int n=0, nw=W.shape()[0]; n<nw; n++) {
        if(n%ncores_per_TG != core_rank) continue;
        W_data[n][2] = base::Overlap<ComplexType>(trialwfn_alpha,W[n][0],IWORK1,TWORK1);
        W_data[n][3] = base::Overlap<ComplexType>(trialwfn_beta,W[n][1],IWORK1,TWORK1);
      }
      TG.local_barrier();  
    }

    template<class WSet, 
             class MatA,
             class MatB
            >
    void propagate(WSet& W, MatA& Propg, MatB& vHS)
    {

      typedef typename std::decay<MatB>::type::element Type;
      boost::multi_array_ref<Type,3> V(vHS.data(), extents[NMO][NMO][vHS.shape()[1]]);
      boost::multi_array_ref<Type,2> TWORK2r(TWORK2.data(), extents[NMO][NAEA]);
      boost::multi_array_ref<Type,2> TWORK3r(TWORK3.data(), extents[NMO][NAEA]);
      for(int nw=0, cnt=0, nwalk=W.shape()[0]; nw<nwalk; nw++) {

        if(cnt%ncores_per_TG == core_rank) {
          ma::product(Propg,W[nw][0],S0);
          // need deep-copy, since stride()[1] == nw otherwise
          DM = V[ indices[range_t(0,NMO)][range_t(0,NMO)][nw] ];
          base::apply_expM(DM,S0,TWORK2r,TWORK3r,6);
          ma::product(Propg,S0,W[nw][0]);
        }
        cnt++;
        if(cnt%ncores_per_TG == core_rank) {
          ma::product(Propg,W[nw][1],S0);
          DM = V[ indices[range_t(0,NMO)][range_t(0,NMO)][nw] ];
          base::apply_expM(DM,S0,TWORK2r,TWORK3r,6);
          ma::product(Propg,S0,W[nw][1]);
        }
        cnt++;

      }
      TG.local_barrier();  

    }    

    template<class WSet>
    void orthogonalize(WSet& W)
    {
      for(int i=0, cnt=0; i<W.shape()[0]; i++) {

          // QR on the transpose
        if(cnt%ncores_per_TG == core_rank) {  
          for(int r=0; r<NMO; r++)
            for(int c=0; c<NAEA; c++)
              TWORK2[c][r] = W[i][0][r][c];   
          ma::geqrf(TWORK2,TAU,TWORKV2);
          ma::gqr(TWORK2,TAU,TWORKV2);
          for(int r=0; r<NMO; r++)
            for(int c=0; c<NAEA; c++)
              W[i][0][r][c] = TWORK2[c][r];   
        }
        cnt++;

        if(cnt%ncores_per_TG == core_rank) {
          for(int r=0; r<NMO; r++)
            for(int c=0; c<NAEA; c++)
              TWORK2[c][r] = W[i][1][r][c];
          ma::geqrf(TWORK2,TAU,TWORKV2);
          ma::gqr(TWORK2,TAU,TWORKV2);
          for(int r=0; r<NMO; r++)
            for(int c=0; c<NAEA; c++)
              W[i][1][r][c] = TWORK2[c][r];
        }
        cnt++;

        // LQ on the direct matrix
        //if(cnt%ncores_per_TG == core_rank) {
        //  ma::gelqf(W[i][0],TAU,TWORKV2);
        //  ma::glq(W[i][0],TAU,TWORKV2);
        //}
        //cnt++;  
        //if(cnt%ncores_per_TG == core_rank) {
        //  ma::gelqf(W[i][1],TAU,TWORKV2);
        //  ma::glq(W[i][1],TAU,TWORKV2);
        //}
        //cnt++;  

      }
      TG.local_barrier();  
    }

  private:

    index_gen indices;

    // work arrays
    ComplexMatrix TWORK1;
    ComplexMatrix TWORK2;
    ComplexMatrix TWORK3;
    IndexVector IWORK1;
    ComplexVector TWORKV1;
    ComplexMatrix S0; 
    ComplexMatrix DM;
    ComplexMatrix Gcloc;
    ComplexVector TWORKV2;  
    ComplexVector TAU;
    ComplexVector locWlkVec;
    
    int nnodes;
    int nodeid;
    int ncores;
    int coreid;
    int core_rank;
    int ncores_per_TG;

    ComplexType one;
    ComplexType zero;
};

}

}

#endif