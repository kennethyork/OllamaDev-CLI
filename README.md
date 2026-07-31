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
```

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
