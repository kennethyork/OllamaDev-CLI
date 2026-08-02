#!/usr/bin/env python3
"""End-to-end probe for the crew's opt-in brains.

These cannot live in odv-tests: every one of them needs a live model, and the
suite must stay runnable offline in a second. They are not optional in spirit,
though — three brains shipped broken in exactly the way a unit test would never
catch, each reporting success while producing nothing:

  --security  the scanner prompt was truncated to the bare word "Focus", so
              scanners asked what was wanted instead of scanning, and the run
              still ended on a line indistinguishable from a clean one
  --learn     wrote its memory notes into a coder's sandbox, deleted seconds
              later, and reported "learned: 3 fact(s)" every time
  --dedupe    judged duplication from titles and file names, which are the
              fields that cannot show the same work done in different files

Each check below fails if its bug returns. Everything runs in a throwaway HOME
and a throwaway git repo, so it never touches your sessions, board or memory.

    python3 tests/crew_probe.py ./build/cli/ollamadev [--backend B] [--model M] [--only NAME]

--only takes a comma-separated list: security, learn, dedupe, dedupe-negative,
route, board. Use it — this is EXPENSIVE. A full pass runs six crews, each
fanning out parallel coders, and will keep a laptop's fans up for several
minutes. `--only route` costs a second or two; `--only learn` took 143s here.

Where the work happens is yours to choose:

  (nothing)                     config's ollama.defaultModel, for every role
  --model qwen3.5:2b            a small LOCAL model: keeps everything on this box
  --model gpt-oss:20b-cloud     a cloud tag: inference runs off-box, but sandbox
                                copying, per-coder git and the coder processes
                                are still local, so it is NOT free here
  --backend claude              a different provider, for every role
  --require-cloud               abort if Ollama loads anything without "cloud"
                                in its tag

--model and --backend are expanded here into the PER-ROLE flags
(--coder-model, --director-model, --auditor-model, --researcher-model, and the
matching --*-backend), which are the most explicit way to say it and leave
nothing to inherit. `crew -m X` now works too — it did not until recently, when
the crew option builder read neither flag and every role quietly fell back to
config, so a run could look pinned to a model it was never going to use.

`route` ignores all of it by design — choosing the model IS its job, and it will
reach for a local one whatever you pass. That, and the fact that the "obviously
correct" flag turned out to do nothing for months, is why --require-cloud
watches what Ollama actually loads rather than trusting any of the above.

Every section has been run end to end and passes: route (3 checks),
amplify (4), swarm (3), pack (8), security (7), learn (5), dedupe (4),
dedupe-negative (2), board (6) — 42 in all.
"""
import argparse
import glob
import json
import os
import re
import shutil
import subprocess
import sys
import tempfile
import threading
import time

VULN_SRC = """import os, sqlite3

def lookup(conn, uid):
    return conn.execute("SELECT * FROM accounts WHERE id = '%s'" % uid).fetchall()

def backup(path):
    os.system("tar czf backup.tgz " + path)
"""

RESULTS = []
ABORT = []  # non-empty once the local-model guard has tripped


def check(ok, label, extra=""):
    RESULTS.append((bool(ok), label, extra))
    print(("  ok    " if ok else "  FAIL  ") + label + (f"\n          {extra}" if extra else ""))


