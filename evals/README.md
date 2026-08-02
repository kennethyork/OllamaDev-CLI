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

## Every task is provably solvable

A benchmark whose checks reject a correct answer measures its own bugs. Each
task here has a reference solution, and the check must go green against it:

```sh
python3 tests/validate_evals.py     # 10/10, no model involved
```

Run that after editing any task. If a check disagrees with a known-good
solution, the task is wrong, not the agent.

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

## Measured so far

Full suite, 36 tasks (26 built-in + these 10), one run each:

| backend | score | hard tasks |
| --- | --- | --- |
| `ollama` / `gpt-oss:20b-cloud` | 35/36 · 97.2% | 9/10 |
| `claude` | 34/36 · 94.4% | 8/10 |

A one-task spread at n=36 is inside the noise, especially with a known-flaky
task in the set — this does **not** establish that either is better. What it
does establish is that the agent loop and both backends work end to end, and
that the suite can now distinguish something, which the built-in one could not.
