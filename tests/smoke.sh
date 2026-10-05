#!/usr/bin/env bash
# smoke.sh — end-to-end smoke tests for flower.
# Usage: tests/smoke.sh [path-to-binary]   (run from the repo root)
set -u

BIN=${1:-./flower}
PORT=${SMOKE_PORT:-8199}
CFG=$(mktemp -d)
LOG=$(mktemp)
PID=""
cleanup() { [ -n "$PID" ] && kill "$PID" 2>/dev/null; rm -rf "$CFG" "$LOG"; }
trap cleanup EXIT

fail() { echo "FAIL: $*" >&2; sed 's/^/  server: /' "$LOG" >&2; exit 1; }

[ -x "$BIN" ] || fail "binary not found: $BIN"
case "$BIN" in */*) ;; *) [ -x "./$BIN" ] && BIN="./$BIN" ;; esac

# a scripted llmkit stand-in: the scan-conversation tests below run it
mkdir -p "$CFG/bin"
cp tests/fakellmkit.py "$CFG/bin/llmkit"
chmod +x "$CFG/bin/llmkit"
LLMKIT="$CFG/bin/llmkit"

"$BIN" -p "$PORT" -c "$CFG" -l "$LLMKIT" >"$LOG" 2>&1 &
PID=$!

B=http://127.0.0.1:$PORT
for _ in $(seq 1 50); do
    curl -s -o /dev/null "$B/" 2>/dev/null && break
    sleep 0.1
done
curl -s -o /dev/null "$B/" || fail "server did not come up on $B"

echo "== 1. embedded assets are byte-identical, no stray bytes =="
for f in index.html config.html components.html conversations.html style.css theme.js config.js llms.js app.js conversations.js emoji.js; do
    curl -s "$B/$f" | cmp -s - "web/$f" || fail "$f: served bytes differ from web/$f"
done
for p in / /config.html /style.css /app.js /api/theme; do
    n=$(curl -s "$B$p" | tr -dc '\0' | wc -c)
    [ "$n" -eq 0 ] || fail "$p: $n NUL bytes in response body"
done

echo "== 2. default theme API =="
curl -s "$B/api/theme" | python3 -c '
import json, sys
d = json.load(sys.stdin)
keys = {"background_primary","background_secondary","background_tertiary",
        "text_primary_color","text_primary_font","text_primary_size",
        "text_secondary_color","text_secondary_font","text_secondary_size",
        "accent","success","warning"}
assert set(d) == keys, f"key mismatch: {set(d) ^ keys}"
for k in keys:
    if "color" in k or k in ("accent","success","warning","background_primary",
                             "background_secondary","background_tertiary"):
        assert d[k].startswith("#") and len(d[k]) == 7, f"bad color {k}={d[k]}"
' || fail "GET /api/theme returned an unexpected document"

echo "== 3. PUT a valid theme, verify persistence =="
curl -s -X PUT -H "Content-Type: application/json" --data-binary @- "$B/api/theme" <<'EOF' | grep -q '"accent":"#ff00ff"' || fail "PUT /api/theme did not echo the saved theme"
{"background_primary":"#101010","background_secondary":"#1a1a1a","background_tertiary":"#242424",
 "text_primary_color":"#eeeeee","text_primary_font":"Verdana, sans-serif","text_primary_size":"15px",
 "text_secondary_color":"#999999","text_secondary_font":"Georgia, serif","text_secondary_size":"12px",
 "accent":"#ff00ff","success":"#00cc66","warning":"#ff9900"}
EOF
[ -f "$CFG/theme.json" ] || fail "theme.json not written to config dir"
curl -s "$B/api/theme" | grep -q '"accent":"#ff00ff"' || fail "GET /api/theme after PUT does not reflect the change"

echo "== 4. validation rejects bad input =="
code=$(curl -s -o /dev/null -w '%{http_code}' -X PUT --data '{"accent":"nothex"}' "$B/api/theme")
[ "$code" = 422 ] || fail "invalid color: expected 422, got $code"
code=$(curl -s -o /dev/null -w '%{http_code}' -X PUT --data '{"nonsense":"x"}' "$B/api/theme")
[ "$code" = 422 ] || fail "unknown key: expected 422, got $code"
code=$(curl -s -o /dev/null -w '%{http_code}' -X PUT --data 'not json' "$B/api/theme")
[ "$code" = 400 ] || fail "malformed JSON: expected 400, got $code"
code=$(curl -s -o /dev/null -w '%{http_code}' -X PUT --data '{"text_primary_size":"9em; background:red"}' "$B/api/theme")
[ "$code" = 422 ] || fail "CSS injection in size: expected 422, got $code"
curl -s "$B/api/theme" | grep -q '"accent":"#ff00ff"' || fail "rejected PUT must not change the stored theme"

echo "== 5. reset restores defaults =="
curl -s -X POST "$B/api/theme/reset" | grep -q '"accent":"#58a6ff"' || fail "reset did not return defaults"
curl -s "$B/api/theme" | grep -q '"accent":"#58a6ff"' || fail "defaults not persisted after reset"

echo "== 6. projects API =="
curl -s "$B/api/projects" | grep -q '^\[\]$' || fail "GET /api/projects should start as []"
mkdir -p "$CFG/alpha" "$CFG/beta" "$CFG/vanishing"
curl -s -X PUT -H "Content-Type: application/json" --data-binary "[
 {\"dir\":\"$CFG/alpha\",\"title\":\"\",\"color\":\"#ff7b72\",\"emoji\":\"🐝\"},
 {\"dir\":\"$CFG/beta\"}]" "$B/api/projects" | python3 -c '
import json, sys
d = json.load(sys.stdin)
assert len(d) == 2, d
assert d[0]["title"] == "alpha", d      # title defaulted to the directory name
assert d[1]["title"] == "beta", d
assert d[1]["color"].startswith("#") and len(d[1]["color"]) == 7, d
assert d[1]["emoji"], d                 # color and emoji defaults applied
assert all("exists" not in p for p in d), d   # exists only on GET
' || fail "PUT /api/projects did not apply the defaults"
curl -s "$B/api/projects" | python3 -c '
import json, sys
d = json.load(sys.stdin)
for p in d:
    assert len(p["id"]) == 16, d                     # server-generated ids
    assert all(c in "0123456789abcdef" for c in p["id"]), d
assert d[0]["id"] != d[1]["id"], d                   # unique
assert [p["seq"] for p in d] == [1, 2], d            # sequential ids, in order
' || fail "projects did not get unique server-generated ids"
grep -q '"id"' "$CFG/projects.json" || fail "project ids not persisted"
grep -q '"seq"' "$CFG/projects.json" || fail "project seq not persisted"
code=$(curl -s -o /dev/null -w '%{http_code}' -X PUT --data "[{\"id\":\"zzz\",\"dir\":\"$CFG/alpha\"}]" "$B/api/projects")
[ "$code" = 422 ] || fail "projects: malformed id: expected 422, got $code"
[ -f "$CFG/projects.json" ] || fail "projects.json not written to config dir"
curl -s "$B/api/projects" | grep -q "$CFG/alpha" || fail "projects not persisted after PUT"

echo "== 6a. project details: round-trip and validation =="
curl -s -X PUT -H "Content-Type: application/json" --data-binary "
 [{\"dir\":\"$CFG/alpha\",\"title\":\"Alpha\",
   \"description\":\"two lines\nof context\",\"objectives\":\"ship it\",
   \"scope\":\"2 weeks\",\"stakeholders\":\"the bees\"}]" "$B/api/projects" | python3 -c '
import json, sys
d = json.load(sys.stdin)
assert d[0]["description"] == "two lines\nof context", d   # newlines allowed
assert d[0]["objectives"] == "ship it", d
assert d[0]["scope"] == "2 weeks", d
assert d[0]["stakeholders"] == "the bees", d
' || fail "PUT /api/projects did not keep the detail fields"
grep -q '"description"' "$CFG/projects.json" || fail "detail fields not persisted to projects.json"
curl -s "$B/api/projects" | python3 -c '
import json, sys
p = json.load(sys.stdin)[0]
assert p["description"] == "two lines\nof context", p   # survives GET (exists flag on)
assert p["seq"] == 1, p        # seq assigned; the client echoes it back on PUTs
' || fail "GET /api/projects lost the detail fields"
code=$(curl -s -o /dev/null -w '%{http_code}' -X PUT --data "[{\"dir\":\"$CFG/alpha\",\"description\":\"$(printf 'x%.0s' $(seq 1 4200))\"}]" "$B/api/projects")
[ "$code" = 422 ] || fail "projects: oversized description: expected 422, got $code"
code=$(curl -s -o /dev/null -w '%{http_code}' -X PUT --data "[{\"dir\":\"$CFG/alpha\",\"description\":\"bad\\u0001ctrl\"}]" "$B/api/projects")
[ "$code" = 422 ] || fail "projects: control character in a detail field: expected 422, got $code"
code=$(curl -s -o /dev/null -w '%{http_code}' -X PUT --data "[{\"dir\":\"$CFG/alpha\",\"scope\":5}]" "$B/api/projects")
[ "$code" = 422 ] || fail "projects: non-string detail field: expected 422, got $code"
curl -s "$B/api/projects" | grep -q '"the bees"' || fail "rejected PUTs must not change stored details"
# a seq echoed back is kept as-is (this PUT replaces the list; the
# sections after bring their own projects)
curl -s -X PUT --data "[{\"seq\":7,\"dir\":\"$CFG/alpha\"}]" "$B/api/projects" | python3 -c '
import json, sys
assert json.load(sys.stdin)[0]["seq"] == 7, "echoed seq"
' || fail "an echoed seq must be kept as-is"
code=$(curl -s -o /dev/null -w '%{http_code}' -X PUT --data 'not json' "$B/api/projects")
[ "$code" = 400 ] || fail "projects: malformed JSON: expected 400, got $code"
code=$(curl -s -o /dev/null -w '%{http_code}' -X PUT --data '{"dir":"/x"}' "$B/api/projects")
[ "$code" = 400 ] || fail "projects: object instead of array: expected 400, got $code"
code=$(curl -s -o /dev/null -w '%{http_code}' -X PUT --data '[{"dir":"relative"}]' "$B/api/projects")
[ "$code" = 422 ] || fail "projects: relative dir: expected 422, got $code"
code=$(curl -s -o /dev/null -w '%{http_code}' -X PUT --data "[{\"dir\":\"$CFG/missing-dir\"}]" "$B/api/projects")
[ "$code" = 422 ] || fail "projects: non-existent dir: expected 422, got $code"
code=$(curl -s -o /dev/null -w '%{http_code}' -X PUT --data "[{\"dir\":\"$CFG/alpha\"},{\"dir\":\"$CFG/alpha\"}]" "$B/api/projects")
[ "$code" = 422 ] || fail "projects: duplicate dir: expected 422, got $code"
code=$(curl -s -o /dev/null -w '%{http_code}' -X PUT --data "[{\"dir\":\"$CFG\",\"color\":\"red\"}]" "$B/api/projects")
[ "$code" = 422 ] || fail "projects: bad color: expected 422, got $code"
code=$(curl -s -o /dev/null -w '%{http_code}' -X PUT --data "[{\"dir\":\"$CFG\",\"nonsense\":1}]" "$B/api/projects")
[ "$code" = 422 ] || fail "projects: unknown key: expected 422, got $code"
code=$(curl -s -o /dev/null -w '%{http_code}' -X PUT --data "[{\"seq\":1,\"dir\":\"$CFG/alpha\"},{\"seq\":1,\"dir\":\"$CFG\"}]" "$B/api/projects")
[ "$code" = 422 ] || fail "projects: duplicate seq: expected 422, got $code"
code=$(curl -s -o /dev/null -w '%{http_code}' -X PUT --data "[{\"seq\":0,\"dir\":\"$CFG/alpha\"}]" "$B/api/projects")
[ "$code" = 422 ] || fail "projects: non-positive seq: expected 422, got $code"
curl -s "$B/api/projects" | grep -q "$CFG/alpha" || fail "rejected PUTs must not change the stored projects"

echo "== 6b. exists flag tracks the filesystem =="
curl -s -X PUT --data "[{\"dir\":\"$CFG/alpha\"},{\"dir\":\"$CFG/vanishing\"}]" "$B/api/projects" >/dev/null
curl -s "$B/api/projects" | python3 -c '
import json, sys
d = {p["dir"]: p["exists"] for p in json.load(sys.stdin)}
assert d["'"$CFG"'/alpha"] is True and d["'"$CFG"'/vanishing"] is True, d
' || fail "GET exists flags should be true while both dirs are on disk"
rmdir "$CFG/vanishing"
curl -s "$B/api/projects" | python3 -c '
import json, sys
d = {p["dir"]: p["exists"] for p in json.load(sys.stdin)}
assert d["'"$CFG"'/vanishing"] is False, d   # vanished dir is flagged, not dropped
' || fail "GET exists should be false after the directory disappears"
code=$(curl -s -o /dev/null -w '%{http_code}' -X PUT --data "[{\"dir\":\"$CFG/alpha\"},{\"dir\":\"$CFG/vanishing\"}]" "$B/api/projects")
[ "$code" = 422 ] || fail "PUT with a vanished dir: expected 422, got $code"
curl -s -X PUT --data '[]' "$B/api/projects" | grep -q '^\[\]$' || fail "PUT [] did not clear the list"

echo "== 6c. llms API =="
curl -s "$B/api/llms" | grep -q '^\[\]$' || fail "GET /api/llms should start as []"
curl -s -X PUT -H "Content-Type: application/json" --data-binary '[
 {"name":"ollama","endpoint_protocol":"openai","api_base":"http://localhost:11434/v1","model":"llama3.1"},
 {"name":"anthropic","endpoint_protocol":"anthropic","api_base":"https://api.anthropic.com/v1","api_key":"sk-x","headers":{"x-api-key":"k"}}]' "$B/api/llms" | python3 -c '
import json, sys
d = json.load(sys.stdin)
assert [x["name"] for x in d] == ["anthropic", "ollama"], d   # sorted by name
assert d[1]["model"] == "llama3.1", d
assert d[0]["headers"] == {"x-api-key": "k"}, d
' || fail "PUT /api/llms did not save and echo the list"
[ -f "$CFG/llms.json" ] || fail "llms.json not written to config dir"
curl -s "$B/api/llms" | grep -q '"name":"ollama"' || fail "GET /api/llms after PUT does not reflect the change"
code=$(curl -s -o /dev/null -w '%{http_code}' -X PUT --data 'not json' "$B/api/llms")
[ "$code" = 400 ] || fail "llms: malformed JSON: expected 400, got $code"
code=$(curl -s -o /dev/null -w '%{http_code}' -X PUT --data '{"name":"x"}' "$B/api/llms")
[ "$code" = 400 ] || fail "llms: object instead of array: expected 400, got $code"
code=$(curl -s -o /dev/null -w '%{http_code}' -X PUT --data '[{"name":"x","endpoint_protocol":"bogus","api_base":"http://a/"}]' "$B/api/llms")
[ "$code" = 422 ] || fail "llms: bad protocol: expected 422, got $code"
code=$(curl -s -o /dev/null -w '%{http_code}' -X PUT --data '[{"name":"x","endpoint_protocol":"openai","api_base":"ftp://a/"}]' "$B/api/llms")
[ "$code" = 422 ] || fail "llms: non-http api_base: expected 422, got $code"
code=$(curl -s -o /dev/null -w '%{http_code}' -X PUT --data '[{"name":"x","endpoint_protocol":"openai","api_base":"http://a/","zzz":1}]' "$B/api/llms")
[ "$code" = 422 ] || fail "llms: unknown key: expected 422, got $code"
code=$(curl -s -o /dev/null -w '%{http_code}' -X PUT --data '[{"name":"x","endpoint_protocol":"openai","api_base":"http://a/"},{"name":"X","endpoint_protocol":"openai","api_base":"http://b/"}]' "$B/api/llms")
[ "$code" = 422 ] || fail "llms: duplicate name: expected 422, got $code"
curl -s "$B/api/llms" | grep -q '"name":"ollama"' || fail "rejected PUTs must not change the stored llms"

echo "== 6d. agents API =="
curl -s "$B/api/agents" | python3 -c '
import json, sys
d = json.load(sys.stdin)
assert [a["name"] for a in d] == \
    ["assistant", "filesystem_researcher", "online_researcher",
     "project_scanner", "task_planner"], d  # builtins
assert d[0]["builtin"] is True and d[0]["llm"] == "", d
assert "llm_ok" not in d[0], d    # builtins pick their llm per task
for i in (1, 2):
    assert d[i]["builtin"] is True, d
assert d[1]["tools"][0]["name"] == "flower", d   # flower own mcp server
assert d[1]["tools"][0]["url"] == "/mcp", d      # resolved when a task runs
assert not d[2].get("tools"), d   # web tools: llmkit builtin-mcp, attached at scan time
assert "researcher" in d[1]["system_prompt"].lower(), d  # prompts compiled in
assert "filesystem" in d[1]["system_prompt"].lower(), d
assert "web" in d[2]["system_prompt"].lower(), d
assert d[3]["builtin"] is True, d
assert d[3]["tools"][0]["url"] == "/scan/mcp", d     # the scan mcp server
assert "project" in d[3]["system_prompt"].lower(), d
assert d[4]["builtin"] is True and not d[4].get("tools"), d  # planner: tools attached at plan time
assert "task" in d[4]["system_prompt"].lower(), d
' || fail "GET /api/agents should start with the builtin agents"
curl -s -X PUT -H "Content-Type: application/json" --data-binary '[
 {"name":"gardener","llm":"ollama","inference_options":{"temperature":0.7,"max_tokens":2048,"stop":"END"},"system_prompt":"You tend flowers.","tools":[{"type":"stdio","name":"fs","command_line":"npx -y @mcp/fs /tmp","required":true,"terminal_tools":["read_file"]}]},
 {"name":"thinker","llm":"anthropic","inference_options":{"max_tokens":4096,"thinking_budget":2048}}]' "$B/api/agents" | python3 -c '
import json, sys
d = json.load(sys.stdin)
assert [a["name"] for a in d] == ["gardener", "thinker"], d
assert d[0]["inference_options"]["stop"] == ["END"], d   # string form normalized to array
assert d[0]["tools"][0]["required"] is True, d
assert "llm_ok" not in d[0] or d[0]["llm_ok"], d         # PUT echo may omit, never false
' || fail "PUT /api/agents did not save and echo the list"
[ -f "$CFG/agents/gardener.json" ] || fail "agents/gardener.json not written"
[ -f "$CFG/agents/thinker.json" ] || fail "agents/thinker.json not written"
curl -s "$B/api/agents" | python3 -c '
import json, sys
d = {a["name"]: a.get("llm_ok") for a in json.load(sys.stdin)}
assert d == {"assistant": None, "filesystem_researcher": None,
             "online_researcher": None, "project_scanner": None,
             "task_planner": None,
             "gardener": True, "thinker": True}, d
' || fail "GET /api/agents should report llm_ok flags (builtins carry none)"
code=$(curl -s -o /dev/null -w '%{http_code}' -X PUT --data '[{"name":"Assistant","llm":"ollama"}]' "$B/api/agents")
[ "$code" = 422 ] || fail "agents: builtin name collision: expected 422, got $code"
code=$(curl -s -o /dev/null -w '%{http_code}' -X PUT --data '[{"name":"x","llm":"missing"}]' "$B/api/agents")
[ "$code" = 422 ] || fail "agents: unknown llm reference: expected 422, got $code"
code=$(curl -s -o /dev/null -w '%{http_code}' -X PUT --data '[{"name":"x","llm":"anthropic"}]' "$B/api/agents")
[ "$code" = 422 ] || fail "agents: anthropic without max_tokens: expected 422, got $code"
code=$(curl -s -o /dev/null -w '%{http_code}' -X PUT --data '[{"name":"x","llm":"ollama","tools":[{"type":"stdio","name":"fs"}]}]' "$B/api/agents")
[ "$code" = 422 ] || fail "agents: stdio without command_line: expected 422, got $code"
code=$(curl -s -o /dev/null -w '%{http_code}' -X PUT --data '[{"name":"x","llm":"ollama","tools":[{"type":"stdio","name":"fs","command_line":"ls"},{"type":"http","name":"fs","url":"http://a/"}]}]' "$B/api/agents")
[ "$code" = 422 ] || fail "agents: duplicate tool names: expected 422, got $code"
code=$(curl -s -o /dev/null -w '%{http_code}' -X PUT --data '[{"name":"x","llm":"ollama","prompt":"y"}]' "$B/api/agents")
[ "$code" = 422 ] || fail "agents: unknown key: expected 422, got $code"
curl -s -X PUT --data '[{"name":"thinker","llm":"anthropic","inference_options":{"max_tokens":4096}}]' "$B/api/agents" >/dev/null
[ ! -f "$CFG/agents/gardener.json" ] || fail "removing an agent must delete its file"
ls "$CFG/agents" | grep -qx 'thinker.json' || fail "the kept agent's file must stay"

echo "== 6d2. scan API (status + rejected starts) =="
curl -s "$B/api/scan" | python3 -c '
import json, sys
d = json.load(sys.stdin)
assert d["running"] is False and d["done"] is False, d  # idle, no result yet
' || fail "GET /api/scan should start idle"
code=$(curl -s -o /dev/null -w '%{http_code}' -X POST --data '{"project":"nope","llm":"ollama"}' "$B/api/scan")
[ "$code" = 422 ] || fail "scan: unknown project: expected 422, got $code"
code=$(curl -s -o /dev/null -w '%{http_code}' -X POST --data '{"project":"nope","llm":"missing"}' "$B/api/scan")
[ "$code" = 422 ] || fail "scan: unknown llm: expected 422, got $code"
curl -s "$B/api/scan" | grep -q '"running":false' || fail "rejected scans must not flip the status"

echo "== 6e. tasks API (actions per project) =="
curl -s "$B/api/tasks" | grep -q '^\[\]$' || fail "GET /api/tasks should start as []"
PRJ=$(curl -s -X PUT --data "[{\"dir\":\"$CFG/alpha\"}]" "$B/api/projects" | python3 -c '
import json, sys
print(json.load(sys.stdin)[0]["id"])')
code=$(curl -s -o /dev/null -w '%{http_code}' -X PUT --data '[{}]' "$B/api/tasks")
[ "$code" = 422 ] || fail "tasks: missing project: expected 422, got $code"
code=$(curl -s -o /dev/null -w '%{http_code}' -X PUT --data "[{\"project\":\"0123456789abcdef0\"}]" "$B/api/tasks")
[ "$code" = 422 ] || fail "tasks: malformed project id: expected 422, got $code"
# a well-formed reference to a deleted project must stay savable —
# the whole list is PUT after every edit, so rejecting it would
# block all further saves; GET flags the dangling reference instead
code=$(curl -s -o /dev/null -w '%{http_code}' -X PUT --data '[{"project":"0011223344556677","title":"orphan"}]' "$B/api/tasks")
[ "$code" = 200 ] || fail "tasks: dangling project reference must be accepted, got $code"
curl -s "$B/api/tasks" | python3 -c '
import json, sys
d = json.load(sys.stdin)
assert d[0]["project_ok"] is False, d
' || fail "GET /api/tasks should flag the dangling reference"
CID=$(curl -s -X PUT -H "Content-Type: application/json" --data-binary "[
 {\"project\":\"$PRJ\",\"title\":\"build\",\"created\":1000,\"actions\":[
   {\"title\":\"plan\",\"state\":\"in_progress\",\"children\":[
     {\"title\":\"sketch\",\"description\":\"rough first\",\"state\":\"completed\"},
     {\"title\":\"review\"}]},
   {\"title\":\"ship\",\"state\":\"partial\"}]},
 {\"project\":\"$PRJ\",\"title\":\"talk\",\"created\":2000}]" "$B/api/tasks" | python3 -c '
import json, sys
d = json.load(sys.stdin)
assert len(d) == 2, d
for c in d:
    assert len(c["id"]) == 32 and all(ch in "0123456789abcdef" for ch in c["id"]), d
    assert c["project"] == "'"$PRJ"'", d
assert d[0]["title"] == "talk" and d[0]["created"] == 2000, d  # newest first
a = d[1]["actions"]
assert a[0]["title"] == "plan" and a[0]["state"] == "in_progress", d
assert a[0]["children"][0]["state"] == "completed", d
assert a[0]["children"][0]["description"] == "rough first", d
assert "state" not in a[0]["children"][1] and "children" not in a[0]["children"][1], d  # defaults omitted
assert a[1]["state"] == "partial", d
print(d[1]["id"])')
[ -f "$CFG/tasks/$CID/task.json" ] || fail "tasks/$CID/task.json not written"
grep -q '"sketch"' "$CFG/tasks/$CID/task.json" || fail "nested actions not persisted"
curl -s "$B/api/tasks" | python3 -c '
import json, sys
d = {c["title"]: c for c in json.load(sys.stdin)}
assert d["build"]["project_ok"] is True, d
assert "agent" not in d["build"] and "llm" not in d["build"], d  # binding is gone
' || fail "GET /api/tasks should report the project_ok flag"
code=$(curl -s -o /dev/null -w '%{http_code}' -X PUT --data 'not json' "$B/api/tasks")
[ "$code" = 400 ] || fail "tasks: malformed JSON: expected 400, got $code"
code=$(curl -s -o /dev/null -w '%{http_code}' -X PUT --data "{\"project\":\"$PRJ\"}" "$B/api/tasks")
[ "$code" = 400 ] || fail "tasks: object instead of array: expected 400, got $code"
code=$(curl -s -o /dev/null -w '%{http_code}' -X PUT --data "[{\"project\":\"$PRJ\",\"actions\":[{\"title\":\"x\",\"state\":\"bogus\"}]}]" "$B/api/tasks")
[ "$code" = 422 ] || fail "tasks: bad action state: expected 422, got $code"
code=$(curl -s -o /dev/null -w '%{http_code}' -X PUT --data "[{\"project\":\"$PRJ\",\"actions\":[{\"title\":\"x\",\"zzz\":1}]}]" "$B/api/tasks")
[ "$code" = 422 ] || fail "tasks: unknown action key: expected 422, got $code"
code=$(curl -s -o /dev/null -w '%{http_code}' -X PUT --data "[{\"project\":\"$PRJ\",\"actions\":[{\"title\":\"\"}]}]" "$B/api/tasks")
[ "$code" = 422 ] || fail "tasks: empty action title: expected 422, got $code"
code=$(curl -s -o /dev/null -w '%{http_code}' -X PUT --data "[{\"project\":\"$PRJ\",\"actions\":[{\"title\":\"x\",\"children\":\"nope\"}]}]" "$B/api/tasks")
[ "$code" = 422 ] || fail "tasks: children not an array: expected 422, got $code"
code=$(curl -s -o /dev/null -w '%{http_code}' -X PUT --data "[{\"project\":\"$PRJ\",\"agent\":\"assistant\"}]" "$B/api/tasks")
[ "$code" = 422 ] || fail "tasks: agent binding removed (unknown key): expected 422, got $code"
code=$(curl -s -o /dev/null -w '%{http_code}' -X PUT --data "[{\"project\":\"$PRJ\",\"title\":\"x\",\"id\":\"short\"}]" "$B/api/tasks")
[ "$code" = 422 ] || fail "tasks: malformed id: expected 422, got $code"
DEEP=$(mktemp)
python3 - "$PRJ" >"$DEEP" <<'EOF'
import json, sys
inner = {"title": "deep"}
for _ in range(70):  # far past the 64-level cap
    inner = {"title": "x", "children": [inner]}
print(json.dumps([{"project": sys.argv[1], "actions": [inner]}]))
EOF
code=$(curl -s -o /dev/null -w '%{http_code}' -X PUT --data-binary "@$DEEP" "$B/api/tasks")
rm -f "$DEEP"
[ "$code" = 422 ] || fail "tasks: over-deep nesting: expected 422, got $code"
curl -s "$B/api/tasks" | grep -q '"build"' || fail "rejected PUTs must not change stored tasks"
# deleting the project leaves its tasks stored but flagged
curl -s -X PUT --data '[]' "$B/api/projects" >/dev/null
curl -s "$B/api/tasks" | python3 -c '
import json, sys
d = json.load(sys.stdin)
assert len(d) == 2 and all(c["project_ok"] is False for c in d), d
' || fail "tasks of a deleted project must carry project_ok:false"
curl -s -X PUT --data '[]' "$B/api/tasks" | grep -q '^\[\]$' || fail "PUT [] did not clear the tasks"
[ -z "$(ls -A "$CFG/tasks" 2>/dev/null)" ] || fail "removed tasks must delete their directories"

echo "== 6f. typed context items (projects + tasks) and action types =="
# one PUT creates the project and its context: explicit type/updated
# kept, bare items defaulted (fact, updated now); the ids are
# generated by position (P for projects); the project id is captured
# from the response — a later PUT without it would generate a new one
PIDX=$(curl -s -X PUT -H "Content-Type: application/json" --data-binary "
 [{\"dir\":\"$CFG/alpha\",\"context\":[
   {\"type\":\"risk\",\"text\":\"server room floods\",\"updated\":123,\"id\":\"P9\"},
   {\"text\":\"two lines\nof fact\"}]}]" "$B/api/projects" | python3 -c '
import json, sys, time
d = json.load(sys.stdin)
c = d[0]["context"]
assert c[0] == {"id":"P1","type":"risk","text":"server room floods",
                "updated":123}, c    # echoed id ignored, position wins
assert c[1]["id"] == "P2", c                         # sequential ids
assert "type" not in c[1], c                       # fact default omitted
assert c[1]["text"] == "two lines\nof fact", c     # newlines allowed
assert abs(c[1]["updated"] - time.time()) < 60, c  # missing -> now
print(d[0]["id"])') || fail "PUT /api/projects did not keep the context items"
[ -n "$PIDX" ] || fail "projects: no id in the context PUT response"
grep -q '"context"' "$CFG/projects.json" || fail "context items not persisted to projects.json"
# task context + action types: defaults omitted, nested types kept
CIDX=$(curl -s -X PUT -H "Content-Type: application/json" --data-binary "
 [{\"project\":\"$PIDX\",\"title\":\"ctx\",\"context\":[
   {\"type\":\"rule\",\"text\":\"no deploys on fridays\"},
   {\"text\":\"a plain fact\"}],
   \"actions\":[
     {\"title\":\"watch logs\",\"type\":\"observe\",\"state\":\"in_progress\"},
     {\"title\":\"fix it\",\"children\":[
       {\"title\":\"prove the fix\",\"type\":\"validate\"}]}]}]" "$B/api/tasks" | python3 -c '
import json, sys
d = json.load(sys.stdin)
c = d[0]["context"]
assert c[0]["id"] == "T1" and c[0]["type"] == "rule", c  # T ids in tasks
assert c[0]["text"] == "no deploys on fridays", c
assert "type" not in c[1] and c[1]["id"] == "T2" and "updated" in c[1], c
a = d[0]["actions"]
assert a[0]["type"] == "observe" and a[0]["state"] == "in_progress", a
assert "type" not in a[1] and "state" not in a[1], a          # act/pending defaults
assert a[1]["children"][0]["type"] == "validate", a
print(d[0]["id"])')
grep -q '"no deploys on fridays"' "$CFG/tasks/$CIDX/task.json" || fail "task context not persisted"
grep -q '"observe"' "$CFG/tasks/$CIDX/task.json" || fail "action type not persisted"
# validation: every context field and the action type are strict
code=$(curl -s -o /dev/null -w '%{http_code}' -X PUT --data "[{\"dir\":\"$CFG/alpha\",\"context\":[{\"type\":\"bogus\",\"text\":\"x\"}]}]" "$B/api/projects")
[ "$code" = 422 ] || fail "projects: bad context type: expected 422, got $code"
code=$(curl -s -o /dev/null -w '%{http_code}' -X PUT --data "[{\"dir\":\"$CFG/alpha\",\"context\":[{\"text\":\"\"}]}]" "$B/api/projects")
[ "$code" = 422 ] || fail "projects: empty context text: expected 422, got $code"
code=$(curl -s -o /dev/null -w '%{http_code}' -X PUT --data "[{\"dir\":\"$CFG/alpha\",\"context\":[{\"text\":\"x\",\"zzz\":1}]}]" "$B/api/projects")
[ "$code" = 422 ] || fail "projects: unknown context key: expected 422, got $code"
code=$(curl -s -o /dev/null -w '%{http_code}' -X PUT --data "[{\"dir\":\"$CFG/alpha\",\"context\":\"nope\"}]" "$B/api/projects")
[ "$code" = 422 ] || fail "projects: context not an array: expected 422, got $code"
code=$(curl -s -o /dev/null -w '%{http_code}' -X PUT --data "[{\"dir\":\"$CFG/alpha\",\"context\":[{\"text\":\"x\",\"updated\":\"soon\"}]}]" "$B/api/projects")
[ "$code" = 422 ] || fail "projects: bad context updated: expected 422, got $code"
code=$(curl -s -o /dev/null -w '%{http_code}' -X PUT --data "[{\"dir\":\"$CFG/alpha\",\"context\":[{\"text\":\"x\",\"created\":1}]}]" "$B/api/projects")
[ "$code" = 422 ] || fail "projects: retired context key created: expected 422, got $code"
CTXMANY=$(mktemp)
python3 - "$CFG/alpha" >"$CTXMANY" <<'EOF'
import json, sys
print(json.dumps([{"dir": sys.argv[1],
                   "context": [{"text": "x"} for _ in range(70)]}]))
EOF
code=$(curl -s -o /dev/null -w '%{http_code}' -X PUT --data-binary "@$CTXMANY" "$B/api/projects")
rm -f "$CTXMANY"
[ "$code" = 422 ] || fail "projects: too many context items: expected 422, got $code"
code=$(curl -s -o /dev/null -w '%{http_code}' -X PUT --data "[{\"project\":\"$PIDX\",\"context\":[{\"type\":\"fact\",\"text\":\"x\",\"huh\":0}]}]" "$B/api/tasks")
[ "$code" = 422 ] || fail "tasks: bad context item: expected 422, got $code"
code=$(curl -s -o /dev/null -w '%{http_code}' -X PUT --data "[{\"project\":\"$PIDX\",\"actions\":[{\"title\":\"x\",\"type\":\"bogus\"}]}]" "$B/api/tasks")
[ "$code" = 422 ] || fail "tasks: bad action type: expected 422, got $code"
code=$(curl -s -o /dev/null -w '%{http_code}' -X PUT --data "[{\"project\":\"$PIDX\",\"actions\":[{\"title\":\"x\",\"type\":7}]}]" "$B/api/tasks")
[ "$code" = 422 ] || fail "tasks: non-string action type: expected 422, got $code"
curl -s "$B/api/projects" | grep -q '"server room floods"' || fail "rejected PUTs must not change stored context"
curl -s "$B/api/tasks" | grep -q '"no deploys on fridays"' || fail "rejected PUTs must not change stored task context"

echo "== 6g. mcp endpoint (flower's own tools) =="
rpc() { curl -s -X POST -H "Content-Type: application/json" --data "$1" "$B/mcp"; }
rpc '{"jsonrpc":"2.0","id":1,"method":"initialize","params":{"protocolVersion":"2025-11-25"}}' | python3 -c '
import json, sys
d = json.load(sys.stdin)
assert d["id"] == 1, d
assert d["result"]["protocolVersion"] == "2025-11-25", d
assert d["result"]["serverInfo"]["name"] == "flower", d
' || fail "mcp: initialize handshake"
code=$(curl -s -o /dev/null -w '%{http_code}' -X POST -H "Content-Type: application/json" \
      --data '{"jsonrpc":"2.0","method":"notifications/initialized"}' "$B/mcp")
[ "$code" = 202 ] || fail "mcp: notification should 202, got $code"
rpc '{"jsonrpc":"2.0","id":2,"method":"tools/list"}' | python3 -c '
import json, sys
d = json.load(sys.stdin)
tools = {t["name"]: t for t in d["result"]["tools"]}
assert set(tools) == {"read_file", "list_files", "grep", "analyze"}, d  # web tools live in llmkit now
rf = tools["read_file"]["inputSchema"]
assert rf["required"] == ["path"], d
assert rf["properties"]["offset"]["type"] == "number", d   # optional numbers
assert tools["list_files"]["inputSchema"]["required"] == ["path", "glob"], d
assert "**/*" in tools["list_files"]["inputSchema"]["properties"]["glob"]["description"], d
assert tools["grep"]["inputSchema"]["required"] == ["glob", "pattern"], d
assert "grep -E" in tools["grep"]["description"], d
assert "**" in tools["grep"]["inputSchema"]["properties"]["glob"]["description"], d
assert tools["analyze"]["inputSchema"]["required"] == ["path"], d
assert "structure" in tools["analyze"]["description"], d
' || fail "mcp: tools/list with prompt-file descriptions"
rpc '{"jsonrpc":"2.0","id":4,"method":"tools/call","params":{"name":"read_file","arguments":{}}}' | python3 -c '
import json, sys
d = json.load(sys.stdin)
assert d["result"]["isError"] is True, d           # tool failure is readable content
assert "path" in d["result"]["content"][0]["text"], d
' || fail "mcp: tools/call with a missing argument"
mkdir -p "$CFG/fstree/sub/deep"
printf "line one\nline two\n" > "$CFG/fstree/notes.txt"
printf "int main(){}\n"       > "$CFG/fstree/sub/main.c"
printf "deep\n"               > "$CFG/fstree/sub/deep/edge.c"
printf "x;\n"                 > "$CFG/fstree/util.js"
rpc "{\"jsonrpc\":\"2.0\",\"id\":7,\"method\":\"tools/call\",\"params\":{\"name\":\"list_files\",\"arguments\":{\"path\":\"$CFG/fstree\",\"glob\":\"**/*.c\"}}}" | python3 -c '
import json, sys
d = json.load(sys.stdin)
t = d["result"]["content"][0]["text"]
assert "sub/main.c" in t and "sub/deep/edge.c" in t, d
assert "util.js" not in t, d
' || fail "mcp: list_files recursive glob"
rpc "{\"jsonrpc\":\"2.0\",\"id\":8,\"method\":\"tools/call\",\"params\":{\"name\":\"read_file\",\"arguments\":{\"path\":\"$CFG/fstree/notes.txt\",\"offset\":2,\"length\":1}}}" | python3 -c '
import json, sys
d = json.load(sys.stdin)
assert d["result"]["content"][0]["text"] == "line two\n", repr(d["result"]["content"][0]["text"])
' || fail "mcp: read_file offset+length"
rpc "{\"jsonrpc\":\"2.0\",\"id\":9,\"method\":\"tools/call\",\"params\":{\"name\":\"read_file\",\"arguments\":{\"path\":\"$CFG/fstree/notes.txt\",\"offset\":999}}}" | python3 -c '
import json, sys
d = json.load(sys.stdin)
assert d["result"]["isError"] is True, d
assert "past the end" in d["result"]["content"][0]["text"], d
' || fail "mcp: read_file offset past eof"
rpc "{\"jsonrpc\":\"2.0\",\"id\":10,\"method\":\"tools/call\",\"params\":{\"name\":\"grep\",\"arguments\":{\"glob\":\"$CFG/fstree/**/*.c\",\"pattern\":\"main|deep\"}}}" | python3 -c '
import json, sys
d = json.load(sys.stdin)
t = d["result"]["content"][0]["text"]
assert "sub/main.c:1:int main(){}" in t, d
assert "sub/deep/edge.c:1:deep" in t, d
assert "util.js" not in t, d
assert "[2 matches in 2 of 2 files]" in t, d
assert d["result"]["isError"] is False, d
' || fail "mcp: grep regex over a recursive glob"
rpc "{\"jsonrpc\":\"2.0\",\"id\":11,\"method\":\"tools/call\",\"params\":{\"name\":\"grep\",\"arguments\":{\"glob\":\"$CFG/fstree/notes.txt\",\"pattern\":\"^line two$\"}}}" | python3 -c '
import json, sys
d = json.load(sys.stdin)
t = d["result"]["content"][0]["text"]
assert "notes.txt:2:line two" in t, d
assert "[1 match in 1 of 1 file]" in t, d
' || fail "mcp: grep a single file with anchors"
rpc "{\"jsonrpc\":\"2.0\",\"id\":12,\"method\":\"tools/call\",\"params\":{\"name\":\"analyze\",\"arguments\":{\"path\":\"$CFG/fstree/sub/main.c\"}}}" | python3 -c '
import json, sys
d = json.load(sys.stdin)
t = d["result"]["content"][0]["text"]
assert "sub/main.c  [c]  1 lines" in t and "int main()" in t, d
assert d["result"]["isError"] is False, d
' || fail "mcp: analyze a c file"
rpc "{\"jsonrpc\":\"2.0\",\"id\":13,\"method\":\"tools/call\",\"params\":{\"name\":\"analyze\",\"arguments\":{\"path\":\"$CFG/fstree/notes.txt\"}}}" | python3 -c '
import json, sys
d = json.load(sys.stdin)
assert d["result"]["isError"] is True, d
assert "unknown file type" in d["result"]["content"][0]["text"], d
assert "markdown" in d["result"]["content"][0]["text"], d
' || fail "mcp: analyze reports an unknown file type"
rpc "{\"jsonrpc\":\"2.0\",\"id\":12,\"method\":\"tools/call\",\"params\":{\"name\":\"grep\",\"arguments\":{\"glob\":\"$CFG/fstree/**/*\",\"pattern\":\"(unclosed[\"}}}" | python3 -c '
import json, sys
d = json.load(sys.stdin)
assert d["result"]["isError"] is True, d
assert "invalid pattern" in d["result"]["content"][0]["text"], d
' || fail "mcp: grep bad regex is readable content"
rpc '{"jsonrpc":"2.0","id":5,"method":"tools/call","params":{"name":"nope","arguments":{}}}' | python3 -c '
import json, sys
d = json.load(sys.stdin)
assert d["error"]["code"] == -32602, d
' || fail "mcp: unknown tool should be -32602"
rpc '{"jsonrpc":"2.0","id":6,"method":"bogus"}' | python3 -c '
import json, sys
d = json.load(sys.stdin)
assert d["error"]["code"] == -32601, d
' || fail "mcp: unknown method should be -32601"
curl -s -X POST --data 'not json' "$B/mcp" | python3 -c '
import json, sys
d = json.load(sys.stdin)
assert d["error"]["code"] == -32700, d
' || fail "mcp: bad json should be -32700"
code=$(curl -s -o /dev/null -w '%{http_code}' "$B/mcp")
[ "$code" = 405 ] || fail "mcp: GET should be 405, got $code"

