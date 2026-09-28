#!/usr/bin/env python3

from __future__ import annotations

import math
import re
import subprocess
import sys
import tempfile
from pathlib import Path


def require(condition: bool, message: str) -> None:
    if not condition:
        raise AssertionError(message)


def matrix_fixture(n: int, sparse: bool) -> list[list[int]]:
    q = [[0] * n for _ in range(n)]
    if sparse:
        for i, j, value in ((0, 63, -7), (2, 7, 4), (19, 20, -3)):
            q[i][j] = q[j][i] = value
        q[3][3] = 5
    else:
        for i in range(n):
            q[i][i] = ((3 * i + 1) % 9) - 4
            for j in range(i + 1, n):
                q[i][j] = q[j][i] = ((i * 7 + j * 11) % 11) - 5
    return q


def objective(q: list[list[int]], x: list[int]) -> int:
    return sum(v * x[i] * x[j] for i, row in enumerate(q) for j, v in enumerate(row))


def serialize(q: list[list[int]], k: int) -> str:
    n = len(q)
    lines = [f"{n} {k}"]
    for i in range(n):
        for j in range(i, n):
            if q[i][j]:
                lines.append(f"{i} {j} {q[i][j]}")
    return "\n".join(lines) + "\n"


def check(executable: Path, q: list[list[int]], k: int,
          sense: str, scale: str, storage: str) -> None:
    n = len(q)
    with tempfile.TemporaryDirectory(prefix="fcbqp-complement-") as tmp:
        path = Path(tmp) / "complement.qsc"
        path.write_text(serialize(q, k), encoding="ascii")
        # No best-known target and no wall-clock deadline: fixed-work search.
        command = [str(executable), sense, scale, str(path), "", "1", "1", "0", "0", "1", "2"]
        run = subprocess.run(command, text=True, capture_output=True, check=False, timeout=90)
    context = f"n={n}, k={k}, {sense}/{scale}, {storage}"
    require(run.returncode == 0, f"{context}: solver failed:\n{run.stderr}\n{run.stdout}")
    require(f"storage = {storage}" in run.stderr,
            f"{context}: unexpected storage:\n{run.stderr}")
    require("Wrong obj value !!" not in run.stdout,
            f"{context}: solver flagged an inconsistent objective")
    found = re.search(r"^Best found obj = ([^\n]+)", run.stdout, re.MULTILINE)
    selected = re.search(r"^Selected vars of best run:([^\n]*)", run.stdout, re.MULTILINE)
    require(found is not None and selected is not None,
            f"{context}: missing objective or selected variables:\n{run.stdout}")
    indices = [int(s) for s in selected.group(1).split()]
    require(len(indices) == k, f"{context}: reported {len(indices)} selected instead of {k}")
    require(len(set(indices)) == k and all(1 <= i <= n for i in indices),
            f"{context}: invalid original-orientation indices: {indices}")
    x = [0] * n
    for one_based in indices:
        x[one_based - 1] = 1
    factor = 1.0 if scale == "full" else 0.5
    expected = factor * objective(q, x)
    reported = float(found.group(1))
    require(math.isclose(reported, expected, rel_tol=0.0, abs_tol=1e-7),
            f"{context}: independently evaluated {expected}, solver reported {reported}")
    print(f"PASS {context}: objective={reported}")


def main() -> int:
    if len(sys.argv) != 2:
        raise SystemExit("Usage: complement_test.py /path/to/main")
    executable = Path(sys.argv[1]).resolve()
    require(executable.is_file(), f"Solver executable not found: {executable}")
    for sense in ("min", "max"):
        for scale in ("full", "half"):
            check(executable, matrix_fixture(7, False), 5, sense, scale, "dense matrix")
            check(executable, matrix_fixture(64, True), 61, sense, scale, "CSR sparse")
    # Complement from n-1 to 1 and from n to 0: trivial-solution paths.
    check(executable, matrix_fixture(7, False), 6, "min", "full", "dense matrix")
    check(executable, matrix_fixture(7, False), 7, "max", "full", "dense matrix")
    print("PASS: 10 end-to-end complement cases")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
