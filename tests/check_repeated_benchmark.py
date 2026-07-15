#!/usr/bin/env python3
"""Verify that repeated fixed-work benchmark result fields are identical."""

from __future__ import annotations

import csv
import sys
from collections import defaultdict
from pathlib import Path


INVARIANT_FIELDS = (
    "status",
    "returncode",
    "best_known",
    "best_found",
    "reached_best_known",
    "target_epoch",
    "start_to_best",
    "logical_work",
    "solution_fingerprint",
    "selected_vars",
)


def main() -> int:
    if len(sys.argv) != 2:
        print(f"Usage: {Path(sys.argv[0]).name} <repeated_benchmark.csv>", file=sys.stderr)
        return 2

    path = Path(sys.argv[1])
    with path.open(newline="") as source:
        rows = list(csv.DictReader(source))
    if not rows:
        print(f"No benchmark rows in {path}", file=sys.stderr)
        return 2

    missing = [field for field in INVARIANT_FIELDS if field not in rows[0]]
    if missing:
        print(f"Missing invariant CSV fields: {', '.join(missing)}", file=sys.stderr)
        return 2

    grouped: dict[str, list[dict[str, str]]] = defaultdict(list)
    for row in rows:
        grouped[row["instance"]].append(row)

    differences: list[str] = []
    for instance, repetitions in sorted(grouped.items()):
        for field in INVARIANT_FIELDS:
            values = {row[field] for row in repetitions}
            if len(values) != 1:
                differences.append(
                    f"{instance}: {field} has {len(values)} distinct values"
                )

    if differences:
        print("Repeated-result determinism check failed:", file=sys.stderr)
        for difference in differences:
            print(f"  {difference}", file=sys.stderr)
        return 1

    repetition_counts = {len(repetitions) for repetitions in grouped.values()}
    print(
        "Repeated-result determinism check passed: "
        f"{len(grouped)} instances, repetition counts {sorted(repetition_counts)}, "
        f"{len(INVARIANT_FIELDS)} invariant fields."
    )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
