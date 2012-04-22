/* Machine problem 2
 *
 *                            2D Heat Diffusion
 *
 * In this homework you will be implementing a finite difference 2D-Heat Diffusion Solver
 * in two different ways - with and without using shared memory.
 * You will impelement stencils of orders 2, 4 and 8.  A reference CPU implementation
 * has been provided.  You should keep all existing classes, method names, function names,
 * and variables as is.
 *
 * The simParams and Grid classes are provided for convenience. The simParams class will
 * load a file containing all the information needed for the simulation and calculate the
 * maximum stable CFL number.  The Grid will set up a grid with the appropriate boundary and
 * initial conditions.
 *
 * Some general notes about declaring N-dimensional arrays.
 * You may have seen / been taught to do this in the past:
 * int **A = (int **)malloc(numRows * sizeof(int *));
 * for (int r = 0; r < numRows; ++r)
 *     A[r] = (int *)malloc(numCols * sizeof(int));
 *
 * so that you can then access elements of A with the notation A[row][col], which involes dereferencing
 * two pointers.  This is a REALLY BAD way to represent 2D arrays for a couple of reasons.
 * 
 * 1) For a NxN array, it does N+1 mallocs which is slow.  And on the gpu setting up this data 
 *    structure is a pain in the butt.  But you _should_ know how to do it.
 * 2) There is absolutely no guarantee that different rows are even remotely close in memory;
 *    subsequent rows could be allocated on complete opposite sides of the address space
 *    which leads to terrible cache characteristics
 * 3) The double indirection leads to really high memory latency.  To access location A[i][j],
 *    first we have to make a trip to memory to fetch A[i], and once we get that pointer, we have to make another
 *    trip to memory to fetch (A[i])[j].  It would be far better if we only had to make one trip to 
 *    memory.  This is _especially_ important on the gpu.
 *
 * The BETTER WAY - just allocate one 1-D array of size N*N.  Then just calculate the correct offset -
 * A[i][j] = *(A + i * numCols + j).  There is only one allocation, adjacent rows are as close as they can be
 * and we only make one trip to memory to fetch a value.  The grid implements this storage scheme 
 * "under the hood" and overloads the () operator to allow the more familiar (x, y) notation.
 *
 * For the GPU code in this exercise you don't need to worry about trying to be fancy and overload an operator
 * or use some #define macro magic to mimic the same behavior - you can just do the raw addressing calculations. 
 *
 * For the first part of the homework where you will implement the kernels without using shared memory
 * each thread should compute exactly one output and you should use 2D blocks that are 16x16.
 *
 * For the second part with shared memory - it is recommended that you use 1D blocks since the ideal
 * implementation will have each thread outputting more than 1 value and the addressing arithmetic
 * will be easier with 1D blocks.
 *
 * Notice that the reference CPU computation and Grid class are templated so that they can be
 * declared with either float or double.  You are required to implement the non-shared memory
 * version to support both floats and doubles. A template is the easiest way to do this.
 * A double version that uses the shared memory is harder since it changes how many values can
 * fit in the memory.  You are not required to implement this.
 *
 * 1) You should make sure all your versions (different techniques and orders) match the results
 *    of the cpu.
 *
 * 2) Once this is achieved you should make plots of bandwidth and flops vs. (total # grid points)
 *    Put all orders on the same plots, but shared/no shared on separate plots.  Make a separate plot,
 *    for 4th order only, and put both float & double on this plot. 
 *
 * 3) How does your shared memory version compared to the cache only version? You should be able to achieve at
 *    least a 50% speedup.  How does the speedup change with order? Why?  Do larger orders favor the cache
 *    or shared memory more?
 * 
 * 4) How do the flops and bandwidth compare between the float and double versions?
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

#include <thrust/host_vector.h>
#include <thrust/device_vector.h>

#include "mp1-util.h"

class simParams {
    public:
        simParams(const char *filename, bool verbose); //parse command line
                                                       //does no error checking
        simParams(); //use some default values

        int    nx()         const {return nx_;}
        int    ny()         const {return ny_;}
        int    gx()         const {return gx_;}
        int    gy()         const {return gy_;}
        double lx()         const {return lx_;}
        double ly()         const {return ly_;}
        double alpha()      const {return alpha_;}
        int    iters()      const {return iters_;}
        double dx()         const {return dx_;}
        double dy()         const {return dy_;}
        double ic()         const {return ic_;}
        int    order()      const {return order_;}
        int    borderSize() const {return borderSize_;}
        double xcfl()       const {return xcfl_;}
        double ycfl()       const {return ycfl_;}
        double topBC()      const {return bc[0];}
        double leftBC()     const {return bc[1];}
        double bottomBC()   const {return bc[2];}
        double rightBC()    const {return bc[3];}

    private:
        int    nx_, ny_;     //number of grid points in each dimension
        int    gx_, gy_;     //number of grid points including halos
        double lx_, ly_;     //extent of physical domain in each dimension
        double alpha_;       //thermal conductivity
        double dt_;          //timestep
        int    iters_;       //number of iterations to do
        double dx_, dy_;     //size of grid cell in each dimension
        double ic_;          //uniform initial condition
        double xcfl_, ycfl_; //cfl numbers in each dimension
        int    order_;       //order of discretization
        int    borderSize_;  //number of halo points
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

    bc[0] = 0.;
    bc[1] = 10.;
    bc[2] = 0.;
    bc[3] = 10.;

    calcDtCFL();

    borderSize_ = 0;
    if (order_ == 2)
        borderSize_ = 1;
    else if (order_ == 4)
        borderSize_ = 2;
    else if (order_ == 8)
        borderSize_ = 4;

    gx_ = nx_ + 2 * borderSize_;
    gy_ = ny_ + 2 * borderSize_;
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
    ifs >> order_;
    ifs >> ic_;
    ifs >> bc[0] >> bc[1] >> bc[2] >> bc[3];

    ifs.close();

    dx_ = lx_ / (nx_ - 1);
    dy_ = ly_ / (ny_ - 1);

    calcDtCFL();

    borderSize_ = 0;
    if (order_ == 2)
        borderSize_ = 1;
    else if (order_ == 4)
        borderSize_ = 2;
    else if (order_ == 8)
        borderSize_ = 4;

    gx_ = nx_ + 2 * borderSize_;
    gy_ = ny_ + 2 * borderSize_;

    if (verbose) {
        printf("nx: %d ny: %d\ngx: %d gy: %d\nlx %f: ly: %f\nalpha: %f\niterations: %d\norder: %d\nic: %f\n", 
                nx_, ny_, gx_, gy_, lx_, ly_, alpha_, iters_, order_, ic_);
        printf("dx: %f dy: %f\ndt: %f xcfl: %f ycfl: %f\n", 
                dx_, dy_, dt_, xcfl_, ycfl_);
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
        std::cerr << "Unsupported discretization order." << std::endl;
        exit(1);
    }
}

template<typename floatType>
class Grid {
    public:
        Grid(const simParams &params, bool debug);
        ~Grid() { }

        typedef int gridState;

        int gx() const {return gx_;}
        int gy() const {return gy_;}
        int nx() const {return nx_;}
        int ny() const {return ny_;}
        int borderSize() const {return borderSize_;}
        const gridState & curr() const {return curr_;}
        const gridState & prev() const {return prev_;}
        void swapState() {prev_ = curr_; curr_ ^= 1;} 

        //for speed doesn't do bounds checking
        floatType operator()(const gridState & selector, 
                                 int xpos, int ypos) const {
            return hGrid_[selector * gx_ * gy_ + ypos * gx_ + xpos];
        }

        floatType& operator()(const gridState & selector, 
                                  int xpos, int ypos) {
            return hGrid_[selector * gx_ * gy_ + ypos * gx_ + xpos];
        }

        void saveStateToFile(std::string identifier) const;
        std::vector<floatType> getGrid() const {return hGrid_;}

        template <class U> friend std::ostream & operator<<(std::ostream &os, const Grid<U>& grid);

    private:
        std::vector<floatType> hGrid_;

        int gx_, gy_;             //total grid extents
        int nx_, ny_;             //non-boundary region
        int borderSize_;          //number of halo cells

        gridState curr_;
        gridState prev_;

        bool debug_;

        //prevent copying and assignment since they are not implemented
        //and don't make sense for this class
        Grid(const Grid &);
        Grid& operator=(const Grid &);

};

template<typename floatType>
std::ostream& operator<<(std::ostream& os, const Grid<floatType> &grid) {
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

template<typename floatType>
Grid<floatType>::Grid(const simParams &params, bool debug) {
    debug_ = debug;

    curr_ = 1;
    prev_ = 0;

    if (params.order() == 2) 
        borderSize_ = 1;
    else if (params.order() == 4)
        borderSize_ = 2;
    else if (params.order() == 8)
        borderSize_ = 4;

    ny_ = params.ny();
    nx_ = params.nx();

    assert(nx_ > 2 * borderSize_);
    assert(ny_ > 2 * borderSize_);

    gx_ = nx_ + 2 * borderSize_;
    gy_ = ny_ + 2 * borderSize_;
   
    if (debug) { 
        printf("(%d, %d) (%d, %d)\n", nx_, ny_, gx_, gy_);
    }

    //resize and set ICs
    hGrid_.resize(gx_ * gy_, params.ic());

    //set BCs
    for (int i = 0; i < gx_; ++i) {
        for (int j = 0; j < borderSize_; ++j) {
            (*this)(prev_, i, j) = params.bottomBC();
        }

        for (int j = 0; j < borderSize_; ++j) {
            (*this)(prev_, i, borderSize_ + ny_ + j) = params.topBC();
        }
    }

    for (int j = 0; j < gy_; ++j) {
        for (int i = 0; i < borderSize_; ++i) {
            (*this)(prev_, i, j) = params.leftBC();
        }

        for (int i = 0; i < borderSize_; ++i) {
            (*this)(prev_, borderSize_ + nx_ + i, j) = params.rightBC();
        }
    }

    //create the copy of the grid we need for ping-ponging
    hGrid_.insert(hGrid_.end(), hGrid_.begin(), hGrid_.end());
}

template<typename floatType>
void Grid<floatType>::saveStateToFile(std::string identifier) const {
    std::stringstream ss;
    ss << "grid" << "_" << identifier << ".txt";
    std::ofstream ofs(ss.str().c_str());
    
    ofs << *this << std::endl;

    ofs.close();
}

template <typename floatType>
inline floatType stencil2(const Grid<floatType> &grid, int x, int y, floatType xcfl, floatType ycfl, const typename Grid<floatType>::gridState &prev) {
    return grid(prev, x, y) + 
           xcfl * (grid(prev, x+1, y) + grid(prev, x-1, y) - 2 * grid(prev, x, y)) + 
           ycfl * (grid(prev, x, y+1) + grid(prev, x, y-1) - 2 * grid(prev, x, y));
}

template<typename floatType>
inline floatType stencil4(const Grid<floatType> &grid, int x, int y, floatType xcfl, floatType ycfl, const typename Grid<floatType>::gridState &prev) {
    return grid(prev, x, y) + 
           xcfl * (   -grid(prev, x+2, y) + 16 * grid(prev, x+1, y) -
                    30 * grid(prev, x, y) + 16 * grid(prev, x-1, y) - grid(prev, x-2, y)) + 
           ycfl * (   -grid(prev, x, y+2) + 16 * grid(prev, x, y+1) -
                    30 * grid(prev, x, y) + 16 * grid(prev, x, y-1) - grid(prev, x, y-2));
}

template<typename floatType>
inline floatType stencil8(const Grid<floatType> &grid, int x, int y, floatType xcfl, floatType ycfl, const typename Grid<floatType>::gridState &prev) {
    return grid(prev, x, y) + 
           xcfl * (   -9*grid(prev, x+4, y) + 128 * grid(prev, x+3, y) - 1008 * grid(prev, x+2, y) + 8064 * grid(prev, x+1, y) -
                                                     14350 * grid(prev, x, y) +
                      8064 * grid(prev, x-1, y) - 1008 * grid(prev, x-2, y) + 128 * grid(prev, x-3, y) -9 * grid(prev,x-4,y)) +
           ycfl * (   -9*grid(prev, x, y+4) + 128 * grid(prev, x, y+3) - 1008 * grid(prev, x, y+2) + 8064 * grid(prev,x, y+1) -
                                                     14350 * grid(prev, x, y) + 
                     8064 * grid(prev, x, y-1) - 1008 * grid(prev, x, y-2) + 128 * grid(prev, x, y-3) - 9 * grid(prev, x, y-4));
}

template <typename floatType>
void cpuComputation(Grid<floatType> &grid, const simParams &params) {
    std::string text;
    if (sizeof(floatType) == 4)
        text = "cpu computation float";
    else
        text = "cpu computation double";

    event_pair timer;
    start_timer(&timer);
    floatType xcfl = params.xcfl();
    floatType ycfl = params.ycfl();

    for (int i = 0; i < params.iters(); ++i) {
        grid.swapState();
        const typename Grid<floatType>::gridState& curr = grid.curr();
        const typename Grid<floatType>::gridState& prev = grid.prev();
        if (params.order() == 2) {
            for (int y = grid.borderSize(); y < grid.ny() + grid.borderSize(); ++y) {
                for (int x = grid.borderSize(); x < grid.nx() + grid.borderSize(); ++x) {
                    grid(curr, x, y) = stencil2(grid, x, y, xcfl, ycfl, prev);
                }
            }
        }
        else if (params.order() == 4) {
            for (int y = grid.borderSize(); y < grid.ny() + grid.borderSize(); ++y) {
                for (int x = grid.borderSize(); x < grid.nx() + grid.borderSize(); ++x) {
                    grid(curr, x, y) = stencil4(grid, x, y, xcfl, ycfl, prev);
                }
            }
        }
        else if (params.order() == 8) {
            for (int y = grid.borderSize(); y < grid.ny() + grid.borderSize(); ++y) {
                for (int x = grid.borderSize(); x < grid.nx() + grid.borderSize(); ++x) {
                    grid(curr, x, y) = stencil8(grid, x, y, xcfl, ycfl, prev);
                }
            }
        }
    }
    stop_timer(&timer, text.c_str());
}

__global__
void gpu2ndOrderStencil(float *curr, float *prev, int gx, int gy, int nx, int ny, float xcfl, float ycfl, int borderSize)
{
    //TODO: fill me in
    //use 2D blocks
}

__global__
void gpu4thOrderStencil(float *curr, float *prev, int gx, int gy, int nx, int ny, float xcfl, float ycfl, int borderSize)
{
    //TODO: fill me in
    //use 2D blocks
}

__global__
void gpu8thOrderStencil(float *curr, float *prev, int gx, int gy, int nx, int ny, float xcfl, float ycfl, int borderSize)
{
    //TODO: fill me in
    //use 2D blocks
}

//because only the stencil computation differs between the different kernels
//we don't want to duplicate all that code, BUT we also don't want to introduce a ton of
//if statements inside the kernel.  So we use a template to compile only the correct code
template<int side, int usefulSide, int borderSize, int order, int numThreads>
__global__
void gpuShared(float *curr, float *prev, int gx, int gy, int nx, int ny, float xcfl, float ycfl)
{
    //TODO: figure out where this block should load from

    //TODO: load into our shared memory
    __syncthreads();

    //now that everything is loaded is smem, do the stencil calculation, we can store directly to global memory if we make sure to coalesce
    //we can use a conditional based on the order, ie,
    //if (order == 2) {
    // ...
    //}
    //else if (order == 4) {
    // ...
    //}
    //...
    //since it is a template parameter and the conditional will be evaluated at compile time
    //NOT runtime!
}

//For the non-shared version we've given a mostly complete implementation of the host function
//that calls the kernels.  This takes care of the ping-ponging for you.  You will have to impelment
//the equivalent function for shared memory only on your own.
void gpuComputation(std::vector<float> &hInitialCondition, const simParams &params, std::vector<float> &hResults) {
    thrust::device_vector<float> dGridVec = hInitialCondition;
    float * dGrid = thrust::raw_pointer_cast(&dGridVec[0]);

    int totalSize = params.gx() * params.gy();
    dim3 threads(16, 16);
    dim3 blocks(1, 1/*TODO fill in correct 2D grid dimensions*/);
    int curr = 0;
    int prev = 1;
    event_pair timer;
    start_timer(&timer);
    if (params.order() == 2) {
        for (int i = 0; i < params.iters(); ++i) {
            prev = curr;
            curr ^= 1; //binary XOR
            gpu2ndOrderStencil<<<blocks, threads >>>(dGrid + curr * totalSize, dGrid + prev * totalSize, 
                    params.gx(), params.gy(), params.nx(), params.ny(), params.xcfl(), params.ycfl(), params.borderSize());
            check_launch("2ndOrderStencil");
        }
    }
    else if (params.order() == 4) {
        for (int i = 0; i < params.iters(); ++i) {
            prev = curr;
            curr ^= 1; //binary XOR
            gpu4thOrderStencil<<<blocks, threads >>>(dGrid + curr * totalSize, dGrid + prev * totalSize, 
                    params.gx(), params.gy(), params.nx(), params.ny(), params.xcfl(), params.ycfl(), params.borderSize());
            check_launch("4thOrderStencil");
        }
    }
    else if (params.order() == 8) {
        for (int i = 0; i < params.iters(); ++i) {
            prev = curr;
            curr ^= 1; //binary XOR
            gpu8thOrderStencil<<<blocks, threads >>>(dGrid + curr * totalSize, dGrid + prev * totalSize, 
                    params.gx(), params.gy(), params.nx(), params.ny(), params.xcfl(), params.ycfl(), params.borderSize());
            check_launch("8thOrderStencil");
        }
    }
    stop_timer(&timer, "gpu computation float");
    hResults.resize(totalSize);
    thrust::copy(dGridVec.begin() + curr * totalSize, dGridVec.end() - prev * totalSize, hResults.begin()); //only copy the last updated copy to the cpu
}

