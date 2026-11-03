#!/bin/sh
# setup: initial flower.md creation from piped name and role.
set -eu
cd "$(dirname "$0")/../.."
BIN=./flower
[ -x "$BIN" ] || { echo "$BIN not found - build it first: make" >&2; exit 1; }

export FLOWER_BASE="$(mktemp -d)"
echo "== throwaway base: $FLOWER_BASE"
echo "== setup: pipe Name then Role on stdin"

printf 'Daniele\nSoftware Developer\n' | $BIN setup

echo
echo "== resulting $FLOWER_BASE/flower.md:"
cat "$FLOWER_BASE/flower.md"
