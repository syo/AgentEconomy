# AgentEconomy
A highly parallel agent-based economic simulation for deployment on the Blue Gene Q supercomputer


Work by Connor Hadley, Staeliam Schipper-Reyes, and Zachary Biletch

Primarily for use on Blue Gene - it currently does not run on kratos, but it ***does run on Blue Gene*** which was the goal of the project

The current working build is in the "working" subdirectory
Compile with make file

If Blue Gene complains about line breaks, run:
sed -i.bak 's/\r$//' run.sh

Example of how to run on Blue Gene:
ssh q
module load xl
make 
sbatch --partition medium --nodes 128 --time 15 ./run.sh

Which will output the results to 'proj.log', in the format:
131.003111
Most profitable agent was 7 with a total profit of 7050.
Higest volume node was 5 with a total volume of 1958065.

