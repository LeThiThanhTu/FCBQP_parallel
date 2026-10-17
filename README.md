# A Parallel Heuristic for FCBQP

This repository contains the implementation of the algorithm described in the paper

*A Parallel Heuristic for the Fixed Cardinality Binary Quadratic Program* (Thi Thanh Tu Le and Thorsten Koch).

The main implementation is in `main.cpp`.

The best-known/reference results are stored in `best_known_results.csv`.

The detailed computational results mentioned in the paper are provided in `summary_results.xlsx`. The code used to obtain these results was compiled using `make build-gcc-fast`.

The fixed-cardinality binary quadratic program is:

```math
\begin{aligned}
\min / \max \quad & f(x)=x^\top Qx \\
\text{s.t.} \quad
& \sum_{i=1}^{n} x_i = k, \\
& x \in \{0,1\}^n,
\end{aligned}
```

where $Q$ is the cost matrix and $k$ represents the cardinality constraint. 

Without loss of generality, we assume that $Q$ is symmetric and $k \leq n/2$.

The smoke benchmark instances are stored in the `dataset` directory. The collection contains instances from:

- Maximum Dispersion Problem (MDP)
- Maximum Clique Problem (MCP)
- Graph Bisection Problem (GBP)

## Requirements

- A C++20 compiler with Parallel STL support
- CMake 3.16 or newer
- GNU Make
- Python 3 for tests and benchmark scripts
- TBB when required by the selected Parallel STL implementation

The compiler and parallel backend depend on the software available on the machine. CMake reports the selected backend during configuration.

## Build

From the repository root, run:

```bash
make
```

Compiler-specific builds are also available:

```bash
make build-gcc
```
```bash
make build-llvm
```

Optimized builds with LTO and native CPU tuning can be created with:

```bash
make build-fast
```
```bash
make build-gcc-fast
```
```bash
make build-llvm-fast
```

To select a compiler explicitly:

```bash
make CXX=/path/to/compiler BUILD_DIR=build-custom
```

Profile-guided builds are available through:

```bash
make pgo-gcc
```
```bash
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
```
```bash
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
./build/main <max|min> <full|half> <instance> [best_known_results.csv_file] [base_seed] [runs] [time_limit_seconds] [index_base] [workers] [starts]
```

or without best known values:

```bash
./build/main <max|min> <full|half> <instance> "" [base_seed] [runs] [time_limit_seconds] [index_base] [workers] [starts]
```

For example: 

```bash
./build/main max half dataset/MDP/GKD-c/GKD-c_1_n500_m50.txt best_known_results.csv 1 5 60 auto 8 160000
```

This command executes 5 independent runs with base seed 1, a time limit of 60 seconds per run, automatic index-base detection, 8 workers, and a maximum of 160000 search starts per run. Each run and logical lane uses a distinct, deterministically derived pseudorandom seed. A run terminates when the best-known/reference value is reached, the time limit expires, or the start budget is exhausted.

The default base seed is 1, the default number of runs is 10, and `time_limit_seconds` defaults to 600; 0 disables the time limit. `index_base` can
be `auto`, `0`, or `1`. If the indices are compatible with both conventions,
the program rejects the ambiguous input unless `0` or `1` is supplied.

Matrix data may contain one triangle of a symmetric matrix or both symmetric
entries. When both `(i,j)` and `(j,i)` are present, their values must agree within a numerical tolerance; they are counted once rather than doubled.

Cardinalities from `0` through `n` are accepted. The trivial cases `k = 0`, `k = 1`, `k = n - 1`, and `k = n`  are solved directly.

`workers` defaults to the value of `FCBQP_NUM_THREADS`, or to 4 when the
environment variable is unset. The fixed default prevents machines with
different core counts from silently defining different searches. The code
prints the selected Parallel STL backend at startup. `starts` sets the maximum total number of search starts per run, distributed among the logical lanes, and defaults to 200000. A best-known value is used as a target for early stopping; if no best-known value is available, execution continues until the time limit or start budget is exhausted.

To run with the default setting:

```bash
./build/main <max|min> <full|half> <instance> [best_known_results.csv_file]
```

For example:

```bash
./build/main max half dataset/MDP/GKD-c/GKD-c_1_n500_m50.txt best_known_results.csv
```

The `max|min` argument chooses the optimization sense. The code internally minimizes (negating matrix coefficients for maximization), then restores the requested sense when reporting the objective. `full` reports the objective as $x^\top Qx$, while `half` reports half that value. Best-known/reference values must use the chosen reporting scale.

## Cardinality-complement transformation

For instances with $k>n/2$, the code automatically applies the complement transformation

$$
y=\mathbf{1}-x,\qquad Q'=Q-2\mathrm{Diag}(Q\mathbf{1}),
$$

resulting in an equivalent FCBQP with cardinality $k'=n-k<n/2$. The constant objective offset $c=\mathbf{1}^{\top}Q\mathbf{1}$ is retained to recover the original objective value:

$$
x^{\top}Qx=y^{\top}Q'y+c.
$$

After solving a complemented instance, the code restores the original variable assignment and independently checks the reported objective using the original cost matrix and original cardinality constraint. 

The repository includes regression tests for code correctness and the cardinality-complement transformation. Run all tests using:

```bash
make test
```

## Determinism

With a fixed input, seed, worker count, epoch-lane factor, and start budget, solution fields are deterministic when `time_limit_seconds` is 0. Elite candidates are merged in lane-ID order. RNG mapping and candidate tie-breaking are independent of the standard-library implementation.

A positive time limit is the explicit exception because machine speed controls how many epochs finish. Timing measurements such as TTS and wall time naturally vary even when the solution is identical. 

Check deterministic fixed-work behavior with 

```bash
make check-determinism
```

## Reproducible smoke benchmark

The ten-instance benchmark covers MDP, MCP, and GBP instances using four
threads and one fixed-seed run per instance:

```bash
make benchmark
```

Run the benchmark sequentially with both configured compiler toolchains using:

```bash
make benchmark-compilers
```

List all available benchmark and build targets with:

```bash
make help
```



## Related repository

- [Benchmark instances used in the computational study conducted in the paper](https://github.com/LeThiThanhTu/Benchmark_instances_FCBQP)
