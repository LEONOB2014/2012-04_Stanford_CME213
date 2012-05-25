 /* 2D Heat Diffusion w/MPI
 * 
 * You will implement the familiar 2D Heat Diffusion from the
 * previous homework on CPUs with MPI.
 * 
 * You have been given the simParams class updated
 * with all necessary parameters and the outline of
 * Grid class that you fill in.  You are also given the 
 * stencil calculations since you have already implemented
 * them in the previous homework.
 *
 * You are also given a macro - MPI_SAFE_CALL which you should
 * wrap all MPI calls with to always check error return codes.
 *
 * You will implement and investigate 2 different domain 
 * decompositions techniques: 1-D stripes and 2-D squares.
 * 
 * You will also investigate the impact of synchronous vs. asynchronous
 * communication.  To minimize programming effort and time you only need
 * to implement asynchronous communication and then implement synchronous
 * communication on top of the asynchronous routine by waiting for
 * the communication to finish.
 *
 * This means you will only use the asyncrhonous MPI communication routines
 * How would the communication pattern have to be different if you used the
 * synchronous communication routines for the synchronous communication.
 * 
 */

#include <ostream>
#include <iostream>
#include <iomanip>
#include <limits>
#include <vector>
#include <fstream>
#include <string>
#include <assert.h>
#include <fstream>
#include <sstream>
#include <cmath>
#include <stdlib.h>

#include "mpi.h"

#define MPI_SAFE_CALL( call ) do {                               \
    int err = call;                                              \
    if (err != MPI_SUCCESS) {                                    \
        fprintf(stderr, "MPI error %d in file '%s' at line %i",  \
               err, __FILE__, __LINE__);                         \
        exit(1);                                                 \
    } } while(0)

class simParams {
    public:
        simParams(const char *filename, bool verbose); //parse command line
                                                       //does no error checking
        simParams(); //use some default values

        int    nx()         const {return nx_;}
        int    ny()         const {return ny_;}
        double lx()         const {return lx_;}
        double ly()         const {return ly_;}
        double alpha()      const {return alpha_;}
        int    iters()      const {return iters_;}
        double dx()         const {return dx_;}
        double dy()         const {return dy_;}
        double ic()         const {return ic_;}
        int    order()      const {return order_;}
        double xcfl()       const {return xcfl_;}
        double ycfl()       const {return ycfl_;}
        int    gridMethod() const {return gridMethod_;}
        bool   sync()       const {return synchronous_;}
        double topBC()      const {return bc[0];}
        double leftBC()     const {return bc[1];}
        double bottomBC()   const {return bc[2];}
        double rightBC()    const {return bc[3];}

    private:
        int    nx_, ny_;     //number of grid points in each dimension
        double lx_, ly_;     //extent of physical domain in each dimension
        double alpha_;       //thermal conductivity
        double dt_;          //timestep
        int    iters_;       //number of iterations to do
        double dx_, dy_;     //size of grid cell in each dimension
        double ic_;          //uniform initial condition
        double xcfl_, ycfl_; //cfl numbers in each dimension
        int    order_;       //order of discretization
        int    gridMethod_;  //1-D or 2-D
        bool   synchronous_; //Sync or Async communication scheme
        double bc[4];        //0 is top, counter-clockwise

        void calcDtCFL();
};

simParams::simParams() {
    nx_ = ny_ = 10;
    lx_ = ly_ = 1;
    alpha_ = 1;
    iters_ = 1000;
    order_ = 2;

    dx_ = lx_ / (nx_ - 1);
    dy_ = ly_ / (ny_ - 1);

    ic_ = 5.;

    gridMethod_ = 1;

    synchronous_ = true;

    bc[0] = 0.;
    bc[1] = 10.;
    bc[2] = 0.;
    bc[3] = 10.;

    calcDtCFL();
}

