#!/bin/sh
# smoke test for flower command layer part 4 (plan.md task 4 + 5):
# `flower mcp` stdio json-rpc servers and `project scan` dry-run +
# error paths in a throwaway FLOWER_BASE. no network access needed.
set -e
cd "$(dirname "$0")/.."
export FLOWER_BASE="$(mktemp -d)"
echo "== base: $FLOWER_BASE"

hdr() { echo; echo "== $*"; }

./flower project P1 set Name "flower"

hdr "mcp usage guards (expect fails)"
./flower mcp P1 bogus </dev/null && echo "FAIL" || echo "ok: rejected"
./flower mcp P </dev/null && echo "FAIL" || echo "ok: rejected"
./flower mcp P9 details </dev/null && echo "FAIL" || echo "ok: rejected"
./flower mcp Px details </dev/null && echo "FAIL" || echo "ok: rejected"

hdr "initialize echoes a supported protocolVersion"
printf '%s\n' \
    '{"jsonrpc":"2.0","id":1,"method":"initialize","params":{"protocolVersion":"2025-03-26"}}' \
    '{"jsonrpc":"2.0","id":2,"method":"initialize","params":{"protocolVersion":"1999-01-01"}}' \
    | ./flower mcp P1 details \
    | grep -c '"protocolVersion":"2025-03-26"'
printf '%s\n' \
    '{"jsonrpc":"2.0","id":1,"method":"initialize","params":{"protocolVersion":"1999-01-01"}}' \
    | ./flower mcp P1 details \
    | grep -c '"protocolVersion":"2025-11-25"'
printf '%s\n' \
    '{"jsonrpc":"2.0","id":1,"method":"initialize","params":{}}' \
    | ./flower mcp P1 context | grep -c '"name":"flower-context"'

hdr "notifications stay silent, ping with id answers"
n=$(printf '%s\n' \
    '{"jsonrpc":"2.0","method":"notifications/initialized"}' \
    '{"jsonrpc":"2.0","method":"ping"}' \
    | ./flower mcp P1 details | wc -l)
[ "$n" -eq 0 ] && echo "ok: no reply" || echo "FAIL: got $n lines"

hdr "tools/list: names, nonempty descriptions, schemas"
./flower mcp P1 details </dev/null 2>/dev/null || true
printf '%s\n' \
    '{"jsonrpc":"2.0","id":1,"method":"tools/list"}' \
    | ./flower mcp P1 details > "$FLOWER_BASE/out1"
for t in details_get details_set details_remove; do
    grep -q "\"name\":\"$t\",\"description\":\"" "$FLOWER_BASE/out1" ||
        { echo "FAIL: $t missing/description empty"; exit 1; }
done
grep -q '"required":\["area"\]' "$FLOWER_BASE/out1" || {
    echo "FAIL: details_get schema"; exit 1; }
grep -q '"required":\["area","value"\]' "$FLOWER_BASE/out1" || {
    echo "FAIL: details_set schema"; exit 1; }
echo "ok: details tool schemas"
printf '%s\n' '{"jsonrpc":"2.0","id":1,"method":"tools/list"}' \
    | ./flower mcp P1 context > "$FLOWER_BASE/out2"
for t in context_list context_get context_add context_edit \
    context_remove; do
    grep -q "\"name\":\"$t\",\"description\":\"" "$FLOWER_BASE/out2" ||
        { echo "FAIL: $t missing"; exit 1; }
done
printf '%s\n' '{"jsonrpc":"2.0","id":1,"method":"tools/list"}' \
    | ./flower mcp P1 resource > "$FLOWER_BASE/out3"
for t in resource_list resource_get resource_add resource_set \
    resource_remove; do
    grep -q "\"name\":\"$t\",\"description\":\"" "$FLOWER_BASE/out3" ||
        { echo "FAIL: $t missing"; exit 1; }
done
echo "ok: context + resource tool tables"

