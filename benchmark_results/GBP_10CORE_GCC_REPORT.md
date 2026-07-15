# Ten-core GBP comparison: `codex` versus `stl`

> This report records the original 15-second experiment. The deterministic
> 230-start follow-up is in `GBP_FIXED_WORK_REPORT.md` and should be used for
> result reproducibility and optimization comparisons.

## Method

- `codex`: commit `2e2891b`.
- `stl`: commit `9d7a0e0` (`origin/stl`).
- Compiler: GCC 16.1.0, C++20, `-O3`, native architecture, LTO, and floating
  point contraction disabled for both branches.
- Ten workers, base seed 1, 200,000-start budget, and a 15-second internal
  limit. `codex` used one deterministic lane per worker.
- Every one of the eight `dataset/GBP` instances was executed five times as a
  fresh process with the same seed and configuration.
- Solver time below is mean +/- sample standard deviation over the five runs.
  GBP is a minimization problem, so lower objectives are better.

## Time and objective

| Instance | Reference | `codex` objective | `stl` objective | `codex` solver time | `stl` solver time | Result |
| --- | ---: | ---: | ---: | ---: | ---: | --- |
| `4elt` | 139 | 139 (5/5) | 139 (5/5) | 0.169816 +/- 0.000604 s | 3.009196 +/- 0.521594 s | `codex` 17.72x faster |
| `bcsstk29` | 2843 | 2843 (5/5) | 2843 (5/5) | 4.106607 +/- 0.003359 s | 7.626770 +/- 0.488739 s | `codex` 1.86x faster |
| `bcsstk33` | 10171 | 10172 (5/5) | 10171 (5/5) | 15.002022 +/- 0.000166 s | 1.646899 +/- 0.008170 s | `stl` reaches the reference |
| `crack` | 184 | 258 (5/5) | 185 (5/5) | 15.000347 +/- 0.000146 s | 15.000609 +/- 0.000098 s | `stl` has the better timed objective |
| `cti` | 334 | 334 (5/5) | 334 (5/5) | 2.264947 +/- 0.018773 s | 8.388631 +/- 2.746947 s | `codex` 3.70x faster |
| `fe_4elt2` | 130 | 221 (5/5) | 536 (5/5) | 15.001051 +/- 0.000441 s | 15.000419 +/- 0.000025 s | `codex` has the better timed objective |
| `fe_sphere` | 386 | 506 (5/5) | 440-444 | 15.000407 +/- 0.000023 s | 15.000460 +/- 0.000102 s | `stl` has the better timed objective |
| `whitaker3` | 127 | 252-254 | 352 (5/5) | 15.002046 +/- 0.000176 s | 15.000542 +/- 0.000074 s | `codex` has the better timed objective |

`codex` reaches the reference on 3/8 instances and `stl` on 4/8. On the three
instances where both reach it, `codex` is faster in every case: 17.72x on
`4elt`, 1.86x on `bcsstk29`, and 3.70x on `cti`. `stl` additionally solves
`bcsstk33` quickly.

For the five branch comparisons involving a 15-second cutoff, `stl` has the
better objective on `bcsstk33`, `crack`, and `fe_sphere`; `codex` is better on
`fe_4elt2` and `whitaker3`. The sum of the eight per-instance mean solver times
is 81.547243 seconds for `codex` and 80.673525 seconds for `stl`, but this total
is not a useful speed ranking because five comparisons are censored at the
same time limit and return different objective qualities. Mean end-to-end wall
totals are similarly close: 81.704676 and 81.464894 seconds.

## Determinism

| Check across five identical runs | `codex` | `stl` |
| --- | ---: | ---: |
| Instances with one objective value | 7/8 | 7/8 |
| Instances with one exact selected solution | 7/8 | 3/8 |
| Target-reaching instances with invariant exact solution | 3/3 | 1/4 |

`codex` reproduced the objective, selected variables, solution fingerprint,
target epoch/start, and logical frontier exactly on all three target-reaching
instances. Four of its five time-limited instances happened to retain the same
incumbent solution, although their completed logical-work counts differed as
expected. `whitaker3` exposed the documented wall-limit exception: it returned
three exact solutions and objectives 252 or 254 depending on how much work fit
before the deadline.

`stl` does not print a fingerprint, so determinism was checked using its full
selected-variable list. It produced different exact solutions on `4elt`,
`bcsstk29`, `crack`, `cti`, and `fe_sphere`. This includes three instances that
reached the target rather than timing out, demonstrating schedule-dependent
elite exchange independently of the wall-limit exception. `fe_sphere` also
varied in objective between 440 and 444.

Timing repeatability on target-reaching cases also favors `codex`. Its solver
time sample standard deviations were 0.000604, 0.003359, and 0.018773 seconds.
The corresponding `stl` deviations on shared target cases were 0.521594,
0.488739, and 2.746947 seconds; `cti` ranged from 3.476883 to 9.756910 seconds.

Changing from four to ten workers changes the deterministic `codex` search
configuration because it changes the lane set and epoch elite merge. Thus the
ten-worker `crack` result of 258 does not contradict the earlier four-worker
result of 184; each fixed worker-count configuration is a different seeded
search.

## Artifacts

- `gbp10_codex_gcc.csv` and `gbp10_codex_gcc_logs/`
- `gbp10_stl_gcc.csv` and `gbp10_stl_gcc_logs/`
- `benchmark_gbp.py` is the repeatable fresh-process harness.
