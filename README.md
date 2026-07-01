# A Parallel Heuristic for FCBQP

This repository contains the implementation of the algorithm described in the paper
*A Parallel Heuristic for the Fixed Cardinality Binary Quadratic Program*.
The main implementation is in `main.cpp`.

The fixed-cardinality binary quadratic program is:

```math
\min / \max \quad f(x)=x^\top Qx
\quad \text{s.t.} \quad
\sum_{i=1}^{n} x_i = k,\quad
x \in \{0,1\}^n .
```

The benchmark instances are stored in `dataset`. The collection contains instances from:

- Maximum Dispersion Problem (MDP)
- Maximum Clique Problem (MCP)
- Graph Bisection Problem (GBP)

## Requirements

- A modern C++17 compiler with standard `<filesystem>` support
- OpenMP
- CMake 3.10 or newer
- GNU Make

## Build

From the repository root:

```bash
cmake -S . -B build -G "Unix Makefiles"
cd build
make
```

## Run All Dataset Sets

From the `build` directory:

```bash
make run
```

The default batch run uses the following settings:

| Dataset set | Objective setup |
| --- | --- |
| `dataset/MDP` | `max half` |
| `dataset/MCP` | `max full` |
| `dataset/GBP` | `min full` |

The default best-known objective file is `best_known_results.csv`.

## Run One Instance

From the repository root:

```bash
./build/FCBQP_parallel <max|min> <full|half> <instance.qsc_or_txt> [best_known_results.csv_or_folder] [base_seed] [runs] [report.csv]
```

For example:

```bash
./build/FCBQP_parallel max half dataset/MDP/LA/LA_n2000_m100.qsc best_known_results.csv
```
