# TaskForge Benchmark & Scalability Report

---

## 1. Executive Summary

This report documents the performance characteristics, parallel scalability, and contention behavior of **TaskForge** across varying worker thread counts and execution modes (Standard Bounded Queue vs. Work-Stealing Per-Worker Deques).

* **Soak Test Reliability**: 1,000,000 tasks processed across 4 concurrent producers and 8 worker threads with **100% verified result accuracy**, with throughput recorded by the test output for the host used.
* **Memory Safety**: covered by the repository's Valgrind Memcheck target; results are host/run-specific.
* **Concurrency Safety**: covered by the repository's TSan and ASan/UBSan targets when those sanitizers are supported by the host.
* **Work-Stealing Advantage**: can improve throughput under some multi-core workloads by reducing pressure on the central queue; exact gains are host- and workload-dependent.

---

## 2. Worker Thread Scaling & Contention Ceiling

### 2.1 Measured Scaling Curve (100,000 Tasks)

| Worker Count | Throughput (Tasks/sec) | Speedup Factor | Efficiency (%) | Contention Regime |
|:---:|:---:|:---:|:---:|:---|
| **1** | 863,885.9 | 1.00x | 100.0% | Baseline Single-Thread |
| **2** | 1,321,807.5 | 1.53x | 76.5% | Near-Linear Scaling |
| **4** | 938,436.1 | 1.09x | 27.2% | Contention Knee / Transition |
| **8** | 259,701.4 | 0.30x | 3.8% | Lock Contention Dominated |
| **12** | 130,354.4 | 0.15x | 1.3% | Cache Line Bouncing |

### 2.2 Identification of the Contention Ceiling

* **Linear Regime (1 - 2 Workers)**: Throughput increases from 863k to 1.32M tasks/second (1.53x speedup). Memory bus contention and lock hold times remain low relative to task execution.
* **The Contention Knee (at 4 Workers)**: For fine-grained micro-tasks (sub-microsecond CPU workloads), the time spent waiting on condition variable signaling and lock acquisition starts to exceed the actual task computation time.
* **Lock Saturation Regime (8 - 12 Workers)**: As 8 to 12 threads aggressively contend for buffer mutexes and condition variables, hardware cache coherence protocols (MESI/MOESI) spend substantial cycles invalidating cache lines across CPU cores (cache-line ping-pong).

---

## 3. Global Bounded Queue vs. Work-Stealing Deques

Under heavy multi-core concurrency, work-stealing isolates workers into their own local double-ended queues:

| Workers | Global Bounded Queue (T/s) | Work-Stealing Deques (T/s) | Stealing Throughput Gain |
|:---:|:---:|:---:|:---:|
| **2** | 1,652,509.8 | 1,567,463.8 | -5.1% (deque overhead) |
| **4** | 574,638.3 | 686,533.7 | **+19.5%** |
| **8** | 186,843.2 | 218,467.2 | **+16.9%** |
| **12** | 105,393.6 | 117,756.3 | **+11.7%** |

### Key Takeaway:
The checked-in measurements show workload-dependent differences between the two scheduling modes. Work-stealing can reduce pressure on the global queue by allowing workers to execute locally, but the measured gain or loss depends on worker count, workload size, CPU topology, and system load.


## Reproducibility

The numbers in this document are illustrative measurements from a particular benchmark environment. The checked-in CSV may contain measurements from a different run. Use `make bench` to regenerate measurements on the same machine before comparing results.
