#!/usr/bin/env python3
"""Argv fuzzer for the OllamaDev CLI.

Generates random command lines from a deliberately hostile vocabulary and checks
the invariants that must hold for ANY input, not just the ones I thought to try:

  1. it terminates (no hang)
  2. it does not crash (no signal death, no sanitizer report)
  3. the exit code is one of the three documented ones: 0, 1, 2
  4. an exit code of 2 means the command line was rejected, so stdout is empty

The vocabulary is restricted to read-only verbs, and HOME/cwd are redirected, so
a random line cannot start a crew run, push a branch or delete anything. Verbs
that talk to a model or the network are excluded too: they would only measure
the timeout.
"""
import itertools
import os
import random
import subprocess
import sys
import tempfile

BIN = sys.argv[1]
ROUNDS = int(sys.argv[2]) if len(sys.argv) > 2 else 400
SEED = int(sys.argv[3]) if len(sys.argv) > 3 else 20260802

# Read-only, no model, no network, no mutation.
# Excluded on purpose, all for the same reason — they would measure something
# other than the argument parser:
#   backends, models, doctor, context   query Ollama or probe the hardware
#   chat, crew, search, route           take free text, so a random line becomes
#                                       a model call and just burns the timeout
VERBS = [
    "board", "stats", "agents", "ws", "memory", "index", "skills", "hooks",
    "commands", "completion", "help", "man", "config", "plugin", "terminal",
    "git", "mcp", "eval", "scan", "diff", "load", "resume", "export", "import",
    "tidy", "pr", "test", "verify", "board", "code-search",
]
SUBS = [
    "list", "status", "get", "set", "show", "presets", "cloud", "chain", "bash",
    "zsh", "fish", "resume", "role", "pack", "graph", "search", "clear", "bogus",
]
FLAGS = [
    "--json", "-q", "--quiet", "--verbose", "--no-color", "--color", "--help",
    "-h", "--version", "-v", "-V", "--new", "--no-web", "--all", "--force",
    "--yes", "--dry-run", "--nonsense", "--jsn", "-Z",
]
VALUED = ["-m", "--model", "--backend", "--max", "--only", "--out", "--focus"]
JUNK = [
    "--", "-", "", " ", "-1", "-0.5", "--=", "=", "ui.color", "x" * 300,
    "héllo", "✓", "a b c", "'", '"', "\\", "$(id)", "`id`", ";id", "|id",
    "../../etc/passwd", "%s%s%s", "{}", "[]", "\t", "-–dash",
]
VOCAB = VERBS + SUBS + FLAGS + VALUED + JUNK


def main() -> int:
    rng = random.Random(SEED)
    home = tempfile.mkdtemp(prefix="odvfuzz-home-")
    work = tempfile.mkdtemp(prefix="odvfuzz-work-")
    env = dict(os.environ)
    # Point the backend at a closed port. Any line the CLI decides is a prompt
    # rather than a command reaches the model, and waiting on a real one measures
    # inference latency, not the parser — the first version of this fuzzer
    # reported 26 "hangs" that were all just `ollamadev presets` being answered
    # by a model, which is the correct behaviour for a word that is not a command.
    # With nothing listening, that path fails in milliseconds, so a run that does
    # time out is a genuine hang.
    env.update(HOME=home, OLLAMADEV_HOME=os.path.join(home, ".state"),
               ASAN_OPTIONS="detect_leaks=0", UBSAN_OPTIONS="print_stacktrace=1",
               NO_COLOR="1", OLLAMA_HOST="127.0.0.1:1")

    failures = []
    for i in range(ROUNDS):
        argv = [rng.choice(VOCAB) for _ in range(rng.randint(1, 6))]
        try:
            p = subprocess.run([BIN] + argv, cwd=work, env=env, timeout=12,
                               stdin=subprocess.DEVNULL, capture_output=True)
        except subprocess.TimeoutExpired:
            failures.append(("HANG", argv, "", ""))
            continue

        rc, out, err = p.returncode, p.stdout, p.stderr
        blob = (out + err).decode("utf-8", "replace")
        if rc < 0:
            failures.append((f"SIGNAL {-rc}", argv, "", blob[:200]))
        elif "AddressSanitizer" in blob or "runtime error:" in blob:
            failures.append(("SANITIZER", argv, "", blob[:300]))
        elif rc not in (0, 1, 2):
            failures.append((f"EXIT {rc}", argv, "", blob[:200]))
        elif rc == 2 and out.strip():
            failures.append(("STDOUT ON RC=2", argv, out[:120].decode("utf-8", "replace"), ""))

    print(f"rounds={ROUNDS} seed={SEED} failures={len(failures)}")
    for kind, argv, out, blob in failures[:25]:
        print(f"  !! {kind}: ollamadev {' '.join(repr(a) for a in argv)}")
        if out:
            print(f"     stdout: {out!r}")
        if blob:
            print(f"     {blob.splitlines()[0] if blob.splitlines() else ''}")
    return 1 if failures else 0


if __name__ == "__main__":
    sys.exit(main())
