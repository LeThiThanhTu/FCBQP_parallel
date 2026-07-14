# Bottleneck optimization report

## Scope

The optimized implementation retains C++20 Parallel STL and the deterministic
epoch model, and builds with both LLVM 22.1.8/libdispatch and GCC 16.1/TBB.
Measurements use four physical workers, base seed 1, one run, a fixed four-start
budget, and no wall-clock limit unless stated otherwise.

The before data is in `deterministic_llvm.csv` and `deterministic_gcc.csv`.
The final portable Release data is in `optimization_after_llvm.csv` and
`optimization_after_gcc.csv`. LTO/native and PGO results are in the matching
`optimization_fast_*` and `optimization_pgo_*` files.

## Implemented changes

- The matrix parser uses direct integer/floating parsing instead of constructing
  a stream for every entry. Already ordered coordinates avoid a redundant sort.
- Dense matrices are filled directly from canonical symmetric entries. CSR is
  built by row counting and direct symmetric insertion, eliminating the global
  expanded-entry vector and sort.
- Matrix diagonals are cached in `Instance`.
- Tabu candidate selection uses a total-order `nth_element` plus a sorted prefix.
- Dense and sparse tabu move evaluation use separate kernels. Dense row updates
  use the unsequenced execution policy for SIMD-friendly contiguous transforms.
- The sparse kernel does not scan the full candidate Cartesian product. It ranks
  zero-interaction pairs using separable terms and evaluates only explicit CSR
  interactions separately, while retaining tabu, aspiration, fallback, and
  deterministic tie rules.
- Per-lane tabu buffers, PSTL dispatch tickets, elite construction candidates,
  and construction state are reused. Construction returns `x`, `h`, and its
  objective so tabu search does not rebuild the gradient.
- Path relinking keeps only its deterministic best ten moves instead of
  materializing and partially sorting every move.
- Epochs are overdecomposed into deterministic logical lanes. Four physical
  workers dynamically claim lanes, and completed candidates are merged in lane
  ID order. The lane factor is explicit in benchmark CSVs.
- CMake and Make support portable Release, LTO/native, and complete PGO
  generate/train/use workflows for both compilers.

## Fixed-work improvement

| Build | Before solver sum (s) | After solver sum (s) | Solver speedup | Before wall sum (s) | After wall sum (s) | Wall speedup |
| --- | ---: | ---: | ---: | ---: | ---: | ---: |
| LLVM/libdispatch | 2.833811 | 0.821021 | 3.45x | 3.632707 | 0.975575 | 3.72x |
| GCC/TBB | 1.175330 | 0.939677 | 1.25x | 2.565740 | 1.165824 | 2.20x |

Representative cases:

| Build / instance | Before solver (s) | After solver (s) | Speedup | Before wall (s) | After wall (s) | Speedup |
| --- | ---: | ---: | ---: | ---: | ---: | ---: |
| LLVM / MDG-a n=2000 | 0.471428 | 0.291069 | 1.62x | 1.098242 | 0.395164 | 2.78x |
| LLVM / `crack` | 2.125847 | 0.398615 | 5.33x | 2.142446 | 0.404328 | 5.30x |
| GCC / MDG-a n=2000 | 0.293603 | 0.303701 | 0.97x | 1.339055 | 0.405243 | 3.30x |
| GCC / `crack` | 0.742260 | 0.476966 | 1.56x | 0.774340 | 0.491295 | 1.58x |

The small GCC dense-kernel regression is outweighed by the parser improvement
in end-to-end time. LLVM benefits more from the new move kernels and is now the
fastest complete build on this machine.

## Build variants after optimization

| Build | Solver sum (s) | Harness wall sum (s) |
| --- | ---: | ---: |
| LLVM portable Release | 0.821021 | 0.975575 |
| LLVM LTO/native | 0.818826 | 1.107081 |
| LLVM PGO + LTO/native | 0.800144 | 0.945188 |
| GCC portable Release | 0.939677 | 1.165824 |
| GCC LTO/native | 0.945992 | 1.321858 |
| GCC PGO + LTO/native | 0.919889 | 1.270213 |

PGO reduces summed solver time modestly for both compilers on this training
set. LTO/native alone is approximately neutral. The very short cases make
whole-process wall sums sensitive to cold runtime startup, especially for TBB,
so solver sums are the useful build-flag comparison. Native and PGO builds
remain optional because they are machine- and training-workload-specific.

## Deterministic overdecomposition

Longer fixed-work checks compare one logical lane per worker with the default
four lanes per worker:

| Instance / starts | Lane factor 1 | Lane factor 4 | Runtime improvement | Result change |
| --- | ---: | ---: | ---: | --- |
| MDG-a n=2000 / 80 | 4.771504 s | 4.066666 s | 1.17x | 114241 to 114261 (higher is better) |
| `crack` / 32 | 4.767828 s | 3.557366 s | 1.34x | 1842 to 1773 (lower is better) |

Changing the lane factor intentionally changes seeded search streams. For a
fixed factor it remains deterministic: LLVM, GCC, LTO/native, and PGO builds
matched all 21 invariant CSV fields on all ten cases. With lane factor 1, the
optimized multi-epoch runs reproduced the previous objectives and fingerprints
exactly.

## Verification

- `make test-compilers` passes, including triangular/symmetric parsing,
  cardinality edge cases, deterministic repeated search, and a nontrivial CSR
  sparse move test.
- The LLVM and GCC ten-instance fixed-work outputs match in every invariant
  benchmark field.
- Portable, LTO/native, and PGO builds produce the same fixed-work solution
  fingerprints.
- AddressSanitizer and UndefinedBehaviorSanitizer pass the smoke suite with
  leak detection disabled; libc++'s libdispatch PSTL backend leaves its small
  process-lifetime dispatch allocations visible to LeakSanitizer.
- A positive wall-clock limit remains the documented determinism exception.
