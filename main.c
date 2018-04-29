#include <mpi.h>
#include <stdio.h>
#include <stdlib.h>

//#define BGQ 1 // when running BG/Q, comment out when running on kratos
#ifdef BGQ
#include <hwi/include/bqc/A2_inlines.h>
#else
#define GetTimeBase MPI_Wtime
#endif

#define TICKS 32 //how many ticks of time to run this for 
#define BLOCK 4 //size of a communication block; each block gets one dispatcher rank

typedef struct{
    int location; //node id of a location
    int price; //price at that location
} LocPrice;

typedef struct {
    int node_id; //id of this node
    int buy_price; //how much it buys an item for
    int sell_price; //how much it sells an item for
    int* connected; //array of nodes it is connected to
} Node;

typedef struct {
    int agent_id; //id of this agent
    int inventory; //size of its inventory
    int location; //node_id of the node it is currently at 
    LocPrice* prices; //array of locations it has visited and their price
} Agent;

/* Writes the agent with specified id to a file for shared memory access */
void writeAgentToFile(int agent_id) {

    
}

/* Handles code for dispatcher ranks */
void dispatcherOp() {
    int mpi_rank = -1;
    int command;
    MPI_Comm_rank(MPI_COMM_WORLD, &mpi_rank);
    //go through event list
    //send completion message out when complete
    if (nomoreventsintime) {
        *command = 0;
        int i, destination;
        for (i=1; i < BLOCK; i++) {
            destination = mpi_rank + i;
            MPI_Send(&command, 1, MPI_INT, destination, 0, MPI_COMM_WORLD);//send exit command out to all ranks in this block
        }
    }
}

/* Handles code for event handler ranks */
void handlerOp() {
    int mpi_rank = -1;
    MPI_Comm_rank(MPI_COMM_WORLD, &mpi_rank);
    int dispatcher = mpi_rank - (mpi_rank % BLOCK); // calculate the dispatcher id for this rank
    int done = 0;
    while (!done) {
        //wait for a command
        int command;
        MPI_Recv(&command, 1, MPI_INT, dispatcher, 0, MPI_COMM_WORLD,
            MPI_STATUS_IGNORE);

        // XXX TODO: full list of commands
        switch(command) { //determine what the command is and execute properly
            case 1: //
                //get an event from MPI recv and schedule it
                
                //process event on node network

                //check for inconsistencies on that node

                // reconcile them

                //update state
            case 0: 
                //return when received completion message
                done = 1;
        }
    }
}

int main (int argc, char** argv) {
    // set up info for timing
    double time_in_secs = 0;
    double processor_frequency = 1600000000.0;
    unsigned long long start_cycles=0;
    unsigned long long end_cycles=0;

    // initialize MPI
    int world_size = -1;
    int mpi_rank = -1;
    MPI_Init(&argc, &argv);
    MPI_Comm_size(MPI_COMM_WORLD, &world_size);
    MPI_Comm_rank(MPI_COMM_WORLD, &mpi_rank);

    if(world_rank == 0) { //start timer
        start_cycles= GetTimeBase();
    }

    if(world_rank == 0) { //config world and agents
        //config world network
        //config agents
    }

    //determine if rank is dispatcher
    //NOTE: there must be at least 1 block of ranks for this program to operate
    int dispatcher = 0;
    if (mpi_rank % BLOCK == 0) { //the first rank in each block gets to be the dispatcher
        dispatcher = 1;
    }

    if (dispatcher == 1) { //if dispatcher
        dispatcherOp();
    } else { //if not dispatcher
        handlerOp();
    }
    if (world_rank == 0) { //end timer
        end_cycles= GetTimeBase();
        time_in_secs = ((double)(end_cycles - start_cycles)) /
        processor_frequency;

        printf("%lld ", global_sum);
        printf("%f\n", time_in_secs);
    }

    MPI_Barrier(MPI_COMM_WORLD); //wait for all processes
    MPI_Finalize();//close down this MPI
    return 0;
}