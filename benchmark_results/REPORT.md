# Before/after benchmark

Method: one run per instance, base seed 1, four OpenMP threads, GCC 15.2,
Release build. The graph-bisection case had a 90-second budget; the baseline
did not enforce its internal limit responsively, so the harness terminated it
at 90 seconds. All objectives are checked against `best_known_results.csv`.
Parallel elite-pool updates make exact timings schedule-dependent despite the
fixed per-thread seeds.

| Instance | Family/storage | Baseline TTS (s) | Optimized TTS (s) | TTS change |
| --- | --- | ---: | ---: | ---: |
| GKD-c n=500 | MDP/dense | 0.000602 | 0.000247 | 2.44x faster |
| MDG-b n=500 | MDP/dense | 0.004926 | 0.005136 | 4.3% slower |
| SOM-b n=500 | MDP/dense | 0.018594 | 0.020871 | 12.2% slower |
| MDG-a n=2000 | MDP/dense | 15.899098 | 3.222328 | 4.93x faster |
| C125.9 | MCP/dense | 0.000103 | 0.000105 | 1.9% slower |
| hamming8-4 | MCP/dense | 0.000159 | 0.000090 | 1.77x faster |
| jazz | MCP/dense | 0.000125 | 0.000091 | 1.37x faster |
| email | MCP/dense | 0.004335 | 0.002300 | 1.88x faster |
| ca-GrQc | MCP/CSR | 0.002959 | 0.000490 | 6.04x faster |
| crack | GBP/CSR | >90 (timeout) | 19.383633 | >4.64x faster |

Summary:

- Reference objectives reached: 9/10 before, 10/10 after.
- Sum of solver runtimes on the nine baseline-completed cases: 15.982737 s
  before and 3.251874 s after, a 4.92x speedup.
- Total harness wall time including the 90-second baseline timeout: 107.330198
  s before and 24.027792 s after, a 4.47x speedup.
- Sub-millisecond TTS values are dominated by startup, scheduling, and timer
  noise; the MDG-a and crack cases are the meaningful end-to-end comparisons.

Raw measurements are in `baseline.csv`, `optimized.csv`, and their log
directories.
