#ifndef AFQMC_CONFIG_H 
#define AFQMC_CONFIG_H 

#include <string>
#include <cstdlib>
#include <ctype.h>
#include <fstream>
#include "Configuration.h"

namespace qmcplusplus
{

struct AFQMCInfo 
{
  public:

  // default constructor
  AFQMCInfo():name("miniAFQMC"),NMO(-1),NAEA(-1),NAEB(-1),MS2(0)
  {}

  // destructor
  ~AFQMCInfo() {}

  // name
  std::string name;

  // number of active orbitals
  int NMO;

  // number of active electrons alpha/beta 
  int NAEA, NAEB;

  // ms2
  int MS2; 

  // no fully spin polarized yet, not sure what it will break 
  bool checkAFQMCInfoState() {
    if(NMO < 1 || NAEA<1 || NAEB<1 || NAEA > NMO || NAEB > NMO ) 
      return false;
    return true; 
  } 

  void print(std::ostream& out) {
    out<<"  AFQMC info: \n"
       <<"    name: " <<name <<"\n"
       <<"    # of molecular orbitals: " <<NMO <<"\n"
       <<"    # of up electrons: " <<NAEA  <<"\n"
       <<"    # of down electrons: " <<NAEB  <<std::endl;
  }

};

}

#endif
