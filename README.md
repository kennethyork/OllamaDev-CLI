# OllamaDev CLI

A local-first AI coding agent for your terminal. Talks to **Ollama** by default, and
can drive every major coding CLI — claude, codex, gemini, cursor-agent, opencode,
qwen, aider, goose, amp, crush, droid — behind one interface. No GUI, no telemetry;
your code stays on your machine unless you point it at a cloud model yourself.

This is the standalone CLI, extracted from the OllamaDev ADE desktop app. It shares
the same `core/` engine (agent loop, tools, crew, git workflow, MCP, LSP) but builds
with **no GUI toolchain** — only Qt Core, Network, and Concurrent.

**Docs / site:** a self-contained landing + command reference lives in
[`docs/index.html`](docs/index.html). To publish it, enable GitHub Pages on this repo
with the source set to the `main` branch `/docs` folder.

## Build

Needs a C++20 compiler, CMake ≥ 3.21, and Qt6 (Core/Network/Concurrent).

```sh
./install.sh            # build → test → install to ~/.local/bin/ollamadev
# or, manually:
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j"$(nproc)"
./build/tests/odv-tests   # 402 assertions
./build/cli/ollamadev --version
```

Package it (`.deb` / `.rpm` / `.tar.gz`):

```sh
cd build && cpack
```

## Quick start

```sh
ollamadev setup                 # detect hardware, recommend + pull a model
ollamadev                       # interactive chat (auto-resumes this folder)
ollamadev "add a --json flag to the export command"   # one-shot agent turn
ollamadev doctor                # health check

ollamadev help                  # every command, one line each
ollamadev help crew             # …or just one, in full (same as `crew --help`)
man ollamadev                   # the manual (installed by install.sh / the packages)

git diff | ollamadev "review this"    # piped stdin is appended to the prompt
ollamadev -- status of the release    # `--` forces prompt text, not a command
```

## How it behaves in a shell

- **Working directory wins.** Every command acts on the folder you ran it in. If
  that folder is a bookmarked workspace it is published as active so the desktop
  app follows along, but the CLI never relocates itself. `ollamadev config set
  workspace.follow true` opts back in to following the active project from
  anywhere.
- **Colour only for terminals.** ANSI styling is suppressed when the stream is a
  pipe or a file, and honours `NO_COLOR`, `TERM=dumb` and `ui.color false`.
  stdout and stderr are judged separately.
- **Exit codes.** `0` success · `1` the command ran and reported a problem (no
  index, a leaked secret, tests red) · `2` the command line itself was wrong.
- **Nothing is ignored in silence.** An unrecognised flag or subcommand is an
  error with a suggestion. `--json` on a command that has no JSON form is an
  error too, rather than a flag that quietly does nothing. Flags after `git`,
  `mcp` and `terminal` belong to the program being wrapped and are passed
  straight through.
- **Flags go on either side.** `ollamadev --json models` and `ollamadev models
  --json` are the same command.
- **`--` means two things, by position.** With no command it forces prompt text
  (`ollamadev -- status of the release`). After a command it is POSIX
  end-of-options, so a value can start with a dash:
  `ollamadev config set retry.delay -- -1`.
- **`-q` for scripts, `--verbose` for debugging.** Quiet drops the progress
  chatter and nothing else — results still print and errors are never hidden.
  Verbose reports the resolved cwd, backend and model, on stderr.
- **XDG paths.** A fresh install stores state in `$XDG_DATA_HOME/ollamadev`
  (default `~/.local/share/ollamadev`) and reads config from
  `$XDG_CONFIG_HOME/ollamadev`. An existing `~/.ollamadev` keeps working
  untouched — nothing is migrated. `OLLAMADEV_HOME` overrides all of it.
- **Help is local and specific.** `ollamadev help` lists every command,
  `ollamadev help <cmd>` (or `<cmd> --help`) documents just that one. No model
  call is involved, so it works offline and costs nothing. `man ollamadev` is
  the same material as a manual page — generated from the same table at build
  time, so it cannot describe a command surface the binary does not have.
- **Colour on demand.** `--no-color` and `--color` outrank the environment;
  `--color` forces styling back on for pipes that render it, like `less -R`.

## What it does

- **Agent** — an interactive or one-shot coding agent with file edits, shell, and a
  full toolset. Sessions auto-resume per folder.
- **Crew** — a parallel bench: research → plan → N coders (each in its own git
  worktree) → audit → land. Opt-in brains (`--route`, `--debate`, `--amplify`, …).
- **Ship it** — AI git workflow: `diff`, `commit` (blocks leaked secrets), `ship`,
  `pr create|review`.