hdr "details set then get returns the text"
printf '%s\n' \
    '{"jsonrpc":"2.0","id":1,"method":"tools/call","params":{"name":"details_set","arguments":{"area":"Output","value":"csv files"}}}' \
    '{"jsonrpc":"2.0","id":2,"method":"tools/call","params":{"name":"details_get","arguments":{"area":"Output"}}}' \
    | ./flower mcp P1 details > "$FLOWER_BASE/out4"
grep -q '"text":"csv files[\\]n"' "$FLOWER_BASE/out4" ||
    { echo "FAIL: details_get did not return the text"; cat "$FLOWER_BASE/out4"; exit 1; }
echo "ok: set + get"
printf '%s\n' \
    '{"jsonrpc":"2.0","id":1,"method":"tools/call","params":{"name":"details_get","arguments":{"area":"Objectives"}}}' \
    '{"jsonrpc":"2.0","id":2,"method":"tools/call","params":{"name":"details_get","arguments":{"area":"Outpt"}}}' \
    '{"jsonrpc":"2.0","id":3,"method":"tools/call","params":{"name":"details_set","arguments":{"value":"x"}}}' \
    | ./flower mcp P1 details > "$FLOWER_BASE/out5"
grep -q 'no such field .Objectives' "$FLOWER_BASE/out5" ||
    { echo "FAIL: missing field"; exit 1; }
grep -q 'unknown field' "$FLOWER_BASE/out5" ||
    { echo "FAIL: unknown field"; exit 1; }
grep -q "missing argument 'area'" "$FLOWER_BASE/out5" ||
    { echo "FAIL: missing arg"; exit 1; }
echo "ok: no such field / unknown field / missing arg are isError"
printf '%s\n' \
    '{"jsonrpc":"2.0","id":1,"method":"tools/call","params":{"name":"details_remove","arguments":{"area":"Output"}}}' \
    '{"jsonrpc":"2.0","id":2,"method":"tools/call","params":{"name":"details_remove","arguments":{"area":"Output"}}}' \
    | ./flower mcp P1 details > "$FLOWER_BASE/out6"
grep -q '"isError":true' "$FLOWER_BASE/out6" ||
    { echo "FAIL: remove twice should fail"; exit 1; }
echo "ok: remove then remove again errors"

hdr "context add/get/edit/remove lifecycle"
printf '%s\n' \
    '{"jsonrpc":"2.0","id":1,"method":"tools/call","params":{"name":"context_add","arguments":{"prefix":"Fact","title":"config location","body":"Configuration lives in directory X"}}}' \
    '{"jsonrpc":"2.0","id":2,"method":"tools/call","params":{"name":"context_list","arguments":{}}}' \
    '{"jsonrpc":"2.0","id":3,"method":"tools/call","params":{"name":"context_get","arguments":{"prefix":"Fact","title":"config location"}}}' \
    | ./flower mcp P1 context > "$FLOWER_BASE/out7"
grep -q '"text":"Fact: config location[\\]n"' "$FLOWER_BASE/out7" ||
    { echo "FAIL: context_list"; cat "$FLOWER_BASE/out7"; exit 1; }
grep -q 'Configuration lives in directory X' "$FLOWER_BASE/out7" ||
    { echo "FAIL: context_get"; exit 1; }
echo "ok: add + list + get"
printf '%s\n' \
    '{"jsonrpc":"2.0","id":1,"method":"tools/call","params":{"name":"context_edit","arguments":{"prefix":"Fact","title":"config location","body":"New body"}}}' \
    '{"jsonrpc":"2.0","id":2,"method":"tools/call","params":{"name":"context_get","arguments":{"prefix":"Fact","title":"config location"}}}' \
    | ./flower mcp P1 context > "$FLOWER_BASE/out8"
grep -q '"text":"New body[\\]n"' "$FLOWER_BASE/out8" ||
    { echo "FAIL: edit did not stick"; exit 1; }
echo "ok: edit"
printf '%s\n' \
    '{"jsonrpc":"2.0","id":1,"method":"tools/call","params":{"name":"context_add","arguments":{"prefix":"Risk","title":"config location","body":"clash"}}}' \
    | ./flower mcp P1 context | grep -q 'duplicate context title' ||
    { echo "FAIL: dup title not rejected"; exit 1; }
