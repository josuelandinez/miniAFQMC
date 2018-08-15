
#include<cassert>
//#include<random>
#include<cstdlib>
#if defined(HAVE_MPI)
#include<mpi.h>
#endif

#include"Walkers/SharedWalkerSet.h"
#include"Walkers/WalkerControl.hpp"
#include"Walkers/WalkerUtilities.hpp"

namespace qmcplusplus
{

namespace afqmc
{

// no xml input: hard coded options
void SharedWalkerSet::parse()
{

    walkerType = COLLINEAR;
    load_balance = ASYNC;
    pop_control = PAIR;

}

void SharedWalkerSet::setup()
{

  TimerNameList_t<SharedWalkerSetTimers> SharedWalkerSetTimerNames =
  {
    {LoadBalance, "SharedWalkerSet::loadBalance"},
    {PopControl, "SharedWalkerSet::popControl"}
  };

  setup_timers(Timers, SharedWalkerSetTimerNames);

  // careful! These are only used to calculate memory needs and access points/partitionings
  int ncol=NAEA;
  int nrow=NMO;
  int ndim_bp = NMO;
  if(walkerType == CLOSED)  { 
    // wlk_descriptor: {nmo, naea, naeb, nback_prop} from the point of view of a single spin SM 
    wlk_desc = {NMO,NAEA,0,nback_prop}; 
  } else if(walkerType == COLLINEAR) {  
    // wlk_descriptor: {nmo, naea, naeb, nback_prop} from the point of view of a single spin SM 
    wlk_desc = {NMO,NAEA,NAEB,nback_prop}; 
    ncol += NAEB;
  } else if(walkerType == NONCOLLINEAR) {   
    // wlk_descriptor: {nmo, naea, naeb, nback_prop} from the point of view of a single spin SM 
    wlk_desc = {2*NMO,NAEA+NAEB,0,nback_prop}; 
    nrow += NMO;
    ncol += NAEB;
    ndim_bp *= 2;
  } else {
    app_error()<<" Error: Incorrect walker_type on SharedWalkerSet::setup \n";
    APP_ABORT("");
  }
  int bp_size = nback_prop*ndim_bp*ndim_bp;

  //   - SlaterMatrix:         NCOL*NROW 
  //   - weight:               1 
  //   - phase:                1
  //   - pseudo energy:        1 
  //   - E1:                   1 
  //   - EXX:                  1
  //   - EJ:                   1
  //   - overlap:              1
  //   - propagators:          NBACK_PROP*NCOL_BP*NROW_BP=BP_SIZE
  //   - head:                 1
  //   - tail:                 1
  //   - SlaterMatrixN:        Same size as Slater Matrix
  //   - cos_fac :             NBACK_PROP
  //   - weight_fac:           NBACK_PROP
  //   Total: 7+2*NROW*NCOL+BP_SIZE+2*NBACK_PROP
  int cnt=0;
  data_displ[SM] = cnt;          cnt+=nrow*ncol;
  data_displ[WEIGHT] = cnt;      cnt+=1; // weight 
  data_displ[PHASE] = cnt;       cnt+=1; // phase
  data_displ[PSEUDO_ELOC_] = cnt;  cnt+=1; // pseudo energy  
  data_displ[E1_] = cnt;          cnt+=1; // E1
  data_displ[EXX_] = cnt;         cnt+=1; // EXX
  data_displ[EJ_] = cnt;          cnt+=1; // EJ 
  data_displ[OVLP] = cnt;        cnt+=1; // overlap
  if(nback_prop > 0) {
    data_displ[PROPAGATORS] = cnt; cnt+=bp_size; // propagators for back propagation.
    data_displ[HEAD] = cnt;        cnt+=1; // current location of propagator matrix in circular buffer.
    data_displ[TAIL] = cnt;        cnt+=1; // position of tail of circular buffer.
    data_displ[SMN] = cnt;         cnt+=nrow*ncol; // Slater Matrix at beggining of BP path 
    data_displ[COS_FAC] = cnt;     cnt+=nback_prop; // Cosine factors along BP path.
    data_displ[WEIGHT_FAC] = cnt;  cnt+=nback_prop; // Missing imaginary weight factors along BP path.
  } else {
    data_displ[PROPAGATORS] = -1; 
    data_displ[HEAD] = -1;        
    data_displ[TAIL] = -1;        
    data_displ[SMN] = -1;         
    data_displ[COS_FAC] = -1;     
    data_displ[WEIGHT_FAC] = -1;  
  }
  walker_size = cnt;
  walker_memory_usage = walker_size*sizeof(ComplexType);

  tot_num_walkers=0;

  min_weight = std::max(std::abs(min_weight),1e-2); 
}

bool SharedWalkerSet::clean()
{
  walker_buffer = std::move(std::make_unique<SHM_Buffer>(TG.TG_local(),0));
  tot_num_walkers=targetN=targetN_per_TG=0;
  return true;
}

/*
 * Increases the capacity of the containers to n.
 */
void SharedWalkerSet::reserve(int n)
{
  if(capacity() < n) 
    walker_buffer->resize(walker_size*n);
}

/*
 * Adds/removes the number of walkers in the set to match the requested value.
 * Walkers are removed from the end of the set 
 *     and buffer capacity remains unchanged in this case.
 * New walkers are initialized from already existing walkers in a round-robin fashion. 
 * If the set is empty, calling this routine will abort. 
 * Capacity is increased if necessary.
 * Target Populations are set to n.
 */
void SharedWalkerSet::resize(int n) {
  if(tot_num_walkers==0) 
    APP_ABORT("error: empty set in resize(n).\n");
  
  reserve(n);
  if(n > tot_num_walkers) {
    if(TG.TG_local().root()) {
      auto W = get_walkers_matrix();
      auto pos = tot_num_walkers;
      auto i0=0;
      while(pos < n) {
        W[pos++] = W[i0];
        i0 = (i0+1)%tot_num_walkers;
      } 
    }
  }
  tot_num_walkers=n;
  targetN_per_TG = tot_num_walkers;
  targetN = GlobalPopulation(); 
  if(targetN != targetN_per_TG*TG.getNumberOfTGs()) {
    app_error()<<" targetN, targetN_per_TG, # of TGs: " 
               <<targetN <<" " <<targetN_per_TG <<" " <<TG.getNumberOfTGs() <<std::endl;   
    APP_ABORT("Error in SharedWalkerSet::resize(n).\n");
  }
}

//  curData:
//  0: factor used to rescale the weights
//  1: sum_i w_i * Eloc_i   (where w_i is the unnormalized weight)
//  2: sum_i w_i            (where w_i is the unnormalized weight)
//  3: sum_i abs(w_i)       (where w_i is the unnormalized weight)
//  4: sum_i abs(<psi_T|phi_i>)
//  5: total number of walkers  
//  6: total number of "healthy" walkers (those with weight > 1e-6, ovlp>1e-8, etc) 
void SharedWalkerSet::popControl(std::vector<ComplexType>& curData)
{
  Timers[PopControl]->start();
  ComplexType minus = ComplexType(-1.0,0.0);
  bool needsLoadBalance = true;

  curData.resize(7);
  std::fill(curData.begin(),curData.begin()+7,ComplexType(0));

  // safety check
  if(tot_num_walkers!=targetN_per_TG)
    APP_ABORT("Error: tot_num_walkers!=targetN_per_TG");

  // gather data and walker information
  if(TG.TG_local().root()) {
    afqmc::BasicWalkerData(*this,curData,TG.TG_heads());
    RealType scl = 1.0/curData[0].real();
    scaleWeight(scl);
  } 
  TG.TG_local().broadcast_n(curData.data(),curData.size());

  // matrix to hold walkers beyond targetN_per_TG
  // doing this to avoid resizing SHMBuffer, instead use local memory
  // will be resized later
  MArray<ComplexType,2> Wexcess({0,walker_size});

  if(TG.TG_local().root()) {
    nwalk_counts_new.resize(TG.TG_heads().size());
    std::fill(nwalk_counts_new.begin(),nwalk_counts_new.end(),targetN_per_TG);
  }

  // population control on master node  
  if(pop_control == PAIR || pop_control == SERIAL_COMB || pop_control == MIN_BRANCH) { 

    if(TG.TG_local().root()) 
      SerialBranching(*this,pop_control,min_weight,max_weight,
                            nwalk_counts_old,Wexcess,*rng,TG.TG_heads()); 

  // distributed routines from here
  } else if(pop_control == COMB) {

    APP_ABORT(" Error: Distributed comb not implemented yet. \n\n\n");
    //afqmc::DistCombBranching(*this,rng_heads,nwalk_counts_old);

  }

  // load balance after population control events
  loadBalance(Wexcess);

  if(tot_num_walkers != targetN_per_TG)
    APP_ABORT(" Error: tot_num_walkers != targetN_per_TG");

  Timers[PopControl]->stop();
}

void SharedWalkerSet::benchmark(std::string& blist,int maxnW,int delnW,int repeat)
{

  if(blist.find("comm")!=std::string::npos) {

    app_log()<<" Testing communication times in WalkerHandler. This should be done using a single TG per node, to avoid timing communication between cores on the same node. \n";
    std::ofstream out;
    if(TG.getGlobalRank() == 0)
      out.open("benchmark.icomm.dat");  

    std::vector<std::string> tags(3);
    tags[0]="M1";
    tags[1]="M2";
    tags[2]="M3";

    int nw=1;
    while(nw <= maxnW) {

      if(TG.TG_local().root() && (TG.TG_heads().rank()==0 || TG.TG_heads().rank()==1)) {
        int sz = nw*walker_size; 
        std::vector<ComplexType> Cbuff(sz);
        MPI_Request req;
        MPI_Status st;    
        TG.TG_heads().barrier();
        for(int i=0; i<repeat; i++) {

          if(TG.TG_heads().rank()==0) {
            MPI_Isend(Cbuff.data(),2*Cbuff.size(),MPI_DOUBLE,1,999,TG.TG_heads().impl_,&req);  
            MPI_Wait(&req,&st);
          } else {
            MPI_Irecv(Cbuff.data(),2*Cbuff.size(),MPI_DOUBLE,0,999,TG.TG_heads().impl_,&req);  
            MPI_Wait(&req,&st);
          }

        }

        if(TG.TG_heads().rank()== 0) {
          out<<nw <<" " ;
          out<<std::endl;
        }
      } else if(TG.TG_local().root()) {
        TG.TG_heads().barrier();
      }

      if(delnW <=0) nw *= 2;
      else nw += delnW;
    }


  } else if(blist.find("comm")!=std::string::npos) {
    std::ofstream out;
    if(TG.getGlobalRank() == 0)
      out.open("benchmark.comm.dat"); 

  }
 
}

}

}

