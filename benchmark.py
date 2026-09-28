#!/usr/bin/env python3

from __future__ import annotations

import argparse
import csv
import os
import re
import subprocess
import time
from pathlib import Path


CASES = [
    ("mdp-gkd-c-500", "max", "half", "dataset/MDP/GKD-c/GKD-c_1_n500_m50.txt"),
    ("mdp-mdg-b-500", "max", "half", "dataset/MDP/MDG-b_n500/MDG-b_1_n500_m50.txt"),
    ("mdp-som-b-500", "max", "half", "dataset/MDP/SOM-b/SOM-b_17_n500_m50.txt"),
    ("mdp-mdg-a-2000", "max", "half", "dataset/MDP/MDG-a_n2000/MDG-a_21_n2000_m200.txt"),
    ("mcp-c125-9", "max", "full", "dataset/MCP/DIMACS/C125.9_m34.qsc"),
    ("mcp-hamming8-4", "max", "full", "dataset/MCP/DIMACS/hamming8-4_m16.qsc"),
    ("mcp-jazz", "max", "full", "dataset/MCP/set_1/jazz_n198_m30.qsc"),
    ("mcp-email", "max", "full", "dataset/MCP/set_1/email_n1133_m12.qsc"),
    ("mcp-ca-grqc", "max", "full", "dataset/MCP/set_1/ca-GrQc_n5241_m44.qsc"),
    ("gbp-crack", "min", "full", "dataset/GBP/crack.graph.qsc"),
]


def capture(pattern: str, text: str, default: str = "") -> str:
    match = re.search(pattern, text, re.MULTILINE)
    return match.group(1) if match else default


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("phase", help="output name, e.g. baseline or optimized")
    parser.add_argument("--executable", default="build/main")
    parser.add_argument("--threads", type=int, default=4)
    parser.add_argument("--starts", type=int, default=200000,
                        help="deterministic total start budget")
    parser.add_argument("--epoch-lanes-per-worker", type=int, default=1,
                        help="deterministic overdecomposition factor")
    parser.add_argument("--solver-time-limit", type=float, default=90.0)
    parser.add_argument("--timeout", type=float, default=105.0,
                        help="external per-process safety timeout")
    parser.add_argument(
        "--legacy-interface",
        action="store_true",
        help="omit the extended index-base, worker, and start arguments",
    )
    args = parser.parse_args()
    if args.threads <= 0:
        parser.error("--threads must be positive")
    if args.starts <= 0:
        parser.error("--starts must be positive")
    if args.epoch_lanes_per_worker <= 0:
        parser.error("--epoch-lanes-per-worker must be positive")
    if args.solver_time_limit < 0:
        parser.error("--solver-time-limit must be nonnegative")

    root = Path(__file__).resolve().parent
    out_dir = root / "benchmark_results"
    log_dir = out_dir / f"{args.phase}_logs"
    log_dir.mkdir(parents=True, exist_ok=True)
    output_csv = out_dir / f"{args.phase}.csv"
    executable = (root / args.executable).resolve()
    best_known = root / "best_known_results.csv"
    env = os.environ.copy()
    env.pop("OMP_NUM_THREADS", None)
    env["FCBQP_NUM_THREADS"] = str(args.threads)
    env["FCBQP_EPOCH_LANES_PER_WORKER"] = str(args.epoch_lanes_per_worker)

    rows: list[dict[str, str]] = []
    for position, (name, sense, scale, relative_instance) in enumerate(CASES, 1):
        command = [
            str(executable), sense, scale, str(root / relative_instance),
            str(best_known), "1", "1", str(args.solver_time_limit),
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
                env=env,
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
        except subprocess.TimeoutExpired as exc:
            status = "timeout"
            stdout = exc.stdout or ""
            stderr = exc.stderr or ""
            if isinstance(stdout, bytes):
                stdout = stdout.decode(errors="replace")
            if isinstance(stderr, bytes):
                stderr = stderr.decode(errors="replace")

        wall_seconds = time.perf_counter() - started
        parts = [part.rstrip() for part in (stderr, stdout) if part.strip()]
        combined = "\n".join(parts) + "\n"
        (log_dir / f"{position:02d}_{name}.log").write_text(combined)

        row = {
            "instance": name,
            "path": relative_instance,
            "sense": sense,
            "scale": scale,
            "base_seed": "1",
            "runs": "1",
            "workers": str(args.threads),
            "epoch_lanes_per_worker": str(args.epoch_lanes_per_worker),
            "epoch_lanes": capture(r"Epoch lanes: ([^\n]+)", stdout),
            "starts": str(args.starts),
            "solver_time_limit": str(args.solver_time_limit),
            "status": status,
            "returncode": returncode,
            "storage": capture(r"storage = ([^,\n]+)", combined),
            "parallel_backend": capture(r"Parallel backend: ([^\n]+)", stdout),
            "best_known": capture(r"Best-known objective = ([^\n]+)", stdout),
            "best_found": capture(r"Run 1/1: best objective = ([^,\n]+)", stdout),
            "reached_best_known": capture(r"reached best-known = (YES|NO)", stdout),
            "tts_seconds": capture(r"TTS to best-known \(s\) = ([^,\n]+)", stdout),
            "target_epoch": capture(r"target epoch = ([^,\n]+)", stdout),
            "start_to_best": capture(r"start to run-best = ([^,\n]+)", stdout),
            "logical_work": capture(
                r"Logical work: deterministic frontier = ([^\n]+)", stdout
            ),
            "solution_fingerprint": capture(r"Solution fingerprint = ([^\n]+)", stdout),
            "selected_vars": capture(r"Selected vars of best run: ?([^\n]*)", stdout),
            "solver_runtime_seconds": capture(r"runtime \(s\) = ([^\n]+)", stdout),
            "wall_seconds": f"{wall_seconds:.6f}",
        }
        rows.append(row)
        print(
            f"[{position:02d}/{len(CASES)}] {name}: {status}, "
            f"best={row['best_found'] or 'n/a'}, "
            f"target={row['reached_best_known'] or 'n/a'}, wall={wall_seconds:.3f}s",
            flush=True,
        )

    with output_csv.open("w", newline="") as handle:
        writer = csv.DictWriter(handle, fieldnames=list(rows[0]), lineterminator="\n")
        writer.writeheader()
        writer.writerows(rows)
    print(f"Wrote {output_csv}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