printf '%s\n' \
    '{"jsonrpc":"2.0","id":1,"method":"tools/call","params":{"name":"context_add","arguments":{"prefix":"Fct","title":"x","body":"y"}}}' \
    | ./flower mcp P1 context | grep -q "unknown prefix" ||
    { echo "FAIL: bad prefix not rejected"; exit 1; }
echo "ok: duplicate title + unknown prefix are isError"
printf '%s\n' \
    '{"jsonrpc":"2.0","id":1,"method":"tools/call","params":{"name":"context_remove","arguments":{"prefix":"Fact","title":"config location"}}}' \
    '{"jsonrpc":"2.0","id":2,"method":"tools/call","params":{"name":"context_get","arguments":{"prefix":"Fact","title":"config location"}}}' \
    | ./flower mcp P1 context > "$FLOWER_BASE/out9"
grep -q 'no such context' "$FLOWER_BASE/out9" ||
    { echo "FAIL: remove left it behind"; exit 1; }
echo "ok: remove"

hdr "resource add/get/set/remove lifecycle"
printf '%s\n' \
    '{"jsonrpc":"2.0","id":1,"method":"tools/call","params":{"name":"resource_add","arguments":{"name":"staging webapp url","pointer":"https://www.example.org/"}}}' \
    '{"jsonrpc":"2.0","id":2,"method":"tools/call","params":{"name":"resource_get","arguments":{"name":"staging webapp url"}}}' \
    '{"jsonrpc":"2.0","id":3,"method":"tools/call","params":{"name":"resource_list","arguments":{}}}' \
    | ./flower mcp P1 resource > "$FLOWER_BASE/out10"
grep -q '"text":"https://www.example.org/[\\]n"' "$FLOWER_BASE/out10" ||
    { echo "FAIL: resource_get"; cat "$FLOWER_BASE/out10"; exit 1; }
grep -q '"text":"staging webapp url[\\]n  https://www.example.org/[\\]n"' \
    "$FLOWER_BASE/out10" ||
    { echo "FAIL: resource_list"; exit 1; }
echo "ok: add + get + list"
printf '%s\n' \
    '{"jsonrpc":"2.0","id":1,"method":"tools/call","params":{"name":"resource_add","arguments":{"name":"staging webapp url","pointer":"http://x"}}}' \
    | ./flower mcp P1 resource | grep -q 'duplicate resource' ||
    { echo "FAIL: dup resource not rejected"; exit 1; }
printf '%s\n' \
    '{"jsonrpc":"2.0","id":1,"method":"tools/call","params":{"name":"resource_set","arguments":{"name":"nope","pointer":"http://x"}}}' \
    | ./flower mcp P1 resource | grep -q 'no such resource' ||
    { echo "FAIL: set missing not rejected"; exit 1; }
echo "ok: duplicate add + set missing are isError"
printf '%s\n' \
    '{"jsonrpc":"2.0","id":1,"method":"tools/call","params":{"name":"resource_set","arguments":{"name":"staging webapp url","pointer":"https://staging.example.org/"}}}' \
    '{"jsonrpc":"2.0","id":2,"method":"tools/call","params":{"name":"resource_remove","arguments":{"name":"staging webapp url"}}}' \
    '{"jsonrpc":"2.0","id":3,"method":"tools/call","params":{"name":"resource_get","arguments":{"name":"staging webapp url"}}}' \
    | ./flower mcp P1 resource > "$FLOWER_BASE/out11"
grep -q 'no such resource' "$FLOWER_BASE/out11" ||
    { echo "FAIL: remove left it behind"; exit 1; }
echo "ok: set + remove"

hdr "rpc level errors"
printf '%s\n' \
    '{"jsonrpc":"2.0","id":7,"method":"tools/call","params":{"name":"nope","arguments":{}}}' \
    | ./flower mcp P1 details | grep -q '"code":-32602' ||
    { echo "FAIL: unknown tool"; exit 1; }
