#!/usr/bin/env python3

from __future__ import annotations

import re
import os
import subprocess
import sys
import tempfile
from pathlib import Path


def run(
    executable: Path,
    data: str,
    *extra: str,
    time_limit: str = "5",
    best_known: str | None = None,
    extra_env: dict[str, str] | None = None,
) -> subprocess.CompletedProcess[str]:
    with tempfile.NamedTemporaryFile("w", suffix=".qsc", delete=False) as handle:
        handle.write(data)
        instance = Path(handle.name)
    best_known_file: Path | None = None
    try:
        best_known_arg = ""
        if best_known is not None:
            with tempfile.NamedTemporaryFile("w", suffix=".csv", delete=False) as handle:
                handle.write("instance,best_known_obj\n")
                handle.write(f"{instance.stem},{best_known}\n")
                best_known_file = Path(handle.name)
            best_known_arg = str(best_known_file)
        environment = os.environ.copy()
        if extra_env:
            environment.update(extra_env)
        return subprocess.run(
            [str(executable), "min", "full", str(instance), best_known_arg, "1", "1", time_limit, *extra],
            text=True,
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
            env=environment,
            check=False,
        )
    finally:
        instance.unlink(missing_ok=True)
        if best_known_file is not None:
            best_known_file.unlink(missing_ok=True)


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
        r"Logical work: ([^\n]+)",
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

    target_baseline = run(
        executable, nontrivial, "0", "2", "4", time_limit="0", best_known="6"
    )
    for delayed_lane in ("0", "1"):
        delayed = run(
            executable,
            nontrivial,
            "0", "2", "4",
            time_limit="0",
            best_known="6",
            extra_env={
                "FCBQP_TEST_DELAY_LANE": delayed_lane,
                "FCBQP_TEST_DELAY_US": "20000",
            },
        )
        assert deterministic_fields(delayed) == deterministic_fields(target_baseline), (
            target_baseline.stdout,
            delayed.stdout,
        )

    grouped_baseline = run(
        executable,
        nontrivial,
        "0", "2", "4",
        time_limit="0",
        best_known="6",
        extra_env={"FCBQP_ELITE_GROUP_SIZE": "1"},
    )
    for delayed_lane in ("0", "1"):
        delayed = run(
            executable,
            nontrivial,
            "0", "2", "4",
            time_limit="0",
            best_known="6",
            extra_env={
                "FCBQP_ELITE_GROUP_SIZE": "1",
                "FCBQP_TEST_DELAY_LANE": delayed_lane,
                "FCBQP_TEST_DELAY_US": "20000",
            },
        )
        assert deterministic_fields(delayed) == deterministic_fields(grouped_baseline), (
            grouped_baseline.stdout,
            delayed.stdout,
        )

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