class LocalModelGuard:
    """Abort the moment Ollama loads a model that is not a cloud tag.

    --require-cloud exists because reasoning about where inference will happen
    turned out to be unreliable: the obvious flag for it (-m) was read by nothing
    on the crew path, so runs that looked pinned to a cloud model were only
    landing there because the configured default happened to be one. That is
    fixed, but the lesson is that the guarantee should not rest on my reading of
    the resolution order.

    This does not reason. It polls `ollama ps` and looks at what is actually
    resident. A tag without "cloud" in it is running on this machine's GPU, and
    the run is stopped rather than allowed to keep heating it.
    """

    def __init__(self, enabled):
        self.enabled = enabled
        self.stop = threading.Event()
        self.seen = set()
        self.thread = None

    @staticmethod
    def loaded():
        try:
            p = subprocess.run(["ollama", "ps"], capture_output=True, timeout=10)
        except Exception:
            return []
        names = []
        for line in p.stdout.decode("utf-8", "replace").splitlines()[1:]:
            parts = line.split()
            if parts:
                names.append(parts[0])
        return names

    @staticmethod
    def clients():
        """Who currently holds a connection to Ollama.

        The guard watches the whole machine, so a local model may belong to
        something else entirely. Without this it can only say a local model
        appeared, not whose it was — which is the difference between a real
        finding and a false alarm from another process on the same box.
        """
        try:
            p = subprocess.run(["ss", "-tnp"], capture_output=True, timeout=10)
        except Exception:
            return []
        who = set()
        for line in p.stdout.decode("utf-8", "replace").splitlines():
            # Ollama's port only. Matching the whole table names every process
            # with any socket open — firefox and the updater included — which
            # would point the finger at whatever happened to be online.
            if ":11434" not in line:
                continue
            who.update(re.findall(r'"([^"]+)",pid=(\d+)', line))
        return sorted(who)

    def _watch(self):
        while not self.stop.wait(2.0):
            for name in self.loaded():
                self.seen.add(name)
                if "cloud" not in name.lower() and not ABORT:
                    who = self.clients()
                    ABORT.append(name)
                    print(f"\n  !! ABORT: ollama loaded a LOCAL model: {name}")
                    print(f"     holding a connection to ollama: "
                          f"{', '.join(f'{n}(pid {p})' for n, p in who) or 'nobody right now'}")
                    print(f"     --require-cloud is on, so the run stops here rather than\n"
                          f"     keep a {name} resident on this machine's GPU.\n")
                    return

    def __enter__(self):
        if self.enabled:
            self.thread = threading.Thread(target=self._watch, daemon=True)
            self.thread.start()
        return self

    def __exit__(self, *a):
        self.stop.set()
        if self.thread:
            self.thread.join(timeout=5)


class Env:
    """A throwaway HOME plus a throwaway git repo to run in."""

    def __init__(self, binary, model, backend=""):
        self.binary = os.path.abspath(binary)
        self.model = model
        self.backend = backend
        self.home = tempfile.mkdtemp(prefix="odv-crew-home-")
        self.state = os.path.join(self.home, "state")
        self.work = tempfile.mkdtemp(prefix="odv-crew-work-")
        # Pin the model in the isolated config too, not only on the command line.
        # The probe runs with an empty OLLAMADEV_HOME, so ollama.defaultModel is
        # unset and ANY resolution path that falls through — a role nobody named,
        # a helper that reads config directly — lands on Backends::defaultModel(),
        # which is whatever Ollama offers first and is typically a LOCAL tag.
        # Flags cover the roles; this closes everything behind them.
        if model or backend:
            os.makedirs(self.state, exist_ok=True)
            cfg = {}
            if model:
                cfg["ollama"] = {"defaultModel": model}
            if backend:
                cfg["model"] = {"backend": backend}
            with open(os.path.join(self.state, "config.json"), "w") as f:
                json.dump(cfg, f)
        self._git("init", "-q", ".")
        self._git("config", "user.email", "probe@example.invalid")
        self._git("config", "user.name", "probe")

    def _git(self, *a):
        subprocess.run(["git", *a], cwd=self.work, capture_output=True)

    def seed(self, files):
        for name, body in files.items():
            with open(os.path.join(self.work, name), "w") as f:
                f.write(body)
        self._git("add", "-A")
        self._git("commit", "-qm", "seed")

    def reset(self):
        """Back to the seeded commit, and forget any crew/board/memory state."""
        self._git("checkout", "-q", ".")
        self._git("clean", "-qfd")
        shutil.rmtree(os.path.join(self.work, ".ollamadev"), ignore_errors=True)
        for sub in ("crew", "board", "memory"):
            shutil.rmtree(os.path.join(self.state, sub), ignore_errors=True)

    def run(self, *args, timeout=1800):
        env = dict(os.environ, HOME=self.home, OLLAMADEV_HOME=self.state, NO_COLOR="1")
        argv = [self.binary, *args]
        # Only the commands that actually run a model take these. Bolting them
        # onto `board` or `memory list` would be accepted and silently ignored,
        # which is the sort of thing that later reads as evidence they applied.
        if args and args[0] in ("crew", "route"):
            # PER-ROLE flags, not -m. The crew option builder never reads -m or
            # --backend — they are listed in the global Options block and parsed
            # nowhere on this path — so `crew "task" -m X` silently ignores X and
            # every role falls back to config's ollama.defaultModel. Passing -m
            # here looked like pinning the model and pinned nothing.
            if self.model:
                for flag in ("--coder-model", "--director-model",
                             "--auditor-model", "--researcher-model"):
                    argv += [flag, self.model]
            if self.backend:
                for flag in ("--coder-backend", "--director-backend",
                             "--auditor-backend", "--researcher-backend"):
                    argv += [flag, self.backend]
        try:
            p = subprocess.run(argv, cwd=self.work, env=env, timeout=timeout,
                               stdin=subprocess.DEVNULL, capture_output=True)
        except subprocess.TimeoutExpired:
            return 124, "", "TIMEOUT"
        return p.returncode, p.stdout.decode("utf-8", "replace"), p.stderr.decode("utf-8", "replace")

    def memory_count(self):
        rc, out, _ = self.run("memory", "list", "--json", timeout=120)
        try:
            return len(json.loads(out))
        except Exception:
            return -1

    def cleanup(self):
        shutil.rmtree(self.home, ignore_errors=True)
        shutil.rmtree(self.work, ignore_errors=True)