printf '%s\n' \
    '{"jsonrpc":"2.0","id":8,"method":"bogus/method"}' \
    | ./flower mcp P1 details | grep -q '"code":-32601' ||
    { echo "FAIL: unknown method"; exit 1; }
printf '%s\n' \
    '{"jsonrpc":"2.0","id":"str-id","method":"ping"}' \
    | ./flower mcp P1 details | grep -q '"id":"str-id"' ||
    { echo "FAIL: string id echo"; exit 1; }
echo "ok: -32602 / -32601 / string id echoed"

hdr "saved files on disk"
cat "$FLOWER_BASE/projects/P1/details.md"
cat "$FLOWER_BASE/projects/P1/context.md"

echo
hdr "scan: setup profiles"
./flower llm add gpt --base "https://api.openai.com/v1" --protocol openai \
    --key k1 --model m1 --max-tokens 512 --header "X-Org=acme"
./flower llm add ant --base "https://api.anthropic.com" --protocol anthropic
./flower llm add hdr --base "https://x.example/v1" --protocol openai \
    --header "broken"
./flower project P1 details set Output "old output line"

hdr "scan dry-run: staged record shape"
./flower project P1 scan gpt --dry-run > scan-dry.jsonl 2> scan-dry.err
# header, llm, tools + three stages of (system, user, flush)
test "$(grep -c "" scan-dry.jsonl)" = 12
grep -q '"type":"header","version":1' scan-dry.jsonl
sed -n 2p scan-dry.jsonl | grep -q '"endpoint_protocol":"openai"'
sed -n 2p scan-dry.jsonl | grep -q '"api_base":"https://api.openai.com/v1"'
sed -n 2p scan-dry.jsonl | grep -q '"api_key":"k1"'
sed -n 2p scan-dry.jsonl | grep -q '"headers":{"X-Org":"acme"}'
sed -n 2p scan-dry.jsonl | grep -q '"max_tokens":512'
sed -n 3p scan-dry.jsonl | grep -q '"name":"details"'
sed -n 3p scan-dry.jsonl | grep -q 'mcp P1 details'
sed -n 3p scan-dry.jsonl | grep -q 'mcp P1 context'
sed -n 3p scan-dry.jsonl | grep -q 'mcp P1 resource'
sed -n 3p scan-dry.jsonl | grep -q 'agent-as-tool'
sed -n 3p scan-dry.jsonl | grep -q 'scout-seed.jsonl'
for n in 6 9 12; do sed -n "${n}p" scan-dry.jsonl | grep -q '"type":"flush"'; done
python3 - "$FLOWER_BASE" <<'EOF'
import json, sys
lines = open("scan-dry.jsonl").read().splitlines()
def text(i): return json.loads(lines[i])["content"][0]["text"]
s1, s2, s3 = text(3), text(6), text(9)
assert s1.startswith("you are a project scanner"), "preamble head"
assert "stage 1 of 3" in s1 and "[STEP_COMPLETE]" in s1
assert "# Current details.md" in s1, "details embedded in stage 1"
assert "## Output" in s1 and "old output line" in s1, "shifted details body"
assert "# Current context.md" not in s1, "no other files in stage 1"
assert text(4).startswith("begin stage 1")
assert "stage 2 of 3" in s2 and "# Current context.md" in s2
assert "stage 3 of 3" in s3 and "# Current resources.md" in s3
print("ok: staged system records")
EOF
echo "ok: dry-run records"
# runner-input.jsonl is gone: the records go to the runner's stdin live
test ! -f "$FLOWER_BASE/projects/P1/scan/runner-input.jsonl"

hdr "scan seed file"
SEED="$FLOWER_BASE/projects/P1/scan/scout-seed.jsonl"
test -f "$SEED"
test "$(stat -c %a "$SEED")" = 600
grep -q 'builtin-mcp --read-only' "$SEED"
grep -q '"type":"agent-as-tool"' "$SEED"
grep -q '"retain_context":true' "$SEED"
grep -q '"tool_description"' "$SEED"
grep -q '"type":"system"' "$SEED"
echo "ok: seed"

