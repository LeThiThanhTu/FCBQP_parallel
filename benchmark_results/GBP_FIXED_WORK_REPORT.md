# Deterministic fixed-work GBP benchmark

## Criterion selection

The preceding 15-second benchmark showed that `whitaker3` improved at
lane-local starts 18 through 22. A budget of 23 starts per worker therefore
completes one full deterministic epoch beyond the unstable boundary. With ten
workers this is 230 total starts. The solver's internal time limit is zero; a
300-second process timeout is retained only as a failure guard and was not
triggered.

A start/restart budget is preferable to one global logical-iteration count for
this suite. The measured cost of a logical step varies by more than 12x between
GBP instances because matrix sizes, row degrees, and the mix of construction,
tabu, path-relink, and restart work differ. Starts are the natural top-level
unit, divide evenly into 23 deterministic epochs, and directly cover the
`whitaker3` improvement boundary observed in the calibration run.

## Method

- `codex`: commit `2e2891b` plus the benchmark harness changes.
- `stl`: commit `9d7a0e0`, with its hard-coded start count changed only from
  200000 to 230 in the temporary benchmark build.
- GCC 16.1.0, C++20, `-O3`, native architecture, LTO, and
  `-ffp-contract=off` for both builds.
- Ten workers, one deterministic lane per `codex` worker, base seed 1, three
  fresh-process repetitions per instance, 230 total starts, no internal time
  limit.
- Times are solver-time mean +/- sample standard deviation. GBP is a
  minimization problem, so lower objectives are better.

All 48 processes completed normally.

## Fixed-work results

| Instance | Reference | `codex` objective | `stl` objective | `codex` time | `stl` time | Main result |
| --- | ---: | ---: | ---: | ---: | ---: | --- |
| `4elt` | 139 | 139 (3/3) | 139 (3/3) | 0.170212 +/- 0.000790 s | 3.208177 +/- 0.425452 s | `codex` 18.85x faster |
| `bcsstk29` | 2843 | 2843 (3/3) | 2843 (3/3) | 4.077980 +/- 0.016343 s | 7.384865 +/- 0.677145 s | `codex` 1.81x faster |
| `bcsstk33` | 10171 | 10171 (3/3) | 10171 (3/3) | 21.193829 +/- 0.037648 s | 1.639657 +/- 0.027665 s | `stl` 12.93x faster |
| `crack` | 184 | 186 (3/3) | 184 (3/3) | 18.809462 +/- 0.039192 s | 19.851916 +/- 1.389372 s | Similar time; `stl` has better quality |
| `cti` | 334 | 334 (3/3) | 334 (3/3) | 2.217901 +/- 0.010005 s | 9.236782 +/- 0.201787 s | `codex` 4.16x faster |
| `fe_4elt2` | 130 | 130 (3/3) | 235-512 | 16.885767 +/- 0.055098 s | 86.544970 +/- 0.492959 s | `codex` is faster, better, deterministic |
| `fe_sphere` | 386 | 386 (3/3) | 386 or 438 | 91.389572 +/- 5.225647 s | 48.075877 +/- 18.074272 s | `codex` reliable; `stl` faster when successful |
| `whitaker3` | 127 | 252 (3/3) | 128 (3/3) | 15.207125 +/- 0.187235 s | 41.122473 +/- 0.271631 s | `codex` faster; `stl` much better quality |

The sum of per-instance mean solver times is 169.951850 seconds for `codex`
and 217.064717 seconds for `stl`, making `codex` 1.28x faster in aggregate for
these actual early-stop/fixed-budget executions. Objective quality is mixed,
so the aggregate alone is not a solver-quality ranking.

## Effect of replacing the time limit

| Instance | Previous 15-second `codex` objective | Fixed-work objective | Fixed-work outcome |
| --- | ---: | ---: | --- |
| `4elt` | 139 | 139 | Reference retained |
| `bcsstk29` | 2843 | 2843 | Reference retained |
| `bcsstk33` | 10172 | 10171 | Now reaches reference |
| `crack` | 258 | 186 | Improves by 72; two above reference |
| `cti` | 334 | 334 | Reference retained |
| `fe_4elt2` | 221 | 130 | Now reaches reference |
| `fe_sphere` | 506 | 386 | Now reaches reference |
| `whitaker3` | 252-254 | 252 | Better timed result retained deterministically |

