# Deterministic performance implementation report

## Scope and method

All final performance measurements used GCC 16.1, the C++20 Parallel STL/TBB
backend, ten workers, one logical lane per worker, seed 1, 230 total starts,
and no solver wall-clock limit. Each final result is from a fresh process. The
committed fixed-work baseline is `gbp10_fixed230_codex_gcc.csv`; the final
three-repeat result is `final_optimized_gcc.csv`.

The old baseline used schedule-dependent mid-kernel target cancellation. The
new solver completes every current start at a deterministic epoch barrier and
then discards results beyond the logical target cutoff. Consequently the table
below includes both the cost of that correctness fix and the subsequent
optimizations. Runtime is inherently physical and variable; objective,
selected variables, fingerprint, target epoch, start-to-best, and logical work
are the deterministic fields.

## Implemented steps

### 1. Phase profiling and residual determinism bug

An optional `FCBQP_ENABLE_PHASE_PROFILING` build records greedy construction,
diversity, tabu prefix selection, sparse move selection, path relinking, and
elite merge time plus deterministic operation counts. The GBP harness can now
select individual instances, record the profile, set the construction retry
budget, and select a fixed elite-group size.

Profiling exposed schedule-dependent executed work that the original output
frontier hid. Two identical `bcsstk33` runs reported the same final result and
logical frontier but executed 531,207 and 531,015 tabu iterations. Removing
mid-kernel target cancellation and finishing the epoch made both repetitions
execute exactly 555,575 tabu iterations and 51,640 path steps. The corrected
profiled runs took 21.167 and 21.402 seconds; this is the performance baseline
for optimizations that follow.

### 2. Exact scratch-storage and representation improvements

The solver now:

- reuses greedy, path, and `h` buffers;
- packs elite solutions into 64-bit words for Hamming distance;
- replaces repeated tabu marker clearing with overflow-safe generation stamps;
- reuses path candidate and move buffers.

These changes preserve the candidate order, RNG consumption, and accepted
trajectory. All accepted deterministic fields matched the prior baseline.

### 3. Greedy-construction bottleneck

On `fe_sphere`, the original profile attributed 689.996 aggregate CPU seconds
to greedy construction: 14,086 attempts and 38,088,544 additions. Hamming
diversity itself used only 0.024 seconds, so optimizing distance calculation
alone could not solve this case.

The accepted change maintains a sorted vector of variables still available to
the construction. Each addition scans only that shrinking set, in the exact
same order as before, instead of all 16,386 variables. The result and all work
counters remain identical. Profiled runtime fell from 81.769 to 66.192 seconds
(1.24x); aggregate greedy CPU time fell to 534.175 seconds.

Rejected variants were retained only in the experimental CSV evidence, not as
the default algorithm:

| Variant | `fe_sphere` objective | Runtime |
|---|---:|---:|
| retry cap 128 | 448 | 99.916 s |
| retry cap 64 | 506 | 83.956 s |
| retry cap 32 | 506 | 61.905 s |
| index-linked free list | 386 | 90.714 s |
| heap top-20 construction | 446 | 83.750 s |

The quality-preserving retry default therefore remains 200. It is exposed as
`FCBQP_CONSTRUCTION_MAX_ATTEMPTS` for deterministic experiments.

### 4. Exact sparse path relinking

For a sparse matrix, a non-edge move delta separates into one outgoing and one
incoming term. The exact global best ten moves can therefore be found by
examining every interacting pair and only the ten best non-edges for each
outgoing candidate. Any later non-edge already has ten better moves from the
same row and cannot enter the global best ten.

This replaces the full candidate cross product without changing the selected
top ten or the RNG mapping. On `bcsstk33`, path CPU time fell from 35.898 to
3.346 aggregate seconds and profiled runtime fell from 21.167 to 4.832 seconds
(4.38x). Objective, fingerprint, selected variables, target epoch,
start-to-best, logical work, 555,575 tabu iterations, and 51,640 path steps all
matched.

