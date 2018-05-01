# AgentEconomy
A highly parallel agent-based economic simulation for deployment on the Blue Gene Q supercomputer


Work by Connor Hadley, Staeliam Schipper-Reyes, and Zachary Biletch

Primarily for use on Blue Gene

If #define BGQ 1 is commented out:
Can be run on kratos with:
mpirun -np <number of ranks> ./main.exe <number of nodes> <number of agents> <ticks> <block size> <time cost> <explore threshold> <irrationality>

If Blue Gene complains about line breaks, run:
sed -i.bak 's/\r$//' run.sh