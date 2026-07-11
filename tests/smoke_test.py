#!/usr/bin/env python3
"""Small integration checks for parser and trivial-cardinality behavior."""

from __future__ import annotations

import re
import subprocess
import sys
import tempfile
from pathlib import Path


def run(executable: Path, data: str, *extra: str) -> subprocess.CompletedProcess[str]:
    with tempfile.NamedTemporaryFile("w", suffix=".qsc", delete=False) as handle:
        handle.write(data)
        instance = Path(handle.name)
    try:
        return subprocess.run(
            [str(executable), "min", "full", str(instance), "", "1", "1", "5", *extra],
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
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
