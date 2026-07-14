#!/usr/bin/env python3
"""Small integration checks for parser and trivial-cardinality behavior."""

from __future__ import annotations

import re
import subprocess
import sys
import tempfile
from pathlib import Path


def run(
    executable: Path,
    data: str,
    *extra: str,
    time_limit: str = "5",
) -> subprocess.CompletedProcess[str]:
    with tempfile.NamedTemporaryFile("w", suffix=".qsc", delete=False) as handle:
        handle.write(data)
        instance = Path(handle.name)
    try:
        return subprocess.run(
            [str(executable), "min", "full", str(instance), "", "1", "1", time_limit, *extra],
            text=True,
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
            check=False,
        )
    finally:
        instance.unlink(missing_ok=True)


def objective(result: subprocess.CompletedProcess[str]) -> float:
    match = re.search(r"Best found obj = ([^\n]+)", result.stdout)
    if result.returncode != 0 or not match:
        raise AssertionError(f"solver failed:\n{result.stderr}\n{result.stdout}")
    return float(match.group(1))


def deterministic_fields(result: subprocess.CompletedProcess[str]) -> tuple[str, ...]:
    if result.returncode != 0:
        raise AssertionError(f"solver failed:\n{result.stderr}\n{result.stdout}")
    patterns = (
        r"Best found obj = ([^\n]+)",
        r"Solution fingerprint = ([^\n]+)",
        r"target epoch = ([^,\n]+)",
        r"start to run-best = ([^,\n]+)",
    )
    fields = tuple(re.search(pattern, result.stdout).group(1) for pattern in patterns)
    return fields


def main() -> int:
    executable = Path(sys.argv[1]).resolve()

    triangular = """3 3
0 0 1
0 1 2
0 2 3
1 1 4
1 2 5
2 2 6
"""
    symmetric = """3 3
0 0 1
0 1 2
1 0 2
0 2 3
2 0 3
1 1 4
1 2 5
2 1 5
2 2 6
"""
    assert objective(run(executable, triangular, "0")) == 31.0
    assert objective(run(executable, symmetric, "0")) == 31.0
    two_workers = run(executable, triangular, "0", "2")
    assert two_workers.returncode == 0
    assert "Workers: 2" in two_workers.stdout

    assert objective(run(executable, "3 0\n0 0 7\n", "0")) == 0.0
    assert objective(run(executable, "3 1\n0 0 4\n1 1 -2\n2 2 3\n", "0")) == -2.0

    asymmetric = run(executable, "2 2\n0 1 2\n1 0 3\n", "0")
    assert asymmetric.returncode != 0
    assert "not symmetric" in asymmetric.stderr

    ambiguous = "4 4\n1 1 1\n2 2 2\n3 3 3\n"
    rejected = run(executable, ambiguous)
    assert rejected.returncode != 0
    assert "ambiguous" in rejected.stderr
    assert run(executable, ambiguous, "1").returncode == 0

    nontrivial = """6 3
0 0 6
1 1 5
2 2 4
3 3 3
4 4 2
5 5 1
"""
    first = run(executable, nontrivial, "0", "2", "4", time_limit="0")
    second = run(executable, nontrivial, "0", "2", "4", time_limit="0")
    assert deterministic_fields(first) == deterministic_fields(second)

    sparse_lines = ["1000 2", "0 1 -10"]
    sparse_lines.extend(f"{i} {i} {i}" for i in range(1000))
    sparse = run(
        executable,
        "\n".join(sparse_lines) + "\n",
        "0", "2", "4",
        time_limit="0",
    )
    assert "storage = CSR sparse" in sparse.stderr
    assert objective(sparse) == -19.0
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