def probe_security(e):
    e.reset()
    e.seed({"svc.py": VULN_SRC})
    rc, out, err = e.run("crew", "--security", "audit this for vulnerabilities", "--max", "1")
    check(rc == 0, "security: the scan completes", err[-200:] if rc else "")

    # The console line must state what was FOUND. "scanned N files" alone is what
    # a run that found two critical vulnerabilities used to end on.
    check("FINDING" in out.upper(),
          "security: the summary says how many findings, not just what was read",
          out.strip().splitlines()[-1] if out.strip() else "")

    m = re.search(r"report written: (\S+)", out)
    check(bool(m), "security: a report path is reported")
    if not m:
        return
    report = open(m.group(1)).read()

    # The planted bugs are a textbook SQL injection and a shell exec. A scanner
    # that is working finds them; the truncated prompt found neither.
    low = report.lower()
    check("injection" in low, "security: the report names the injection it was given",
          report[:160].replace("\n", " "))
    check(any(w in low for w in ("os.system", "command injection", "shell")),
          "security: …and the unsafe shell call")

    # The signature of the truncated prompt: the scanner asking what to look for.
    asked = any(p in low for p in ("could you let me know", "what specifically",
                                   "what would you like", "please clarify"))
    check(not asked, "security: the scanner scanned instead of asking what to scan")
    check("0 finding" not in low, "security: a scan that found things does not report zero")


def probe_learn(e):
    e.reset()
    e.seed({"svc.py": VULN_SRC})
    before = e.memory_count()
    started = time.time()
    rc, out, err = e.run(
        "crew", "--learn",
        "add cache.py: an LRU cache class taking max_size, evicting the least-recently-used "
        "entry when full; document the eviction policy in a comment",
        "--max", "1")
    check(rc == 0, "learn: the run completes", err[-200:] if rc else "")

    claimed = re.search(r"learned: (\d+) fact", out)
    check(bool(claimed), "learn: the run reports what it learned")
    n = int(claimed.group(1)) if claimed else 0

    after = e.memory_count()
    # The bug: the count was real, the notes were written into a sandbox that is
    # deleted moments later, so this stayed at zero however much was "learned".
    if n > 0:
        check(after > before,
              "learn: what it claims to have learned is actually kept",
              f"claimed {n} fact(s); memory went {before} -> {after}")
        check(os.path.isdir(os.path.join(e.work, ".ollamadev", "memory")),
              "learn: …in the project's own memory directory")
    else:
        check(False, "learn: extracted at least one fact from a task with a real convention",
              "claimed 0 — cannot tell a working extractor from a broken one")

    # Nothing may be left behind in a sandbox, which is where they used to go.
    stranded = [p for p in glob.glob("/tmp/ollamadev-crew/*/*/.ollamadev/memory/*.md")
                if os.path.getmtime(p) >= started]
    check(not stranded, "learn: no notes stranded in a coder sandbox",
          stranded[0] if stranded else "")


DUP_TASK = ("write summary-one.md explaining what this project does, and write "
            "summary-two.md explaining what this project does")
DISJOINT_TASK = ("add a docstring to lookup() in svc.py, and add a README.md "
                 "describing the project")


def probe_dedupe(e):
    e.reset()
    e.seed({"svc.py": VULN_SRC})
    rc, out, err = e.run("crew", "--dedupe", DUP_TASK, "--max", "2")
    check(rc == 0, "dedupe: the run completes", err[-200:] if rc else "")
    check("dedupe:" in out, "dedupe: the phase runs")
    # The bug: judging from titles and file names, which never group anything.
    check("duplicates coder" in out,
          "dedupe: two files with the same content are recognised as duplicates",
          out.strip().splitlines()[-1] if out.strip() else "")
    # Held, never destroyed — the user must be able to take it anyway.
    rc2, board, _ = e.run("board", timeout=120)
    check("duplicates coder" in board, "dedupe: the hold reason survives onto the board")


