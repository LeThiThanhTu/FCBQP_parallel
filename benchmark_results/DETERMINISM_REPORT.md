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
- A lane that reaches the target publishes its logical-work counter as an
  epoch-local cutoff. Other lanes stop at the same algorithmic frontier, and
  results produced beyond a subsequently lowered cutoff are discarded before
  the lane-order merge. Target detection therefore does not cancel or retain
  work based on wall-clock order.
- Fisher-Yates shuffling and low-bit rejection sampling are implemented
  directly from the standardized `std::mt19937_64` output. This preserves the
  fast libc++ trajectory without depending on a library distribution class.
- Candidate selection uses a fully specified `nth_element` implementation.
  The heuristic samples the selected prefix, so merely giving library
  `std::nth_element` a total comparator would not specify the prefix's internal
  order across standard libraries.
- GCC and Clang builds disable floating-point contraction so an FMA decision
  cannot change a heuristic comparison.
- The CLI accepts a deterministic total `starts` budget. Benchmark CSVs record
  the seed, worker count, epoch-lane factor and count, start budget, target
  epoch, logical start-to-best, deterministic logical-work frontier, complete
  selected-variable list, and a stable solution fingerprint.
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
on all ten cases, including exact selected-variable lists, fingerprints, and
logical-work frontiers. The automated CSV comparison checks 22 invariant
fields per case. As expected, elapsed times differed.

- Current four-start LLVM harness wall total: 1.046053 seconds.
- Current four-start GCC harness wall total: 1.169222 seconds.
- MDG-a n=2000: objective 114271, logical start 1, fingerprint
  `90f897278dfc330f` with both compilers.
- `crack`: objective 566, logical start 1, fingerprint `56088a91a3571268`
  and logical frontier 86768 with both compilers.
- A scheduling test delays lane 0 and lane 1 independently by 20 ms and checks
  that the result, target epoch/start, fingerprint, and logical frontier remain
  identical.

Run the permanent cross-compiler check with:

```bash
make check-determinism
```

The current raw fixed-work results are in `deterministic_llvm.csv` and
`deterministic_gcc.csv`. `tests/compare_benchmarks.py` performs the invariant
comparison. The counter-scheduler implementation and the current `stl` branch
comparison are documented in `COUNTER_SCHEDULER_REPORT.md`.
