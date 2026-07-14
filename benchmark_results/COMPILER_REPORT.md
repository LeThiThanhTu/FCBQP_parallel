# LLVM/libdispatch versus GCC/TBB

> Historical note: these measurements predate the deterministic search-epoch
> conversion. See `DETERMINISM_REPORT.md` for current reproducibility results.

## Configuration

Both executables were built from the same source in CMake Release mode and
used four solver workers:

- LLVM/Clang 22.1.8, libc++, C++20 PSTL/libdispatch
- GCC 16.1.0, libstdc++, C++20 PSTL/TBB 2023.1.0

The ten-instance benchmark used base seed 1, one run per instance, the same
`best_known_results.csv`, and a 90-second solver limit. A CPU-time check on
`crack` measured approximately 12 CPU seconds per 3 wall seconds for both
executables, confirming four-worker execution. The LLVM binary has no TBB or
OpenMP dependency; the GCC binary links TBB and has no OpenMP dependency.

## Ten-instance results

| Instance | LLVM result/TTS (s) | GCC result/TTS (s) |
| --- | ---: | ---: |
| GKD-c n=500 | 19485.186970 / 0.000399 | 19485.186970 / 0.000203 |
| MDG-b n=500 | 778030.570000 / 0.008265 | 778030.570000 / 0.004050 |
| SOM-b n=500 | 7141 / 0.026545 | 7141 / 0.015789 |
| MDG-a n=2000 | 114271 / 0.319190 | 114271 / 0.566467 |
| C125.9 | 1122 / 0.000175 | 1122 / 0.000081 |
| hamming8-4 | 240 / 0.000133 | 240 / 0.000099 |
| jazz | 870 / 0.000248 | 870 / 0.000086 |
| email | 132 / 0.004931 | 132 / 0.002365 |
| ca-GrQc | 1892 / 0.000577 | 1892 / 0.000509 |
| crack | 258 / not reached in 90 s | 184 / 18.033978 |

Both builds reached all nine short/medium references. For those cases:

- LLVM used 1.180072 s total harness wall time versus GCC's 1.974007 s, so
  LLVM was 1.67x faster including startup.
- LLVM used 0.360814 s summed solver time versus GCC's 0.589935 s, so LLVM was
  1.64x faster in aggregate. This is dominated by MDG-a n=2000, where LLVM was
  1.77x faster; GCC had lower TTS on the eight much shorter searches.
- Sub-millisecond results are strongly affected by timer, scheduler, and
  runtime-startup noise.

On `crack`, GCC/TBB reached the best-known objective in 18.033978 seconds;
LLVM/libdispatch ended at 258 after 90 seconds. Consequently the complete
harness took 20.045018 seconds with GCC and 91.198771 seconds with LLVM, making
GCC 4.55x faster for this particular end-to-end benchmark.

## Fixed-budget `crack` repetitions

To check whether the long-case result was an isolated scheduling outcome,
three base seeds were run with a ten-second budget:

| Backend | Seed 1 | Seed 2 | Seed 3 | Median objective |
| --- | ---: | ---: | ---: | ---: |
| LLVM/libdispatch | 562 | 453 | 444 | 453 |
| GCC/TBB | 185 | 184 at 2.733 s | 184 at 4.846 s | 184 |

Lower is better. GCC reached the reference in two of three runs and was one
unit away in the third; LLVM did not reach it in any run.

This quality difference is not a pure compiler-throughput measurement. The
solver shares an elite pool between workers, so task timing changes the search
trajectory. In addition, the C++ standard does not require `std::shuffle` and
random-distribution implementations to produce identical transformations
across libc++ and libstdc++, even with the same engine seed. The measured
result is therefore the effectiveness of each complete compiler, standard
library, and PSTL-backend combination.

## Recommendation

Use GCC 16 with TBB for the current mixed benchmark: it is far more effective
on the difficult graph-bisection instance and wins the end-to-end suite. Use
LLVM 22/libdispatch when startup latency or the MDG-a dense workload is the
priority. Retain both in testing because they exercise independent Parallel
STL implementations.

Raw data is in `llvm22_dispatch.csv`, `gcc16_tbb.csv`, their log directories,
`compiler_comparison.csv`, and `crack_fixed_budget.csv`.
