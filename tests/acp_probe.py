#!/usr/bin/env python3
"""Drive `ollamadev acp` over stdio and check it speaks the Agent Client Protocol.

ACP is newline-delimited JSON-RPC, not LSP's Content-Length framing. Everything
up to session/new is model-free, so the handshake half of this costs nothing and
runs offline; `--prompt` opts in to a real turn, which does call a model.

    python3 tests/acp_probe.py ./build/cli/ollamadev [--prompt]
"""
import argparse
import json
import os
import subprocess
import sys
import tempfile
import threading

RESULTS = []


def check(ok, label, extra=""):
    RESULTS.append((bool(ok), label, extra))
    print(("  ok    " if ok else "  FAIL  ") + label + (f"\n          {extra}" if extra else ""))


class Acp:
    def __init__(self, binary, cwd, home):
        env = dict(os.environ, HOME=home, OLLAMADEV_HOME=os.path.join(home, "st"), NO_COLOR="1")
        self.p = subprocess.Popen([binary, "acp"], cwd=cwd, env=env,
                                  stdin=subprocess.PIPE, stdout=subprocess.PIPE,
                                  stderr=subprocess.PIPE)
        self.n = 0
        self.lock = threading.Lock()

    def send(self, obj):
        self.p.stdin.write((json.dumps(obj) + "\n").encode())
        self.p.stdin.flush()

    def call(self, method, params=None, timeout=30):
        """Send a request and return the matching response, skipping notifications."""
        self.n += 1
        rid = self.n
        self.send({"jsonrpc": "2.0", "id": rid, "method": method, "params": params or {}})
        box = {}

        def read():
            while True:
                line = self.p.stdout.readline()
                if not line:
                    return
                try:
                    msg = json.loads(line)
                except Exception:
                    continue
                # Notifications (session/update and friends) carry no id.
                if msg.get("id") == rid:
                    box["msg"] = msg
                    return

        t = threading.Thread(target=read, daemon=True)
        t.start()
        t.join(timeout)
        return box.get("msg")

    def close(self):
        try:
            self.p.stdin.close()
            self.p.wait(timeout=10)
        except Exception:
            self.p.kill()
        return self.p.returncode


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("binary")
    ap.add_argument("--prompt", action="store_true",
                    help="also run one real session/prompt turn (calls a model)")
    args = ap.parse_args()

    work = tempfile.mkdtemp(prefix="odv-acp-work-")
    home = tempfile.mkdtemp(prefix="odv-acp-home-")
    with open(os.path.join(work, "hello.py"), "w") as f:
        f.write("def hi():\n    return 'hi'\n")

    a = Acp(os.path.abspath(args.binary), work, home)

    init = a.call("initialize", {"protocolVersion": 1, "clientCapabilities": {}})
    check(init is not None, "initialize is answered")
    res = (init or {}).get("result", {})
    check("protocolVersion" in res, "…with a protocol version", str(res.get("protocolVersion")))
    check("agentCapabilities" in res, "…and the agent's capabilities")
    info = res.get("agentInfo", {})
    check(info.get("name") == "ollamadev", "…and identifies itself", json.dumps(info))
    # No auth against a local Ollama, and saying so beats leaving a client to guess.
    check(res.get("authMethods") == [], "…and declares that no authentication is needed")

    auth = a.call("authenticate", {})
    check(auth is not None and "error" not in auth, "authenticate succeeds with nothing to do")

    new = a.call("session/new", {"cwd": work, "mcpServers": []})
    check(new is not None, "session/new is answered")
    sid = (new or {}).get("result", {}).get("sessionId", "")
    check(sid.startswith("acp_"), "…returning a session id", sid)

    # An unknown method must be a JSON-RPC error, not silence or a crash.
    bogus = a.call("session/nonexistent", {}, timeout=15)
    check(bogus is not None, "an unknown method still gets a response")
    check(bogus is not None and "error" in bogus,
          "…and that response is a JSON-RPC error", json.dumps((bogus or {}).get("error", ""))[:90])

    # Cancelling a session that is not running must not take the agent down.
    if sid:
        a.send({"jsonrpc": "2.0", "method": "session/cancel", "params": {"sessionId": sid}})
        still = a.call("initialize", {"protocolVersion": 1, "clientCapabilities": {}}, timeout=15)
        check(still is not None, "an idle session/cancel does not kill the agent")

    if args.prompt and sid:
        pr = a.call("session/prompt", {
            "sessionId": sid,
            "prompt": [{"type": "text", "text": "Reply with exactly: ACP_OK"}],
        }, timeout=300)
        check(pr is not None, "session/prompt is answered")
        check(pr is not None and "error" not in pr, "…without a protocol error",
              json.dumps((pr or {}).get("error", ""))[:90])
        check(pr is not None and "stopReason" in pr.get("result", {}),
              "…and reports a stop reason", json.dumps((pr or {}).get("result", ""))[:90])

    rc = a.close()
    check(rc is not None, "closing stdin terminates the agent", f"rc={rc}")

    bad = sum(1 for ok, _, _ in RESULTS if not ok)
    print(f"\nacp: {len(RESULTS) - bad} passed, {bad} failed")
    return 1 if bad else 0


if __name__ == "__main__":
    sys.exit(main())