### 5. Build-level opportunities

The Makefile now trains GCC PGO with dense, general sparse, path-heavy GBP, and
construction-heavy GBP workloads, and has a dedicated PGO GBP benchmark
target. Representative PGO was slower than the existing Release + LTO + native
build:

| Instance | LTO/native | PGO + LTO/native | PGO change |
|---|---:|---:|---:|
| `bcsstk33` | 4.837 s | 5.206 s | 7.6% slower |
| `fe_sphere` | 66.545 s | 69.357 s | 4.2% slower |

LTO/native remains the recommended build. Keeping the PGO workflow makes this
conclusion reproducible and permits retraining after future kernel changes.

### 6. Deterministic elite portfolios

`FCBQP_ELITE_GROUP_SIZE` partitions logical lanes into fixed independent elite
groups. Lanes still exchange only at deterministic epoch barriers and merge in
lane-ID order. Group size four produces the planned `4+4+2` topology with ten
lanes.

It closes the previous quality gap on the difficult pair and outperforms the
recorded `stl` branch at the same fixed-work budget:

| Instance | `stl` objective / mean | deterministic `4+4+2` objective / mean |
|---|---:|---:|
| `crack` | 184 / 19.852 s | 184 / 9.010 s |
| `whitaker3` | 128 / 41.122 s | 128 / 16.858 s |

Three repetitions of the portfolio have identical values for all ten checked
invariant fields. The portfolio is deliberately opt-in because applying it to
all GBP instances regressed `cti` to 365, `fe_sphere` to 430, and made
`fe_4elt2` much slower. `make benchmark-gbp-portfolio` reproduces the useful
two-instance configuration.

## Final default GBP comparison

The default remains one global elite pool because it has the strongest result
over the complete suite. Times are mean solver seconds over three runs; the
new standard deviation is included to expose physical timing variability.

| Instance | Objective (old/new) | Old mean | New mean +/- sd | Speedup |
|---|---:|---:|---:|---:|
| `4elt` | 139 / 139 | 0.170 | 0.463 +/- 0.002 | 0.37x |
| `bcsstk29` | 2843 / 2843 | 4.078 | 4.169 +/- 0.074 | 0.98x |
| `bcsstk33` | 10171 / 10171 | 21.194 | 4.936 +/- 0.075 | 4.29x |
| `crack` | 186 / 186 | 18.809 | 11.837 +/- 0.069 | 1.59x |
| `cti` | 334 / 334 | 2.218 | 2.621 +/- 0.064 | 0.85x |
| `fe_4elt2` | 130 / 130 | 16.886 | 11.380 +/- 0.040 | 1.48x |
| `fe_sphere` | 386 / 386 | 91.390 | 71.818 +/- 6.754 | 1.27x |
| `whitaker3` | 252 / 252 | 15.207 | 13.634 +/- 0.231 | 1.12x |

The sum of instance means fell from 169.952 to 120.859 seconds: 1.41x overall,
or 28.9% less time. The geometric-mean speedup is 1.20x. The `4elt`, `cti`, and
small `bcsstk29` regressions include the cost of completing a deterministic
epoch instead of schedule-dependent early cancellation.

## Validation

- GCC 16.1/TBB Release + LTO + native build: passed.
- LLVM 22/libdispatch Release + LTO + native build: passed.
- GCC and LLVM smoke tests: passed.
- Cross-compiler fixed-work comparison: 10 cases, 22 invariant fields each,
  all identical.
- Default GBP benchmark: 8 instances, 3 repetitions, 10 invariant fields,
  all identical.
- `4+4+2` portfolio: 2 instances, 3 repetitions, 10 invariant fields, all
  identical.
- `git diff --check`: passed.

The only intentional determinism exception remains a positive wall-clock
limit; machine speed then controls how many complete epochs fit before the
deadline. Runtime and TTS measurements naturally vary even in fixed-work mode.
