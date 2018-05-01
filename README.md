# AgentEconomy
A highly parallel agent-based economic simulation for deployment on the Blue Gene Q supercomputer


Work by Connor Hadley, Staeliam Schipper-Reyes, and Zachary Biletch

Can be run on kratos with:
mpirun -np <number of ranks> ./main.exe <number of nodes> <number of agents> <ticks> <block size> <time cost> <explore threshold> <irrationality>
