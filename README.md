# A Parallel Heuristic for FCBQP: $\min / \max \ f(x)=x^\top Qx \quad \text{s.t.} \quad \sum_{i=1}^{n}x_i=k,\quad x \in \lbrace 0,1 \rbrace^n$

The heuristic implementation is in `main.cpp`. Paper: A Parallel Heuristic for the Fixed Cardinality Binary Quadratic Program.

The benchmark instances are in `dataset`. The collection contains instances from Maximum Dispersion Problem (MDP), Maximum Clique Problem (MCP), and Graph Bisection Problem (GBP).

## Requirements

- A C++17 compiler with standard `<filesystem>` support.
- OpenMP.
- CMake 3.10 or newer.

## Build

```bash
cmake -S . -B build
cmake --build build
```

With Unix Makefiles, this is equivalent to:

```bash
cmake -S . -B build
cd build
make
make run
```

## Run All Dataset Sets

From the repository root:

```bash
cmake --build build --target run
```

The default batch run uses:

- `dataset/MDP`: `max half`
- `dataset/MCP`: `max full`
- `dataset/GBP`: `min full`
- `best_known_results.csv` as the best-known objective file

## Run One Instance

```bash
./build/FCBQP_parallel <max|min> <full|half> <instance.qsc_or_txt> [best_known_results.csv_or_folder] [base_seed] [runs] [report.csv]
```