simParams::simParams(const char *filename, bool verbose) {
    std::ifstream ifs(filename);

    if (!ifs.good()) {
        std::cerr << "Couldn't open parameter file!" << std::endl;
        exit(1);
    }

    ifs >> nx_ >> ny_;
    ifs >> lx_ >> ly_;
    ifs >> alpha_;
    ifs >> iters_;
    ifs >> order_; assert( order_ == 2 | order_ == 4 | order_ == 8);
    ifs >> ic_;
    ifs >> gridMethod_;
    ifs >> synchronous_;
    ifs >> bc[0] >> bc[1] >> bc[2] >> bc[3];

    ifs.close();

    dx_ = lx_ / (nx_ - 1);
    dy_ = ly_ / (ny_ - 1);

    calcDtCFL();

    int rank;

    MPI_SAFE_CALL( MPI_Comm_rank(MPI_COMM_WORLD, &rank) );
    if (verbose && rank == 0) {
        printf("nx: %d ny: %d\nlx %f: ly: %f\nalpha: %f\niterations: %d\norder: %d\nic: %f\nsync: %d\n", 
                nx_, ny_, lx_, ly_, alpha_, iters_, order_, ic_, synchronous_);
        printf("domainDecomp: %d\ntopBC: %f lftBC: %f botBC: %f rgtBC: %f\ndx: %f dy: %f\ndt: %f xcfl: %f ycfl: %f\n", 
                gridMethod_, bc[0], bc[1], bc[2], bc[3], dx_, dy_, dt_, xcfl_, ycfl_);
    }
}

void simParams::calcDtCFL() {
    //check cfl number and make sure it is ok
    if (order_ == 2) {
        //make sure we come in just under the limit
        dt_ = (.5 - .0001) * (dx_ * dx_ * dy_ * dy_) / (alpha_ * (dx_ * dx_ + dy_ * dy_));
        xcfl_ = (alpha_ * dt_) / (dx_ * dx_);
        ycfl_ = (alpha_ * dt_) / (dy_ * dy_);
    }
    else if (order_ == 4) {
        dt_ = (.5 - .0001) * (12 * dx_ * dx_ * dy_ * dy_) / (16 * alpha_ * (dx_ * dx_ + dy_ * dy_));
        xcfl_ = (alpha_ * dt_) / (12 * dx_ * dx_);
        ycfl_ = (alpha_ * dt_) / (12 * dy_ * dy_);
    }
    else if (order_ == 8) {
        dt_ = (.5 - .0001) * (5040 * dx_ * dx_ * dy_ * dy_) / (8064 * alpha_ * (dx_ * dx_ + dy_ * dy_));
        xcfl_ = (alpha_ * dt_) / (5040 * dx_ * dx_);
        ycfl_ = (alpha_ * dt_) / (5040 * dy_ * dy_);
    }
    else {
        std::cerr << "Unsupported discretization order. \
                      TODO: implement exception" << std::endl;
    }
}

class Grid {
    public:
        Grid(const simParams &params, bool debug);
        ~Grid() { }

        typedef int gridState;

        enum MessageTag {
            TOP_TAG,
            BOT_TAG,
            LFT_TAG,
            RGT_TAG
        };

        int gx() const {return gx_;}
        int gy() const {return gy_;}
        int nx() const {return nx_;}
        int ny() const {return ny_;}
        int borderSize() const {return borderSize_;}
        int rank() const {return ourRank_;}
        const gridState & curr() const {return curr_;}
        const gridState & prev() const {return prev_;}
        void swapState() {prev_ = curr_; curr_ = (curr_ + 1) & 1;} 

        //for speed doesn't do bounds checking
        double operator()(const gridState & selector, 
                                 int xpos, int ypos) const {
            return grid_[selector * gx_ * gy_ + ypos * gx_ + xpos];
        }

        double& operator()(const gridState & selector, 
                                  int xpos, int ypos) {
            return grid_[selector * gx_ * gy_ + ypos * gx_ + xpos];
        }

        void transferHaloDataASync();
        void waitForSends(); //block until sends are finished
        void waitForRecvs(); //block until receives are finished

        void saveStateToFile(std::string identifier) const;

        friend std::ostream & operator<<(std::ostream &os, const Grid& grid);

    private:
        std::vector<double> grid_;
        int gx_, gy_;             //total grid extents - non-boundary size + halos
        int nx_, ny_;             //non-boundary region
        int borderSize_;          //number of halo cells

        int procLeft_;            //MPI processor numbers
        int procRight_;           //of our neighbors
        int procTop_;             //negative if not used
        int procBot_;

