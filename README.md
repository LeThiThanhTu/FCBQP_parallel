# FCBQP Parallel

Parallel FCBQP runner for the benchmark instances in `dataset`.

## Requirements

- A modern C++17 compiler with standard `<filesystem>` support, such as GCC 9+, Clang 9+, or a recent MSVC.
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