echo "== 6g2. per-project mcp surface (relative paths, jail) =="
mkdir -p "$CFG/projroot/src/deep"
printf 'project root readme\n' > "$CFG/projroot/README.md"
printf 'int main(){}\n' > "$CFG/projroot/src/main.c"
printf 'a deep edge\n' > "$CFG/projroot/src/deep/edge.c"
printf 'outside the project\n' > "$CFG/outside.txt"
curl -s -X PUT --data "[{\"dir\":\"$CFG/projroot\",\"title\":\"projroot\"}]" "$B/api/projects" >/dev/null
SQ=$(curl -s "$B/api/projects" | python3 -c '
import json, sys
d = json.load(sys.stdin)
assert len(d) == 1 and d[0]["seq"] == 1, d
print(d[0]["seq"])')
prpc() { curl -s -X POST -H "Content-Type: application/json" --data "$1" "$B/projects/$SQ/mcp"; }
prpc '{"jsonrpc":"2.0","id":1,"method":"tools/list"}' | python3 -c '
import json, sys
d = json.load(sys.stdin)
names = {t["name"] for t in d["result"]["tools"]}
assert names == {"read_file", "list_files", "grep", "analyze"}, d  # web tools live in llmkit now
' || fail "project mcp: tools/list shows the research set"
prpc '{"jsonrpc":"2.0","id":2,"method":"tools/call","params":{"name":"read_file","arguments":{"path":"README.md"}}}' | python3 -c '
import json, sys
d = json.load(sys.stdin)
assert d["result"]["content"][0]["text"] == "project root readme\n", d
' || fail "project mcp: read_file takes relative paths"
prpc '{"jsonrpc":"2.0","id":3,"method":"tools/call","params":{"name":"list_files","arguments":{"path":".","glob":"**/*"}}}' | python3 -c '
import json, sys
d = json.load(sys.stdin)
t = d["result"]["content"][0]["text"]
assert "README.md\n" in t and "src/main.c\n" in t and "src/deep/edge.c\n" in t, d
assert "'"$CFG"'" not in t, d                # no absolute path leaks
' || fail "project mcp: listing is project-relative"
prpc '{"jsonrpc":"2.0","id":4,"method":"tools/call","params":{"name":"grep","arguments":{"glob":"**/*.c","pattern":"main|edge"}}}' | python3 -c '
import json, sys
d = json.load(sys.stdin)
t = d["result"]["content"][0]["text"]
assert "src/main.c:1:int main(){}" in t and "src/deep/edge.c:1:a deep edge" in t, d
assert "'"$CFG"'" not in t, d
' || fail "project mcp: grep is project-relative"
prpc '{"jsonrpc":"2.0","id":5,"method":"tools/call","params":{"name":"read_file","arguments":{"path":"../outside.txt"}}}' | python3 -c '
import json, sys
d = json.load(sys.stdin)
assert d["result"]["isError"] is True, d
assert "escapes" in d["result"]["content"][0]["text"], d
assert "outside the project" not in d["result"]["content"][0]["text"], d
' || fail "project mcp: .. may not escape the project"
prpc '{"jsonrpc":"2.0","id":6,"method":"tools/call","params":{"name":"read_file","arguments":{"path":"/etc/hostname"}}}' | python3 -c '
import json, sys
d = json.load(sys.stdin)
assert d["result"]["isError"] is True, d
assert "relative to the project" in d["result"]["content"][0]["text"], d
' || fail "project mcp: absolute paths are refused"
code=$(curl -s -o /dev/null -w '%{http_code}' -X POST --data '{}' "$B/projects/999/mcp")
[ "$code" = 404 ] || fail "project mcp: unknown project: expected 404, got $code"
code=$(curl -s -o /dev/null -w '%{http_code}' "$B/projects/$SQ/mcp")
[ "$code" = 405 ] || fail "project mcp: GET should be 405, got $code"
curl -s -X PUT --data '[]' "$B/api/projects" >/dev/null
curl -s -X PUT --data '[]' "$B/api/tasks" >/dev/null

echo "== 7. SSE still streams =="
curl -sN --max-time 3 "$B/api/time" | grep -m1 -q '^data: {"unix"' || fail "no SSE event within 3s"

echo "== 8. connection semantics =="
python3 - "$PORT" <<'EOF' || exit 1
import socket, sys
s = socket.create_connection(("127.0.0.1", int(sys.argv[1])), timeout=2)
s.sendall(b"GET / HTTP/1.1\r\nHost: x\r\nConnection: close\r\n\r\n")
data, eof = b"", False
try:
    while True:
        c = s.recv(65536)
        if not c: eof = True; break
        data += c
except socket.timeout:
    pass
assert eof and b"200 OK" in data and b"\x00" not in data, "Connection: close not honored"
EOF
[ $? -eq 0 ] || fail "Connection: close handling regressed"

echo "== 9. conversations: a scan is recorded, live and durable =="
mkdir -p "$CFG/scanproj"
printf 'flower test fixture\n' > "$CFG/scanproj/README.md"
curl -s -X PUT --data "[{\"dir\":\"$CFG/scanproj\",\"title\":\"scan target\"}]" "$B/api/projects" >/dev/null
curl -s -X PUT --data '[{"name":"ollama","endpoint_protocol":"openai","api_base":"http://localhost:11434/v1","model":"m"}]' "$B/api/llms" >/dev/null
PROJ=$(curl -s "$B/api/projects" | python3 -c 'import json,sys; print(json.load(sys.stdin)[0]["id"])')
code=$(curl -s -o /dev/null -w '%{http_code}' -X POST --data "{\"project\":\"$PROJ\",\"llm\":\"ollama\"}" "$B/api/scan")
[ "$code" = 200 ] || fail "scan: fake runner did not start: expected 200, got $code"
# the seeds and the web tools proxy config exist while the scan runs
# (they are unlinked when it ends, like every scan scratch file)
[ -f "$CFG/scan/web-tools.jsonl" ] || fail "scan: web tools proxy config not written"
python3 - "$CFG/scan/web-tools.jsonl" <<'EOF' || fail "scan: web tools proxy config is wrong"
import json, sys
recs = [json.loads(l) for l in open(sys.argv[1]) if l.strip()]
upstream = recs[0]
assert upstream["type"] == "tools", recs
srv = upstream["tools"][0]
assert srv["type"] == "stdio" and srv["name"] == "builtin", recs
assert "builtin-mcp" in srv["command_line"], recs
exposed = {r["tool"]: r for r in recs if r["type"] == "expose"}
assert set(exposed) == {"builtin.web_search", "builtin.web_fetch"}, recs
ws = exposed["builtin.web_search"]
assert ws["name"] == "web_search", recs
assert ws["arguments"]["keywords"]["name"] == "query", recs  # flower's arg name
assert "duckduckgo" in ws["description"].lower(), recs       # flower's prompt text
assert not any(r["type"] == "hide" for r in recs), recs      # a pure whitelist
EOF
grep -q "mcp-proxy .*web-tools.jsonl" "$CFG/scan/online_researcher.jsonl" \
    || fail "scan: online researcher seed lacks the web tools server"
grep -q "/mcp" "$CFG/scan/filesystem_researcher.jsonl" \
    || fail "scan: fs researcher seed lacks flower's mcp server"
for _ in $(seq 1 60); do
    curl -s "$B/api/scan" | grep -q '"done":true' && break
    sleep 0.25
done
curl -s "$B/api/scan" | python3 -c '
import json, sys
d = json.load(sys.stdin)
assert d["done"] is True and d["ok"] is True, d
assert d["writes"] == 1, d
assert len(d["conversation"]) == 32, d
' || fail "scan: the fake runner did not complete"
CONV=$(curl -s "$B/api/scan" | python3 -c 'import json,sys; print(json.load(sys.stdin)["conversation"])')
curl -s "$B/api/conversations" | python3 -c '
import json, sys
d = json.load(sys.stdin)
assert len(d) == 2, d                       # the scan and its sub-agent
main = [c for c in d if c["agent"] == "project_scanner"][0]
sub = [c for c in d if c["agent"] == "filesystem_researcher"][0]
assert main["state"] == "completed", d
assert sub["state"] == "completed", d
assert sub["parent"] == main["id"], d       # linked both ways
assert main["project"] == "'"$PROJ"'", d
assert "scan target" in main["title"], d
assert "build files" in sub["title"], d
' || fail "conversations: list should show the scan and its sub-agent"
curl -s "$B/api/conversations/$CONV" | python3 -c '
import json, sys
d = json.load(sys.stdin)
kinds = [r["k"] for r in d["records"]]
assert kinds[0] == "user" and "thinking" in kinds and "response" in kinds, d
calls = [r for r in d["records"] if r["k"] == "tool_call"]
assert any(r["tool"] == "filesystem_researcher.invoke" and
           "build files" in r["text"] for r in calls), d
assert any(r["tool"] == "flower.set_project_details" for r in calls), d
results = [r for r in d["records"] if r["k"] == "tool_result"]
assert any("saved 1 field" in r.get("text", "") for r in results), d
' || fail "conversations: the scan transcript should be complete"
SUB=$(curl -s "$B/api/conversations" | python3 -c '
import json, sys
print([c for c in json.load(sys.stdin)
       if c["agent"] == "filesystem_researcher"][0]["id"])')
curl -s "$B/api/conversations/$SUB" | python3 -c '
import json, sys
d = json.load(sys.stdin)
kinds = [r["k"] for r in d["records"]]
assert kinds == ["user", "tool_call", "tool_result", "response"], d
assert d["records"][1]["tool"] == "read_file", d
assert "README.md" in d["records"][1]["text"], d
assert "flower test fixture" in d["records"][2]["text"], d
assert "Found:" in d["records"][3]["text"], d
' || fail "conversations: the sub-agent transcript should be captured"
curl -s "$B/api/projects" | python3 -c '
import json, sys
d = json.load(sys.stdin)
assert d[0]["description"] == "A single-binary webapp in C11.", d
' || fail "scan: the write-back did not land in projects.json"
code=$(curl -s -o /dev/null -w '%{http_code}' "$B/api/conversations/deadbeefdeadbeefdeadbeefdeadbeef")
[ "$code" = 404 ] || fail "conversations: unknown id: expected 404, got $code"
code=$(curl -s -o /dev/null -w '%{http_code}' "$B/api/conversations/nope")
[ "$code" = 404 ] || fail "conversations: bad id: expected 404, got $code"
curl -sN --max-time 3 "$B/api/conversations/stream" | grep -m1 -q '^event: conv' \
    || fail "no conv SSE event within 3s"

echo "== 9b. a restart closes interrupted conversations =="
code=$(curl -s -o /dev/null -w '%{http_code}' -X POST --data "{\"project\":\"$PROJ\",\"llm\":\"ollama\"}" "$B/api/scan")
[ "$code" = 200 ] || fail "scan: second start: expected 200, got $code"
sleep 1  # mid-run (the fake takes ~3s; the sub-agent may or may not
          # have opened yet — both outcomes must sweep cleanly)
kill "$PID" 2>/dev/null
wait "$PID" 2>/dev/null
"$BIN" -p "$PORT" -c "$CFG" -l "$LLMKIT" >>"$LOG" 2>&1 &
PID=$!
for _ in $(seq 1 50); do
    curl -s -o /dev/null "$B/" 2>/dev/null && break
    sleep 0.1
done
curl -s "$B/api/conversations" | python3 -c '
import json, sys
d = json.load(sys.stdin)
assert 3 <= len(d) <= 4, d                  # two scans + one or two subs
assert all(c["state"] != "running" for c in d), d   # no zombies
mains = [c for c in d if c["agent"] == "project_scanner"]
assert len(mains) == 2 and mains[0]["state"] == "stopped", d  # newest
' || fail "conversations: a restart should close the interrupted one"
curl -s "$B/api/conversations/$(curl -s "$B/api/conversations" | python3 -c '
import json, sys
print([c for c in json.load(sys.stdin)
       if c["agent"] == "project_scanner"][0]["id"])')" | python3 -c '
import json, sys
d = json.load(sys.stdin)
assert d["records"] and d["records"][-1]["k"] == "error", d
assert "interrupted" in d["records"][-1]["text"], d
' || fail "conversations: the interrupted one should say so"

echo "== 9c. the researcher debug subcommand =="
# usage and validation errors
"$BIN" researcher bogus --project "$PROJ" --llm ollama -c "$CFG" >/dev/null 2>&1
[ $? -eq 2 ] || fail "researcher: an unknown agent must exit 2"
"$BIN" researcher filesystem_researcher --llm ollama -c "$CFG" >/dev/null 2>&1
[ $? -eq 2 ] || fail "researcher: a missing --project must exit 2"
"$BIN" researcher filesystem_researcher --project deadbeefdeadbeefdeadbeefdeadbeef --llm ollama -c "$CFG" >/dev/null 2>&1
[ $? -eq 2 ] || fail "researcher: an unknown project must exit 2"
"$BIN" researcher filesystem_researcher --project "$PROJ" --llm nope -c "$CFG" >/dev/null 2>&1
[ $? -eq 2 ] || fail "researcher: an unknown llm must exit 2"

# --llm defaults to the config's own pick: the only endpoint there
# is, else the first of the name-sorted list (the scan dialog's own
# fallback); an empty config is a clean error
"$BIN" researcher filesystem_researcher --project "$PROJ" -c "$CFG" -l "$LLMKIT" \
    >"$CFG/nollm-rpc.out" 2>"$CFG/nollm-rpc.err" </dev/null
grep -q "using 'ollama' (the only llm" "$CFG/nollm-rpc.err" \
    || fail "researcher: no --llm should note the single-endpoint default"
curl -s -X PUT --data '[
 {"name":"zzz","endpoint_protocol":"openai","api_base":"http://localhost:1/v1","model":"z"},
 {"name":"ollama","endpoint_protocol":"openai","api_base":"http://localhost:11434/v1","model":"m"}]' \
    "$B/api/llms" >/dev/null
"$BIN" researcher filesystem_researcher --project "$PROJ" -c "$CFG" -l "$LLMKIT" \
    >"$CFG/nollm2-rpc.out" 2>"$CFG/nollm2-rpc.err" </dev/null
grep -q "using 'ollama' (first of the config" "$CFG/nollm2-rpc.err" \
    || fail "researcher: no --llm with several endpoints should note the first-pick default"
grep -q '"model":"m"' "$CFG/scan/debug-filesystem_researcher.jsonl" \
    || fail "researcher: the default llm did not land in the seed"
curl -s -X PUT --data '[]' "$B/api/llms" >/dev/null
"$BIN" researcher filesystem_researcher --project "$PROJ" -c "$CFG" -l "$LLMKIT" \
    >/dev/null 2>"$CFG/empty-rpc.err" </dev/null
[ $? -eq 2 ] || fail "researcher: an empty llm config must exit 2"
grep -q "no llm endpoints" "$CFG/empty-rpc.err" \
    || fail "researcher: an empty llm config should say so"
curl -s -X PUT --data '[{"name":"ollama","endpoint_protocol":"openai","api_base":"http://localhost:11434/v1","model":"m"}]' \
    "$B/api/llms" >/dev/null

# researcher_rpc <seed-summary-out> <agent>: one full mcp session —
# flower execs $LLMKIT (the fake, in its agent-as-tool mode) over the
# debug seed, and the scripted handshake drives it
researcher_rpc() {
    FAKE_AGENT_SEED_OUT="$1" bash -c '
      printf "%s\n" \
        "{\"jsonrpc\":\"2.0\",\"id\":1,\"method\":\"initialize\",\"params\":{}}" \
        "{\"jsonrpc\":\"2.0\",\"method\":\"notifications/initialized\"}" \
        "{\"jsonrpc\":\"2.0\",\"id\":2,\"method\":\"tools/list\"}" \
        "{\"jsonrpc\":\"2.0\",\"id\":3,\"method\":\"tools/call\",\"params\":{\"name\":\"invoke\",\"arguments\":{\"input\":\"look at the README\"}}}" |
      exec "$0" researcher "$5" --project "$1" --llm ollama --base-url "$2" -c "$3" -l "$4"' \
      "$BIN" "$PROJ" "$B" "$CFG" "$LLMKIT" "$2"
}
researcher_rpc "$CFG/seen-fs.json" filesystem_researcher >"$CFG/fs-rpc.out" 2>/dev/null \
    || fail "researcher: the fs debug session did not run"
grep -q '"name":"invoke"' "$CFG/fs-rpc.out" \
    || fail "researcher: the fs session listed no invoke tool"
grep -q 'fake researcher (m) ran: look at the README' "$CFG/fs-rpc.out" \
    || fail "researcher: the fs invoke call did not answer"
[ -f "$CFG/scan/debug-filesystem_researcher.jsonl" ] \
    || fail "researcher: the debug seed was not written"
[ ! -f "$CFG/scan/filesystem_researcher.jsonl" ] \
    || fail "researcher: the debug run must not leave a scan-mode seed"
# the seed is the scan's own shape: the project-grounded mcp url, the
# chosen llm, the researcher's rendered system prompt
grep -qF "$B/projects/" "$CFG/scan/debug-filesystem_researcher.jsonl" \
    || fail "researcher: the fs seed lacks the project mcp url"
grep -q '"model":"m"' "$CFG/scan/debug-filesystem_researcher.jsonl" \
    || fail "researcher: the fs seed lacks the chosen llm"
grep -q "You are filesystem_researcher" "$CFG/scan/debug-filesystem_researcher.jsonl" \
    || fail "researcher: the fs seed lacks the system prompt"
python3 - "$CFG/seen-fs.json" <<'EOF' || fail "researcher: the fs seed summary is wrong"
import json, sys
d = json.load(open(sys.argv[1]))
assert d["model"] == "m", d
assert d["servers"] == ["flower"], d      # flower's own mcp, project-scoped
EOF
researcher_rpc "$CFG/seen-online.json" online_researcher >"$CFG/online-rpc.out" 2>/dev/null \
    || fail "researcher: the online debug session did not run"
grep -q 'fake researcher (m) ran: look at the README' "$CFG/online-rpc.out" \
    || fail "researcher: the online invoke call did not answer"
# the online researcher reads the web: its tool server is the mcp-proxy
# over llmkit's builtin web tools, under the debug tag
grep -q "debug-web-tools.jsonl" "$CFG/scan/debug-online_researcher.jsonl" \
    || fail "researcher: the online seed lacks its web tools server"
grep -q "builtin-mcp" "$CFG/scan/debug-web-tools.jsonl" \
    || fail "researcher: the debug web tools config was not written"
python3 - "$CFG/seen-online.json" <<'EOF' || fail "researcher: the online seed summary is wrong"
import json, sys
d = json.load(open(sys.argv[1]))
assert d["servers"] == ["flower"], d      # the proxy, named flower
EOF

echo "== 9d. a follow-up scan continues from the last run =="
# the note is free text like every other field: control characters and
# overflow are refused before anything spawns
code=$(curl -s -o /dev/null -w '%{http_code}' -X POST \
    --data "{\"project\":\"$PROJ\",\"llm\":\"ollama\",\"note\":\"nope\\u0007bad\"}" \
    "$B/api/scan")
[ "$code" = 422 ] || fail "scan: a control character in the note: expected 422, got $code"
LONGNOTE=$(python3 -c 'print("x" * 1200)')
code=$(curl -s -o /dev/null -w '%{http_code}' -X POST \
    --data "{\"project\":\"$PROJ\",\"llm\":\"ollama\",\"note\":\"$LONGNOTE\"}" \
    "$B/api/scan")
[ "$code" = 422 ] || fail "scan: an over-long note: expected 422, got $code"
curl -s "$B/api/scan" | grep -q '"running":false' \
    || fail "scan: a rejected note must not start anything"

newest_main() {
    curl -s "$B/api/conversations" | python3 -c '
import json, sys
print([c for c in json.load(sys.stdin)
       if c["agent"] == "project_scanner"][0]["id"])'
}
PREV=$(newest_main)
code=$(curl -s -o /dev/null -w '%{http_code}' -X POST \
    --data "{\"project\":\"$PROJ\",\"llm\":\"ollama\",\"note\":\"check the test layout\"}" \
    "$B/api/scan")
[ "$code" = 200 ] || fail "scan: a follow-up: expected 200, got $code"
for _ in $(seq 1 60); do
    curl -s "$B/api/scan" | grep -q '"done":true' && break
    sleep 0.25
done
NEW=$(newest_main)
[ "$NEW" != "$PREV" ] || fail "scan: a follow-up should be a new conversation"
curl -s "$B/api/conversations" | python3 -c '
import json, sys
d = json.load(sys.stdin)
mine = [c for c in d if c["project"] == "'"$PROJ"'" and not c["parent"]]
assert len(mine) >= 3, d                      # every run stays linked
assert mine[0]["id"] == "'"$NEW"'", d         # newest first
' || fail "conversations: runs should stay linked to their project"
curl -s "$B/api/conversations/$NEW" | python3 -c '
import json, sys
d = json.load(sys.stdin)
u = d["records"][0]
assert u["k"] == "user", d
assert "Another round on the project \"scan target\"" in u["text"], d
assert "# Request" in u["text"] and "check the test layout" in u["text"], d
assert "# Previous Run" in u["text"], d
# the seed is the last run itself, transcript and all — not a stub
assert "Scan the project \"scan target\" now." in u["text"], d
' || fail "scan: the follow-up should carry the previous run and the request"

echo "== 10. the task planner: New Task Auto =="
# rejects before anything spawns: a missing field, a bad reference
code=$(curl -s -o /dev/null -w '%{http_code}' -X POST --data "{}" "$B/api/plan")
[ "$code" = 422 ] || fail "plan: missing fields: expected 422, got $code"
code=$(curl -s -o /dev/null -w '%{http_code}' -X POST --data "{\"project\":\"$PROJ\",\"llm\":\"missing\",\"prompt\":\"plan a thing\"}" "$B/api/plan")
[ "$code" = 422 ] || fail "plan: unknown llm: expected 422, got $code"
code=$(curl -s -o /dev/null -w '%{http_code}' -X POST --data "{\"project\":\"nope\",\"llm\":\"ollama\",\"prompt\":\"plan a thing\"}" "$B/api/plan")
[ "$code" = 422 ] || fail "plan: unknown project: expected 422, got $code"
BAD=$(python3 -c 'print("x" * 5000)')
code=$(curl -s -o /dev/null -w '%{http_code}' -X POST --data "{\"project\":\"$PROJ\",\"llm\":\"ollama\",\"prompt\":\"$BAD\"}" "$B/api/plan")
[ "$code" = 422 ] || fail "plan: over-long prompt: expected 422, got $code"
curl -s "$B/api/plan" | grep -q '"running":false' || fail "plan: rejected starts must not flip the status"

# the write-back surface lists its three tools
curl -s -X POST --data '{"jsonrpc":"2.0","id":1,"method":"tools/list","params":{}}' "$B/plan/mcp" | python3 -c '
import json, sys
d = json.load(sys.stdin)
names = [t["name"] for t in d["result"]["tools"]]
assert names == ["set_task_title", "add_action", "clear_actions"], d
' || fail "plan: tools/list should name the three write-back tools"

# start: the task exists at once, the conversation is interactive
R=$(curl -s -X POST --data "{\"project\":\"$PROJ\",\"llm\":\"ollama\",\"prompt\":\"Plan the test layout fix\nmake it so\"}" "$B/api/plan")
PLAN_CONV=$(echo "$R" | python3 -c 'import json,sys; print(json.load(sys.stdin)["conversation"])')
PLAN_TASK=$(echo "$R" | python3 -c 'import json,sys; print(json.load(sys.stdin)["task"])')
[ ${#PLAN_CONV} = 32 ] || fail "plan: no conversation id in the reply: $R"
curl -s "$B/api/tasks" >"$CFG/plan-tasks.json"
python3 - "$PLAN_TASK" "$CFG/plan-tasks.json" <<'EOF' || fail "plan: the task should exist from the start"
import json, sys
d = json.load(open(sys.argv[2]))
t = [x for x in d if x["id"] == sys.argv[1]]
assert t and t[0]["title"] == "Plan the test layout fix", d
assert t[0].get("actions", []) == [], d   # empty lists are omitted on the wire
EOF
curl -s "$B/api/conversations/$PLAN_CONV" >"$CFG/plan-conv.json"
python3 - "$CFG/plan-conv.json" <<'EOF' || fail "plan: the conversation should be interactive"
import json, sys
d = json.load(open(sys.argv[1]))
assert d["interactive"] is True and d["state"] == "running", d
assert d["records"][0]["k"] == "user", d
assert "Plan the test layout fix" in d["records"][0]["text"], d
EOF
code=$(curl -s -o /dev/null -w '%{http_code}' -X POST --data "{\"project\":\"$PROJ\",\"llm\":\"ollama\",\"prompt\":\"another\"}" "$B/api/plan")
[ "$code" = 409 ] || fail "plan: one at a time: expected 409, got $code"

# the seeds exist while the plan runs, under the plan- tag
[ -f "$CFG/scan/plan-filesystem_researcher.jsonl" ] || fail "plan: fs researcher seed not written"
grep -q "/plan/research/mcp" "$CFG/scan/plan-filesystem_researcher.jsonl" \
    || fail "plan: the fs seed should point at the plan research surface"
grep -q "task_planner\|You are" "$CFG/scan/plan-online_researcher.jsonl" \
    || fail "plan: the online seed was not written"

# the fake plays its turn (~4s): title + three actions land, pending
for _ in $(seq 1 80); do
    curl -s "$B/api/plan" | grep -q '"writes":4' && break
    sleep 0.25
done
curl -s "$B/api/tasks" >"$CFG/plan-tasks.json"
python3 - "$PLAN_TASK" "$CFG/plan-tasks.json" <<'EOF' || fail "plan: the write-backs did not land"
import json, sys
d = json.load(open(sys.argv[2]))
t = [x for x in d if x["id"] == sys.argv[1]][0]
assert t["title"] == "Fake planned task", t
flat = []
def walk(lst, depth=0):
    for a in lst:
        flat.append((depth, a["title"], a.get("type", "act"), a.get("state", "pending")))
        walk(a.get("children", []), depth + 1)
walk(t["actions"])
assert [f[1] for f in flat] == ["Research the codebase", "Write the change",
                                "Check the edge cases"], t
assert flat[0][2] == "observe", t          # the type the fake passed
assert all(f[3] == "pending" for f in flat), t  # mcp actions start pending
assert flat[2][0] == 1, t                  # parent "1" nested it
EOF
# the researcher round is recorded as a sub-conversation of the plan
curl -s "$B/api/conversations" >"$CFG/plan-convs.json"
python3 - "$PLAN_CONV" "$CFG/plan-convs.json" <<'EOF' || fail "plan: the researcher sub-conversation is missing"
import json, sys
d = json.load(open(sys.argv[2]))
subs = [c for c in d if c["parent"] == sys.argv[1]]
assert subs and subs[0]["agent"] == "filesystem_researcher", d
EOF
SUB=$(curl -s "$B/api/conversations" | python3 -c '
import json, sys
print([c for c in json.load(sys.stdin) if c["parent"] == "'"$PLAN_CONV"'"][0]["id"])')
curl -s "$B/api/conversations/$SUB" >"$CFG/plan-sub.json"
python3 - "$CFG/plan-sub.json" <<'EOF' || fail "plan: the researcher transcript should carry the fs call"
import json, sys
d = json.load(open(sys.argv[1]))
kinds = [r["k"] for r in d["records"]]
assert "user" in kinds and "tool_call" in kinds, d
tools = [r["tool"] for r in d["records"] if r["k"] == "tool_call"]
assert "read_file" in tools, d
EOF

# the reply loop: a reply cannot delete; unknown ids and idle
# conversations are told apart
code=$(curl -s -o /dev/null -w '%{http_code}' -X POST --data '{}' "$B/api/conversations/$PLAN_CONV/reply")
[ "$code" = 422 ] || fail "reply: missing text: expected 422, got $code"
code=$(curl -s -o /dev/null -w '%{http_code}' -X POST --data '{"text":"hi"}' "$B/api/conversations/deadbeefdeadbeefdeadbeefdeadbeef/reply")
[ "$code" = 404 ] || fail "reply: unknown id: expected 404, got $code"
code=$(curl -s -o /dev/null -w '%{http_code}' -X POST --data '{"text":"hi"}' "$B/api/conversations/$SUB/reply")
[ "$code" = 409 ] || fail "reply: a sub-conversation is not interactive: expected 409, got $code"
# a turn takes its time: the reply only lands once it is over
for _ in $(seq 1 60); do
    curl -s "$B/api/plan" | grep -q '"awaiting_reply":true' && break
    sleep 0.25
done
curl -s "$B/api/plan" | grep -q '"awaiting_reply":true' \
    || fail "plan: a finished turn should wait for a reply"
code=$(curl -s -o /dev/null -w '%{http_code}' -X POST --data '{"text":"add a follow-up action"}' "$B/api/conversations/$PLAN_CONV/reply")
[ "$code" = 200 ] || fail "reply: the running plan should take it: expected 200, got $code"
for _ in $(seq 1 80); do
    curl -s "$B/api/conversations/$PLAN_CONV" | grep -q '"Plan updated."' && break
    sleep 0.25
done
curl -s "$B/api/tasks" >"$CFG/plan-tasks.json"
python3 - "$PLAN_TASK" "$CFG/plan-tasks.json" <<'EOF' || fail "reply: the refinement turn did not write"
import json, sys
d = json.load(open(sys.argv[2]))
t = [x for x in d if x["id"] == sys.argv[1]][0]
flat = []
def walk(lst, depth=0):
    for a in lst:
        flat.append(a["title"]); walk(a.get("children", []))
walk(t["actions"])
assert any(x.startswith("Follow-up:") for x in flat), t
EOF
curl -s "$B/api/conversations/$PLAN_CONV" >"$CFG/plan-conv.json"
python3 - "$CFG/plan-conv.json" <<'EOF' || fail "plan: a finished turn must leave the conversation running"
import json, sys
d = json.load(open(sys.argv[1]))
assert d["state"] == "running", d          # interactive: waits for a reply
assert any(r.get("text") == "Plan ready. Reply to refine it."
           for r in d["records"]), d
EOF
python3 - "$CFG/plan-conv.json" <<'EOF' || fail "reply: the transcript should carry both turns"
import json, sys
d = json.load(open(sys.argv[1]))
users = [r for r in d["records"] if r["k"] == "user"]
assert len(users) == 2 and "add a follow-up action" in users[-1]["text"], d
assert any(r.get("text") == "Plan updated." for r in d["records"]), d
EOF

# a conversation still running cannot be deleted — the whole tree goes
# only after it stopped
code=$(curl -s -o /dev/null -w '%{http_code}' -X DELETE "$B/api/conversations/$PLAN_CONV")
[ "$code" = 409 ] || fail "delete: a running conversation: expected 409, got $code"
curl -s -X POST "$B/api/plan/stop" >/dev/null
for _ in $(seq 1 40); do
    curl -s "$B/api/plan" | grep -q '"running":false' && break
    sleep 0.25
done
curl -s "$B/api/conversations/$PLAN_CONV" | python3 -c '
import json, sys
d = json.load(sys.stdin)
assert d["state"] == "stopped", d
' || fail "plan: stopping should close the conversation"
# the sub-agent still sits in the tree: deleting the root takes it too
code=$(curl -s -o /dev/null -w '%{http_code}' "$B/api/conversations/$SUB")
[ "$code" = 200 ] || fail "delete: the sub-conversation should still exist"
code=$(curl -s -o /dev/null -w '%{http_code}' -X DELETE "$B/api/conversations/$PLAN_CONV")
[ "$code" = 200 ] || fail "delete: expected 200, got $code"
code=$(curl -s -o /dev/null -w '%{http_code}' "$B/api/conversations/$PLAN_CONV")
[ "$code" = 404 ] || fail "delete: the conversation should be gone"
code=$(curl -s -o /dev/null -w '%{http_code}' "$B/api/conversations/$SUB")
[ "$code" = 404 ] || fail "delete: the sub-conversation should go with its parent"
code=$(curl -s -o /dev/null -w '%{http_code}' -X DELETE "$B/api/conversations/$PLAN_CONV")
[ "$code" = 404 ] || fail "delete: an unknown id: expected 404, got $code"

# a scan follow-up still seeds from the last scan — the plan's
# conversations never leak into it
code=$(curl -s -o /dev/null -w '%{http_code}' -X POST \
    --data "{\"project\":\"$PROJ\",\"llm\":\"ollama\",\"note\":\"after the plan\"}" "$B/api/scan")
[ "$code" = 200 ] || fail "scan: a follow-up after a plan: expected 200, got $code"
for _ in $(seq 1 60); do
    curl -s "$B/api/scan" | grep -q '"done":true' && break
    sleep 0.25
done
curl -s "$B/api/conversations" >"$CFG/plan-convs.json"
python3 - "$CFG/plan-convs.json" <<'EOF' || fail "scan: the follow-up should continue the scan, not the plan"
import json, sys
d = json.load(open(sys.argv[1]))
main = [c for c in d if c["agent"] == "project_scanner"][0]
assert main["title"].startswith("Project scan"), d
EOF

echo "ALL SMOKE TESTS PASSED"
