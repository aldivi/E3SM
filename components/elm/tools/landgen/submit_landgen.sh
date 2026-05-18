#!/bin/bash
#SBATCH --job-name=parallel_python
#SBATCH --nodes=1               # Ensure single node
#SBATCH --ntasks=1              # Run one task (master process)
#SBATCH --exclusive             # Ensure exclusive access to the node (uses all cpus)
#SBATCH --mem=0                 # 0=Request all memory in this node (adjust as needed)
#SBATCH --time=01:00:00
#SBATCH --account e3sm
#SBATCH --qos=regular
#SBATCH --constraint=cpu

# with --exclusive and --nodes=1 and --ntasks=1, the job will have access to all cores on the node
# and SLURM_CPUS_PER_TASK will be set to the total number of cores on the node
# this allows multiprocessing to use all cores without oversubscribing,
#    when combined with OMP_NUM_THREADS=1 and MKL_NUM_THREADS=1 below

# but to automatically adjust cores based on the node, we can use srun instead, with SRUN_CPUS_PER_TASK
#    so launch with srun below and then the code looks for SRUN_CPUS_PER_TASK instead of SLURM_CPUS_PER_TASK
# to reduce the number of cores, set SRUN_CPUS_PER_TASK below to the desired number
#    and keep --exclusive to ensure the entire node is reserved for this job

# perlmutter has a regular and debug queue for cpus
# perlmutter has 128 cores and 512G per node

# Calculate half of the node CPUs
NODE_CPUS=$SLURM_CPUS_ON_NODE
HALF_CPUS=$(( NODE_CPUS / 2 ))

# set the srun cpus to use per task
export SRUN_CPUS_PER_TASK=$HALF_CPUS

# Tell Python math and OpenMP to use the requested number of threads
# set these to 1 so that each process uses one thread on each core
# If these are >1 they allow multiple threads per process, which asks for #process * #thread cores
export OMP_NUM_THREADS=1
export MKL_NUM_THREADS=1

# Run the landgen package
# This command will run the __main__.py file inside the landgen package/directory
srun python -m landgen