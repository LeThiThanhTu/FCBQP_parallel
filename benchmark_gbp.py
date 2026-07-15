#!/usr/bin/env python3
"""Benchmark every GBP instance with repeatable fresh-process runs."""

from __future__ import annotations

import argparse
import csv
import os
import re
import subprocess
import time
from pathlib import Path


def capture(pattern: str, text: str, default: str = "") -> str:
    match = re.search(pattern, text, re.MULTILINE)
    return match.group(1) if match else default


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("phase", help="output basename under benchmark_results")
    parser.add_argument("--executable", required=True)
    parser.add_argument("--threads", type=int, default=10)
    parser.add_argument("--repetitions", type=int, default=5)
    parser.add_argument(
        "--starts",
        type=int,
        default=230,
        help="deterministic total start budget (default: 23 per 10 workers)",
    )
    parser.add_argument("--epoch-lanes-per-worker", type=int, default=1)
    parser.add_argument(
        "--solver-time-limit",
        type=float,
        default=0.0,
        help="internal wall limit; zero keeps the benchmark fixed-work",
    )
    parser.add_argument(
        "--timeout",
        type=float,
        default=300.0,
        help="external failure guard only; not a benchmark stopping criterion",
    )
    parser.add_argument(
        "--legacy-interface",
        action="store_true",
        help="omit the codex-only index-base, worker, and start arguments",
    )
    args = parser.parse_args()
    if args.threads <= 0 or args.repetitions <= 0 or args.starts <= 0:
        parser.error("threads, repetitions, and starts must be positive")
    if args.epoch_lanes_per_worker <= 0:
        parser.error("epoch-lanes-per-worker must be positive")
    if args.solver_time_limit < 0 or args.timeout <= 0:
        parser.error("solver-time-limit must be nonnegative and timeout positive")

    root = Path(__file__).resolve().parent
    executable = Path(args.executable).expanduser().resolve()
    if not executable.is_file():
        parser.error(f"executable not found: {executable}")
    best_known = root / "best_known_results.csv"
    instances = sorted((root / "dataset" / "GBP").glob("*.qsc"))
    if not instances:
        parser.error("no dataset/GBP/*.qsc instances found")

    result_dir = root / "benchmark_results"
    log_dir = result_dir / f"{args.phase}_logs"
    log_dir.mkdir(parents=True, exist_ok=True)
    output_csv = result_dir / f"{args.phase}.csv"

    environment = os.environ.copy()
    environment.pop("OMP_NUM_THREADS", None)
    environment["FCBQP_NUM_THREADS"] = str(args.threads)
    environment["FCBQP_EPOCH_LANES_PER_WORKER"] = str(
        args.epoch_lanes_per_worker
    )

    rows: list[dict[str, str]] = []
    total = len(instances) * args.repetitions
    position = 0
    for instance in instances:
        for repetition in range(1, args.repetitions + 1):
            position += 1
            command = [
                str(executable),
                "min",
                "full",
                str(instance),
                str(best_known),
                "1",
                "1",
                str(args.solver_time_limit),
            ]
            if not args.legacy_interface:
                command.extend(["auto", str(args.threads), str(args.starts)])

            started = time.perf_counter()
            status = "completed"
            stdout = ""
            stderr = ""
            returncode = ""
            try:
                result = subprocess.run(
                    command,
                    cwd=root,
                    env=environment,
                    text=True,
                    stdout=subprocess.PIPE,
                    stderr=subprocess.PIPE,
                    timeout=args.timeout,
                    check=False,
                )
                stdout = result.stdout
                stderr = result.stderr
                returncode = str(result.returncode)
                if result.returncode != 0:
                    status = "failed"
            except subprocess.TimeoutExpired as error:
                status = "timeout"
                stdout = error.stdout or ""
                stderr = error.stderr or ""
                if isinstance(stdout, bytes):
                    stdout = stdout.decode(errors="replace")
                if isinstance(stderr, bytes):
                    stderr = stderr.decode(errors="replace")

            wall_seconds = time.perf_counter() - started
            combined = "\n".join(
                part.rstrip() for part in (stderr, stdout) if part.strip()
            ) + "\n"
            log_name = f"{instance.stem}_r{repetition}.log"
            (log_dir / log_name).write_text(combined)

            row = {
                "instance": instance.stem,
                "repetition": str(repetition),
                "base_seed": "1",
                "workers": str(args.threads),
                "epoch_lanes_per_worker": str(args.epoch_lanes_per_worker),
                "starts": str(args.starts),
                "solver_time_limit": str(args.solver_time_limit),
                "status": status,
                "returncode": returncode,
                "storage": capture(r"storage = ([^,\n]+)", combined),
                "parallel_backend": capture(r"Parallel backend: ([^\n]+)", stdout),
                "best_known": capture(r"Best-known objective = ([^\n]+)", stdout),
                "best_found": capture(r"Run 1/1: best objective = ([^,\n]+)", stdout),
                "reached_best_known": capture(
                    r"reached best-known = (YES|NO)", stdout
                ),
                "tts_seconds": capture(
                    r"TTS to best-known \(s\) = ([^,\n]+)", stdout
                ),
                "target_epoch": capture(r"target epoch = ([^,\n]+)", stdout),
                "start_to_best": capture(r"start to run-best = ([^,\n]+)", stdout),
                "logical_work": capture(
                    r"Logical work: deterministic frontier = ([^\n]+)", stdout
                ),
                "solution_fingerprint": capture(
                    r"Solution fingerprint = ([^\n]+)", stdout
                ),
                "selected_vars": capture(
                    r"Selected vars of best run: ?([^\n]*)", stdout
                ),
                "solver_runtime_seconds": capture(r"runtime \(s\) = ([^\n]+)", stdout),
                "wall_seconds": f"{wall_seconds:.6f}",
            }
            rows.append(row)
            print(
                f"[{position:03d}/{total}] {instance.stem} r{repetition}: "
                f"{status}, best={row['best_found'] or 'n/a'}, "
                f"target={row['reached_best_known'] or 'n/a'}, "
                f"solver={row['solver_runtime_seconds'] or 'n/a'}s, "
                f"wall={wall_seconds:.3f}s",
                flush=True,
            )

    with output_csv.open("w", newline="") as output:
        writer = csv.DictWriter(output, fieldnames=list(rows[0]), lineterminator="\n")
        writer.writeheader()
        writer.writerows(rows)
    print(f"Wrote {output_csv}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
