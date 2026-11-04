#!/bin/sh
# smoke test for flower command layer part 3 (plan.md task 6):
# llm profiles in $FLOWER_BASE/llms.md. runs the docs/config_cli.md
# examples against a throwaway FLOWER_BASE.
set -e
cd "$(dirname "$0")/.."
export FLOWER_BASE="$(mktemp -d)"
echo "== base: $FLOWER_BASE"

hdr() { echo; echo "== $*"; }

hdr "llm add with full flags + cat llms.md"
./flower llm add "claude" --base "https://api.anthropic.com" --protocol anthropic --key "sk-..." --model "claude-sonnet-4-5" --max-tokens 8192 --reasoning-effort high --header "X-Org=acme" --mcp-proxy "mcp.json" --terminal-tool "shell.exec"
./flower llm add "gpt" --base "https://api.openai.com" --protocol openai --header "X-A=1" --header "X-B=2"
cat "$FLOWER_BASE/llms.md"

hdr "llm list"
./flower llm list

hdr "llm show one profile"
./flower llm claude show

hdr "llm set Reasoning effort + show"
./flower llm claude set "Reasoning effort" "low"
./flower llm claude show

hdr "llm set bad Protocol (expect fail)"
./flower llm claude set Protocol "bogus" && echo "FAIL: should error" || echo "ok: rejected"

hdr "llm add duplicate name (expect fail)"
./flower llm add "claude" --base "https://x" --protocol openai && echo "FAIL: should error" || echo "ok: rejected"

hdr "llm unset Header (both occurrences) + show"
./flower llm gpt unset Header
./flower llm gpt show

hdr "llm set unknown key (expect fail)"
./flower llm gpt set "Bogus key" "x" && echo "FAIL" || echo "ok: rejected"

hdr "llm unset on missing profile (expect fail)"
./flower llm nope unset Header && echo "FAIL" || echo "ok: rejected"

hdr "llm remove one profile + list"
./flower llm gpt remove
./flower llm list

hdr "llm add bad profile name (expect fail)"
./flower llm add "9bad" --base "https://x" --protocol openai && echo "FAIL" || echo "ok: rejected"

hdr "final file"
cat "$FLOWER_BASE/llms.md"

echo
echo "ALL SMOKE STEPS RAN"
