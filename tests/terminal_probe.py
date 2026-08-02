#!/usr/bin/env python3
"""Probe the named-terminal subsystem: ptys, argv pass-through, attach/detach.

No model is involved anywhere here — terminals are ptys and a control socket —
so this runs offline in a few seconds and costs essentially nothing.

It exists because `terminal spawn` discarded every argument beginning with a
dash before handing the command to the pty, so `spawn build sh -c "echo hi"`
reached sh as `sh "echo hi"` and sh tried to open the script as a file. Every
ordinary form was broken the same way: make -j8, tail -f, python -m http.server.
The pass-through assertions below are the ones that keep that fixed.

    python3 tests/terminal_probe.py ./build/cli/ollamadev

Everything runs under a throwaway HOME, so your own terminals are never touched.
The temp root is deliberately SHORT: a unix socket path is capped at 107 bytes
by the kernel, and the control socket lives under OLLAMADEV_HOME.
"""
import argparse
import os
import pty
import select
import shutil
import subprocess
import sys
import tempfile
import time

RESULTS = []


def check(ok, label, extra=""):
    RESULTS.append((bool(ok), label, extra))
    print(("  ok    " if ok else "  FAIL  ") + label + (f"\n          {extra}" if extra else ""))


class Term:
    def __init__(self, binary):
        self.binary = os.path.abspath(binary)
        # Short on purpose — see the module docstring.
        self.home = tempfile.mkdtemp(prefix="odvt-", dir="/tmp")
        self.state = os.path.join(self.home, "s")
        self.work = os.path.join(self.home, "w")
        os.makedirs(self.work, exist_ok=True)

    def env(self, home=None, state=None):
        return dict(os.environ, HOME=home or self.home,
                    OLLAMADEV_HOME=state or self.state, NO_COLOR="1")

    def run(self, *args, timeout=30):
        try:
            p = subprocess.run([self.binary, *args], cwd=self.work, env=self.env(),
                               timeout=timeout, stdin=subprocess.DEVNULL, capture_output=True)
        except subprocess.TimeoutExpired:
            return 124, "", "TIMEOUT"
        return p.returncode, p.stdout.decode("utf-8", "replace"), p.stderr.decode("utf-8", "replace")

    def names(self):
        """The terminal names in `terminal list`, parsed rather than substring-matched.

        The empty-state message is "no terminals — create one: …", so testing
        `"one" in output` reports a terminal named `one` as still present after
        it has been deleted. Parse the marker column instead.
        """
        out = []
        for line in self.run("terminal", "list")[1].splitlines():
            parts = line.split()
            if len(parts) >= 2 and parts[0] in ("●", "○"):
                out.append(parts[1])
        return out

    def log_contains(self, name, needle, tries=12):
        """Poll the log — the shell needs a moment to run what we sent it."""
        for _ in range(tries):
            _, out, _ = self.run("terminal", "log", name)
            if needle in out:
                return True
            time.sleep(0.5)
        return False

    def cleanup(self):
        for line in self.run("terminal", "list")[1].splitlines():
            parts = line.split()
            if len(parts) >= 2 and parts[0] in ("●", "○"):
                self.run("terminal", "delete", parts[1], timeout=20)
        shutil.rmtree(self.home, ignore_errors=True)


def probe_lifecycle(t):
    rc, out, _ = t.run("terminal", "list")
    check(rc == 0 and "no terminals" in out, "list: an empty state says so", out.strip()[:70])

    rc, out, err = t.run("terminal", "create", "one")
    check(rc == 0 and "running" in out, "create: starts a terminal", (out + err).strip()[:90])
    check("one" in t.names(), "list: shows it", str(t.names()))
    check("running" in t.run("terminal", "list")[1], "…as running")

    check(t.run("terminal", "stop", "one")[0] == 0, "stop: succeeds")
    check("stopped" in t.run("terminal", "list")[1], "…and the state says stopped")
    check(t.run("terminal", "start", "one")[0] == 0, "start: brings it back")
    check(t.run("terminal", "delete", "one")[0] == 0, "delete: succeeds")
    check("one" not in t.names(), "…and it is gone from the list", str(t.names()))

    # A name that does not exist must fail, not act on something else.
    for sub in ("log", "attach", "send"):
        args = ["terminal", sub, "nosuch"] + (["hi"] if sub == "send" else [])
        rc, out, _ = t.run(*args, timeout=20)
        check(rc != 0, f"{sub}: an unknown terminal is an error (rc={rc})")


