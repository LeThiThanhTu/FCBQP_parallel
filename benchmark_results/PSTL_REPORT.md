# OpenMP to C++20 Parallel STL benchmark

> Historical note: these measurements predate the deterministic search-epoch
> conversion. See `DETERMINISM_REPORT.md` for current reproducibility results.

## Method

Both versions were compiled in Release mode with LLVM/Clang 22.1.8 and run
with four workers, one fixed-seed run per instance, and a 90-second solver
limit. The OpenMP executable was built from commit `82d96c7` immediately before
the conversion. Every reported objective was checked against
`best_known_results.csv`.

The converted build reports `C++20 PSTL/libdispatch`. A CPU-time check on the
long graph-bisection case measured about 12 CPU seconds over 3 wall seconds,
confirming that four solver workers execute concurrently. Exact heuristic
timings and the shared elite-pool trajectory remain scheduling-dependent.

## Conversion plan and implementation

1. Remove the OpenMP header, pragmas, runtime calls, CMake discovery, and link
   dependency.
2. Express preprocessing and independent solver starts through
   `std::for_each(std::execution::par, ...)`.
3. Preserve a configurable worker cap with lightweight scheduling tickets.
   This is necessary because PSTL implementations may execute very short input
   ranges serially even when each element represents a long-running task.
4. Replace OpenMP critical regions with standard mutexes, publish immutable
   elite snapshots atomically, and reduce per-worker results from independent
   result slots after the parallel algorithm completes.
5. Catch worker exceptions inside the execution-policy callback, where an
   escaping exception would otherwise terminate the process.
6. Move the project to C++20, enable LLVM libc++'s experimental Parallel STL
   library, expose `FCBQP_NUM_THREADS` and the final CLI `workers` argument, and
   make LLVM 22 the preferred macOS compiler.

## Results

| Instance | OpenMP result/TTS (s) | Parallel STL result/TTS (s) |
| --- | ---: | ---: |
| GKD-c n=500 | 19485.186970 / 0.000439 | 19485.186970 / 0.000422 |
| MDG-b n=500 | 778030.570000 / 0.008202 | 778030.570000 / 0.008377 |
| SOM-b n=500 | 7141 / 0.027024 | 7141 / 0.026634 |
| MDG-a n=2000 | 114271 / 0.317856 | 114271 / 0.320213 |
| C125.9 | 1122 / 0.000181 | 1122 / 0.000176 |
| hamming8-4 | 240 / 0.000137 | 240 / 0.000131 |
| jazz | 870 / 0.000244 | 870 / 0.000245 |
| email | 132 / 0.004828 | 132 / 0.004837 |
| ca-GrQc | 1892 / 0.000613 | 1892 / 0.000609 |
| crack | 258 / not reached in 90 s | 184 / 85.247355 |

Summary:

- Best-known objectives reached: 9/10 with OpenMP, 10/10 with Parallel STL.
- Total harness wall time: 91.464709 s to 86.444745 s, a 1.06x speedup
  (5.8% less time).
- Wall time for the nine short/medium cases: 1.446635 s to 1.179438 s, a
  1.23x speedup. Most of this is lower process/runtime startup overhead.
- Geometric-mean TTS speedup over the nine cases solved by both versions:
  1.01x. Their summed solver runtime changed from 0.359846 s to 0.362031 s
  (0.6% slower), which is within scheduling and timer noise.
- On `crack`, Parallel STL found the best-known 184 after 85.247355 s; the
  OpenMP baseline ended at 258 after 90 seconds.

Raw data and logs are in `openmp_llvm22.csv`, `pstl_llvm22.csv`, their matching
log directories, and `pstl_comparison.csv`.

## Verification

- LLVM 22.1.8 Release build and regression test: passed.
- GCC 16.1 Release build and regression test: passed. At the time of this
  initial conversion benchmark, TBB was not installed and libstdc++ reported
  `C++20 PSTL/serial`; the later compiler comparison uses GCC with TBB.
- Clang warnings (`-Wall -Wextra -Wpedantic -Wshadow`): clean.
- AddressSanitizer and UndefinedBehaviorSanitizer: clean with leak detection
  disabled.
- ThreadSanitizer, four-worker graph-bisection run: clean.

LeakSanitizer reports 24 bytes per libc++ `dispatch_apply` invocation. The same
report occurs in an isolated one-line Parallel STL program, so this is an LLVM
22 experimental-library/runtime issue rather than a solver allocation leak.