        gridState curr_;
        gridState prev_;

        int ourRank_;
        bool debug_;

        std::vector<MPI_Request> send_requests_;
        std::vector<MPI_Request> recv_requests_;
        std::vector<double> recv_right_buffer_;
        std::vector<double> recv_left_buffer_;
        std::vector<double> send_right_buffer_;
        std::vector<double> send_left_buffer_;
        //prevent copying and assignment since they are not implemented
        //and don't make sense for this class
        Grid(const Grid &);
        Grid& operator=(const Grid &);

};

std::ostream& operator<<(std::ostream& os, const Grid &grid) {
    os << std::setprecision(3);
    for (int y = grid.gy() - 1; y != -1; --y) {
        for (int x = 0; x < grid.gx(); x++) {
            os << std::setw(5) << grid(grid.curr(), x, y) << " ";
        }
        os << std::endl;
    }
    os << std::endl;
    return os;
}

Grid::Grid(const simParams &params, bool debug) {
    debug_ = debug;

    curr_ = 1;
    prev_ = 0;

    //need to figure out which processor we are and who our neighbors are...
    int totalNumProcessors;
    //TODO: set ourRank_ and totalNumProcessors
    MPI_SAFE_CALL( MPI_Comm_size(MPI_COMM_WORLD, &totalNumProcessors) );
    MPI_SAFE_CALL( MPI_Comm_rank(MPI_COMM_WORLD, &ourRank_) );
	    
    //based on total number of processors and grid configuration
    //determine our neighbors
    procLeft_ = -1;
    procRight_ = -1;
    procTop_ = -1;
    procBot_ = -1;

    //1D decomposition - horizontal stripes
    if (params.gridMethod() == 1) {
		//TODO: set proc* and nx, ny correctly
		nx_ = params.nx();
		ny_ = (params.ny() + totalNumProcessors - 1)/totalNumProcessors;

		if( totalNumProcessors == 1 )
		{
			ny_ = params.ny();
		}
		else if( ourRank_ == 0 )
		{
			procBot_ = ourRank_ + 1;
		}
		else if( ourRank_ ==  totalNumProcessors - 1)
		{
			procTop_  = ourRank_ - 1;
			ny_ = params.ny() - (totalNumProcessors-1)*ny_;
		}
		else
		{	
			procTop_  = ourRank_ - 1;
			procBot_  = ourRank_ + 1;
		}
    }
    else if (params.gridMethod() == 2) { 
		//2D decomposition
        //you are only required to implement decomposition for square grids of processors ie 1x1, 2x2, 3x3, etc.
        //handling of arbitrary # of processors is extra credit
        //TODO: set proc* and nx, ny correctly
        
        int n_grid_x = sqrt(double(totalNumProcessors));
        int n_grid_y = n_grid_x;
        assert( n_grid_x*n_grid_y == totalNumProcessors);
     	
     	nx_ = (params.nx() + n_grid_x - 1)/n_grid_x;
     	ny_ = (params.ny() + n_grid_y - 1)/n_grid_y;
     	     	                	
     	if( totalNumProcessors == 1 )
		{
			nx_ = params.nx();
			ny_ = params.ny();
		}
     	else if( ourRank_ % n_grid_x == 0)
     	{
			procRight_ = ourRank_ + 1;
			if( ourRank_ == 0){ procBot_ = ourRank_ + n_grid_x; }
			else if((ourRank_/n_grid_x + 1) == n_grid_y)
			{ 
				ny_      = params.ny() - (n_grid_y-1) * ny_;
				procTop_ = ourRank_ - n_grid_x; 
			}
			else
			{ 
				procBot_ = ourRank_ + n_grid_x; 
				procTop_ = ourRank_ - n_grid_x; 
			}
		}
		else if( ourRank_ < n_grid_x - 1)
		{
			procBot_   = ourRank_ + n_grid_x;
			procRight_ = ourRank_ + 1;
			procLeft_  = ourRank_ - 1;
		}
		else if( (ourRank_ + 1)% n_grid_x == 0)
		{
			procLeft_  = ourRank_ - 1;
			nx_		   = params.nx() - (n_grid_x-1) * nx_;
			if( ourRank_ == (n_grid_x - 1)){ procBot_ = ourRank_ + n_grid_x; }
			else if( (ourRank_+1)/n_grid_x == n_grid_y)
			{ 
				procTop_ = ourRank_ - n_grid_x; 				
				ny_      = params.ny() - (n_grid_y-1) * ny_;
			}
			else
			{ 
				procBot_ = ourRank_ + n_grid_x; 
				procTop_ = ourRank_ - n_grid_x; 
			}
		}
		else if( ourRank_ > n_grid_x * (n_grid_y-1) )
		{
			ny_      = params.ny() - (n_grid_y-1) * ny_;
			procTop_ = ourRank_ - n_grid_x;
			procRight_ = ourRank_ + 1;
			procLeft_  = ourRank_ - 1;
		}
		else
		{
			procRight_ = ourRank_ + 1;
			procLeft_  = ourRank_ - 1;
			procTop_   = ourRank_ - n_grid_x;
			procBot_   = ourRank_ + n_grid_x;
		}
    }
    else {
        std::cerr << "Unsupported grid decomposition method! " << params.gridMethod() << std::endl;
        exit(1);
    }
    
    if (params.order() == 2) 
        borderSize_ = 1;
    else if (params.order() == 4)
        borderSize_ = 2;
    else if (params.order() == 8)
        borderSize_ = 4;
    assert(nx_ > 2 * borderSize_);
    assert(ny_ > 2 * borderSize_);

    //TODO: set gx and gy correctly
    gx_ = nx_ + 2 * borderSize_;
    gy_ = ny_ + 2 * borderSize_;

    if (debug) { 
        printf("%d: (%d, %d) (%d, %d) lft: %d rgt: %d top: %d bot: %d\n", \
                ourRank_, nx_, ny_, gx_, gy_, procLeft_, procRight_, procTop_, procBot_);
    }

    //resize and set ICs
    grid_.resize(gx_ * gy_, params.ic());

    int number_of_request = 4;
    //set BCs
    //TODO: fill in locations in grid_ with the correct boundary conditions
    if( procTop_ == -1)
	{
		for(int i=0; i<gx_; ++i)
		{
			for(int j=0; j<borderSize_; ++j)
			{
				grid_[i+j*gx_] = params.topBC();
			}
		}
        number_of_request--;
	}
	if(procBot_ == -1)
	{
		for(int i=0; i<gx_; ++i)
		{
			for(int j=0; j<borderSize_; ++j)
			{
				grid_[i+gx_*(gy_-1)-j*gx_] = params.bottomBC(); 
			}
		}
        number_of_request--;
	}
    if(procRight_ == -1)
    {
		for(int i=0; i<gy_; ++i)
		{
			for(int j=0; j<borderSize_; ++j)
			{
				grid_[gx_*(i+1)-1-j] = params.rightBC();
			}
		}
        number_of_request--;
	}
	if(procLeft_ == -1)
    {
		for(int i=0; i<gy_; ++i)
		{
			for(int j=0; j<borderSize_; ++j)
			{
				grid_[gx_*i+j] = params.leftBC();				
			}
		}
        number_of_request--;
	}
    send_requests_.resize(number_of_request);
    recv_requests_.resize(number_of_request);
    
    if(procLeft_ != -1)
    {
        send_left_buffer_.resize(gy_*borderSize_);       
        recv_left_buffer_.resize(gy_*borderSize_);
    }
    if(procRight_ != -1)
    {
        send_right_buffer_.resize(gy_*borderSize_);
        recv_right_buffer_.resize(gy_*borderSize_);
    }
    //create the copy of the grid we need for ping-ponging
    grid_.insert(grid_.end(), grid_.begin(), grid_.end());
}

