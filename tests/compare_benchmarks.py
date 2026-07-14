#!/usr/bin/env python3
"""Compare all non-timing fields from two deterministic benchmark CSVs."""

from __future__ import annotations

import csv
import sys
from pathlib import Path


INVARIANT_FIELDS = (
    "instance",
    "path",
    "sense",
    "scale",
    "base_seed",
    "runs",
    "workers",
    "epoch_lanes_per_worker",
    "epoch_lanes",
    "starts",
    "solver_time_limit",
    "status",
    "returncode",
    "storage",
    "best_known",
    "best_found",
    "reached_best_known",
    "target_epoch",
    "start_to_best",
    "solution_fingerprint",
    "selected_vars",
)


def read_rows(path: Path) -> list[dict[str, str]]:
    with path.open(newline="") as handle:
        return list(csv.DictReader(handle))


def main() -> int:
    left_path = Path(sys.argv[1])
    right_path = Path(sys.argv[2])
    left = read_rows(left_path)
    right = read_rows(right_path)
    if len(left) != len(right):
        raise AssertionError(f"row count differs: {len(left)} != {len(right)}")

    for position, (lhs, rhs) in enumerate(zip(left, right), 1):
        differences = {
            field: (lhs.get(field, "<missing>"), rhs.get(field, "<missing>"))
            for field in INVARIANT_FIELDS
            if lhs.get(field) != rhs.get(field)
        }
        if differences:
            raise AssertionError(f"row {position} differs: {differences}")

    print(
        f"Determinism check passed: {len(left)} cases, "
        f"{len(INVARIANT_FIELDS)} invariant fields each."
    )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
