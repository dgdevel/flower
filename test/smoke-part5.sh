#!/bin/sh
# smoke test for flower command layer part 5: `flower mcp <P> task <T>`
# servers (covered in part 4) plus `project task create` dry-run and
# fake-llmkit live session, in a throwaway FLOWER_BASE. no network.
set -e
cd "$(dirname "$0")/.."
export FLOWER_BASE="$(mktemp -d)"
echo "== base: $FLOWER_BASE"

hdr() { echo; echo "== $*"; }

./flower project P1 set Name "flower"
./flower project P1 set Path "$PWD"
./flower project P1 details set Output "old output line"
./flower project P1 context add Fact "configuration location" "Configuration always happens in directory X"
./flower project P1 resource add "staging webapp url" "https://www.example.org/"
./flower llm add mock --base "http://localhost:9/v1" --protocol openai \
    --model m1 --max-tokens 64

hdr "task create error paths (expect fails)"
./flower project P1 task create nosuch --dry-run </dev/null 2>&1 |
    grep -q "no such profile" || { echo "FAIL: profile"; exit 1; }
./flower project P9 task create mock --dry-run </dev/null 2>&1 |
    grep -q "no such project" || { echo "FAIL: project"; exit 1; }
./flower llm add other --base "http://localhost:9/v1" --protocol openai \
    >/dev/null
printf 'x\ny\n' | ./flower project P1 task create --dry-run 2>&1 |
    grep -q "multiple llm profiles" || { echo "FAIL: default"; exit 1; }
./flower llm other remove >/dev/null
printf '' | ./flower project P1 task create mock --dry-run 2>&1 |
    grep -q "aborted" || { echo "FAIL: eof abort"; exit 1; }
test ! -d "$FLOWER_BASE/projects/P1/tasks" &&
    echo "ok: eof created nothing" || { echo "FAIL: eof created a task"; exit 1; }
echo "ok: task create guards"

hdr "task create dry-run"
printf 'my task\ndo the thing\n' | ./flower project P1 task create mock \
    --dry-run > tc-dry.jsonl 2> tc-dry.err
grep -q "created task T1" tc-dry.err || { echo "FAIL: T1"; exit 1; }
grep -q "^Task name:" tc-dry.err || { echo "FAIL: prompt"; exit 1; }
sed -n 1p tc-dry.jsonl | grep -q '"type":"header","version":1'
sed -n 2p tc-dry.jsonl | grep -q '"type":"llm"'
sed -n 3p tc-dry.jsonl | grep -q '"tool_call_timeout":240'
sed -n 4p tc-dry.jsonl | grep -q '"name":"project-details"'
sed -n 4p tc-dry.jsonl | grep -q 'mcp P1 details'
sed -n 4p tc-dry.jsonl | grep -q '"name":"project-context"'
sed -n 4p tc-dry.jsonl | grep -q '"name":"project-resource"'
sed -n 4p tc-dry.jsonl | grep -q '"name":"task-config"'
sed -n 4p tc-dry.jsonl | grep -q 'mcp P1 task T1 config'
sed -n 4p tc-dry.jsonl | grep -q '"name":"task-context"'
sed -n 4p tc-dry.jsonl | grep -q '"name":"task-action"'
sed -n 4p tc-dry.jsonl | grep -q 'mcp P1 task T1 action'
sed -n 4p tc-dry.jsonl | grep -q '"name":"scout"'
sed -n 4p tc-dry.jsonl | grep -q 'agent-as-tool'
for n in 7 10; do sed -n "${n}p" tc-dry.jsonl | grep -q '"type":"flush"'; done
SEED="$FLOWER_BASE/projects/P1/tasks/T1/scout-seed.jsonl"
test -f "$SEED"
test "$(stat -c %a "$SEED")" = 600
grep -q 'builtin-mcp --read-only' "$SEED"
grep -q '"retain_context":true' "$SEED"
python3 - <<'EOF'
import json
lines = open("tc-dry.jsonl").read().splitlines()
recs = [json.loads(l) for l in lines]
seq = [r["type"] for r in recs]
assert seq == ["header", "llm", "options", "tools",
               "system", "user", "flush", "system", "user", "flush"], seq
