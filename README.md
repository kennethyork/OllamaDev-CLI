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
./build/tests/odv-tests   # 200 assertions
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
  error with a suggestion. Flags after `git`, `mcp` and `terminal` belong to the
  program being wrapped and are passed straight through.

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

## Layout

```
core/    the engine — agent loop, tools, crew, backends, git, MCP, LSP, …
cli/     the CLI + REPL (main.cpp, Repl.cpp)
tests/   smoke suite (odv-tests)
```

## License

AGPL-3.0-or-later. See [LICENSE](LICENSE).
