#!/bin/sh
# context-edit: add, edit body, rename title, duplicate rejection, remove.
set -eu
cd "$(dirname "$0")/../.."
BIN=./flower
[ -x "$BIN" ] || { echo "$BIN not found - build it first: make" >&2; exit 1; }

export FLOWER_BASE="$(mktemp -d)"
echo "== throwaway base: $FLOWER_BASE"

echo "== project + context add"
$BIN project P1 set Name "flower"
$BIN project P1 context add Fact "configuration location" "Configuration always happens in directory X"
$BIN project P1 context list

echo "== context edit body"
$BIN project P1 context edit Fact "configuration location" "New body text"
$BIN project P1 context list

echo "== context rename title"
$BIN project P1 context rename Fact "configuration location" "config location"
$BIN project P1 context list

echo "== duplicate prefix+title add (expect exit 1)"
if $BIN project P1 context add Fact "config location" "clash"; then
    echo "FAIL: should error" >&2; exit 1
fi
echo "rejected as expected"

echo "== context remove"
$BIN project P1 context remove Fact "config location"
$BIN project P1 context list
