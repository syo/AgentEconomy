#include <mpi.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdbool.h>
#include <string.h>

//#define BGQ 1 // when running BG/Q, comment out when running on kratos
#ifdef BGQ
#include <hwi/include/bqc/A2_inlines.h>
#else
#define GetTimeBase MPI_Wtime
#endif

#define TICKS 32 //how many ticks of time to run this for 
#define BLOCK 2 //size of a communication block; each block gets one dispatcher rank
#define TIME_COST 1 //how much each unit of distance travelled costs the agent
#define INVENTORY_CAP 10 //how much an agent can carry at maximum
#define EXPLORE_THRESHOLD -100 //At what point a lack of profitable moves causes the agent to explore randomly
#define AGENTS 20 //Number of agents, temporary, should be replaced with variable
#define NODES 20 // Number of nodes

int num_agents, num_nodes, ticks, block_size, time_cost, explore_threshold; // set by arguments of program call

typedef struct{
    int location; //node id of a location
    int price; //price at that location
} LocPrice;

typedef struct Node{
    int node_id; //id of this node
    int buy_price; //how much it buys an item for
    int sell_price; //how much it sells an item for
	int advanced_time; //What the locally updated time is(for determining consistency)
	struct Node* previous_state; //For rolling back node states
    int* connected; //array of nodes it is connected to
	unsigned int connection_size; //number of nodes it is connected to
} Node;

typedef struct {
    int agent_id; //id of this agent
    int inventory; //size of its inventory
    int location; //node_id of the node it is currently at 
	int advanced_time; //
	
    LocPrice* prices; //array of locations it has visited and their price
	unsigned int prices_size;
} Agent;

typedef struct Event{
	int location;//Id of node the event occurs at
	int agent_id;//Id of agent that is invoking the event
	int time;//Time event is scheduled to occur
	struct Event* next;
	struct Event* previous;
} Event;

typedef struct Subrank{
	int mpi_rank;
	struct Subrank* next;
} Subrank;

Node* nodes; // Array of all nodes in the project, ordered by node_id

Agent* agents; // Array of all agents in the project, by agent_id

/* Writes the agent with specified id to a file for shared memory access */
void writeAgentToFile(Agent agent) {
    // Open the agents file
    MPI_File outfile;
    MPI_Status status;
    MPI_File_open(MPI_COMM_WORLD, "agents.txt", MPI_MODE_WRONLY | MPI_MODE_CREATE, MPI_INFO_NULL, &outfile);
    
    // Get the agent string
    // Placeholder
    char* agent_str = "some agent string";

    // Write the agent string to agents.txt
    MPI_File_write_at(outfile, agent.agent_id, agent_str, strlen(agent_str), MPI_CHAR, &status);

    // Close the file
    MPI_File_close(&outfile);
}

bool isEventBefore(Event* next_event,int time){
	while(next_event != NULL)
	{
		if(next_event->time < TICKS)
		{
			return true;
		}
		next_event = next_event->previous;
	}
	return false;
}

/* check to see if two nodes are connected */
bool isNeighbor(int current, int q) {
    int i;
    int arrsize = sizeof(nodes[current].connected) / sizeof(int);
    for (i=0; i < arrsize; i++) { // loop through all neighbor nodes
        if (nodes[current].connected[i] == q) { //if it matches, return true
            return true;
        }
    } 
    return false;
}

/* implements dijkstras algorithm for finding the shortest path length */
int shortestPath(int start_node, int dest_node, int* next_hop) {
     int dist[NODES]; //distance of node i to starting node
     int prev[NODES]; //previous nodes in best path
     int visited[NODES] = {0}; //has node been visited
     int current = start_node; //currently at starting node
     
	 if(start_node == dest_node)
	 {
		*next_hop = start_node;
		return 0;
	 }
	 
     int i;
     for (i=0; i < NODES; i++) {
         dist[i] = 9999;
         prev[i] = -1;
     }
     
     dist[start_node] = 0;

     int min, d, m;
     while(visited[dest_node] == 0) { //while the target has not been visited
        min = 9999;
        m = 0;
        for(i=0; i < NODES; i++) { //go through each neighbor node
            if (isNeighbor(current, i) == false) { //if this node is not connected to the current node
                continue;
            }
            d = dist[current] + TIME_COST; //increment distance
            if (d < dist[i] && visited[i] == 0) { //if this is the shortest path and we have not visited it
                dist[i] = d; //the distance to it is d
                prev[i] = current; //the previous node is the current node
            }
            if (dist[i] < min && visited[i] == 0) { //if this is the minimum path
                min = dist[i]; //set it to be so
                m = i;
            }
        }
        current = m;
        visited[current] = 1;
     }
	 *next_hop = dest_node;
	 while(prev[*next_hop] != start_node)
		*next_hop = prev[*next_hop];
     return dist[dest_node];
}

