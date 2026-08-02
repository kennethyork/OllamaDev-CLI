#!/usr/bin/env python3
"""Prove each hard eval task is passable: run its check against a reference solution.

A benchmark whose checks reject a correct answer measures nothing but its own
bugs, so every task here is solved by hand first and the check must go green.
"""
import json, glob, os, shutil, subprocess, sys, tempfile

PY = sys.executable

REFERENCE = {
    "hard-cross-file-rename": {
        "core.py": "def calculate_total(items):\n    return sum(items)\n",
        "report.py": "from core import calculate_total\n\n\ndef summary(items):\n    return 'total=%d' % calculate_total(items)\n",
        "main.py": "from core import calculate_total\nfrom report import summary\n\n\ndef run(items):\n    return (calculate_total(items), summary(items))\n",
    },
    "hard-follow-project-pattern": {
        "net.py": "import os\n\nTIMEOUT = int(os.environ.get('APP_TIMEOUT', '30'))\n\n\ndef timeout():\n    return int(os.environ.get('APP_TIMEOUT', '30'))\n",
    },
    "hard-three-step-pipeline": {
        "tokens.py": (
            "import re\n\n\ndef tokenize(text):\n"
            "    return [w for w in re.findall(r\"[a-z0-9']+\", text.lower()) if w]\n"
        ),
        "counts.py": (
            "from collections import Counter\n\nfrom tokens import tokenize\n\n\n"
            "def top(text, n):\n"
            "    c = Counter(tokenize(text))\n"
            "    return sorted(c.items(), key=lambda kv: (-kv[1], kv[0]))[:n]\n"
        ),
        "cli.py": (
            "import sys\n\nfrom counts import top\n\n\n"
            "if __name__ == '__main__':\n"
            "    print(top(open(sys.argv[1]).read(), 1)[0][0])\n"
        ),
    },
    "hard-extend-without-breaking": {
        "parser.py": (
            "def parse_range(s):\n"
            "    if '-' not in s:\n        return [int(s)]\n"
            "    lo, hi = s.split('-')\n    return list(range(int(lo), int(hi) + 1))\n"
        ),
    },
    "hard-preserve-unknown-keys": {
        "migrate.py": (
            "def migrate(d):\n"
            "    out = dict(d)\n"
            "    out['version'] = 2\n"
            "    if 'timeout' in out:\n"
            "        out['timeout_ms'] = out.pop('timeout') * 1000\n"
            "    return out\n"
        ),
    },
    "hard-refactor-same-behaviour": {
        "shapes.py": (
            "_AREAS = {\n"
            "    'square': lambda s: s * s,\n"
            "    'circle': lambda s: 3.14159 * s * s,\n"
            "    'triangle': lambda s: 0.5 * s * s,\n"
            "}\n\n\n"
            "def area(kind, size):\n"
            "    try:\n        fn = _AREAS[kind]\n"
            "    except KeyError:\n        raise ValueError('unknown shape: ' + kind)\n"
            "    return fn(size)\n"
        ),
    },
    "hard-error-contract": {
        "validate.py": (
            "def validate(d):\n"
            "    v = d['id']\n"
            "    if isinstance(v, bool) or not isinstance(v, int):\n        raise TypeError('id must be an int')\n"
            "    if v <= 0:\n        raise ValueError('id must be positive')\n"
            "    return True\n"
        ),
    },
    "hard-lru-eviction-order": {
        "lru.py": (
            "from collections import OrderedDict\n\n\n"
            "class LRU:\n"
            "    def __init__(self, capacity):\n        self.capacity = capacity\n        self._d = OrderedDict()\n\n"
            "    def get(self, key):\n"
            "        if key not in self._d:\n            return None\n"
            "        self._d.move_to_end(key)\n        return self._d[key]\n\n"
            "    def put(self, key, value):\n"
            "        if key in self._d:\n            self._d.move_to_end(key)\n"
            "        self._d[key] = value\n"
            "        while len(self._d) > self.capacity:\n            self._d.popitem(last=False)\n"
        ),
    },
    "hard-idempotent-append": {
        "ensure.py": (
            "def ensure(path, line):\n"
            "    text = open(path).read()\n"
            "    if line in text.splitlines():\n        return\n"
            "    sep = '' if text == '' or text.endswith('\\n') else '\\n'\n"
            "    with open(path, 'a') as f:\n        f.write(sep + line + '\\n')\n"
        ),
    },
    "hard-stable-group": {
        "group.py": (
            "def group_by_length(words):\n"
            "    out = {}\n"
            "    for w in words:\n        out.setdefault(len(w), []).append(w)\n"
            "    return out\n"
        ),
    },
}


def main():
    tasks = []
    for path in sorted(glob.glob("evals/*.json")):
        try:
            data = json.load(open(path))
        except Exception as exc:
            print(f"  FAIL  {path}: not valid JSON — {exc}")
            return 1
        tasks += data if isinstance(data, list) else [data]

    bad = 0
    for t in tasks:
        name = t["name"]
        ref = REFERENCE.get(name)
        if ref is None:
            print(f"  FAIL  {name}: no reference solution — cannot prove it is passable")
            bad += 1
            continue
        d = tempfile.mkdtemp(prefix="evalval-")
        try:
            for fn, body in (t.get("files") or {}).items():
                open(os.path.join(d, fn), "w").write(body)
            for fn, body in ref.items():
                open(os.path.join(d, fn), "w").write(body)
            cmd = t["check"]["cmd"].replace("{python}", PY)
            r = subprocess.run(cmd, shell=True, cwd=d, capture_output=True, text=True, timeout=60)
            want = t["check"].get("expect", "")
            ok = r.returncode == 0 and (want in r.stdout if want else True)
            print(("  ok    " if ok else "  FAIL  ") + name)
            if not ok:
                bad += 1
                print(f"          rc={r.returncode}")
                for line in (r.stdout + r.stderr).strip().splitlines()[-4:]:
                    print(f"          {line}")
        finally:
            shutil.rmtree(d, ignore_errors=True)

    print(f"\n{len(tasks) - bad}/{len(tasks)} hard tasks are solvable and their checks agree")
    return 1 if bad else 0


if __name__ == "__main__":
    sys.exit(main())