void gpuComputationShared8thOrder(std::vector<float> &hInitialCondition, const simParams &params, std::vector<float> &hResults) {
    const int borderSize = 4;
    assert(borderSize == params.borderSize()); //we hard code the borderSize so that we can use it with templates
                                               //but make sure that the value in the parameters agrees with us, just in case

    //TODO: Everything else is up to you
}

void gpuComputationShared4thOrder(std::vector<float> &hInitialCondition, const simParams &params, std::vector<float> &hResults) {
    const int borderSize = 2;
    assert(borderSize == params.borderSize()); //we hard code the borderSize so that we can use it with templates
                                               //but make sure that the value in the parameters agrees with us, just in case

    //TODO: Everything else is up to you
}

void gpuComputationShared2ndOrder(std::vector<float> &hInitialCondition, const simParams &params, std::vector<float> &hResults) {
    const int borderSize = 1;
    assert(borderSize == params.borderSize()); //we hard code the borderSize so that we can use it with templates
                                               //but make sure that the value in the parameters agrees with us, just in case

    //TODO: Everything else is up to you
}

template<typename floatType>
void outputGrid(std::vector<floatType> &data, const simParams &params, std::string txt)
{
    std::stringstream ss;
    ss << "grid" << "_" << txt << ".txt";
    std::ofstream ofs(ss.str().c_str());
    
    ofs << std::setprecision(3);
    for (int y = params.gy() - 1; y != -1; --y) {
        for (int x = 0; x < params.gx(); x++) {
            ofs << std::setw(5) << data[y * params.gx() + x] << " ";
        }
        ofs << std::endl;
    }
    ofs << std::endl;

    ofs.close();
}