/* return true if there is an event occurring before end_time 
bool isEventBefore(Event* events,int events_size,int end_time){
    int i;
    for (i=0; i < events_size; i++) { //iterate through events
        if (events[i].time < end_time) { //if there is an event that occurs before the specified time
            return true; 
        }
    }
    return false;
}
/*

/* Handles code for dispatcher ranks */
void dispatcherOp() {
    int mpi_rank = -1;
    int command;
    MPI_Comm_rank(MPI_COMM_WORLD, &mpi_rank);
	Event* events = NULL;
	Event* next_event = NULL;
	//Initialize event list
	for(int i = 0; i < AGENTS; i++)
	{
		Event* new_event = malloc(sizeof(Event));
		new_event->location = i;
		new_event->agent_id = i;
		new_event->time = 0;
		if(events != NULL)
		{
			events->previous = new_event;
		}
		new_event->previous = NULL;
		new_event->next = events;
		events = new_event;
		if(next_event == NULL)
		{
			next_event = events;
		}
	}
	
	Subrank* available_ranks = NULL;
	
	//Acquire all the handler ranks under us
	for(int i = 1; i < BLOCK; i++)
	{
		Subrank* new_rank = malloc(sizeof(Subrank));
		new_rank->mpi_rank = mpi_rank + i;
		new_rank->next = available_ranks;
		available_ranks = new_rank;
	}
	
	//Create buffer for buffered sends
	void* buf = calloc(sizeof(char),3*BLOCK*sizeof(int)+3*MPI_BSEND_OVERHEAD+1);
	MPI_Buffer_attach(buf,3*BLOCK*sizeof(int)+3*MPI_BSEND_OVERHEAD+1);
	
    //go through event list
	while(isEventBefore(next_event,TICKS)){
		//Dispatch events here.
		//Get next event that is within time limit
		if(available_ranks != NULL)
		{
			while(next_event != NULL)
			{
				Event* e = next_event;
				if( e->time < TICKS)
				{
					break;
				}
				next_event = next_event->previous;
				free(e);
			}
			command = 1;
			int agent_id = next_event->agent_id;
			//Send event handling command to task
			MPI_Bsend(&command, 1, MPI_INT,available_ranks->mpi_rank,0,MPI_COMM_WORLD);
			MPI_Bsend(&agent_id, 1, MPI_INT, available_ranks->mpi_rank,1,MPI_COMM_WORLD);
			Subrank* temp_rank = available_ranks;
			available_ranks = available_ranks->next;
			free(temp_rank);
		}
		
		
		
		int flag;
		MPI_Status stat;
		int in_command;
		MPI_Iprobe(MPI_ANY_SOURCE,0,MPI_COMM_WORLD,&flag,&stat);
			
		while(flag != 0)
		{
			MPI_Recv(&in_command, 1, MPI_INT, stat.MPI_SOURCE, 0, MPI_COMM_WORLD,
				MPI_STATUS_IGNORE);
			
			Subrank* new_rank = malloc(sizeof(Subrank));
			int best_hop,agent_id,time;
			switch(in_command)
			{
				case 0:
					//Add rank back to available ranks
					new_rank->mpi_rank = stat.MPI_SOURCE;
					new_rank->next = available_ranks;
					available_ranks = new_rank;
					break;
				case 2:
					//Create new event
					MPI_Recv(&best_hop, 1, MPI_INT, stat.MPI_SOURCE, 2, MPI_COMM_WORLD,MPI_STATUS_IGNORE);
					MPI_Recv(&agent_id, 1, MPI_INT, stat.MPI_SOURCE, 2, MPI_COMM_WORLD,MPI_STATUS_IGNORE);
					MPI_Recv(&time, 1, MPI_INT, stat.MPI_SOURCE, 2, MPI_COMM_WORLD,MPI_STATUS_IGNORE);
					Event* new_event = malloc(sizeof(Event));
					new_event->location = best_hop;
					new_event->agent_id = agent_id;
					new_event->time = time;
					break;
				default:
					break;
			}
			
			MPI_Iprobe(MPI_ANY_SOURCE,0,MPI_COMM_WORLD,&flag,&stat);
		}
	}
	
    //send completion message out when complete
	command = 0;
	int i, destination;
	for (i=1; i < BLOCK; i++) {
		destination = mpi_rank + i;
		MPI_Send(&command, 1, MPI_INT, destination, 0, MPI_COMM_WORLD);//send exit command out to all ranks in this block
	}
}