def probe_dedupe_negative(e):
    """A dedupe that holds everything is worse than one that holds nothing."""
    e.reset()
    e.seed({"svc.py": VULN_SRC})
    rc, out, err = e.run("crew", "--dedupe", DISJOINT_TASK, "--max", "2")
    check(rc == 0, "dedupe-negative: the run completes", err[-200:] if rc else "")
    check("duplicates coder" not in out,
          "dedupe-negative: genuinely different work is NOT held as duplicate",
          out.strip().splitlines()[-1] if out.strip() else "")


def probe_route(e):
    tiers = {}
    for label, task in (("simple", "fix a typo in a comment"),
                        ("hard", "design and implement a distributed consensus protocol")):
        rc, out, _ = e.run("route", task, timeout=300)
        tiers[label] = out.strip()
        check(rc == 0 and out.strip(), f"route: answers for a {label} task", out.strip()[:90])
    check(tiers.get("simple") != tiers.get("hard"),
          "route: a trivial task and a hard one do not get the same answer",
          f"{tiers.get('simple')!r} vs {tiers.get('hard')!r}")


def probe_board(e):
    """accept/discard round trip: one lands, one is thrown away."""
    e.reset()
    e.seed({"svc.py": VULN_SRC})
    rc, out, err = e.run("crew", "--review", DISJOINT_TASK, "--max", "2")
    check(rc == 0, "board: a --review run completes", err[-200:] if rc else "")
    rc, board, _ = e.run("board", timeout=120)
    held = len(re.findall(r"^\s+#\d+\s", board, re.M))
    check(held >= 1, "board: --review holds the work instead of applying it", board[:160])
    if held < 1:
        return
    # Hash the tree, do not just list it: a changeset that adds a docstring
    # MODIFIES a file and creates none, so comparing directory listings reports
    # a perfectly good accept as having done nothing.
    def snapshot():
        out = {}
        for root, dirs, files in os.walk(e.work):
            dirs[:] = [d for d in dirs if d not in (".git", ".ollamadev")]
            for f in files:
                p = os.path.join(root, f)
                try:
                    out[os.path.relpath(p, e.work)] = open(p, "rb").read()
                except OSError:
                    pass
        return out

    before = snapshot()
    check(e.run("crew", "accept", "1", timeout=300)[0] == 0, "board: accept applies a changeset")
    after = snapshot()
    added = set(after) - set(before)
    changed = {k for k in set(after) & set(before) if after[k] != before[k]}
    check(bool(added or changed), "board: …and the work actually lands in the tree",
          f"added={sorted(added)} modified={sorted(changed)}")
    if held >= 2:
        check(e.run("crew", "discard", "2", timeout=300)[0] == 0, "board: discard drops one")
    rc, board, _ = e.run("board", timeout=120)
    check("nothing pending" in board, "board: the queue is empty afterwards")


def probe_amplify(e):
    """N director plans, modal pick, N-reviewer panel — both halves must show."""
    e.reset()
    e.seed({"svc.py": VULN_SRC})
    rc, out, err = e.run("crew", "--amplify", "3",
                         "add a module-level docstring to svc.py", "--max", "1")
    check(rc == 0, "amplify: the run completes", err[-200:] if rc else "")
    # "director drew 3 plans · 3 agreed on N subtasks"
    m = re.search(r"director drew (\d+) plans", out)
    check(m is not None and int(m.group(1)) == 3,
          "amplify: the director really drew 3 plans, not 1",
          (m.group(0) if m else out.strip()[:90]))
    check(re.search(r"3-reviewer panel", out) is not None,
          "amplify: …and the audit is a 3-reviewer panel",
          next((l for l in out.splitlines() if "audit" in l), "")[:90])

    # --amplify 1 must NOT pay for a panel: the brain is opt-in per run.
    e.reset()
    rc, out, _ = e.run("crew", "add a comment to svc.py", "--max", "1")
    check("director drew" not in out,
          "amplify: a plain run draws one plan and says nothing about panels")