def probe_passthrough(t):
    """The regression that matters: the wrapped program's flags are its own."""
    cases = [
        ("p1", ["sh", "-c", "echo MARKER_ONE"], "MARKER_ONE",
         "spawn: `sh -c <script>` reaches sh with -c intact"),
        ("p2", ["sh", "-c", "printf 'A%s\\n' B"], "AB",
         "spawn: a script with its own quoting survives"),
        ("p3", ["sh", "-c", "echo D1; echo D2"], "D2",
         "spawn: a multi-statement script survives"),
    ]
    for name, cmd, needle, label in cases:
        rc, out, err = t.run("terminal", "spawn", name, *cmd)
        if rc != 0:
            check(False, label, (out + err).strip()[:120])
            continue
        check(t.log_contains(name, needle), label,
              f"expected {needle!r} in the log of `{' '.join(cmd)}`")

    # --cwd is OURS and may precede the name; the program still gets its flags.
    rc, out, err = t.run("terminal", "spawn", "--cwd", "/tmp", "p4", "sh", "-c", "pwd")
    check(rc == 0, "spawn: --cwd before the name is accepted", (out + err).strip()[:90])
    check(t.log_contains("p4", "/tmp"), "…and the program runs there")

    # A program flag that collides with one of ours must NOT be eaten. `-m` is
    # ours; here it belongs to sh's script and must arrive untouched.
    rc, out, err = t.run("terminal", "spawn", "p5", "sh", "-c", "echo GOT $1", "sh", "-m")
    check(rc == 0, "spawn: a program argument colliding with our -m is accepted",
          (out + err).strip()[:90])
    check(t.log_contains("p5", "GOT -m"), "…and reaches the program unchanged")

    # No command at all is a usage error, not a silently empty terminal.
    rc, out, err = t.run("terminal", "spawn", "p6")
    check(rc == 2, f"spawn: a missing command is a usage error (rc={rc})")
    check("usage:" in (out + err), "…and prints usage")


def probe_send_and_broadcast(t):
    check(t.run("terminal", "create", "s1")[0] == 0, "send: a terminal to talk to")
    check(t.run("terminal", "send", "s1", "echo SENT_OK")[0] == 0, "send: accepted")
    check(t.log_contains("s1", "SENT_OK"), "send: the shell actually ran it")

    t.run("terminal", "create", "s2")
    rc, out, _ = t.run("terminal", "broadcast", "echo BCAST_OK")
    check(rc == 0, "broadcast: accepted", out.strip()[:70])
    check(t.log_contains("s1", "BCAST_OK") and t.log_contains("s2", "BCAST_OK"),
          "broadcast: reaches every terminal")