s1, s2 = recs[4]["content"][0]["text"], recs[7]["content"][0]["text"]
assert s1.startswith("you are a task planner"), s1[:40]
assert "## The task" in s1 and "- id: T1" in s1, "task identity"
assert "- name: my task" in s1 and "- description: do the thing" in s1
assert "# Project config.md" in s1, "config embedded"
assert "# Project details.md" in s1, "details embedded"
assert "## Output" in s1 and "old output line" in s1, "shifted details"
assert "# Project context.md" in s1 and "## Fact: configuration location" in s1
assert "# Project resources.md" in s1 and "## staging webapp url" in s1
assert "# Current tasks/T1/config.md" in s1
assert "# Current tasks/T1/context.md" in s1
assert "# Project details.md" not in s2, "stage 2 lean"
assert "# Current tasks/T1/context.md" in s2
assert "# Current actions.md" in s2
assert recs[5]["content"][0]["text"].startswith("begin stage 1")
assert recs[8]["content"][0]["text"].startswith("begin stage 2")
print("ok: staged system records")
EOF
test "$(grep -c "" tc-dry.jsonl)" = 10
echo "ok: dry-run records + seed"

hdr "task create live session (fake llmkit, staged)"
rc=0
mkdir -p fakellm
cat > fakellm/llmkit <<'FAKE'
#!/usr/bin/env python3
# fake llmkit: answers each flush with a start marker and ends every
# stage turn with [STEP_COMPLETE]. no network.
import sys, json
stage = 0
for raw in sys.stdin:
    line = raw.strip()
    if not line:
        continue
    rec = json.loads(line)
    t = rec.get("type")
    if t == "system":
        txt = rec["content"][0]["text"]
        if "fill the task context" in txt:
            stage = 1
        elif "action hierarchy" in txt:
            stage = 2
    elif t == "flush":
        print(json.dumps({"type": "start"}), flush=True)
        print(json.dumps({"type": "response",
                          "text": "stage %d done [STEP_COMPLETE]" % stage,
                          "finish_reason": "stop"}), flush=True)
sys.exit(0)
FAKE
chmod +x fakellm/llmkit
printf 'second task\ndescribe it\n' |
    FLOWER_LLMKIT="$PWD/fakellm/llmkit" ./flower project P1 task create mock \
    >tc-live.jsonl 2>tc-live.err || rc=$?
test "$rc" = 0
grep -q "created task T2" tc-live.err
test "$(grep -c "" tc-live.jsonl)" = 4
SESSDIR="$FLOWER_BASE/projects/P1/llm-sessions"
SESS=$(ls "$SESSDIR"/taskcreate-*.jsonl | grep -v -- -scout | head -1)
test "$(stat -c %a "$SESS")" = 600
python3 - "$SESS" <<'EOF'
import json, sys
lines = open(sys.argv[1]).read().splitlines()
seq = [json.loads(l)["type"] for l in lines]
assert seq == ["header", "llm", "options", "tools",
               "system", "user", "flush", "start", "response",
               "system", "user", "flush", "start", "response"], seq
print("ok: staged transcript")
EOF
head -1 "$SESS" | grep -q '"type":"header","version":1'
sed -n 2p "$SESS" | grep -q '"type":"llm"'
test -f "$SESSDIR/$(basename "$SESS" .jsonl)-scout.jsonl" ||
    { echo "FAIL: scout twin"; exit 1; }
echo "ok: live session + transcripts"

hdr "the created task config"
cat "$FLOWER_BASE/projects/P1/tasks/T2/config.md"
grep -q "^Name: second task$" "$FLOWER_BASE/projects/P1/tasks/T2/config.md" ||
    { echo "FAIL: Name"; exit 1; }
grep -q "^Description: describe it$" \
    "$FLOWER_BASE/projects/P1/tasks/T2/config.md" ||
    { echo "FAIL: Description"; exit 1; }
echo "ok: task config"

hdr "task mcp servers over the created task"
printf '%s\n' '{"jsonrpc":"2.0","id":1,"method":"tools/list"}' |
    ./flower mcp P1 task T1 action > "$FLOWER_BASE/ta"
for t in action_add action_set action_remove action_list; do
    grep -q "\"name\":\"$t\",\"description\":\"" "$FLOWER_BASE/ta" ||
        { echo "FAIL: $t missing"; exit 1; }
done
echo "ok: action tools listed"

rm -f tc-dry.jsonl tc-dry.err tc-live.jsonl tc-live.err
echo
echo "== part 5 all ok"