template <typename floatType>
int checkErrors(const Grid<floatType> &grid, const std::vector<floatType> &hGpuGrid, const simParams &params)
{
    //check that we got the same answer
    int error = 0;
    for (int x = params.borderSize(); x < params.gx() - params.borderSize(); ++x) {
        for (int y = params.borderSize(); y < params.gy() - params.borderSize(); ++y) {
            if (!AlmostEqual2sComplement(hGpuGrid[y * params.gx() + x], grid(grid.curr(), x, y), 10)) {
                if (error < 10) {
                    printf("Mis-match at pos: (%d, %d) cpu: %f, gpu: %f\n", x, y, grid(grid.curr(), x, y), hGpuGrid[y * params.gx() + x]);
                }
                ++error;
            }
        }
    }

    if (error)
        printf("There were %d total locations where there was a difference between the cpu and gpu\n", error);

    return error;
}


int main(int argc, char *argv[])
{
    if (argc != 2) {
        std::cerr << "Please supply a parameter file!" << std::endl;
        exit(1);
    }

    simParams params(argv[1], true);
    Grid<float> grid(params, true);

    grid.saveStateToFile("init"); //save our initial state, useful for making sure we
                                  //got setup and BCs right

    std::vector<float> hInitialCondition = grid.getGrid(); //make a copy of the initial state for the GPU
    std::vector<float> hInitialConditionShared = hInitialCondition;

    cpuComputation(grid, params);
    grid.saveStateToFile("final_cpu");

    std::vector<float> hOutput;
    gpuComputation(hInitialCondition, params, hOutput);
    checkErrors(grid, hOutput, params);
    outputGrid(hOutput, params, "final_gpu_simple");
    
    if (params.order() == 2)
        gpuComputationShared2ndOrder(hInitialConditionShared, params, hOutput);
    else if (params.order() == 4)
        gpuComputationShared4thOrder(hInitialConditionShared, params, hOutput);
    else if (params.order() == 8)
        gpuComputationShared8thOrder(hInitialConditionShared, params, hOutput);

    checkErrors(grid, hOutput, params);

    outputGrid(hOutput, params, "final_gpu_shared");

    return 0;
}