`codex` now reaches 6/8 references instead of 3/8. Most importantly, the
`whitaker3` wall-limit effect is gone: objective 252, target status,
start-to-best 22, logical work, fingerprint, and the complete selected-variable
list are identical in all three runs.

## Determinism

The automated checker compared ten non-timing fields across repetitions:
status, return code, reference and found objectives, target status, target
epoch, start-to-best, logical work, fingerprint, and selected variables.

- `codex`: all ten fields identical for all eight instances.
- `stl`: exact selected solutions differed on `bcsstk29`, `crack`, `cti`,
  `fe_4elt2`, and `fe_sphere`; objectives also differed on `fe_4elt2` and
  `fe_sphere`. On `fe_sphere`, two runs reached 386 while one exhausted the
  same budget at 438.

Thus removing the deadline eliminates the only observed `codex` result
variation, while `stl` remains schedule-dependent under identical fixed work.

## Statistics guiding further optimization

| Instance | `codex` logical work | Work rate | Time CV | Target epoch / start-to-best |
| --- | ---: | ---: | ---: | --- |
| `4elt` | 66,220 | 0.389 M/s | 0.46% | 1 / 1 |
| `bcsstk29` | 652,824 | 0.160 M/s | 0.40% | 7 / 7 |
| `bcsstk33` | 732,729 | 0.035 M/s | 0.18% | 14 / 14 |
| `crack` | 2,257,177 | 0.120 M/s | 0.21% | no target / 23 |
| `cti` | 578,041 | 0.261 M/s | 0.45% | 4 / 4 |
| `fe_4elt2` | 2,607,643 | 0.154 M/s | 0.33% | 18 / 18 |
| `fe_sphere` | 39,680,283 | 0.434 M/s | 5.72% | 18 / 18 |
| `whitaker3` | 3,417,248 | 0.225 M/s | 1.23% | no target / 22 |

Logical-work categories have different costs, so the rates are diagnostic
rather than exact instruction throughput. They nevertheless separate two
different bottleneck classes:

1. **`fe_sphere` search volume:** it consumes 53.8% of total `codex` time and
   39.7 million logical steps, yet has the highest step rate. Kernel tuning
   alone will not address this. The priority is a better deterministic search
   trajectory or earlier elite information exchange. The fact that `stl`
   sometimes reaches the reference in 36-39 seconds, but sometimes misses it,
   suggests there is useful search information in its asynchronous exchange
   pattern that could be reproduced at deterministic micro-epoch boundaries.
2. **`bcsstk33` cost per step:** it executes relatively little logical work but
   runs at only 0.035 M steps/s, 4.6x slower than `bcsstk29` and 12.4x slower
   than `fe_sphere`. Its CSR density is 0.775%, the highest in the GBP set.
   Profiling should focus on `find_sparse_tabu_move`: repeated incoming-order
   sorting, CSR row scans, edge-generation clearing, and candidate membership.
   A candidate-restricted interaction cache or an adaptive denser/block kernel
   for high-degree sparse instances is the strongest build-level opportunity.
3. **`crack` and `whitaker3` trajectory quality:** both exhaust all 23 starts
   per lane. `stl` reaches 184/128 while deterministic `codex` returns 186/252.
   The next algorithm experiment should change deterministic elite-exchange
   cadence or seed/lane portfolio, not merely make the current kernel faster.
4. **Runtime stability:** excluding `fe_sphere`, `codex` time CV is at most
   1.23%. `stl` reaches 37.6% on `fe_sphere` and 13.3% on `4elt`. The remaining
   `codex` `fe_sphere` variance is timing-only; all result and work fields match,
   so it is suitable for profiling but should use more than three timing samples
   when evaluating small optimizations.

## Reproduction and artifacts

Run the permanent benchmark and invariant check with:

```bash
make benchmark-gbp-fixed
```

Raw results and logs:

- `gbp10_fixed230_codex_gcc.csv` and `gbp10_fixed230_codex_gcc_logs/`
- `gbp10_fixed230_stl_gcc.csv` and `gbp10_fixed230_stl_gcc_logs/`
- `benchmark_gbp.py`
- `tests/check_repeated_benchmark.py`
