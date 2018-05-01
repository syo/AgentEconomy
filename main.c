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


int num_agents;
int num_nodes;
int ticks;
int block_size;
int time_cost;
int explore_threshold;
int irrationality;//Chance out of 1000 the agent will pick a random destination instead of the most profitable one

MPI_Datatype price_loc_type;


#define LOCAL_UPDATE_BUFFER 6//Number of buffer spaces to allow for node updates.
#define TICKS ticks //32 //how many ticks of time to run this for 
#define BLOCK block_size //2 //size of a communication block; each block gets one dispatcher rank
#define TIME_COST time_cost // //how much each unit of distance travelled costs the agent
#define INVENTORY_CAP 10 //how much an agent can carry at maximum
#define EXPLORE_THRESHOLD explore_threshold //100 //At what point a lack of profitable moves causes the agent to explore randomly
#define AGENTS num_agents //20 //Number of agents, temporary, should be replaced with variable
#define NODES num_nodes //20 // Number of nodes

typedef struct{
    int location; //node id of a location
    int buy_price; //price at that location
	int sell_price;
} LocPrice;

typedef struct Node{
    int node_id; //id of this node
    int buy_price; //how much it buys an item for
    int sell_price; //how much it sells an item for
	int advanced_time; //What the locally updated time is(for determining consistency)
	struct Node* previous_state; //For rolling back node states
    int* connected; //array of nodes it is connected to
	unsigned int connection_size; //number of nodes it is connected to
	long trade_volume;
} Node;

typedef struct {
    int agent_id; //id of this agent
    int inventory; //size of its inventory
    int location; //node_id of the node it is currently at 
	int advanced_time; //
	
    LocPrice* prices; //array of locations it has visited and their price
	unsigned int prices_size;
	long profit;
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

typedef struct State{
    int advanced_time;
    Node* nodes;
    Agent* agents;
    struct State* next;
    struct State* prev;
} State;

typedef struct EventState{
    int advanced_time;
	Event* events;
    struct EventState* next;
    struct EventState* prev;
} EventState;

Node* nodes; // Array of all nodes in the project, ordered by node_id

Agent* agents; // Array of all agents in the project, by agent_id

State* saved_states = NULL;
State* last_state = NULL;

EventState* saved_event_states = NULL;
EventState* last_event_state = NULL;

//Returns a copy of the given event list.
Event* copyList(Event* events)
{
	Event* copy = NULL;
	Event* copyPrev = NULL;
	while(events != NULL)
	{
		copy = malloc(sizeof(Event));
		copy->location = events->location;
		copy->agent_id = events->agent_id;
		copy->time = events->time;
		if(copyPrev != NULL)
		{	
			copyPrev->next = copy;
		}
		copy->previous = copyPrev;
		copyPrev = copy;
	}
}


//Frees all the events in the given list.
void freeList(Event* events)
{
	while(events->next != NULL)
	{
		events = events->next;
		free(events->previous);
	}
	free(events);
}

//Replaces the event list beginning and end with the given list
void replaceList(Event** events,Event**next_event,Event* replacement)
{
	Event* temp = *events;
	*events = replacement;
	while(replacement->next != NULL)
	{
		replacement = replacement->next;
	}
	*next_event = replacement;
	freeList(temp);
}

// Adds a state to the list of states
void saveState(int time) {
    State* tmp = malloc(sizeof(State));
    tmp->advanced_time = time;
    tmp->nodes = calloc(num_nodes, sizeof(Node));
    for (int i = 0; i < num_nodes; i++) {
        memcpy(&(tmp->nodes[i]), &(nodes[i]), sizeof(Node));
    }
    tmp->agents = calloc(num_agents, sizeof(Agent));
    for (int i = 0; i < num_agents; i++) {
        memcpy(&(tmp->agents[i]), &(agents[i]), sizeof(Agent));
    }
    tmp->next = NULL;
    tmp->prev = last_state;

    last_state->next = tmp;
    last_state = tmp;
}

// Remove all states before time in the saved list
void cleanStates(int time) {
    State* cur_state = saved_states;
    while (cur_state != NULL || cur_state->advanced_time < time) {
        State* tmp = cur_state->next;
		free(cur_state->nodes);
		free(cur_state->agents);
        free(cur_state);
        cur_state = tmp;
    }
    saved_states = cur_state;
}

// Rollback the current state to a time and remove any saved states after it
void rollback(int time) {
    // Remove states after time
    State* cur_state = last_state;
    while(cur_state->advanced_time > time) {
        State* tmp = cur_state->prev;
        free(cur_state);
        cur_state = tmp;
    }
    last_state = cur_state;

    // Update current state values
    for (int i = 0; i < num_nodes; i++) {
        memcpy(&(nodes[i]), &(cur_state->nodes[i]), sizeof(Node));
    }
    for (int i = 0; i < num_agents; i++) {
        memcpy(&(agents[i]), &(cur_state->agents[i]), sizeof(Agent));
    }
}

void saveEventState(int time,Event* events) {
    EventState* tmp = malloc(sizeof(EventState));
    tmp->advanced_time = time;
    tmp->events = copyList(events);
    tmp->next = NULL;
    tmp->prev = last_event_state;

    last_event_state->next = tmp;
    last_event_state = tmp;
}

// Remove all states before time in the saved list
void cleanEventStates(int time) {
    EventState* cur_state = saved_event_states;
    while (cur_state != NULL || cur_state->advanced_time < time) {
        EventState* tmp = cur_state->next;
		freeList(cur_state->events);
        free(cur_state);
        cur_state = tmp;
    }
    saved_event_states = cur_state;
}

// Rollback the current state to a time and remove any saved states after it
void rollbackEvent(int time,Event** events,Event** next_event) {
    // Remove states after time
    EventState* cur_state = last_event_state;
    while(cur_state->advanced_time > time) {
        EventState* tmp = cur_state->prev;
        free(cur_state);
        cur_state = tmp;
    }
    last_event_state = cur_state;

    // Update current state values
	replaceEvents(events,next_event,cur_state->events);
}


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
		if(next_event->time < time)
		{
			return true;
		}
		next_event = next_event->previous;
	}
	return false;
}


