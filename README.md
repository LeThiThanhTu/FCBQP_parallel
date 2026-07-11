# A Parallel Heuristic for FCBQP

This repository contains the implementation of the algorithm described in the paper

*A Parallel Heuristic for the Fixed Cardinality Binary Quadratic Program* (Thi Thanh Tu Le and Thorsten Koch).

The main implementation is in `main.cpp`.

The best-known/reference results are stored in `best_known_results.csv`.

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

- A C++17 compiler with standard `<filesystem>` support
- OpenMP
- CMake 3.10 or newer
- GNU Make

## Build

From the repository root:

```bash
make
```

The `Makefile` wraps the CMake build. To select a compiler or build directory:

```bash
make CXX=g++-15 BUILD_DIR=build-gcc JOBS=8
```

The equivalent direct CMake commands remain available:

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build --parallel
```

The default settings for the benchmark instances are:

| Dataset set | Objective setup |
| --- | --- |
| `dataset/MDP` | `max half` |
| `dataset/MCP` | `max full` |
| `dataset/GBP` | `min full` |

The best-known objective file is `best_known_results.csv`.

## Run

From the repository root:

```bash
./build/main <max|min> <full|half> <instance.qsc_or_txt> [best_known_results.csv_or_folder] [base_seed] [runs] [time_limit_seconds] [index_base]
```

or simply

```bash
./build/main <max|min> <full|half> <instance.qsc_or_txt> [best_known_results.csv_or_folder]
```

For example:

```bash
./build/main max half dataset/MDP/LA/LA_n2000_m100.qsc best_known_results.csv
```

`time_limit_seconds` defaults to 600; zero disables the limit. `index_base` can
be `auto`, `0`, or `1`. If the indices are compatible with both conventions,
the program rejects the ambiguous input unless `0` or `1` is supplied.

Matrix data may contain one triangle of a symmetric matrix or both symmetric
entries. When both `(i,j)` and `(j,i)` are present, their values must agree;
they are counted once rather than doubled.

Cardinalities from `0` through `n` are accepted. The trivial cases `k=0`,
`k=1`, and `k=n` are solved directly.

On macOS, CMake automatically checks the standard Homebrew locations for
`libomp` when Apple Clang cannot discover OpenMP itself.

## Reproducible smoke benchmark

The ten-instance benchmark covers MDP, MCP, and GBP instances using four
threads and one fixed-seed run per instance:

```bash
make benchmark
```

Raw logs and a CSV summary are written under `benchmark_results/`.

Benchmark settings can be overridden without editing files:

```bash
make benchmark BENCHMARK_PHASE=trial BENCHMARK_THREADS=8 SOLVER_TIME_LIMIT=60 BENCHMARK_TIMEOUT=75
```

Run the regression tests with `make test` and list all Make targets with
`make help`.
