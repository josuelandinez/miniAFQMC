#if COMPILATION_INSTRUCTIONS
mpicxx -O3 -std=c++14 `#-Wfatal-errors` $0 -o $0x.x && time mpirun -np 8 $0x.x $@ && rm -f $0x.x; exit
#endif

#include "alf/boost/mpi3/main.hpp"
#include "alf/boost/mpi3/communicator.hpp"
#include "alf/boost/mpi3/detail/strided.hpp"
#include "alf/boost/mpi3/process.hpp"

namespace mpi3 = boost::mpi3;
using std::cout;

int mpi3::main(int argc, char* argv[], mpi3::communicator& world){

	assert( world.size() > 1);
	{
		int count = 120;
		std::vector<int> send_buffer(count);
		iota(send_buffer.begin(), send_buffer.end(), 0);

		std::vector<int> recv_buffer(count, -1);
		world.all_reduce(send_buffer.begin(), send_buffer.end(), recv_buffer.begin());//, std::plus<>{});
		for(int i = 0; i != recv_buffer.size(); ++i) assert(recv_buffer[i] == i*world.size());
	}
	{
		double v = world.rank();
		double total = world.all_reduce_value(v, std::plus<>{});
		assert( total == world.size()*(world.size()-1)/2 );
	}
	{
		double total = (world += world.rank());
		assert( total == world.size()*(world.size()-1)/2 );
	}
	return 0;
}