void Grid::waitForSends() {
    MPI_Status status;
    for(int i=0; i< send_requests_.size(); ++i)
    {
        MPI_SAFE_CALL(MPI_Wait(&send_requests_[i], &status));
    }
}

void Grid::waitForRecvs() {
    MPI_Status status;
    for(int i=0; i< recv_requests_.size(); ++i)
    {
        MPI_SAFE_CALL(MPI_Wait(&recv_requests_[i], &status));
    }
    // Copy the recv buffer
    if(( procRight_ != -1) || (procLeft_ != -1))
    {
        for(int i=0; i<gy_; ++i)
        {
            for(int j=0; j<borderSize_; ++j)
            {
                if( procLeft_!= -1) 
                {
                    grid_[prev() * gx_ * gy_ + gx_*i + j] = recv_left_buffer_[i*borderSize_+j];
                }
                if( procRight_ != -1)
                {
                    grid_[prev() * gx_ * gy_ + gx_*i + j + nx_ + borderSize_] = recv_right_buffer_[i*borderSize_+j];
                }
            }
        }
    }
}

//sends from previous to current
void Grid::transferHaloDataASync() {
    int i = 0;
    if( procTop_ != -1)
    {
        MPI_SAFE_CALL(MPI_Isend(&grid_[prev() * gx_ * gy_ + gx_*borderSize_], gx_*borderSize_, MPI_DOUBLE, procTop_, 0, MPI_COMM_WORLD, &send_requests_[i]));
        MPI_SAFE_CALL(MPI_Irecv(&grid_[prev() * gx_ * gy_                  ], gx_*borderSize_, MPI_DOUBLE, procTop_, 0, MPI_COMM_WORLD, &recv_requests_[i]));
        ++i;
    }
    if( procBot_ != -1)
    {
        MPI_SAFE_CALL(MPI_Isend(&grid_[prev() * gx_ * gy_ + (gy_-2*borderSize_) * gx_], gx_*borderSize_, MPI_DOUBLE, procBot_, 0, MPI_COMM_WORLD, &send_requests_[i]));
        MPI_SAFE_CALL(MPI_Irecv(&grid_[prev() * gx_ * gy_ + (gy_-  borderSize_) * gx_], gx_*borderSize_, MPI_DOUBLE, procBot_, 0, MPI_COMM_WORLD, &recv_requests_[i]));
        ++i;
    }
    // Copy the send buffer
    if(( procRight_ != -1) || (procLeft_ != -1))
    {
        for(int i=0; i<gy_; ++i)
        {
            for(int j=0; j<borderSize_; ++j)
            {
                if( procLeft_ != -1) 
                {
                    send_left_buffer_[i*borderSize_+j]  = grid_[prev() * gx_ * gy_ + gx_*i + j + borderSize_];
                }
                if( procRight_!= -1)
                {
                    send_right_buffer_[i*borderSize_+j] = grid_[prev() * gx_ * gy_ + gx_*i + j + nx_];
                }
            }
        }
    }
    if( procRight_ != -1)
    {   
        MPI_SAFE_CALL(MPI_Isend(&send_right_buffer_[0], send_right_buffer_.size(), MPI_DOUBLE, procRight_, 0, MPI_COMM_WORLD, &send_requests_[i]));
        MPI_SAFE_CALL(MPI_Irecv(&recv_right_buffer_[0], recv_right_buffer_.size(), MPI_DOUBLE, procRight_, 0, MPI_COMM_WORLD, &recv_requests_[i]));
        ++i;
    }
    if( procLeft_ != -1)
    {   // Copy the send buffer
        MPI_SAFE_CALL(MPI_Isend(&send_left_buffer_[0], send_left_buffer_.size(), MPI_DOUBLE, procLeft_, 0, MPI_COMM_WORLD, &send_requests_[i]));
        MPI_SAFE_CALL(MPI_Irecv(&recv_left_buffer_[0], recv_left_buffer_.size(), MPI_DOUBLE, procLeft_, 0, MPI_COMM_WORLD, &recv_requests_[i]));
        ++i;
    }
}

