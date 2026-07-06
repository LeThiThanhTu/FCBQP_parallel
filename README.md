# A Parallel Heuristic for FCBQP

This repository contains the implementation of the algorithm described in the paper

*A Parallel Heuristic for the Fixed-Cardinality Binary Quadratic Program* (Thi Thanh Tu Le and Thorsten Koch).

The main implementation is in `main.cpp`.

The detailed computational results mentioned in the paper are provided in `summary_results.xlsx`.

The fixed-cardinality binary quadratic program is:

```math
\begin{aligned}
\min / \max \quad & f(x)=x^\top Qx \\
\text{s.t.} \quad
& \sum_{i=1}^{n} x_i = k, \\
& x \in \{0,1\}^n,
\end{aligned}
```

where `Q` is a symmetric matrix and `k` represents the cardinality constraint.

The benchmark instances are stored in the `dataset` directory. The collection contains instances from:

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
mkdir -p build
cd build
cmake .. -DCMAKE_BUILD_TYPE=Release
make -j
```

The default settings for the benchmark instances are:

| Dataset set | Objective setup |
| --- | --- |
| `dataset/MDP` | `max half` |
| `dataset/MCP` | `max full` |
| `dataset/GBP` | `min full` |

The best-known objective file is `best_known_results.csv`.

## Run One Instance

From the repository root:

```bash
./build/FCBQP_parallel <max|min> <full|half> <instance.qsc_or_txt> [best_known_results.csv_or_folder] [base_seed] [runs]
```

For example:

```bash
./build/FCBQP_parallel max half dataset/MDP/LA/LA_n2000_m100.qsc best_known_results.csv
```