def probe_attach(t):
    """attach needs a real tty, and detaching must NOT kill the terminal."""
    check(t.run("terminal", "create", "a1")[0] == 0, "attach: a terminal to attach to")
    master, slave = pty.openpty()
    p = subprocess.Popen([t.binary, "terminal", "attach", "a1"], cwd=t.work, env=t.env(),
                         stdin=slave, stdout=slave, stderr=slave, close_fds=True)
    os.close(slave)
    seen = b""
    try:
        # The pty echoes what we type the instant we type it, so waiting for the
        # marker to appear proves nothing about the shell having RUN it. Splitting
        # the word with empty quotes means the echoed line reads ATTACH_""OK while
        # only the command's own output can read ATTACH_OK — so waiting for that
        # form is waiting for execution, not for the echo.
        os.write(master, b'echo ATTACH_""OK\n')
        deadline = time.time() + 20
        while time.time() < deadline and b"ATTACH_OK" not in seen:
            if select.select([master], [], [], 0.5)[0]:
                try:
                    seen += os.read(master, 4096)
                except OSError:
                    break
        check(b"ATTACH_OK" in seen, "attach: the session is interactive and the shell ran it",
              seen[-80:].decode("utf-8", "replace"))

        os.write(master, b"\x1d")  # Ctrl-] detaches
        # Keep draining while we wait: if the master buffer fills, the client
        # blocks writing to stdout and never gets round to reading our keystroke.
        # Reading raises EIO the moment the child closes the slave, which is the
        # normal way this ends — so treat that as "gone" and confirm with wait()
        # rather than polling, which can still read None a beat after exit.
        deadline = time.time() + 20
        while time.time() < deadline and p.poll() is None:
            if select.select([master], [], [], 0.5)[0]:
                try:
                    seen += os.read(master, 4096)
                except OSError:
                    break
        try:
            rc = p.wait(timeout=10)
        except subprocess.TimeoutExpired:
            rc = None
        check(rc is not None, "attach: Ctrl-] detaches and the client exits", f"rc={rc}")
        # "[detached from" is the real message; a bare "detach" would also match
        # the banner's "Ctrl-] detaches" and pass without anything having happened.
        check(b"[detached from" in seen or rc == 0,
              "…reporting the detach, or at least exiting cleanly")
    finally:
        if p.poll() is None:
            p.kill()
        os.close(master)

    # The whole point of a named terminal: it outlives the client.
    check("running" in t.run("terminal", "list")[1],
          "attach: the terminal SURVIVES the detach")
    check(t.log_contains("a1", "ATTACH_OK"),
          "attach: what was typed is in the terminal's own log")


def probe_socket_path_limit(t):
    """A unix socket path over 107 bytes must explain itself, not say 'Name error'."""
    deep = os.path.join(t.home, "d" * 60, "e" * 60)
    os.makedirs(deep, exist_ok=True)
    state = os.path.join(deep, "s")
    p = subprocess.run([t.binary, "terminal", "create", "x"], cwd=t.work,
                       env=t.env(state=state), timeout=30,
                       stdin=subprocess.DEVNULL, capture_output=True)
    blob = (p.stdout + p.stderr).decode("utf-8", "replace")
    check(p.returncode != 0, "socket limit: an over-long path fails rather than half-works")
    check("107" in blob or "limit" in blob.lower(),
          "socket limit: the error names the limit, not Qt's bare 'Name error'",
          blob.strip()[:150])
    check("OLLAMADEV_HOME" in blob, "…and says which knob to turn")


PROBES = {
    "lifecycle": probe_lifecycle,
    "passthrough": probe_passthrough,
    "send": probe_send_and_broadcast,
    "attach": probe_attach,
    "socket-limit": probe_socket_path_limit,
}


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("binary")
    ap.add_argument("--only", default="", help="comma-separated: " + ", ".join(PROBES))
    args = ap.parse_args()

    wanted = [s.strip() for s in args.only.split(",") if s.strip()] or list(PROBES)
    unknown = [w for w in wanted if w not in PROBES]
    if unknown:
        print(f"unknown probe(s): {', '.join(unknown)}", file=sys.stderr)
        return 2

    t = Term(args.binary)
    print(f"terminal probe — {t.binary}\n                 {t.home}\n")
    try:
        for name in wanted:
            print(f"[{name}]")
            try:
                PROBES[name](t)
            except Exception as exc:
                check(False, f"{name}: probe raised", repr(exc))
            print()
    finally:
        t.cleanup()

    bad = sum(1 for ok, _, _ in RESULTS if not ok)
    print(f"terminal: {len(RESULTS) - bad} passed, {bad} failed")
    return 1 if bad else 0


if __name__ == "__main__":
    sys.exit(main())