void Grid::saveStateToFile(std::string identifier) const {
    std::stringstream ss;
    ss << "grid" << ourRank_ << "_" << identifier << ".txt";
    std::ofstream ofs(ss.str().c_str());
    
    ofs << *this << std::endl;

    ofs.close();
}

inline double stencil2(const Grid &grid, int x, int y, double xcfl, double ycfl, const Grid::gridState &prev) {
    return grid(prev, x, y) + 
           xcfl * (grid(prev, x+1, y) + grid(prev, x-1, y) - 2 * grid(prev, x, y)) + 
           ycfl * (grid(prev, x, y+1) + grid(prev, x, y-1) - 2 * grid(prev, x, y));
}

inline double stencil4(const Grid &grid, int x, int y, double xcfl, double ycfl, const Grid::gridState &prev) {
    return grid(prev, x, y) + 
           xcfl * (   -grid(prev, x+2, y) + 16 * grid(prev, x+1, y) -
                    30 * grid(prev, x, y) + 16 * grid(prev, x-1, y) - grid(prev, x-2, y)) + 
           ycfl * (   -grid(prev, x, y+2) + 16 * grid(prev, x, y+1) -
                    30 * grid(prev, x, y) + 16 * grid(prev, x, y-1) - grid(prev, x, y-2));
}