hdr "scan error paths (expect fails)"
./flower project P1 scan nosuch 2>&1 | grep -q "no such profile"
./flower project P1 scan ant 2>&1 | grep -q "requires Max tokens"
./flower project P1 scan hdr 2>&1 | grep -q "is not <name>=<value>"
./flower project P9 scan gpt --dry-run > scan-p9.jsonl 2> scan-p9.err
grep -q "created project P9" scan-p9.err
test -f "$FLOWER_BASE/projects/P9/config.md"
grep -q "^Name: $(basename "$PWD")$" "$FLOWER_BASE/projects/P9/config.md"
grep -q "^Path: $PWD$" "$FLOWER_BASE/projects/P9/config.md"
grep -q '"type":"header","version":1' scan-p9.jsonl
echo "ok: scan errors"

hdr "scan session transcript (fake llmkit, staged)"
rc=0
mkdir -p fakellm
cat > fakellm/llmkit <<'FAKE'
#!/usr/bin/env python3
# fake llmkit: two subcommands.
# runner: reads stdin incrementally (proves the records are fed live,
# not from a file), answers each flush with a start marker and ends
# every stage turn with [STEP_COMPLETE]. no network.
# agent-as-tool <seed>: reads the seed, tees every seed record into the
# path named by options.transcript (what the real subagent does at
# startup), then exits on stdin eof. no network.
import sys, json
if len(sys.argv) > 1 and sys.argv[1] == "agent-as-tool":
    tee = None
    seed = open(sys.argv[2]).read().splitlines()
    for line in seed:
        rec = json.loads(line)
        if rec.get("type") == "options":
            tee = rec.get("transcript")
    if not tee:
        sys.exit(1)
    with open(tee, "w") as f:
        for line in seed:
            f.write(line + "\n")
    sys.exit(0)
stage = 0
for raw in sys.stdin:
    line = raw.strip()
    if not line:
        continue
    rec = json.loads(line)
    t = rec.get("type")
    if t == "system":
        txt = rec["content"][0]["text"]
        if "stage 1 of 3" in txt:
            stage = 1
        elif "stage 2 of 3" in txt:
            stage = 2
        elif "stage 3 of 3" in txt:
            stage = 3
    elif t == "flush":
        print(json.dumps({"type": "start"}), flush=True)
        print(json.dumps({"type": "response",
                          "text": "stage %d done [STEP_COMPLETE]" % stage,
                          "finish_reason": "stop"}), flush=True)
sys.exit(0)
FAKE
chmod +x fakellm/llmkit
./flower llm add mock --base "http://localhost:9/v1" --protocol openai \
    --model m1 --max-tokens 64
FLOWER_LLMKIT="$PWD/fakellm/llmkit" ./flower project P1 scan mock >scan-out.jsonl || rc=$?
test "$rc" = 0
# stdout carries the runner output only: three start/response pairs
test "$(grep -c "" scan-out.jsonl)" = 6
grep -q '"type": *"start"' scan-out.jsonl
grep -q 'stage 1 done \[STEP_COMPLETE\]' scan-out.jsonl
grep -q 'stage 2 done \[STEP_COMPLETE\]' scan-out.jsonl
grep -q 'stage 3 done \[STEP_COMPLETE\]' scan-out.jsonl
SESSDIR="$FLOWER_BASE/projects/P1/llm-sessions"
SESS=$(ls "$SESSDIR"/scan-*.jsonl | grep -v -- -scout | head -1)
test "$(stat -c %a "$SESS")" = 600
# initial records (3) + per stage (system, user, flush, start, response)
test "$(grep -c "" "$SESS")" = 18
head -1 "$SESS" | grep -q '"type":"header","version":1'
sed -n 2p "$SESS" | grep -q '"type":"llm"'
python3 - "$SESS" <<'EOF'
import json, sys
lines = open(sys.argv[1]).read().splitlines()
seq = [json.loads(l)["type"] for l in lines]
assert seq == ["header", "llm", "tools",
               "system", "user", "flush", "start", "response",
               "system", "user", "flush", "start", "response",
               "system", "user", "flush", "start", "response"], seq
