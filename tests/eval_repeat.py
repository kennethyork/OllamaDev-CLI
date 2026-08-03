#!/usr/bin/env python3
"""Run eval tasks repeatedly and report a per-task pass rate.

`eval` gives one pass/fail per task per run, and at least one task in the hard
suite sits on the capability boundary — measured at 2 passes in 5 runs. A single
headline percentage is therefore noisy to about a task, and reading a one-task
gap between two backends as a ranking is exactly the mistake that invites.

This runs each task N times and prints the rate, so a flaky task is visible as
flaky instead of averaging into the total as if it were a verdict.

    python3 tests/eval_repeat.py ./build/cli/ollamadev --tasks hard --reps 3 \
        --backend ollama --model gpt-oss:20b-cloud
"""
import argparse
import json
import glob
import re
import subprocess
import sys
import time


def hard_task_names():
    names = []
    for path in sorted(glob.glob("evals/*.json")):
        try:
            data = json.load(open(path))
        except Exception:
            continue
        for t in (data if isinstance(data, list) else [data]):
            if t.get("name"):
                names.append(t["name"])
    return names


def run_once(binary, task, backend, model, timeout):
    argv = [binary, "eval", "--only", task]
    if backend:
        argv += ["--backend", backend]
    if model:
        argv += ["-m", model]
    t0 = time.time()
    try:
        p = subprocess.run(argv, capture_output=True, text=True, timeout=timeout,
                           stdin=subprocess.DEVNULL)
    except subprocess.TimeoutExpired:
        return None, timeout
    out = p.stdout
    # A skipped task (missing interpreter) is neither a pass nor a failure and
    # must stay out of the denominator, exactly as the suite itself treats it.
    if re.search(r"^\s*[-~]\s+" + re.escape(task), out, re.M):
        return None, time.time() - t0
    if re.search(r"^\s*✓\s+" + re.escape(task), out, re.M):
        return True, time.time() - t0
    if re.search(r"^\s*✗\s+" + re.escape(task), out, re.M):
        return False, time.time() - t0
    return None, time.time() - t0


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("binary")
    ap.add_argument("--tasks", default="hard",
                    help="'hard' for everything in ./evals, or a comma-separated list")
    ap.add_argument("--reps", type=int, default=3)
    ap.add_argument("--backend", default="")
    ap.add_argument("--model", default="")
    ap.add_argument("--timeout", type=int, default=300)
    args = ap.parse_args()

    tasks = hard_task_names() if args.tasks == "hard" else \
        [s.strip() for s in args.tasks.split(",") if s.strip()]
    if not tasks:
        print("no tasks found (run from the repo root, so ./evals is visible)", file=sys.stderr)
        return 2

    label = f"{args.backend or 'default'}" + (f" / {args.model}" if args.model else "")
    print(f"{len(tasks)} tasks x {args.reps} reps on {label}\n")

    rows, wall = [], time.time()
    for task in tasks:
        results, secs = [], []
        for _ in range(args.reps):
            ok, took = run_once(args.binary, task, args.backend, args.model, args.timeout)
            results.append(ok)
            secs.append(took)
        counted = [r for r in results if r is not None]
        passed = sum(1 for r in counted if r)
        rate = (passed / len(counted) * 100) if counted else float("nan")
        marks = "".join("." if r is None else ("P" if r else "F") for r in results)
        rows.append((task, passed, len(counted), rate, marks, sum(secs) / len(secs)))
        print(f"  {task:32} {passed}/{len(counted)}  {rate:5.1f}%  [{marks}]  {sum(secs)/len(secs):5.1f}s")

    solid = [r for r in rows if r[2] and r[3] == 100.0]
    dead = [r for r in rows if r[2] and r[3] == 0.0]
    flaky = [r for r in rows if r[2] and 0.0 < r[3] < 100.0]
    total_p = sum(r[1] for r in rows)
    total_n = sum(r[2] for r in rows)

    print(f"\n  overall {total_p}/{total_n}  {total_p / total_n * 100:.1f}%"
          f"   ({time.time() - wall:.0f}s wall)")
    print(f"  always passes: {len(solid)}   never passes: {len(dead)}   FLAKY: {len(flaky)}")
    if flaky:
        print("\n  the flaky ones are the only tasks carrying information about"
              "\n  capability; the rest are ceiling or floor:")
        for t, p, n, rate, marks, _ in flaky:
            print(f"    {t:32} {p}/{n}  [{marks}]")
    return 0


if __name__ == "__main__":
    sys.exit(main())
