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
./build/tests/odv-tests   # 366 assertions
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

## Layout

```
core/    the engine — agent loop, tools, crew, backends, git, MCP, LSP, …
cli/     the CLI + REPL (main.cpp, Repl.cpp)
tests/   smoke suite (odv-tests)
```

## License

AGPL-3.0-or-later. See [LICENSE](LICENSE).
