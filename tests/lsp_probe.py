#!/usr/bin/env python3
"""Drive `ollamadev lsp` over stdio and check it speaks LSP properly."""
import json
import subprocess
import sys
import os
import tempfile

BIN = sys.argv[1]
work = tempfile.mkdtemp(prefix="odv-lsp-")
src = os.path.join(work, "sample.py")
open(src, "w").write("def add(a, b):\n    return a + b\n\nresult = add(1, 2)\n")
uri = "file://" + src

env = dict(os.environ, HOME=tempfile.mkdtemp(prefix="odv-lsp-home-"), NO_COLOR="1")
p = subprocess.Popen([BIN, "lsp"], stdin=subprocess.PIPE, stdout=subprocess.PIPE,
                     stderr=subprocess.PIPE, cwd=work, env=env)


def send(obj):
    body = json.dumps(obj).encode()
    p.stdin.write(b"Content-Length: %d\r\n\r\n" % len(body) + body)
    p.stdin.flush()


def read():
    """Read one LSP message. Returns None at EOF."""
    length = None
    while True:
        line = p.stdout.readline()
        if not line:
            return None
        line = line.strip()
        if not line:
            break
        if line.lower().startswith(b"content-length:"):
            length = int(line.split(b":")[1])
    if length is None:
        return None
    return json.loads(p.stdout.read(length))


results = []


def check(ok, label, extra=""):
    results.append((ok, label, extra))


send({"jsonrpc": "2.0", "id": 1, "method": "initialize",
      "params": {"processId": os.getpid(), "rootUri": "file://" + work, "capabilities": {}}})
init = read()
check(bool(init) and init.get("id") == 1, "initialize gets a response")
caps = (init or {}).get("result", {}).get("capabilities", {})
check(bool(caps), "…advertising server capabilities", str(list(caps))[:120])

send({"jsonrpc": "2.0", "method": "initialized", "params": {}})
send({"jsonrpc": "2.0", "method": "textDocument/didOpen",
      "params": {"textDocument": {"uri": uri, "languageId": "python", "version": 1,
                                  "text": open(src).read()}}})

# Ask for the things the README claims: hover, definition, completion.
probes = [
    (2, "textDocument/hover", {"textDocument": {"uri": uri}, "position": {"line": 3, "character": 10}}),
    (3, "textDocument/definition", {"textDocument": {"uri": uri}, "position": {"line": 3, "character": 10}}),
    (4, "textDocument/completion", {"textDocument": {"uri": uri}, "position": {"line": 3, "character": 12}}),
]
for pid, method, params in probes:
    send({"jsonrpc": "2.0", "id": pid, "method": method, "params": params})

seen = {}
for _ in range(24):
    msg = read()
    if msg is None:
        break
    if "id" in msg and msg.get("id") in (2, 3, 4):
        seen[msg["id"]] = msg
    if len(seen) == 3:
        break

for pid, method, _ in probes:
    m = seen.get(pid)
    check(m is not None, f"{method} answers")
    if m is not None:
        check("error" not in m, f"{method} answers without a protocol error",
              json.dumps(m.get("error", ""))[:100])

send({"jsonrpc": "2.0", "id": 9, "method": "shutdown", "params": {}})
sd = read()
check(sd is not None and sd.get("id") == 9, "shutdown is answered")
send({"jsonrpc": "2.0", "method": "exit", "params": {}})
try:
    rc = p.wait(timeout=15)
    check(rc == 0, f"exit terminates the server cleanly (rc={rc})")
except subprocess.TimeoutExpired:
    p.kill()
    check(False, "exit terminates the server (it hung)")

bad = 0
for ok, label, extra in results:
    print(("  ok    " if ok else "  FAIL  ") + label + (f"   [{extra}]" if extra else ""))
    bad += 0 if ok else 1
print(f"\nlsp: {len(results)-bad} passed, {bad} failed")
sys.exit(1 if bad else 0)
