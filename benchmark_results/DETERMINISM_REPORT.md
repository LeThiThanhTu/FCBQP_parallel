# Determinism audit

## Finding

The previous parallel implementation was not deterministic. It had three
independent schedule or standard-library dependencies:

1. Workers read and updated a shared elite pool asynchronously, so mutex
   acquisition order changed later search decisions.
2. The first worker reaching the best-known objective cancelled the others,
   making the returned solution depend on relative worker speed.
3. `std::shuffle`, `std::uniform_int_distribution`, and tie-ambiguous
   `std::nth_element` calls can produce different search paths across standard
   libraries or sorting implementations.

## Changes

- Search now advances in deterministic epochs. Overdecomposed logical lanes
  execute against the same read-only elite state; after the Parallel STL call
  completes, candidates are merged in lane-ID order. Physical workers claim
  lanes dynamically, so load balancing cannot change the merge order.
- A target ends the search only at an epoch boundary, after every lane in
  that epoch has completed. Target detection no longer cancels other workers
  based on wall-clock order.
- Fisher-Yates shuffling and bounded sampling are implemented directly from
  the standardized `std::mt19937_64` output using rejection sampling.
- Candidate selection uses `std::nth_element` with a total comparator followed
  by a sorted selected prefix. Explicit index tie-breakers make the selected
  set and order independent of the library's partitioning strategy. Elite and
  preprocessing sorts also have total tie-breakers.
- GCC and Clang builds disable floating-point contraction so an FMA decision
  cannot change a heuristic comparison.
- The CLI accepts a deterministic total `starts` budget. Benchmark CSVs record
  the seed, worker count, epoch-lane factor and count, start budget, target
  epoch, logical start-to-best, complete selected-variable list, and a stable
  solution fingerprint.
- The default worker count is fixed at four instead of using detected hardware
  concurrency; `FCBQP_NUM_THREADS` or the CLI can still override it explicitly.

## Guarantee and exception

For fixed input, base seed, run count, worker count, epoch-lane factor, and
start budget, all non-timing result fields are deterministic when
`time_limit_seconds` is zero.
This includes the objective, selected solution, reference status, target
epoch, logical start-to-best, and summary statistics. Parallel task scheduling
and processor speed do not affect those fields.

A positive wall-clock limit is the explicit exception: a faster machine may
complete more deterministic epochs before the deadline. External harness
timeouts have the same limitation. TTS, time-to-best, solver runtime, and
harness wall time are measurements and are therefore intentionally not
expected to be identical.

Changing the seed, worker count, epoch-lane factor, or start budget changes the
defined search and is not expected to preserve a result.

## Verification

The ten benchmark cases were run with four workers, four total starts, and no
internal time limit using both LLVM 22.1.8/libdispatch and GCC 16.1/TBB.

The current optimized LLVM and GCC executions matched in every invariant field
on all ten cases, including exact selected-variable lists and fingerprints.
The automated CSV comparison checks 21 invariant fields per case. Portable,
LTO/native, and PGO builds also matched. As expected, elapsed times differed.

- Original LLVM runs: 3.632707 s and 3.661088 s total harness wall time.
- Original GCC runs: 2.565740 s and 2.603787 s total harness wall time.
- Current optimized runs: 0.975575 s for LLVM and 1.165824 s for GCC.

Additional multi-epoch checks with one lane per worker reproduced the original
trajectory exactly:

- MDG-a n=2000, 12 starts: objective 114230, logical start 3, fingerprint
  `1f236c29b1a7ea28` with both compilers.
- `crack`, 8 starts: objective 1842, logical start 1, fingerprint
  `08bdf7b88fa4aaf1` with both compilers.

Run the permanent cross-compiler check with:

```bash
make check-determinism
```

The original raw fixed-work results are in `deterministic_llvm.csv` and
`deterministic_gcc.csv`; current results are in `optimization_after_llvm.csv`
and `optimization_after_gcc.csv`. `tests/compare_benchmarks.py` performs the
invariant comparison.