print("ok: staged transcript")
EOF
FLOWER_LLMKIT="$PWD/fakellm/llmkit" ./flower project P1 scan mock >/dev/null 2>&1 || true
test "$(ls "$SESSDIR" | grep -c scan-)" = 4
# the scout tee: one per run. flower creates the file (0600, like the
# session); the subagent's own llmkit fills it at startup. the fake
# runner spawns nothing, so exercise the agent-as-tool subcommand
# directly: it tees the seed records into the options.transcript path
test "$(ls "$SESSDIR" | grep -c -- '-scout.jsonl')" = 2
"$PWD/fakellm/llmkit" agent-as-tool "$FLOWER_BASE/projects/P1/scan/scout-seed.jsonl"
SCOUT=$(python3 -c 'import json,sys
for l in open(sys.argv[1]):
    r = json.loads(l)
    if r.get("type") == "options":
        print(r["transcript"]); break' \
    "$FLOWER_BASE/projects/P1/scan/scout-seed.jsonl")
test "$(stat -c %a "$SCOUT")" = 600
grep -q '"type":"header","version":1' "$SCOUT"
grep -q '"retain_context":true' "$SCOUT"
grep -q '"agent-as-tool"' "$SCOUT"
echo "ok: session transcript"

hdr "scan resolves llmkit from PATH when FLOWER_LLMKIT is unset"
mkdir -p fakebin
cp fakellm/llmkit fakebin/llmkit
cat > fakellm/llmkit <<'FAKE'
#!/usr/bin/env python3
# same fake runner, reused via PATH: proves execlp finds a bare name
import sys, json
stage = 0
for raw in sys.stdin:
    line = raw.strip()
    if not line: continue
    rec = json.loads(line)
    t = rec.get("type")
    if t == "system":
        txt = rec["content"][0]["text"]
        if "stage 1 of 3" in txt: stage = 1
        elif "stage 2 of 3" in txt: stage = 2
        elif "stage 3 of 3" in txt: stage = 3
    elif t == "flush":
        print(json.dumps({"type": "start"}), flush=True)
        print(json.dumps({"type": "response",
                          "text": "stage %d done [STEP_COMPLETE]" % stage,
                          "finish_reason": "stop"}), flush=True)
sys.exit(0)
FAKE
chmod +x fakellm/llmkit
env -u FLOWER_LLMKIT PATH="$PWD/fakebin:$PATH" \
    ./flower project P1 scan mock >scan-out3.jsonl || rc=$?
test "$rc" = 0
test "$(grep -c "" scan-out3.jsonl)" = 6
grep -q 'stage 3 done \[STEP_COMPLETE\]' scan-out3.jsonl
rm -rf fakebin scan-out3.jsonl
echo "ok: PATH lookup"

hdr "scan error path: runner dies mid-stage (expect fail)"
cat > fakellm/llmkit <<'FAKE'
#!/bin/sh
# fake llmkit: emits a fatal error before any stage can complete
printf '%s\n' '{"type":"header","version":1}'
printf '%s\n' '{"type":"error","code":"connect_failed","message":"fake","fatal":true}'
exit 3
FAKE
chmod +x fakellm/llmkit
FLOWER_LLMKIT="$PWD/fakellm/llmkit" ./flower project P1 scan mock >scan-out2.jsonl || rc=$?
test "$rc" = 3
grep -q '"code":"connect_failed"' scan-out2.jsonl
SESS=$(ls -t "$SESSDIR"/scan-*.jsonl | grep -v -- -scout | head -1)
tail -1 "$SESS" | grep -q '"code":"connect_failed"'
rm -rf fakellm scan-out.jsonl scan-out2.jsonl
echo "ok: mid-stage failure"

echo "ALL SMOKE STEPS RAN"