inline double stencil8(const Grid &grid, int x, int y, double xcfl, double ycfl, const Grid::gridState &prev) {
    return grid(prev, x, y) +
           xcfl*(-9*grid(prev,x+4,y) + 128*grid(prev,x+3,y) - 1008*grid(prev,x+2,y) + 8064*grid(prev,x+1,y) -
                                                  14350*grid(prev, x, y) + 
                 8064*grid(prev,x-1,y) - 1008*grid(prev,x-2,y) + 128*grid(prev,x-3,y) - 9*grid(prev,x-4,y)) + 
           ycfl*(-9*grid(prev,x,y+4) + 128*grid(prev,x,y+3) - 1008*grid(prev,x,y+2) + 8064*grid(prev,x,y+1) -
                                                  14350*grid(prev,x,y) +
                8064*grid(prev,x,y-1) -1008*grid(prev,x,y-2) + 128*grid(prev,x,y-3) - 9*grid(prev,x,y-4));
}

void syncComputation(Grid &grid, const simParams &params) {
    //TODO
    MPI_Status status;
    MPI_Request send_right_request, send_left_request, send_top_request, send_bot_request;
    MPI_Request recv_right_request, recv_left_request, recv_top_request, recv_bot_request;

    for(int i=0; i< params.iters(); ++i)
    {
        grid.swapState();
        const Grid::gridState& curr = grid.curr();
        const Grid::gridState& prev = grid.prev();
        grid.transferHaloDataASync();
        grid.waitForSends();
        grid.waitForRecvs();
              
        if (params.order() == 2) 
        {
            for (int y = 2*grid.borderSize(); y < grid.ny(); ++y) 
            {
                for (int x = 2*grid.borderSize(); x < grid.nx(); ++x) 
                {
                    grid(curr, x, y) = stencil2(grid, x, y, params.xcfl(), params.ycfl(), prev);
                }
            }
            // Top and Bottom  
            for (int y = 0; y < grid.borderSize(); ++y) 
            {   
                int y1 = y + grid.borderSize();
                int y2 = y + grid.ny();
                for (int x = grid.borderSize(); x < grid.nx() + grid.borderSize(); ++x) 
                {
                    grid(curr, x, y1) = stencil2(grid, x, y1, params.xcfl(), params.ycfl(), prev);
                    grid(curr, x, y2) = stencil2(grid, x, y2, params.xcfl(), params.ycfl(), prev);
                }
            } 
            // Left and Right
            for (int y = 2*grid.borderSize(); y < grid.ny(); ++y) 
            {
                for (int x = 0; x < grid.borderSize(); ++x) 
                {
                    int x1 = x + grid.borderSize();
                    int x2 = x + grid.nx();
                    grid(curr, x1, y) = stencil2(grid, x1, y, params.xcfl(), params.ycfl(), prev);
                    grid(curr, x2, y) = stencil2(grid, x2, y, params.xcfl(), params.ycfl(), prev);
                }
            } 
        }
        else if (params.order() == 4) {
            for (int y = 2*grid.borderSize(); y < grid.ny(); ++y) 
            {
                for (int x = 2*grid.borderSize(); x < grid.nx(); ++x) 
                {
                    grid(curr, x, y) = stencil4(grid, x, y, params.xcfl(), params.ycfl(), prev);
                }
            }
            // Top and Bottom  
            for (int y = 0; y < grid.borderSize(); ++y) 
            {   
                int y1 = y + grid.borderSize();
                int y2 = y + grid.ny();
                for (int x = grid.borderSize(); x < grid.nx() + grid.borderSize(); ++x) 
                {
                    grid(curr, x, y1) = stencil4(grid, x, y1, params.xcfl(), params.ycfl(), prev);
                    grid(curr, x, y2) = stencil4(grid, x, y2, params.xcfl(), params.ycfl(), prev);
                }
            } 
            // Left and Right
            for (int y = 2*grid.borderSize(); y < grid.ny(); ++y) 
            {
                for (int x = 0; x < grid.borderSize(); ++x) 
                {
                    int x1 = x + grid.borderSize();
                    int x2 = x + grid.nx();
                    grid(curr, x1, y) = stencil4(grid, x1, y, params.xcfl(), params.ycfl(), prev);
                    grid(curr, x2, y) = stencil4(grid, x2, y, params.xcfl(), params.ycfl(), prev);
                }
            } 
        }
        else if (params.order() == 8) 
        {
            for (int y = 2*grid.borderSize(); y < grid.ny(); ++y) 
            {
                for (int x = 2*grid.borderSize(); x < grid.nx(); ++x) 
                {
                    grid(curr, x, y) = stencil8(grid, x, y, params.xcfl(), params.ycfl(), prev);
                }
            }
            // Top and Bottom 
            for (int y = 0; y < grid.borderSize(); ++y) 
            {   
                int y1 = y + grid.borderSize();
                int y2 = y + grid.ny();
                for (int x = grid.borderSize(); x < grid.nx() + grid.borderSize(); ++x) 
                {
                    grid(curr, x, y1) = stencil8(grid, x, y1, params.xcfl(), params.ycfl(), prev);
                    grid(curr, x, y2) = stencil8(grid, x, y2, params.xcfl(), params.ycfl(), prev);
                }
            } 
            // Left and Right
            for (int y = 2*grid.borderSize(); y < grid.ny(); ++y) 
            {
                for (int x = 0; x < grid.borderSize(); ++x) 
                {
                    int x1 = x + grid.borderSize();
                    int x2 = x + grid.nx();
                    grid(curr, x1, y) = stencil8(grid, x1, y, params.xcfl(), params.ycfl(), prev);
                    grid(curr, x2, y) = stencil8(grid, x2, y, params.xcfl(), params.ycfl(), prev);
                }
            } 
        }
    }
}