def probe_swarm(e):
    """--swarm raises the coder cap; the run must respect it, not ignore it."""
    e.reset()
    e.seed({"svc.py": VULN_SRC})
    rc, out, err = e.run(
        "crew", "--swarm", "6",
        "add a docstring to lookup(), a docstring to backup(), and a README.md",
        "--max", "6")
    check(rc == 0, "swarm: the run completes", err[-200:] if rc else "")
    coders = {int(m) for m in re.findall(r"coder #(\d+)", out)}
    check(bool(coders), "swarm: coders were planned", f"saw {sorted(coders)}")
    # The cap is the contract: --swarm 6 --max 6 must never exceed 6.
    check(not coders or max(coders) <= 6,
          "swarm: the coder count stays within the cap", f"saw {sorted(coders)}")


def probe_pack(e):
    """A saved pack must actually shape the run it is applied to."""
    e.reset()
    e.seed({"svc.py": VULN_SRC})
    # A pack that pins the coder model to something distinctive, so the plan
    # line proves the pack was read rather than merely accepted.
    pin = e.model or "gpt-oss:20b-cloud"
    rc, out, err = e.run("crew", "pack", "save", "probepack",
                         "--focus", "keep the change tiny", "--coder-model", pin)
    check(rc == 0, "pack: save succeeds", (out + err).strip()[:90])
    rc, out, _ = e.run("crew", "pack", "list")
    check("probepack" in out, "pack: it appears in the list", out.strip()[:80])
    rc, out, _ = e.run("crew", "pack", "show", "probepack")
    check(pin in out and "keep the change tiny" in out,
          "pack: show reports what was saved", out.strip()[:120])

    rc, out, err = e.run("crew", "--pack", "probepack",
                         "add a comment to svc.py", "--max", "1")
    check(rc == 0, "pack: a run using the pack completes", err[-200:] if rc else "")
    check(pin in out, "pack: the pack's model reaches the plan",
          next((l for l in out.splitlines() if "coder #1" in l), "")[:100])

    check(e.run("crew", "pack", "rm", "probepack")[0] == 0, "pack: rm succeeds")
    check("probepack" not in e.run("crew", "pack", "list")[1], "pack: …and it is gone")


PROBES = {
    "route": probe_route,
    "amplify": probe_amplify,
    "swarm": probe_swarm,
    "pack": probe_pack,
    "security": probe_security,
    "learn": probe_learn,
    "dedupe": probe_dedupe,
    "dedupe-negative": probe_dedupe_negative,
    "board": probe_board,
}


def main():
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("binary")
    ap.add_argument("--model", default="", help="model to run every role on")
    ap.add_argument("--backend", default="",
                    help="backend for every role (ollama, claude, codex, gemini, …). "
                         "Omit to use your configured default, which is local Ollama "
                         "unless you have changed it.")
    ap.add_argument("--only", default="", help="comma-separated: " + ", ".join(PROBES))
    ap.add_argument("--require-cloud", action="store_true",
                    help="abort the run if Ollama loads any model without 'cloud' in its "
                         "tag. Checks what is actually resident rather than trusting the "
                         "flags, because -m does not pin the crew's models.")
    args = ap.parse_args()

    wanted = [s.strip() for s in args.only.split(",") if s.strip()] or list(PROBES)
    unknown = [w for w in wanted if w not in PROBES]
    if unknown:
        print(f"unknown probe(s): {', '.join(unknown)}", file=sys.stderr)
        return 2

    e = Env(args.binary, args.model, args.backend)
    print(f"crew probe — binary  {e.binary}\n           backend {args.backend or '(configured default)'}"
          f"\n           model   {args.model or '(configured default)'}"
          f"\n           work    {e.work}\n")
    try:
        with LocalModelGuard(args.require_cloud) as guard:
            for name in wanted:
                if ABORT:
                    print(f"[{name}] skipped — local model {ABORT[0]} was loaded\n")
                    continue
                print(f"[{name}]")
                t = time.time()
                try:
                    PROBES[name](e)
                except Exception as exc:  # a probe must not take the run down with it
                    check(False, f"{name}: probe raised", repr(exc))
                print(f"          ({time.time() - t:.0f}s)\n")
            if args.require_cloud:
                print(f"models Ollama loaded during this run: "
                      f"{', '.join(sorted(guard.seen)) or '(none seen)'}\n")
    finally:
        e.cleanup()

    bad = sum(1 for ok, _, _ in RESULTS if not ok)
    print(f"crew: {len(RESULTS) - bad} passed, {bad} failed")
    return 1 if bad else 0


if __name__ == "__main__":
    sys.exit(main())
