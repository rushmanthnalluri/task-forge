# TaskForge Workload Specification File
# Syntax:
#   TASK <id> <priority: LOW|NORMAL|HIGH> <sleep_ms> <compute_iters> <payload>
#   REPEAT <count> TASK <priority: LOW|NORMAL|HIGH> <sleep_ms> <compute_iters> <payload>

# Initial warmup tasks
TASK 1 HIGH 5 1000 42
TASK 2 HIGH 5 1000 84
TASK 3 NORMAL 10 5000 128
TASK 4 LOW 20 10000 256

# High-priority batch
REPEAT 10 TASK HIGH 2 2000 999

# Standard batch
REPEAT 20 TASK NORMAL 1 1000 1234

# Low-priority background tasks
REPEAT 15 TASK LOW 5 500 7777
