# The hard suite

Ten tasks that exist because the built-in suite is **saturated**. Both backends
score 96–100% on it, which tells you the tasks are too easy rather than that the
agents are equal — a frontier CLI would score the same. A benchmark everything
passes cannot rank anything.

These are picked to fail in the ways agents actually fail, not to be fiddly:

| task | what it is really testing |
| --- | --- |
| `hard-cross-file-rename` | consistency across three files; the naive fix renames the definition and leaves the call sites |
| `hard-follow-project-pattern` | reading the codebase for an unstated convention instead of inventing one |
| `hard-three-step-pipeline` | three files where each depends on the last, and the CLI must be runnable |
| `hard-extend-without-breaking` | adding a case without regressing the one that already worked |
| `hard-preserve-unknown-keys` | a migration that must not drop keys it has never seen, and must not mutate its input |
| `hard-refactor-same-behaviour` | behaviour-preserving refactor, including the error path |
| `hard-error-contract` | four distinct exception branches, with `bool` not counting as `int` |
| `hard-lru-eviction-order` | an invariant held across a sequence of calls, not one answer |
| `hard-idempotent-append` | running twice must not duplicate; the file has no trailing newline |
| `hard-stable-group` | order and duplicates preserved, which the obvious `set`-based answer loses |

## Every task is solvable by more than one correct answer

A benchmark whose checks reject a correct answer measures its own bugs. Each
task has a reference solution and the check must go green against it — and
because that turned out not to be enough, each also has a **variant**: a second
correct answer written deliberately differently.

```sh
python3 tests/validate_evals.py     # 10/10, both solutions each, no model
```

One reference proves a task is passable. It cannot prove the check is grading
behaviour rather than shape, because the reference tends to have whatever shape
the check was written around. Both over-specified checks in this suite were
found by a variant, not by a reference:

- `hard-follow-project-pattern` demanded `APP_TIMEOUT` in `net.py`, rejecting the
  equally good answer that puts it in `settings.py`
- `hard-refactor-same-behaviour` demanded a literal `{`, rejecting
  `dict(square=..., circle=...)`

Both had passed their reference solution the whole time. So: run this after
editing any task, and if a check disagrees with a known-good answer, the task is
wrong, not the agent.

## Read the numbers with the variance in mind

`hard-three-step-pipeline` sits right on the capability boundary and is
genuinely flaky — measured at **2 passes in 5 runs** on `gpt-oss:20b-cloud`,
passing alone and failing inside a full suite run. That is what a discriminating
task looks like, and it means a single run's headline percentage is noisy to
about one task.

`eval` has no repeat flag, so measuring a boundary task means running it several
times by hand:

```sh
for i in 1 2 3 4 5; do ollamadev eval --only hard-three-step-pipeline; done
```

## Measured

Each hard task run three times per backend (`tests/eval_repeat.py`), after the
`hard-follow-project-pattern` check was corrected:

| task | `gpt-oss:20b-cloud` | `claude` |
| --- | --- | --- |
| `hard-three-step-pipeline` | **1/3** | **1/3** |
| `hard-preserve-unknown-keys` | **2/3** | 3/3 |
| the other eight | 3/3 | 3/3 |
| **total** | **27/30 · 90.0%** | **28/30 · 93.3%** |

Read that honestly: **nine of ten tasks are ceiling for both backends.** Only
`hard-three-step-pipeline` is at the boundary, and it beats both about two runs
in three. The suite discriminates better than the built-in 26 — which separated
nothing — but it is still close to saturated, and a one-task total gap is not a
ranking.

If you want it to rank things, the next tasks need to be harder again: longer
horizons, more files, and prompts where the requirement has to be inferred from
the codebase rather than read off the sentence.

### A caution paid for once

`hard-follow-project-pattern` first measured 1/3 and 0/3. Almost all of that was
the check, not the agents: it demanded the literal `APP_TIMEOUT` appear in
`net.py`, while putting it in `settings.py` beside the other settings is at
least as good an answer. With the check fixed, both backends score 3/3.

A flaky task is evidence of *something*, and the first thing to rule out is your
own check. Run `tests/validate_evals.py`, and reference-solve any task that
starts failing before concluding anything about the agent.
