#!/bin/sh

srun --ntasks 64 --overcommit -o proj.log ./project 256 256 128 64 1 -75 50