/* Handles code for event handler ranks */
void handlerOp() {
	void* buf = calloc(sizeof(char),5*sizeof(int)+5*MPI_BSEND_OVERHEAD+1);
	MPI_Buffer_attach(buf,5*sizeof(int)+5*MPI_BSEND_OVERHEAD+1);
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
			case 2: //Update state for node
				command = 2;
				int node_id;
				int new_price;
				int new_time;
				
				MPI_Recv(&node_id, 1, MPI_INT, dispatcher,2,MPI_COMM_WORLD,MPI_STATUS_IGNORE);
				MPI_Recv(&new_price, 1, MPI_INT, dispatcher,2,MPI_COMM_WORLD,MPI_STATUS_IGNORE);
				MPI_Recv(&new_time, 1, MPI_INT, dispatcher,2,MPI_COMM_WORLD,MPI_STATUS_IGNORE);
								
				Node* node = &nodes[node_id];
				
				//Create a new node with updated parameters and replace the old node with it.
				//TEMPORARY: Just use the reference into the list
				Node* new_node = node;
				//new_node->node_id = node->node_id;
				new_node->buy_price = new_price;
				//new_node->sell_price = sell_price;
				new_node->advanced_time = new_time;
				//new_node->previous_state = node;
				//new_node->connected = calloc(sizeof(int),node->connection_size);
				/*for(int i = 0; i < node->connection_size; i++)
					new_node.connected[i] = node.connected[i];
				*/
				
                //update state
				break;
				
            case 1: //Tag corresponds to case of command, i.e. a case 1/evaluate event reads messages with tag 1
                //get an event from MPI recv and schedule it
				command = 1;
				int agent_id;
				
				MPI_Recv(&agent_id, 1, MPI_INT, dispatcher,1,MPI_COMM_WORLD,MPI_STATUS_IGNORE);
				
				Agent* agent = &agents[agent_id];
				
				int most_profit_node;
				int highest_profit = -1000000000;//Arbitrary large negative number. INT_MIN can invoke over/underflow when manipulated
				int best_hop;
				int arrive_time;
				
				node = &nodes[agent->location];
				node->advanced_time = agent->advanced_time;
				
				//Find the most profitable way to sell/fill our inventory.
				for(int i = 0; i < agent->prices_size ; i++)
				{
					//Check if we're considering the current node, update price knowledge
					if(i == agent->location)
					{
						agent->prices[i].price = node->buy_price;
					}
					//Check if we have a valid price for the node we're considering
					if(agent->prices[i].price == -1)
					{
						continue;
					}
					
					int next_hop;
					int dist_cost = shortestPath(agent->location,agent->prices[i].location,&next_hop) * TIME_COST;
					int profit;
					//If we have inventory sell it, if we don't try to fill it
					if(agent->inventory != 0)
						profit = agent->inventory * agent->prices[i].price;
					else
						profit = (INVENTORY_CAP - agent->inventory) * -agent->prices[i].price;
					profit -= dist_cost;
					if(profit > highest_profit)
					{
						best_hop = next_hop;
						most_profit_node = agent->prices[i].location;
						highest_profit = profit;
					}
				}
				
				if(most_profit_node == agent->location)
				{
					//Buying/selling at current node
					if(agent->inventory != 0)
						agent->inventory = 0;
					else
						agent->inventory = INVENTORY_CAP;
				}
				
				//Update agent state
				agent->advanced_time++;
				arrive_time = agent->advanced_time;
				agent->location = best_hop;
				
				
				//Create a new event in the most profitable direction.
				int disp_command = 2;
				MPI_Bsend(&disp_command, 1, MPI_INT, dispatcher, 0, MPI_COMM_WORLD);
				MPI_Bsend(&best_hop, 1, MPI_INT, dispatcher, disp_command, MPI_COMM_WORLD);
				MPI_Bsend(&agent_id, 1, MPI_INT, dispatcher, disp_command, MPI_COMM_WORLD);
				MPI_Bsend(&arrive_time, 1, MPI_INT, dispatcher, disp_command, MPI_COMM_WORLD);
				
				//check for inconsistencies on that node
				
				Node* next_node = &nodes[best_hop];
				
				
				if(next_node->advanced_time > agent->advanced_time)
				{
					// reconcile them
				
					// Update local state
				}
				
				disp_command = 0;
				MPI_Bsend(&disp_command,1,MPI_INT,dispatcher,0,MPI_COMM_WORLD);
				
				break;
            case 0: 
                //return when received completion message
                done = 1;
        }
    }
}

