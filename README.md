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

The smoke benchmark instances are stored in the `dataset` directory. The collection contains instances from:

- Maximum Dispersion Problem (MDP)
- Maximum Clique Problem (MCP)
- Graph Bisection Problem (GBP)

## Requirements

- A C++20 compiler with Parallel STL support
- CMake 3.16 or newer
- GNU Make

LLVM 22 with libc++ uses the native libdispatch backend; the build enables
LLVM's experimental library support automatically. LLVM is the default when
the Homebrew LLVM installation is available because it is fastest on the
current optimized benchmark. The optimized kernels should still be benchmarked
with both compilers. The
libstdc++ Parallel STL needs TBB for concurrent execution. CMake detects and
links TBB automatically for GNU builds; without it, libstdc++ falls back to
serial execution.

## Build

From the repository root:

```bash
make
```

The `Makefile` wraps the CMake build. To select a compiler or build directory:

```bash
make CXX=/path/to/clang++ BUILD_DIR=build-llvm JOBS=8
make CXX=/path/to/g++-16 BUILD_DIR=build-gcc JOBS=8
```

The equivalent convenience targets are `make build-llvm`, `make build-gcc`,
and `make test-compilers`. Optimized machine-specific builds enable LTO and
native CPU tuning:

```bash
make build-fast          # preferred LLVM/libdispatch build
make build-gcc-fast
make build-llvm-fast
```

Profile-guided builds train on one representative dense and sparse instance,
then rebuild with LTO, native tuning, and the collected profile:

```bash
make pgo-gcc
make pgo-llvm
```

The resulting executables are `build-gcc-pgo/main` and
`build-llvm-pgo/main`. PGO training instances and start counts can be changed
with the `PGO_DENSE_INSTANCE`, `PGO_SPARSE_INSTANCE`, `PGO_DENSE_STARTS`, and
`PGO_SPARSE_STARTS` Make variables. Native builds are not portable to older or
different CPU families.

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


## Run

From the repository root:

```bash
./build/main <max|min> <full|half> <instance.qsc_or_txt> [best_known_results.csv_or_folder] [base_seed] [runs] [time_limit_seconds] [index_base] [workers] [starts]
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

`workers` defaults to the value of `FCBQP_NUM_THREADS`, or to four when the
environment variable is unset. The fixed default prevents machines with
different core counts from silently defining different searches. The solver
prints the selected Parallel STL backend at startup. `starts` is the
deterministic total search-start budget and defaults to 200000.

Each deterministic epoch uses one logical lane per worker by default. Physical
workers dynamically claim lanes, while results are merged in lane-ID order.
The solver exchanges a deterministic logical-work cutoff when a lane reaches
the target. All lanes finish their current start at the epoch barrier, and
results beyond that cutoff are discarded deterministically. This avoids a
speed-dependent mid-kernel cancellation point. Set
`FCBQP_EPOCH_LANES_PER_WORKER` to a positive integer to overdecompose the
search; because it changes the seeded search streams, it is part of the
deterministic configuration.
`FCBQP_ELITE_GROUP_SIZE` can partition those lanes into fixed independent
elite-sharing groups. Group membership and all exchanges remain deterministic.
`FCBQP_CONSTRUCTION_MAX_ATTEMPTS` sets the deterministic retry budget for a
diverse greedy construction; the quality-preserving default is 200.

## Determinism

With a fixed input, seed, worker count, epoch-lane factor, and start budget,
solution fields are deterministic when `time_limit_seconds` is zero. Logical
lanes advance in synchronous search epochs and elite candidates are merged in
lane-ID order. RNG mapping and candidate tie-breaking are independent of the
standard-library implementation.

A positive time limit is the explicit exception because machine speed controls
how many epochs finish. Timing measurements such as TTS and wall time naturally
vary even when the solution is identical. Run the LLVM/GCC fixed-work
regression with `make check-determinism`. Full audit details are in
`benchmark_results/DETERMINISM_REPORT.md`.

## Reproducible smoke benchmark

The ten-instance benchmark covers MDP, MCP, and GBP instances using four
threads and one fixed-seed run per instance:

```bash
make benchmark
```

Raw logs and a CSV summary are written under `benchmark_results/`.
The measured OpenMP-to-Parallel-STL comparison is documented in
`benchmark_results/PSTL_REPORT.md`.
The LLVM/libdispatch versus GCC/TBB comparison is documented in
`benchmark_results/COMPILER_REPORT.md`.
The current parser, kernel, deterministic overdecomposition, LTO/native, and
PGO measurements are documented in `benchmark_results/OPTIMIZATION_REPORT.md`.
The deterministic logical-counter scheduler and current comparison against the
`stl` branch are documented in
`benchmark_results/COUNTER_SCHEDULER_REPORT.md`.
The ten-core, time-limit-free GBP benchmark is documented in
`benchmark_results/GBP_FIXED_WORK_REPORT.md` and can be reproduced with
`make benchmark-gbp-fixed`.

Benchmark settings can be overridden without editing files:

```bash
make benchmark BENCHMARK_PHASE=trial BENCHMARK_THREADS=8 SOLVER_TIME_LIMIT=60 BENCHMARK_TIMEOUT=75
```

Use `BENCHMARK_LANES_PER_WORKER` to record and test a different deterministic
overdecomposition factor. `make benchmark-fast` benchmarks the LLVM
LTO/native build, and `make benchmark-pgo` trains and benchmarks both PGO
builds.

Run the same benchmark sequentially with LLVM/libdispatch and GCC/TBB using:

```bash
make benchmark-compilers
```

Run the regression tests with `make test` and list all Make targets with
`make help`.

## Related link:

- [Benchmark instances used in the computational study conducted in the paper](https://github.com/LeThiThanhTu/Benchmark_instances_FCBQP)
