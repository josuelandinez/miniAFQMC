#!/bin/bash
echo Configuring and building froom inside the build directory.
echo Check the results of the CMake configuration to ensure that the preferred
echo compilers and libraries have been selected. See README and documentation 
echo for guidance.


# setenv CXX mpicxx
# setenv CC mpicc

#on Cray:
export CXX=CC

#module load mkl
#source /usr/tce/packages/mkl/mkl-2017.1/mkl/bin/mklvars.csh intel64

# MAM:
# Careful with threaded MKL. Only seems to work on LLNL with Intel compilers.
# Can't seem to get -DBLA_VENDOR=Intel10_64lp working in LLNL.

# Debug with 
cmake \
      -DCMAKE_BUILD_TYPE=Debug \
      -DBUILD_AFQMC=1 -DQMC_COMPLEX=1 ../; 
# Release
#cmake -DBLA_VENDOR=Intel10_64lp_seq -DCMAKE_PREFIX_PATH=$MKLROOT/lib \
#      -DCMAKE_BUILD_TYPE=Release \
#      -DVERBOSE=TRUE \
#      -DBUILD_AFQMC=1 -DQMC_COMPLEX=1 ../../; 
#make