/* Initializes the node network and agents representing the world */
void initWorld() {
    nodes = (Node*) malloc (NODES * sizeof(Node));
    int i;
    for (i=0; i < NODES; i++) { //create NODES nodes
        // initialize basic properties
        nodes[i].node_id = i;
        nodes[i].buy_price = 5; //temporary, can make it more interesting later
        nodes[i].sell_price = 5; 
        nodes[i].advanced_time = -1;
        nodes[i].previous_state = NULL; // will point to old versions later

        // just connect the network in a circle for now
        // can change this up later, wanted to get something out for testing asap
        // probably implement read-network-from-file if there's time?
        nodes[i].connection_size = 2;
        nodes[i].connected = (int *) malloc (nodes[i].connection_size * sizeof(int));
        nodes[i].connected[0] = (i + 1) % NODES;
        nodes[i].connected[0] = (i - 1) % NODES;            
    }
	
	agents = calloc(sizeof(Agent),AGENTS);
	
	for (i = 0; i < AGENTS;i++) {
		//property initialization
		agents[i].agent_id = i;
		agents[i].inventory = 0;
		agents[i].location = 0; //Agents get updated with events to start off, so this doesn't matter
		agents[i].advanced_time = 0; //
	
		agents[i].prices = calloc(sizeof(LocPrice),NODES);//I figure that eventually, all agents will visit all nodes- save on reallocation.
		for(int j = 0; j < NODES;j++)
		{
			agents[i].prices[j].location = j;
			agents[i].prices[j].price = -1;//Flag invalid prices.
		}
		agents[i].prices_size = NODES;
	}
}

int main (int argc, char** argv) {
    /* 
    * run with: 
    * mpirun -np <number of ranks> ./main.exe <number of nodes> <number of agents> <ticks> <block size> <time cost> <explore threshold 
    */
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

    // set global vars to arguments
    num_agents = atoi(argv[1]);
    num_nodes = atoi(argv[2]);
    ticks = atoi(argv[3]);
    block_size = atoi(argv[4]);
    time_cost = atoi(argv[5]);
    explore_threshold = atoi(argv[6]);

    if(mpi_rank == 0) { //start timer
        start_cycles= GetTimeBase();
    }

    //config world and agents
    //config world network
    initWorld();
    //config agents

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
    if (mpi_rank == 0) { //end timer
        end_cycles= GetTimeBase();
        time_in_secs = ((double)(end_cycles - start_cycles)) /
        processor_frequency;

        //printf("%lld ", global_sum);
        printf("%f\n", time_in_secs);
    }

    MPI_Barrier(MPI_COMM_WORLD); //wait for all processes
    MPI_Finalize();//close down this MPI
    return 0;
}