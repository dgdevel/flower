#!/bin/sh
# project: create P1, set details/context/resources, then show them.
set -eu
cd "$(dirname "$0")/../.."
BIN=./flower
[ -x "$BIN" ] || { echo "$BIN not found - build it first: make" >&2; exit 1; }

export FLOWER_BASE="$(mktemp -d)"
echo "== throwaway base: $FLOWER_BASE"

echo "== project P1 set Name/Path/Description"
$BIN project P1 set Name "flower"
$BIN project P1 set Path "/home/user/projects/flower"
$BIN project P1 set Description "A project description"

echo "== details set"
$BIN project P1 details set Output "The project output are csv files."
$BIN project P1 details set Stakeholders "Team A, Team B"

echo "== context add Fact + Rule"
$BIN project P1 context add Fact "configuration location" "Configuration always happens in directory X"
$BIN project P1 context add Rule "temporary files" "Temp files are discarded automatically at process termination"

echo "== resource add (two)"
$BIN project P1 resource add "staging webapp url" "https://www.example.org/"
$BIN project P1 resource add "bulk csv generator" "src/product/feeds/generator.py#generate_csv"

echo
echo "== project P1 show:"
$BIN project P1 show

echo
echo "== $FLOWER_BASE/projects/P1/context.md:"
cat "$FLOWER_BASE/projects/P1/context.md"

echo
echo "== resource list:"
$BIN project P1 resource list