int minEventTime(Event* events)
{
	int min_time = 10000000;
	while(events != NULL)
	{
		if(events->time < min_time)
		{
			min_time = events->time;
		}
		events = events->next;
	}
	return min_time;
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

void printEventTimes(Event* events)
{
	printf("[");
	while(events != NULL)
	{
		printf("%d,",events->time);
	}
	printf("]");
}

/* implements dijkstras algorithm for finding the shortest path length */
int shortestPath(int start_node, int dest_node, int* next_hop) {
     int* dist = calloc(sizeof(int),num_nodes); //distance of node i to starting node
     int* prev = calloc(sizeof(int),num_nodes); //previous nodes in best path
     int* visited = calloc(sizeof(int),num_nodes); //has node been visited
	 for(int i = 0; i < num_nodes; i++)
		visited[i] = 0;
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
        for(i=0; i < nodes[current].connection_size;i++) { //go through each neighbor node
            d = dist[current] + TIME_COST; //increment distance
            if (d < dist[nodes[current].connected[i]] && visited[nodes[current].connected[i]] == 0) { //if this is the shortest path and we have not visited it
                dist[nodes[current].connected[i]] = d; //the distance to it is d
                prev[nodes[current].connected[i]] = current; //the previous node is the current node
            }
            if (dist[nodes[current].connected[i]] < min && visited[nodes[current].connected[i]] == 0) { //if this is the minimum path
                min = dist[nodes[current].connected[i]]; //set it to be so
                m = nodes[current].connected[i];
            }
        }
        current = m;
        visited[current] = 1;
     }
	 *next_hop = dest_node;
	 while(prev[*next_hop] != start_node)
		*next_hop = prev[*next_hop];
	free(dist);
	free(prev);
	free(visited);
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
	int world_size;
    MPI_Comm_size(MPI_COMM_WORLD, &world_size);
    MPI_Comm_rank(MPI_COMM_WORLD, &mpi_rank);
	Event* events = NULL;
	Event* next_event = NULL;
	//Initialize event list
	for(int i = 0; i < AGENTS; i++)
	{
		Event* new_event = malloc(sizeof(Event));
		agents[i].location = i;
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
	Event** processing_events = calloc(sizeof(Event*),BLOCK);
	
	//Acquire all the handler ranks under us
	for(int i = 1; i < BLOCK; i++)
	{
		Subrank* new_rank = malloc(sizeof(Subrank));
		new_rank->mpi_rank = mpi_rank + i;
		new_rank->next = available_ranks;
		available_ranks = new_rank;
		processing_events[i] = NULL;
	}
	
	//Create buffer for buffered sends
	void* buf = calloc(sizeof(char),3*BLOCK*sizeof(int)+4*world_size/BLOCK*sizeof(int)+3*MPI_BSEND_OVERHEAD+1);
	MPI_Buffer_attach(buf,3*BLOCK*sizeof(int)+4*world_size/BLOCK*sizeof(int)+3*MPI_BSEND_OVERHEAD+1);
	
	int* dispatching_locks = calloc(sizeof(int),world_size/BLOCK);
	for(int i = 0; i < world_size/BLOCK; i++)
	{
		dispatching_locks[i] = -1;
	}
    //go through event list
	int old_min_global_time = 0;
	while(isEventBefore(next_event,TICKS)){
		//Dispatch events here.
		//Get next event that is within time limit
		if(available_ranks != NULL)
		{
			Event* e;
			bool isAvail = true;
			//Check if the node of this event is currently locked
			do
			{	
				e = next_event;
				while(e != NULL)
				{	
					if( e->time < TICKS)
					{
						break;
					}
					e = e->previous;
				}
				if(e == NULL)
					break;
				for(int i = 0; i < world_size/BLOCK; i++)
				{
					if(dispatching_locks[i] == e->location)
						isAvail = false;
				}
			}while(isAvail != true);
				
			if(e != NULL)
			{
				
			//Attempt to acquire the given node
			//Send lock requests to all nodes
			//printf("**Dispatcher %d attempting to lock %d.\n",mpi_rank,e->location);
			for(int i = 0; i < world_size; i += BLOCK)
			{	
				if(i == mpi_rank)
					continue;
					
				//printf("**Dispatcher %d sent lock request to rank %d.\n",mpi_rank,i);
				command = 3;//Request to lock a node for dispatching
				MPI_Bsend(&command, 1, MPI_INT,i,0,MPI_COMM_WORLD);
				MPI_Bsend(&e->location,1,MPI_INT,i,3,MPI_COMM_WORLD);
			}
			bool is_cleared = true;
			
			//Wait for all other dispatchers to respond
			int i = 0;
			while(i < world_size/BLOCK - 1 && is_cleared == true)
			{
				MPI_Status lock_stat;
				int response;
				int command;
				int local_flag;
				MPI_Iprobe(MPI_ANY_SOURCE,3,MPI_COMM_WORLD,&local_flag,&lock_stat);
				//Listen for any 
				
				MPI_Recv(&response,1,MPI_INT,MPI_ANY_SOURCE,10+e->location,MPI_COMM_WORLD,&lock_stat);
				switch(response)
				{
					case -1://Permit message
						i++;
						break;
					case -2://I have already given my lock to a higher-priority dispatcher
						is_cleared = false;
						break;
					case -3://I gave you my lock, but a higher-priority dispatcher usurped it
						is_cleared = false;
						i--;//Double message from another task
						break;
					default:
						if(mpi_rank < lock_stat.MPI_SOURCE)//I outrank you and will usurp the lock
						{
							response = -2;
						}
						else
						{
							response = -1;
							is_cleared = false;
						}
						MPI_Send(&response,1,MPI_INT,i,3,MPI_COMM_WORLD);
						break;
						
				}
			}
			
			if(is_cleared == true)
			{
				
				processing_events[available_ranks->mpi_rank - 1] = e;
			
				command = 1;
				int agent_id = e->agent_id;
				//Send event handling command to task
				MPI_Bsend(&command, 1, MPI_INT,available_ranks->mpi_rank,0,MPI_COMM_WORLD);
				MPI_Bsend(&agent_id, 1, MPI_INT, available_ranks->mpi_rank,1,MPI_COMM_WORLD);
				MPI_Bsend(&e->time, 1, MPI_INT, available_ranks->mpi_rank,1,MPI_COMM_WORLD);
				Subrank* temp_rank = available_ranks;
				available_ranks = available_ranks->next;
				
				
				//printf("**Dispatcher %d tasked event by agent %d on node %d on task %d.\n",mpi_rank,e->agent_id,e->location,temp_rank->mpi_rank);
				for(int i = 0; i < world_size; i += BLOCK)
				{	
					if(i == mpi_rank)
						continue;
					command = 4;
					MPI_Bsend(&command, 1, MPI_INT,i,0,MPI_COMM_WORLD);
					MPI_Bsend(&e->location, 1, MPI_INT,i,4,MPI_COMM_WORLD);
					MPI_Bsend(&agent_id, 1, MPI_INT,i,4,MPI_COMM_WORLD);
					MPI_Bsend(&e->time, 1, MPI_INT,i,4,MPI_COMM_WORLD);
				}
			
				//Remove event from list
				if(e->previous != NULL)
					e->previous->next = e->next;
				if(e->next != NULL)
					e->next->previous = e->previous;
				if(e == next_event);
					next_event = e->previous;
				if(e == events)
					events = e->next;
				
				
				free(temp_rank);
				
				
			}
			
			
			}
		}
		int min_global_time = minEventTime(events);
		for(int i = 0; i < BLOCK; i++)
		{
			if(processing_events[i] != NULL && min_global_time > processing_events[i]->time)
				min_global_time = processing_events[i]->time;
			
		}
		if(old_min_global_time != min_global_time)
		{
			for(int i = 1; i < BLOCK; i++)
			{
				int update_com = 4;
				MPI_Bsend(&update_com,1,MPI_INT,i,0,MPI_COMM_WORLD);
				MPI_Bsend(&min_global_time,1,MPI_INT,i,4,MPI_COMM_WORLD);
				
			}
			old_min_global_time = min_global_time;
		}
		//printf("%d\n",min_global_time);
		
		int flag;
		MPI_Status stat;
		int in_command;
		MPI_Iprobe(MPI_ANY_SOURCE,0,MPI_COMM_WORLD,&flag,&stat);
		//if(stat.MPI_ERROR != 0);
			//printf("%d\n",stat.MPI_ERROR);
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
					
					//Free processing event
					free(processing_events[new_rank->mpi_rank - 1]);
					processing_events[new_rank->mpi_rank - 1] = NULL;
					
					new_rank->next = available_ranks;
					available_ranks = new_rank;
					//printf("**Dispatcher %d had rank %d sucessfuly complete an event.\n",mpi_rank,new_rank->mpi_rank);
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
					//printf("**Dispatcher %d executed a request by task %d to add an event at %d for agent %d at time %d.\n",mpi_rank,stat.MPI_SOURCE,best_hop,agent_id,time);
					//Insert event in a time-sorted order
					Event* insert_before = events;
					if(events == NULL)
					{
						events = new_event;
						new_event->next = NULL;
						new_event->previous = NULL;
						next_event = new_event;
					}
					else
					{
						while(insert_before->time > new_event->time && insert_before->next != NULL)
						{
							insert_before = insert_before->next;
						}
						new_event->next = insert_before;
						new_event->previous = insert_before->previous;
						insert_before->previous = new_event;
						if(new_event->previous != NULL)
							new_event->previous->next = new_event;
						else
							events = new_event;
					}
					//Send message to all other dispatchers to add the event if this is not a dispatcher originated message
					if(stat.MPI_SOURCE % BLOCK != 0)
					{
						for(int i = 0; i < world_size;i += BLOCK)
						{
							if(i == mpi_rank)
								continue;
							MPI_Bsend(&in_command, 1, MPI_INT, i, 0, MPI_COMM_WORLD);
							MPI_Bsend(&best_hop, 1, MPI_INT, i, 2, MPI_COMM_WORLD);
							MPI_Bsend(&agent_id, 1, MPI_INT, i, 2, MPI_COMM_WORLD);
							MPI_Bsend(&time, 1, MPI_INT, i, 2, MPI_COMM_WORLD);
						}
					}
					break;
				case 3://Request for lock on node
					in_command = 3;
					int req_node;
					int response;
					MPI_Recv(&req_node,1,MPI_INT,stat.MPI_SOURCE,3,MPI_COMM_WORLD,MPI_STATUS_IGNORE);
					for(int i = 0; i < stat.MPI_SOURCE / BLOCK; i++)
					{
						if(dispatching_locks[i] == req_node)
						{
							response = -2;
							MPI_Send(&response, 1, MPI_INT, 10+req_node, 3, MPI_COMM_WORLD);
							//printf("**Dispatcher %d responed to a lock request concerning node %d from %d with code %d.\n",mpi_rank,req_node,stat.MPI_SOURCE,response);
							break;
						}
					}
					dispatching_locks[stat.MPI_SOURCE / BLOCK] = req_node;
					response = -1;
					MPI_Send(&response, 1, MPI_INT, stat.MPI_SOURCE, 10+req_node, MPI_COMM_WORLD);
					//printf("**Dispatcher %d responed to a lock request concerning node %d from %d with code %d.\n",mpi_rank,req_node,stat.MPI_SOURCE,response);
					for(int i = stat.MPI_SOURCE / BLOCK + 1; i < world_size / BLOCK; i++)
					{
						if(dispatching_locks[i] == req_node)
						{
							response = -3;
							MPI_Send(&response, 1, MPI_INT, i * BLOCK, 10+req_node, MPI_COMM_WORLD);
							dispatching_locks[i] = -1;
							//printf("**Dispatcher %d responed to a lock request concerning node %d from %d with code %d.\n",mpi_rank,req_node,stat.MPI_SOURCE,response);
							break;
						}
					}
				case 4://Clear lock for node and remove it from event list
					dispatching_locks[stat.MPI_SOURCE / BLOCK] = -1;
					int agent_id,time;
					MPI_Recv(&req_node,1,MPI_INT,stat.MPI_SOURCE,4,MPI_COMM_WORLD,MPI_STATUS_IGNORE);
					MPI_Recv(&agent_id,1,MPI_INT,stat.MPI_SOURCE,4,MPI_COMM_WORLD,MPI_STATUS_IGNORE);
					MPI_Recv(&time,1,MPI_INT,stat.MPI_SOURCE,4,MPI_COMM_WORLD,MPI_STATUS_IGNORE);
					Event * e = events;
					while(e != NULL)
					{
						if(e->location == req_node && e->agent_id == agent_id && e->time == time)
						{
							if(e->previous != NULL)
								e->previous->next = e->next;
							if(e->next != NULL)
								e->next->previous = e->previous;
							if(e == next_event);
								next_event = e->previous;
							if(e == events && e->next != NULL)
								events = e->next;
							free(e);
							break;
						}
						e = e->next;
					}
					//printf("**Dispatcher %d cleared lock from task %d and removed event at %d by %d.\n",mpi_rank,stat.MPI_SOURCE,req_node,agent_id);
					break;
				default:
					break;
			}
			
			MPI_Iprobe(MPI_ANY_SOURCE,0,MPI_COMM_WORLD,&flag,&stat);
		}
	}
	
	free(dispatching_locks);
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
    int mpi_rank = -1;
	int world_size;
    MPI_Comm_size(MPI_COMM_WORLD, &world_size);
    MPI_Comm_rank(MPI_COMM_WORLD, &mpi_rank);
	void* buf = calloc(sizeof(char),6*sizeof(int)*world_size*LOCAL_UPDATE_BUFFER+6*world_size*MPI_BSEND_OVERHEAD*LOCAL_UPDATE_BUFFER+1);
	MPI_Buffer_attach(buf,6*sizeof(int)*world_size*LOCAL_UPDATE_BUFFER+6*world_size*MPI_BSEND_OVERHEAD*LOCAL_UPDATE_BUFFER+1);
    int current_time = 0;
    int dispatcher = mpi_rank - (mpi_rank % BLOCK); // calculate the dispatcher id for this rank
    int done = 0;
    while (!done) {
        //wait for a command
        int command;
		MPI_Status stat;
        MPI_Recv(&command, 1, MPI_INT, dispatcher, 0, MPI_COMM_WORLD,
            &stat);

        // XXX TODO: full list of commands
        switch(command) { //determine what the command is and execute properly
			case 4:
				command = 4;
				int limit_time;
				MPI_Recv(&limit_time, 1, MPI_INT, stat.MPI_SOURCE,4,MPI_COMM_WORLD,MPI_STATUS_IGNORE);
                
				cleanStates(limit_time);
				break;
			case 3: //Update state for agent
				command = 3;
				int agent_id;
				MPI_Recv(&agent_id, 1, MPI_INT, stat.MPI_SOURCE,3,MPI_COMM_WORLD,MPI_STATUS_IGNORE);
				Agent* agent = &agents[agent_id];
                saveState(current_time);
				MPI_Recv(&agent->inventory, 1, MPI_INT, stat.MPI_SOURCE,3,MPI_COMM_WORLD,MPI_STATUS_IGNORE);
				MPI_Recv(&agent->advanced_time, 1, MPI_INT, stat.MPI_SOURCE,3,MPI_COMM_WORLD,MPI_STATUS_IGNORE);
				MPI_Recv(&agent->location,1, MPI_INT, stat.MPI_SOURCE,3,MPI_COMM_WORLD,MPI_STATUS_IGNORE);
				MPI_Recv(&agent->profit,1, MPI_INT, stat.MPI_SOURCE,3,MPI_COMM_WORLD,MPI_STATUS_IGNORE);
				MPI_Recv(&agent->prices,sizeof(LocPrice)*num_nodes,MPI_BYTE,stat.MPI_SOURCE,3,MPI_COMM_WORLD,MPI_STATUS_IGNORE);

                current_time = agent->advanced_time;
				
				//Also consider how to send the old state saves as well.
				break;
			case 2: //Update state for node
				command = 2;
				int node_id;
				int new_price;
				int new_time;
				int new_volume;
				
				MPI_Recv(&node_id, 1, MPI_INT, stat.MPI_SOURCE,2,MPI_COMM_WORLD,MPI_STATUS_IGNORE);
				MPI_Recv(&new_price, 1, MPI_INT, stat.MPI_SOURCE,2,MPI_COMM_WORLD,MPI_STATUS_IGNORE);
				MPI_Recv(&new_time, 1, MPI_INT, stat.MPI_SOURCE,2,MPI_COMM_WORLD,MPI_STATUS_IGNORE);
				MPI_Recv(&new_volume,1, MPI_INT, stat.MPI_SOURCE,2,MPI_COMM_WORLD,MPI_STATUS_IGNORE);
								
				Node* node = &nodes[node_id];
				
                saveState(current_time);
                current_time = new_time;
				
				//Create a new node with updated parameters and replace the old node with it.
				//TEMPORARY: Just use the reference into the list
				Node* new_node = node;
				//new_node->node_id = node->node_id;
				new_node->buy_price = new_price;
				//new_node->sell_price = sell_price;
				new_node->advanced_time = new_time;
				new_node->trade_volume = new_volume;
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
				
				int arrive_time;
				MPI_Recv(&agent_id, 1, MPI_INT, dispatcher,1,MPI_COMM_WORLD,MPI_STATUS_IGNORE);
				MPI_Recv(&arrive_time, 1, MPI_INT, dispatcher,1,MPI_COMM_WORLD,MPI_STATUS_IGNORE);

                // Check for rollback condition
                if (arrive_time < current_time) {
                    // Rollback to earlier state
                    rollback(arrive_time);
                    //XXX Send rollback to dispatcher
                }
				
				agent = &agents[agent_id];
				
				int most_profit_node;
				int highest_profit = -1000000000;//Arbitrary large negative number. INT_MIN can invoke over/underflow when manipulated
				int best_hop;
				
				node = &nodes[agent->location];
				node->advanced_time = agent->advanced_time;
				
				//Find the most profitable way to sell/fill our inventory.
				for(int i = 0; i < agent->prices_size ; i++)
				{
					//Check if we're considering the current node, update price knowledge
					if(i == agent->location)
					{
						agent->prices[i].buy_price = node->buy_price;
						agent->prices[i].sell_price = node->buy_price;
					}
					//Check if we have a valid price for the node we're considering
					if(agent->prices[i].sell_price == -1)
					{
						continue;
					}
					
					int next_hop;
					int dist_cost = shortestPath(agent->location,agent->prices[i].location,&next_hop) * TIME_COST;
					int profit;
					//If we have inventory sell it, if we don't try to fill it
					if(agent->inventory != 0)
						profit = agent->inventory * agent->prices[i].sell_price;
					else
						profit = (INVENTORY_CAP - agent->inventory) * -agent->prices[i].buy_price;
					profit -= dist_cost;
					if(profit > highest_profit)
					{
						best_hop = next_hop;
						most_profit_node = agent->prices[i].location;
						highest_profit = profit;
					}
					
				}
				
				if(highest_profit < explore_threshold)
				{
					most_profit_node = rand() % num_nodes;
					shortestPath(agent->location,most_profit_node,&best_hop);
				}
				
				//Randomly deviate from known optimal
				if(rand() % 1000 < irrationality)
				{
					most_profit_node = rand() % num_nodes;
					shortestPath(agent->location,most_profit_node,&best_hop);
				}
				
				if(most_profit_node == agent->location)
				{
					//Buying/selling at current node
					if(agent->inventory != 0)
					{
						agent->inventory = 0;
						node->trade_volume += INVENTORY_CAP * node->sell_price;
						agent->profit += INVENTORY_CAP * node->sell_price;
					}
					else
					{
						agent->inventory = INVENTORY_CAP;
						node->trade_volume += INVENTORY_CAP * node->buy_price;
						agent->profit -= INVENTORY_CAP * node->buy_price;
					}
					
				}
				
				//Update agent state
				arrive_time++;
				agent->advanced_time = arrive_time;
				agent->location = best_hop;
				node->advanced_time = agent->advanced_time;
                current_time = arrive_time;
                saveState(current_time);
				//Send state updates to all other workers
				for(int i = 0; i < world_size; i++)
				{
					if(i % BLOCK == 0)
						continue;
					int worker_command = 2;
					//Update node
					MPI_Bsend(&worker_command, 1, MPI_INT, i,0,MPI_COMM_WORLD);
					MPI_Bsend(&node->node_id, 1, MPI_INT, i,2,MPI_COMM_WORLD);
					MPI_Bsend(&node->buy_price, 1, MPI_INT, i,2,MPI_COMM_WORLD);
					MPI_Bsend(&node->advanced_time, 1, MPI_INT, i,2,MPI_COMM_WORLD);
					MPI_Bsend(&node->trade_volume, 1, MPI_INT, i,2,MPI_COMM_WORLD);
					//Update agent
					worker_command = 3;
					MPI_Bsend(&worker_command, 1, MPI_INT, i,0,MPI_COMM_WORLD);
					MPI_Bsend(&agent_id, 1, MPI_INT, i,3,MPI_COMM_WORLD);
					Agent* agent = &agents[agent_id];
					MPI_Bsend(&agent->inventory, 1, MPI_INT, i,3,MPI_COMM_WORLD);
					MPI_Bsend(&agent->advanced_time, 1, MPI_INT, i,3,MPI_COMM_WORLD);
					MPI_Bsend(&agent->location,1, MPI_INT, i,3,MPI_COMM_WORLD);
					MPI_Bsend(&agent->profit,1, MPI_INT, i,3,MPI_COMM_WORLD);
					MPI_Bsend(&agent->prices,sizeof(LocPrice)*num_nodes,MPI_BYTE,i,3,MPI_COMM_WORLD);
					
				}
				//Create a new event in the most profitable direction.
				int disp_command = 2;
				MPI_Bsend(&disp_command, 1, MPI_INT, dispatcher, 0, MPI_COMM_WORLD);
				MPI_Bsend(&best_hop, 1, MPI_INT, dispatcher, disp_command, MPI_COMM_WORLD);
				MPI_Bsend(&agent_id, 1, MPI_INT, dispatcher, disp_command, MPI_COMM_WORLD);
				MPI_Bsend(&arrive_time, 1, MPI_INT, dispatcher, disp_command, MPI_COMM_WORLD);
				
				Node* next_node = &nodes[best_hop];
				
				
				//check for inconsistencies on that node
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
        nodes[i].buy_price = rand() % 20; //temporary, can make it more interesting later
		nodes[i].sell_price = nodes[i].buy_price + rand() % 10; 
        nodes[i].advanced_time = -1;
        nodes[i].previous_state = NULL; // will point to old versions later

        // just connect the network in a circle for now
        // can change this up later, wanted to get something out for testing asap
        // probably implement read-network-from-file if there's time?
        nodes[i].connection_size = 2;
        nodes[i].connected = (int *) malloc (nodes[i].connection_size * sizeof(int));
        nodes[i].connected[0] = (i + 1) % NODES;
		if(i == 0)
			nodes[i].connected[1] = NODES - 1;
		else
			nodes[i].connected[1] = (i - 1) % NODES;            
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
			agents[i].prices[j].buy_price = -1;//Flag invalid prices.
			agents[i].prices[j].sell_price = -1;//Flag invalid prices.
		}
		agents[i].prices_size = NODES;
	}
}


