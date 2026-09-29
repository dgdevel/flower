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

"$BIN" -p "$PORT" -c "$CFG" >"$LOG" 2>&1 &
PID=$!

B=http://127.0.0.1:$PORT
for _ in $(seq 1 50); do
    curl -s -o /dev/null "$B/" 2>/dev/null && break
    sleep 0.1
done
curl -s -o /dev/null "$B/" || fail "server did not come up on $B"

echo "== 1. embedded assets are byte-identical, no stray bytes =="
for f in index.html config.html components.html style.css theme.js config.js llms.js app.js emoji.js; do
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
' || fail "projects did not get unique server-generated ids"
grep -q '"id"' "$CFG/projects.json" || fail "project ids not persisted"
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
' || fail "GET /api/projects lost the detail fields"
code=$(curl -s -o /dev/null -w '%{http_code}' -X PUT --data "[{\"dir\":\"$CFG/alpha\",\"description\":\"$(printf 'x%.0s' $(seq 1 4200))\"}]" "$B/api/projects")
[ "$code" = 422 ] || fail "projects: oversized description: expected 422, got $code"
code=$(curl -s -o /dev/null -w '%{http_code}' -X PUT --data "[{\"dir\":\"$CFG/alpha\",\"description\":\"bad\\u0001ctrl\"}]" "$B/api/projects")
[ "$code" = 422 ] || fail "projects: control character in a detail field: expected 422, got $code"
code=$(curl -s -o /dev/null -w '%{http_code}' -X PUT --data "[{\"dir\":\"$CFG/alpha\",\"scope\":5}]" "$B/api/projects")
[ "$code" = 422 ] || fail "projects: non-string detail field: expected 422, got $code"
curl -s "$B/api/projects" | grep -q '"the bees"' || fail "rejected PUTs must not change stored details"
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
assert [a["name"] for a in d] == ["assistant"], d      # only the builtin so far
assert d[0]["builtin"] is True and d[0]["llm"] == "", d
assert "llm_ok" not in d[0], d    # builtins pick their llm per task
' || fail "GET /api/agents should start with only the builtin agents"
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
assert d == {"assistant": None, "gardener": True, "thinker": True}, d
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

echo "ALL SMOKE TESTS PASSED"
