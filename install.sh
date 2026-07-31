#!/usr/bin/env bash
# Build and install the OllamaDev CLI into ~/.local.
#
# This exists because of a genuinely nasty failure mode: the binary on your PATH
# and the binary you just built are two different files. You change something, you
# build it, you run `ollamadev` — and you are running last week's copy, wondering
# why your change did nothing. One command, and the thing on your PATH is the
# thing you just built.
set -euo pipefail

cd "$(dirname "$0")"
PREFIX="${1:-$HOME/.local}"

echo "building…"
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release >/dev/null
cmake --build build -j"$(nproc)"

echo "testing…"
./build/tests/odv-tests | tail -1

echo "installing to $PREFIX…"
cmake --install build --prefix "$PREFIX" >/dev/null

echo
echo "installed:"
echo "  $PREFIX/bin/ollamadev        $("$PREFIX/bin/ollamadev" --version)"

case ":$PATH:" in
    *":$PREFIX/bin:"*) ;;
    *) echo
       echo "WARNING: $PREFIX/bin is not on your PATH, so the shell will keep using"
       echo "         whatever older ollamadev it finds first. Add this to ~/.bashrc:"
       echo "           export PATH=\"$PREFIX/bin:\$PATH\"" ;;
esac
