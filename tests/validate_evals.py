#!/usr/bin/env python3
"""Prove each hard eval task is passable — by more than one correct answer.

A benchmark whose checks reject a correct answer measures its own bugs, so every
task is solved by hand and the check must go green against that solution.

That is necessary and it is not sufficient, which this file learned the hard
way. hard-follow-project-pattern passed its reference and still measured 1/3 and
0/3 against real agents, because the check demanded the literal APP_TIMEOUT
appear in net.py while putting it in settings.py is at least as good an answer.
The reference happened to have the shape the check insisted on, so validating
one solution confirmed the bias instead of exposing it.

So each task also carries VARIANTS: correct answers written deliberately
differently — the constant in another file, a dict comprehension instead of a
literal, a different-but-legal structure. A check that accepts the reference and
rejects a variant is grading shape rather than behaviour, and is broken.
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
    "harder-tests-are-the-spec": {
        "calc.py": (
            "def add(a, b):\n    return a + b\n\n\n"
            "def divide(a, b):\n"
            "    if b == 0:\n        return None\n    return a / b\n\n\n"
            "def mean(xs):\n"
            "    if not xs:\n        return 0\n    return round(sum(xs) / len(xs), 2)\n\n\n"
            "_OPS = {'add': add, 'divide': divide}\n\n\n"
            "def apply(name, a, b):\n    return _OPS[name](a, b)\n"
        ),
    },
    "harder-deep-structure-no-recursion": {
        "tree.py": (
            "def depth(node):\n"
            "    best = 0\n"
            "    stack = [(node, 0)]\n"
            "    while stack:\n"
            "        cur, d = stack.pop()\n"
            "        if not isinstance(cur, dict) or not cur:\n"
            "            best = max(best, d)\n            continue\n"
            "        for v in cur.values():\n            stack.append((v, d + 1))\n"
            "    return best\n"
        ),
    },
    "harder-merge-touching-intervals": {
        "intervals.py": (
            "def merge_intervals(intervals):\n"
            "    if not intervals:\n        return []\n"
            "    items = sorted(tuple(i) for i in intervals)\n"
            "    out = [items[0]]\n"
            "    for s, e in items[1:]:\n"
            "        if s <= out[-1][1]:\n"
            "            out[-1] = (out[-1][0], max(out[-1][1], e))\n"
            "        else:\n            out.append((s, e))\n"
            "    return out\n"
        ),
    },
    "harder-registry-across-files": {
        "handlers/csvh.py": (
            "import csv\nimport io\n\nfrom handlers import register\n\n\n"
            "@register\nclass CsvHandler:\n"
            "    name = 'csv'\n\n"
            "    @staticmethod\n    def parse(raw):\n"
            "        return [row for row in csv.reader(io.StringIO(raw))]\n"
        ),
        "dispatch.py": (
            "import handlers.csvh  # noqa: F401\n"
            "import handlers.jsonh  # noqa: F401\n"
            "import handlers.text  # noqa: F401\n"
            "from handlers import REGISTRY\n\n\n"
            "def dispatch(fmt, raw):\n    return REGISTRY[fmt].parse(raw)\n"
        ),
    },
    "harder-exit-codes-and-streams": {
        "check.py": (
            "import os\nimport sys\n\n\n"
            "def main(argv):\n"
            "    if len(argv) != 1:\n"
            "        print('usage: check.py <file>', file=sys.stderr)\n        return 2\n"
            "    path = argv[0]\n"
            "    if not os.path.exists(path):\n"
            "        print('no such file: ' + path, file=sys.stderr)\n        return 1\n"
            "    text = open(path).read()\n"
            "    if text == '':\n"
            "        print('empty', file=sys.stderr)\n        return 1\n"
            "    print(len(text.splitlines()))\n    return 0\n\n\n"
            "if __name__ == '__main__':\n    sys.exit(main(sys.argv[1:]))\n"
        ),
    },
}


# A second correct answer per task, written to look as different from the
# reference as the task legally allows. Only tasks whose checks inspect SOURCE
# (rather than just behaviour) really need one — those are the ones that can
# quietly grade style — but a variant is cheap insurance anywhere.
VARIANTS = {
    # The rename could just as well land on a different call style.
    "hard-cross-file-rename": {
        "core.py": "def calculate_total(items):\n    total = 0\n    for i in items:\n        total += i\n    return total\n",
        "report.py": "import core\n\n\ndef summary(items):\n    return 'total=%d' % core.calculate_total(items)\n",
        "main.py": "import core\nfrom report import summary\n\n\ndef run(items):\n    return (core.calculate_total(items), summary(items))\n",
    },
    # THE ONE THAT CAUGHT THE BUG: settings.py is where this project keeps
    # settings, so putting it there and importing is at least as good.
    "hard-follow-project-pattern": {
        "settings.py": "import os\n\nRETRIES = int(os.environ.get('APP_RETRIES', '3'))\nWORKERS = int(os.environ.get('APP_WORKERS', '4'))\n\n\ndef timeout_setting():\n    return int(os.environ.get('APP_TIMEOUT', '30'))\n",
        "net.py": "from settings import timeout_setting\n\n\ndef timeout():\n    return timeout_setting()\n",
    },
    # A dict comprehension over a table is still a dispatch dict.
    "hard-refactor-same-behaviour": {
        "shapes.py": (
            "import math\n\n"
            "_FORMULAS = dict(\n"
            "    square=lambda s: s * s,\n"
            "    circle=lambda s: 3.14159 * s * s,\n"
            "    triangle=lambda s: 0.5 * s * s,\n"
            ")\n\n\n"
            "def area(kind, size):\n"
            "    fn = _FORMULAS.get(kind)\n"
            "    if fn is None:\n        raise ValueError('unknown shape: ' + kind)\n"
            "    return fn(size)\n"
        ),
    },
    "hard-extend-without-breaking": {
        "parser.py": (
            "def parse_range(s):\n"
            "    parts = s.split('-')\n"
            "    if len(parts) == 1:\n        return [int(parts[0])]\n"
            "    return list(range(int(parts[0]), int(parts[1]) + 1))\n"
        ),
    },
    "hard-preserve-unknown-keys": {
        "migrate.py": (
            "import copy\n\n\n"
            "def migrate(d):\n"
            "    out = copy.deepcopy(d)\n"
            "    out['version'] = 2\n"
            "    if 'timeout' in out:\n"
            "        secs = out['timeout']\n        del out['timeout']\n"
            "        out['timeout_ms'] = secs * 1000\n"
            "    return out\n"
        ),
    },
    "hard-stable-group": {
        "group.py": (
            "from collections import defaultdict\n\n\n"
            "def group_by_length(words):\n"
            "    out = defaultdict(list)\n"
            "    for w in words:\n        out[len(w)].append(w)\n"
            "    return dict(out)\n"
        ),
    },
    # Different tokenizer, different sort, argparse instead of sys.argv.
    "hard-three-step-pipeline": {
        "tokens.py": (
            "import string\n\n\n"
            "def tokenize(text):\n"
            "    table = str.maketrans('', '', string.punctuation)\n"
            "    return [w for w in text.lower().translate(table).split() if w]\n"
        ),
        "counts.py": (
            "from tokens import tokenize\n\n\n"
            "def top(text, n):\n"
            "    counts = {}\n"
            "    for w in tokenize(text):\n        counts[w] = counts.get(w, 0) + 1\n"
            "    pairs = list(counts.items())\n"
            "    pairs.sort(key=lambda kv: kv[0])\n"
            "    pairs.sort(key=lambda kv: kv[1], reverse=True)\n"
            "    return pairs[:n]\n"
        ),
        "cli.py": (
            "import argparse\n\nfrom counts import top\n\n\n"
            "def main():\n"
            "    ap = argparse.ArgumentParser()\n"
            "    ap.add_argument('path')\n"
            "    a = ap.parse_args()\n"
            "    with open(a.path) as f:\n        print(top(f.read(), 1)[0][0])\n\n\n"
            "if __name__ == '__main__':\n    main()\n"
        ),
    },
    # A plain dict plus a recency list, rather than OrderedDict.
    "hard-lru-eviction-order": {
        "lru.py": (
            "class LRU:\n"
            "    def __init__(self, capacity):\n"
            "        self.capacity = capacity\n        self.values = {}\n        self.order = []\n\n"
            "    def _touch(self, key):\n"
            "        if key in self.order:\n            self.order.remove(key)\n"
            "        self.order.append(key)\n\n"
            "    def get(self, key):\n"
            "        if key not in self.values:\n            return None\n"
            "        self._touch(key)\n        return self.values[key]\n\n"
            "    def put(self, key, value):\n"
            "        self.values[key] = value\n        self._touch(key)\n"
            "        while len(self.order) > self.capacity:\n"
            "            del self.values[self.order.pop(0)]\n"
        ),
    },
    # Rewrites the whole file instead of appending.
    "hard-idempotent-append": {
        "ensure.py": (
            "def ensure(path, line):\n"
            "    with open(path) as f:\n        lines = f.read().splitlines()\n"
            "    if line in lines:\n        return\n"
            "    lines.append(line)\n"
            "    with open(path, 'w') as f:\n        f.write('\\n'.join(lines) + '\\n')\n"
        ),
    },
    # Guard clauses in a different order, type() rather than isinstance.
    "hard-error-contract": {
        "validate.py": (
            "def validate(d):\n"
            "    if 'id' not in d:\n        raise KeyError('id')\n"
            "    v = d['id']\n"
            "    if type(v) is not int:\n        raise TypeError('id must be an int')\n"
            "    if v < 1:\n        raise ValueError('id must be positive')\n"
            "    return True\n"
        ),
    },
    # Classes instead of functions, and a different rounding route.
    "harder-tests-are-the-spec": {
        "calc.py": (
            "from decimal import Decimal, ROUND_HALF_UP\n\n\n"
            "def add(a, b):\n    return a + b\n\n\n"
            "def divide(a, b):\n"
            "    try:\n        return float(a) / float(b)\n"
            "    except ZeroDivisionError:\n        return None\n\n\n"
            "def mean(xs):\n"
            "    if len(xs) == 0:\n        return 0\n"
            "    q = Decimal(sum(xs)) / Decimal(len(xs))\n"
            "    return float(q.quantize(Decimal('0.01'), rounding=ROUND_HALF_UP))\n\n\n"
            "def apply(name, a, b):\n"
            "    table = {'add': add, 'divide': divide}\n"
            "    if name not in table:\n        raise KeyError(name)\n"
            "    return table[name](a, b)\n"
        ),
    },
    # Explicit queue and a generation counter rather than depth-per-node.
    "harder-deep-structure-no-recursion": {
        "tree.py": (
            "def depth(node):\n"
            "    level = [node]\n    seen = 0\n"
            "    while True:\n"
            "        nxt = []\n"
            "        for cur in level:\n"
            "            if isinstance(cur, dict) and cur:\n"
            "                nxt.extend(cur.values())\n"
            "        if not nxt:\n            return seen\n"
            "        seen += 1\n        level = nxt\n"
        ),
    },
    # Builds the result by scanning rather than mutating the tail.
    "harder-merge-touching-intervals": {
        "intervals.py": (
            "def merge_intervals(intervals):\n"
            "    items = sorted((tuple(i) for i in intervals))\n"
            "    out = []\n"
            "    for s, e in items:\n"
            "        if out and s <= out[-1][1]:\n"
            "            prev_s, prev_e = out.pop()\n"
            "            out.append((prev_s, e if e > prev_e else prev_e))\n"
            "        else:\n            out.append((s, e))\n"
            "    return out\n"
        ),
    },
    # Hand-rolled split instead of the csv module, registered from its own module.
    "harder-registry-across-files": {
        "handlers/csvh.py": (
            "from handlers import register\n\n\n"
            "@register\nclass CsvHandler:\n"
            "    name = 'csv'\n\n"
            "    @staticmethod\n    def parse(raw):\n"
            "        return [line.split(',') for line in raw.splitlines() if line != '']\n"
        ),
        "dispatch.py": (
            "from handlers import REGISTRY\n"
            "import handlers.text  # noqa: F401\n"
            "import handlers.jsonh  # noqa: F401\n"
            "import handlers.csvh  # noqa: F401\n\n\n"
            "def dispatch(fmt, raw):\n    return REGISTRY[fmt].parse(raw)\n"
        ),
    },
    # argparse, and errors raised rather than returned.
    "harder-exit-codes-and-streams": {
        "check.py": (
            "import pathlib\nimport sys\n\n\n"
            "def main():\n"
            "    args = sys.argv[1:]\n"
            "    if len(args) != 1:\n"
            "        sys.stderr.write('usage: check.py <file>\\n')\n        sys.exit(2)\n"
            "    p = pathlib.Path(args[0])\n"
            "    if not p.is_file():\n"
            "        sys.stderr.write('cannot open %s\\n' % args[0])\n        sys.exit(1)\n"
            "    body = p.read_text()\n"
            "    if not body:\n"
            "        sys.stderr.write('empty file\\n')\n        sys.exit(1)\n"
            "    sys.stdout.write('%d\\n' % len(body.splitlines()))\n\n\n"
            "main()\n"
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

    def attempt(task, solution):
        d = tempfile.mkdtemp(prefix="evalval-")
        try:
            # A task may seed nested paths (handlers/text.py); the eval harness
            # mkpaths the parent, so this has to as well or the validator fails
            # on tasks that are perfectly fine.
            def write(rel, body):
                p = os.path.join(d, rel)
                os.makedirs(os.path.dirname(p), exist_ok=True) if os.path.dirname(rel) else None
                open(p, "w").write(body)

            for fn, body in (task.get("files") or {}).items():
                write(fn, body)
            for fn, body in solution.items():
                write(fn, body)
            cmd = task["check"]["cmd"].replace("{python}", PY)
            r = subprocess.run(cmd, shell=True, cwd=d, capture_output=True, text=True, timeout=60)
            want = task["check"].get("expect", "")
            ok = r.returncode == 0 and (want in r.stdout if want else True)
            return ok, (r.stdout + r.stderr).strip()
        finally:
            shutil.rmtree(d, ignore_errors=True)

    bad, variants_run = 0, 0
    for t in tasks:
        name = t["name"]
        ref = REFERENCE.get(name)
        if ref is None:
            print(f"  FAIL  {name}: no reference solution — cannot prove it is passable")
            bad += 1
            continue

        ok, detail = attempt(t, ref)
        if not ok:
            bad += 1
            print(f"  FAIL  {name}: the reference solution does not pass its own check")
            for line in detail.splitlines()[-3:]:
                print(f"          {line}")
            continue

        var = VARIANTS.get(name)
        if var is None:
            print(f"  ok    {name}  (reference only — no variant to catch over-specification)")
            continue
        variants_run += 1
        ok2, detail2 = attempt(t, var)
        if ok2:
            print(f"  ok    {name}  (reference + variant)")
        else:
            bad += 1
            print(f"  FAIL  {name}: rejects a DIFFERENT but correct answer — the check is "
                  f"grading shape, not behaviour")
            for line in detail2.splitlines()[-3:]:
                print(f"          {line}")

    print(f"\n{len(tasks) - bad}/{len(tasks)} tasks pass their own checks "
          f"({variants_run} also checked against a second, deliberately different solution)")
    return 1 if bad else 0


if __name__ == "__main__":
    sys.exit(main())