int main (int argc, char** argv) {
    /* 
    * run with: 
    * mpirun -np <number of ranks> ./main.exe <number of nodes> <number of agents> <ticks> <block size> <time cost> <explore threshold> <irrationality>
    */
    // set up info for timing
    double time_in_secs = 0;
    double processor_frequency = 1600000000.0;
    unsigned long long start_cycles=0;
    unsigned long long end_cycles=0;
	
	//MPI_Type_contiguous(3,MPI_INT,&price_loc_type);

	srand(12180);
	
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
	irrationality = atoi(argv[7]);

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
	
	if(mpi_rank == 1)
	{
		int max_profit = 0;
		int best_agent = 0;
		for(int i = 0; i < num_agents; i++)
		{
			if(max_profit < agents[i].profit)
			{
				max_profit = agents[i].profit;
				best_agent = i;
			}
		}
		
		int most_popular_node = 0;
		int max_trade_volume = 0;
		for(int i = 0; i < num_nodes; i++)
		{
			if(max_trade_volume < nodes[i].trade_volume)
			{
				max_trade_volume = nodes[i].trade_volume;
				most_popular_node = i;
			}
		}
		
		printf("Most profitable agent was %d with a total profit of %d.\n",best_agent,max_profit);
		printf("Higest volume node was %d with a total volume of %d.\n",most_popular_node,max_trade_volume);
	}

    MPI_Barrier(MPI_COMM_WORLD); //wait for all processes
    MPI_Finalize();//close down this MPI
    return 0;
}
