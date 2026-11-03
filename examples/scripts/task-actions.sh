#!/bin/sh
# task-actions: task T1 with three actions, dependencies, status, listing.
set -eu
cd "$(dirname "$0")/../.."
BIN=./flower
[ -x "$BIN" ] || { echo "$BIN not found - build it first: make" >&2; exit 1; }

export FLOWER_BASE="$(mktemp -d)"
echo "== throwaway base: $FLOWER_BASE"

echo "== task T1 set Name/Description"
$BIN project P1 task T1 set Name "mix ingredients"
$BIN project P1 task T1 set Description "Combine water and flour"

echo "== action add x3 (A2 with --description)"
$BIN project P1 task T1 action add "take water"
$BIN project P1 task T1 action add "take flour" --description "Flour is in the cupboard."
$BIN project P1 task T1 action add "mix water and flour"

echo "== A3 set Depends, A1 set Status complete"
$BIN project P1 task T1 action A3 set Depends "A1, A2"
$BIN project P1 task T1 action A1 set Status "complete"

echo
echo "== action list:"
$BIN project P1 task T1 action list

echo "== action list --pending:"
$BIN project P1 task T1 action list --pending