- **Context** — semantic code index (`index build`, `code-search`), web `search`,
  wiki-linked `memory`, progressive-disclosure `skills`.
- **Integration** — `mcp serve` (expose tools to any MCP client), `lsp` (completion,
  hover, go-to-def, diagnostics), `hooks`, custom `/slash` commands.

Run `ollamadev --help` for the full command surface.

## Measuring it: `eval`

`ollamadev eval` runs a fixed suite of small coding tasks and reports a pass
rate. Every task runs isolated in its own temp dir, and the verdict is
deterministic — expected file content, or a command's exit code — never a model
judging another model. `--compare a,b,c` scores several models, `--json` is
machine-readable, and `--min N` exits 1 below N% so CI can gate on it.

Your own tasks join the suite: drop `*.json` into `./evals` or
`./.ollamadev/evals`.

```json
{
  "name": "sum-helper",
  "prompt": "Create sum.py with total(xs) returning the sum of a list.",
  "files": { "note.txt": "optional seed files for the working dir\n" },
  "check": { "type": "command", "cmd": "python3 -c \"from sum import total; assert total([1,2,3])==6\"" }
}
```

`check.type` is one of:

| type | keys | passes when |
| --- | --- | --- |
| `file_exists` | `path` | the file is there |
| `file_contains` | `path`, `needle`, `normalize` | the file contains the needle (`normalize` ignores whitespace) |
| `command` | `cmd`, `expect` | the command exits 0 (and its output contains `expect`, if given) |

A check whose interpreter is missing is **skipped**, not failed — it stays out of
the denominator, so the rate measures the model rather than the box.

The built-in 26 are deliberately small, and they are **saturated** — everything
capable scores 96–100%, so they answer "does the agent work" and nothing finer.
[`evals/`](evals/README.md) adds ten harder ones that discriminate: cross-file
consistency, following an unstated project convention, multi-step pipelines,
migrations that must not drop unknown keys. Each has a reference solution, and
`python3 tests/validate_evals.py` proves the checks agree with a correct answer
before you trust a failure.

## Layout

```
core/    the engine — agent loop, tools, crew, backends, git, MCP, LSP, …
cli/     the CLI + REPL (main.cpp, Repl.cpp)
tests/   smoke suite (odv-tests) + the argv fuzzer
```

## Checking it

```sh
./build/tests/odv-tests          # 402 assertions

# Sanitizers — the suite drives the real binary, so the CLI paths are covered too.
cmake -S . -B build-asan -DCMAKE_BUILD_TYPE=Debug \
  -DCMAKE_CXX_FLAGS="-fsanitize=address,undefined -fno-omit-frame-pointer -g" \
  -DCMAKE_EXE_LINKER_FLAGS="-fsanitize=address,undefined"
cmake --build build-asan -j"$(nproc)" && ASAN_OPTIONS=detect_leaks=0 ./build-asan/tests/odv-tests

# Fuzz the argument parser. Random command lines from a hostile vocabulary;
# asserts it terminates, does not crash, exits 0/1/2, and prints nothing to
# stdout when it rejects the line. Run it against the sanitized binary.
python3 tests/fuzz_argv.py ./build-asan/cli/ollamadev 250 <seed>

# Drive the language server over stdio: handshake, hover, go-to-def,
# completion, clean shutdown. Needs no editor.
python3 tests/lsp_probe.py ./build/cli/ollamadev

# Drive the Agent Client Protocol over stdio. The handshake half is model-free
# and runs offline; --prompt adds one real turn.
python3 tests/acp_probe.py ./build/cli/ollamadev [--prompt]

# Named terminals: pty lifecycle, argv pass-through to the wrapped program,
# attach/detach over a real pty. No model, runs offline in seconds.
python3 tests/terminal_probe.py ./build/cli/ollamadev

# The crew's opt-in brains, end to end against a live model. EXPENSIVE — a full
# pass runs six crews with parallel coders; use --only, and a small --model.
python3 tests/crew_probe.py ./build/cli/ollamadev --only route
python3 tests/crew_probe.py ./build/cli/ollamadev --only security,learn,dedupe
```

The crew probe exists because three brains shipped broken in the same way —
each reported success while producing nothing (`--security` wrote a report with
no analysis, `--learn` wrote memory into a doomed sandbox, `--dedupe` judged
duplication from filenames). None of that is reachable from `odv-tests`, which
must stay offline and instant, so it lives here instead.

The suite enumerates commands from `ollamadev help` rather than a list of its
own, so the per-command invariants — help exists, `help X` == `X --help`, global
flags mean the same on either side, a rejected line writes nothing to stdout —
automatically cover any command added later.

## License

AGPL-3.0-or-later. See [LICENSE](LICENSE).