void asyncComputation(Grid &grid, const simParams &params) {
    //TODO
    //The whole point of the asynchronous communication to is do communication while
    //the border regions are being transferred.  You should structure this routine so that
    //the transfer starts, the computation on the inner region is performed, then the computation
    //on the halo region is performed after making sure the communication is finished.
    MPI_Status status;
    MPI_Request send_right_request, send_left_request, send_top_request, send_bot_request;
    MPI_Request recv_right_request, recv_left_request, recv_top_request, recv_bot_request;

    for(int i=0; i< params.iters(); ++i)
    {
        grid.swapState();
        const Grid::gridState& curr = grid.curr();
        const Grid::gridState& prev = grid.prev();
        grid.transferHaloDataASync();
                
        if (params.order() == 2) 
        {
            for (int y = 2*grid.borderSize(); y < grid.ny(); ++y) 
            {
                for (int x = 2*grid.borderSize(); x < grid.nx(); ++x) 
                {
                    grid(curr, x, y) = stencil2(grid, x, y, params.xcfl(), params.ycfl(), prev);
                }
            }
            grid.waitForSends();
            grid.waitForRecvs();
            // Top and Bottom  
            for (int y = 0; y < grid.borderSize(); ++y) 
            {   
                int y1 = y + grid.borderSize();
                int y2 = y + grid.ny();
                for (int x = grid.borderSize(); x < grid.nx() + grid.borderSize(); ++x) 
                {
                    grid(curr, x, y1) = stencil2(grid, x, y1, params.xcfl(), params.ycfl(), prev);
                    grid(curr, x, y2) = stencil2(grid, x, y2, params.xcfl(), params.ycfl(), prev);
                }
            } 
            // Left and Right
            for (int y = 2*grid.borderSize(); y < grid.ny(); ++y) 
            {
                for (int x = 0; x < grid.borderSize(); ++x) 
                {
                    int x1 = x + grid.borderSize();
                    int x2 = x + grid.nx();
                    grid(curr, x1, y) = stencil2(grid, x1, y, params.xcfl(), params.ycfl(), prev);
                    grid(curr, x2, y) = stencil2(grid, x2, y, params.xcfl(), params.ycfl(), prev);
                }
            } 
        }
        else if (params.order() == 4) {
            for (int y = 2*grid.borderSize(); y < grid.ny(); ++y) 
            {
                for (int x = 2*grid.borderSize(); x < grid.nx(); ++x) 
                {
                    grid(curr, x, y) = stencil4(grid, x, y, params.xcfl(), params.ycfl(), prev);
                }
            }
            grid.waitForSends();
            grid.waitForRecvs();
            // Top and Bottom  
            for (int y = 0; y < grid.borderSize(); ++y) 
            {   
                int y1 = y + grid.borderSize();
                int y2 = y + grid.ny();
                for (int x = grid.borderSize(); x < grid.nx() + grid.borderSize(); ++x) 
                {
                    grid(curr, x, y1) = stencil4(grid, x, y1, params.xcfl(), params.ycfl(), prev);
                    grid(curr, x, y2) = stencil4(grid, x, y2, params.xcfl(), params.ycfl(), prev);
                }
            } 
            // Left and Right
            for (int y = 2*grid.borderSize(); y < grid.ny(); ++y) 
            {
                for (int x = 0; x < grid.borderSize(); ++x) 
                {
                    int x1 = x + grid.borderSize();
                    int x2 = x + grid.nx();
                    grid(curr, x1, y) = stencil4(grid, x1, y, params.xcfl(), params.ycfl(), prev);
                    grid(curr, x2, y) = stencil4(grid, x2, y, params.xcfl(), params.ycfl(), prev);
                }
            } 
        }
        else if (params.order() == 8) 
        {
            for (int y = 2*grid.borderSize(); y < grid.ny(); ++y) 
            {
                for (int x = 2*grid.borderSize(); x < grid.nx(); ++x) 
                {
                    grid(curr, x, y) = stencil8(grid, x, y, params.xcfl(), params.ycfl(), prev);
                }
            }
            grid.waitForSends();
            grid.waitForRecvs();
            // Top and Bottom 
            for (int y = 0; y < grid.borderSize(); ++y) 
            {   
                int y1 = y + grid.borderSize();
                int y2 = y + grid.ny();
                for (int x = grid.borderSize(); x < grid.nx() + grid.borderSize(); ++x) 
                {
                    grid(curr, x, y1) = stencil8(grid, x, y1, params.xcfl(), params.ycfl(), prev);
                    grid(curr, x, y2) = stencil8(grid, x, y2, params.xcfl(), params.ycfl(), prev);
                }
            } 
            // Left and Right
            for (int y = 2*grid.borderSize(); y < grid.ny(); ++y) 
            {
                for (int x = 0; x < grid.borderSize(); ++x) 
                {
                    int x1 = x + grid.borderSize();
                    int x2 = x + grid.nx();
                    grid(curr, x1, y) = stencil8(grid, x1, y, params.xcfl(), params.ycfl(), prev);
                    grid(curr, x2, y) = stencil8(grid, x2, y, params.xcfl(), params.ycfl(), prev);
                }
            } 
        }
    }
}

int main(int argc, char *argv[])
{
    if (argc != 2) {
        std::cerr << "Please supply a parameter file!" << std::endl;
        exit(1);
    }

    MPI_Init(&argc, &argv);

    simParams params(argv[1], true);
    Grid grid(params, true);

    grid.saveStateToFile("init"); //save our initial state, useful for making sure we
                                  //got setup and BCs right

    double start = MPI_Wtime();

    if (params.sync()) {
        syncComputation(grid, params);
    }
    else {
        asyncComputation(grid, params);
    }

    double end = MPI_Wtime();

    if (grid.rank() == 0) {
        std::cout << params.iters() << " iterations on a " << params.nx() << " by " 
                  << params.ny() << " grid took: " << end - start << " seconds." << std::endl;
    }
    grid.saveStateToFile("final"); //final output for correctness checking of computation

    MPI_Finalize(); 
    return 0;
}
