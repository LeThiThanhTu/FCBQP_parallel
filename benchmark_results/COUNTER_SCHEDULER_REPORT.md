# Deterministic counter scheduler: implementation and branch comparison

## Outcome

The deterministic `codex` working tree now outperforms the `stl` branch on the
nine instances for which both reach the reference objective.  It also reaches
the `crack` reference value of 184 within the 15-second limit with both
compilers; `stl` does not.

Relative to the previous `codex`/`stl` comparison, the first-nine solver-time
total fell from 10.777 to 0.200596 seconds with LLVM (53.72x) and from 11.887
to 0.216112 seconds with GCC (55.00x).  The previous `codex` result on `crack`
was 1773 at the time limit; it is now the reference value 184.

## Method

- `codex`: working tree based on commit `67211b1`.
- `stl`: commit `9d7a0e0` (`origin/stl`).
- Compilers: LLVM/Clang 22.1.8 and GCC 16.1.0 on arm64.
- Release builds with native-architecture tuning and link-time optimization.
- Four workers, base seed 1, one run, 200,000-start budget and a 15-second
  internal safety limit per instance.
- `codex` used one deterministic lane per worker.  `stl` used four
  `std::thread` workers via `FCBQP_NUM_THREADS=4`.
- The first nine cases terminate on the objective from
  `best_known_results.csv`.  `crack` is the only case for which `stl` reaches
  the time limit.

One run is enough to compare deterministic search results, but sub-millisecond
timings remain sensitive to process startup and normal measurement noise.  The
MDG-a and `crack` cases are the useful timing anchors.

## Results versus `stl`

| Compiler | First 9 solver time: codex | First 9 solver time: stl | Solver speedup | First 9 wall: codex | First 9 wall: stl | Wall speedup |
| --- | ---: | ---: | ---: | ---: | ---: | ---: |
| LLVM 22.1.8 | 0.200596 s | 0.361752 s | 1.80x | 0.530876 s | 1.182137 s | 2.23x |
| GCC 16.1.0 | 0.216112 s | 0.588777 s | 2.72x | 0.567760 s | 1.999415 s | 3.52x |

| Compiler | MDG-a codex | MDG-a stl | Speedup | `crack` codex | `crack` stl | All-10 wall speedup |
| --- | ---: | ---: | ---: | --- | --- | ---: |
| LLVM 22.1.8 | 0.190382 s | 0.319287 s | 1.68x | **184**, 9.666081 s | 365, 15.000342 s | 1.59x |
| GCC 16.1.0 | 0.204677 s | 0.565711 s | 2.76x | **184**, 10.199478 s | 185, 15.000250 s | 1.58x |

All first-nine objective values match the reference file on both branches.
The total all-10 wall comparison also favors `codex`, despite it doing enough
work to obtain the better `crack` result: 10.202924 versus 16.200357 seconds
with LLVM and 10.782418 versus 17.034325 seconds with GCC.

Raw results and per-instance logs:

- `counter_codex_llvm.csv` and `counter_codex_llvm_logs/`
- `counter_stl_llvm.csv` and `counter_stl_llvm_logs/`
- `counter_codex_gcc.csv` and `counter_codex_gcc_logs/`
- `counter_stl_gcc.csv` and `counter_stl_gcc_logs/`

## What changed in `codex`

1. Each worker now has a monotonic logical-work counter.  An epoch-local
   atomic publishes the smallest counter value at which any lane reaches the
   target.  Other lanes stop at that same algorithmic frontier.
2. Work beyond a newly lowered cutoff is speculative.  Its result and elite
   candidate are discarded before the deterministic lane-order merge.  This
   prevents a faster core from contributing extra search work.
3. The reported logical frontier is derived from the cutoff rather than the
   amount of speculative physical work, so it is invariant under scheduling.
4. The default was changed from four lanes per worker to one.  Optional
   overdecomposition remains available, but it deliberately defines a
   different deterministic search configuration.
5. Bounded random selection and shuffle are fully specified while retaining
   the fast libc++ search stream.  This removes standard-library-dependent RNG
   mapping without paying the earlier trajectory-quality penalty.
6. Candidate partitioning uses a fully specified `nth_element` equivalent.
   The C++ standard does not define order within the selected prefix, and the
   heuristic randomly samples that prefix, so using the library implementation
   directly made GCC and LLVM follow different searches.
7. The benchmark harness records logical work and has a legacy-interface mode
   so the same driver can measure `stl` fairly.

## Code differences from `stl`

The `stl` branch creates and joins `std::thread` objects directly.  Workers
publish elite snapshots asynchronously under a mutex, stop according to
physical timing, use library `std::shuffle`, `uniform_int_distribution`, and
`std::nth_element`, and keep whichever result wins the scheduling race.  That
design is compact and was previously much faster, but its search path changes
with compiler library, worker timing, and machine speed.

`codex` uses the C++20 Parallel STL (`libdispatch` with LLVM and TBB with GCC),
immutable epoch snapshots, lane-ID merge order, canonical tie handling,
specified random/partition algorithms, and the logical target frontier.  Its
larger amount of coordination code buys reproducible results across both
backends while now exceeding `stl` performance on this benchmark.

## Validation

- `make check-determinism`: passed for 10 cases and 22 invariant fields per
  case across LLVM and GCC.  These fields include objective, selected
  variables, solution fingerprint, target epoch, start-to-best, and logical
  work.
- The smoke test delays lane 0 and lane 1 independently by 20 ms and verifies
  that the deterministic fields are unchanged.
- LLVM and GCC smoke-test suites pass.
- LLVM ASan and UBSan smoke-test suite passes.
- LLVM `-Wall -Wextra -Wpedantic` build is clean.  The linker emits only a
  macOS deployment-version warning for Homebrew's prebuilt
  `libc++experimental.a`.

Wall-clock time and TTS are measurements and are intentionally not invariant.
If no target is reached and the configured wall-clock safety limit expires,
the final solution can still depend on how much deterministic work the
machine completed before that limit.  With a fixed start budget and no runtime
limit, all benchmark result fields above are deterministic.
