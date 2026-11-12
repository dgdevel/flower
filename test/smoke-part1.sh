#!/bin/sh
# smoke test for flower command layer part 1 (plan.md task 4a).
# runs the docs/config_cli.md examples against a throwaway FLOWER_BASE.
set -e
cd "$(dirname "$0")/.."
export FLOWER_BASE="$(mktemp -d)"
echo "== base: $FLOWER_BASE"

hdr() { echo; echo "== $*"; }

hdr "setup (create)"
printf 'Daniele\nSoftware Developer\n' | ./flower setup
cat "$FLOWER_BASE/flower.md"

hdr "setup (update, keep Name via empty line)"
printf '\nSenior Developer\n' | ./flower setup
cat "$FLOWER_BASE/flower.md"

hdr "setup guided llm add"
printf 'Daniele\nSoftware Developer\ny\nclaude\nhttps://api.anthropic.com\nanthropic\nsk-test\nclaude-sonnet-4-5\n\n\nn\n' | ./flower setup
cat "$FLOWER_BASE/llms.md"
./flower llm list
./flower llm claude show
grep -q 'claude-sonnet-4-5' "$FLOWER_BASE/llms.md" && echo "ok: model in llms.md" || { echo "FAIL: model missing"; exit 1; }
grep -q 'Scout profile: claude' "$FLOWER_BASE/flower.md" && echo "ok: scout profile defaulted" || { echo "FAIL: scout default missing"; exit 1; }

hdr "setup guided llm edit"
printf '\n\ny\nclaude\n\n\n\nclaude-opus-4-1\n\n\n' | ./flower setup
./flower llm claude show
grep -q 'Daniele' "$FLOWER_BASE/flower.md" && echo "ok: flower.md kept Name" || { echo "FAIL: Name lost"; exit 1; }
grep -q 'claude-opus-4-1' "$FLOWER_BASE/llms.md" && echo "ok: new model in llms.md" || { echo "FAIL: new model missing"; exit 1; }

hdr "setup guided llm invalid retry (protocol)"
printf '\n\ny\nclaude\n\n\nbogus\nanthropic\n\n\n\n\nn\n' | ./flower setup
grep -q 'Protocol: anthropic' "$FLOWER_BASE/llms.md" && echo "ok: protocol recovered" || { echo "FAIL: protocol wrong"; exit 1; }

hdr "setup eof-safe"
printf 'x\ny\n' | ./flower setup && echo "ok: rc 0"
./flower llm list
./flower llm list | grep -q '^claude$' && echo "ok: claude still listed" || { echo "FAIL: claude gone"; exit 1; }

hdr "setup scout profile choice"
./flower llm add gpt --base "https://api.openai.com/v1" --protocol openai --key k1 --model m1
printf '\n\nn\ny\ngpt\n' | ./flower setup
grep -q 'Scout profile: gpt' "$FLOWER_BASE/flower.md" && echo "ok: scout profile changed" || { echo "FAIL: scout profile not written"; exit 1; }

hdr "setup scout profile invalid retry"
printf '\n\nn\ny\nbogus\ngpt\n' | ./flower setup 2> "$FLOWER_BASE/scout.err"
grep -q "no such profile 'bogus'" "$FLOWER_BASE/scout.err" && echo "ok: bad scout name rejected" || { echo "FAIL: no retry error"; exit 1; }
grep -q 'Scout profile: gpt' "$FLOWER_BASE/flower.md" && echo "ok: scout profile still gpt" || { echo "FAIL: scout profile lost"; exit 1; }

hdr "project set + show"
./flower project P1 set Name "flower"
./flower project P1 set Path "/home/user/projects/flower"
./flower project P1 set Description "A project description"
./flower project P1 show

hdr "project set unknown key (expect fail, unchanged)"
./flower project P1 set Nmae "x" && echo "FAIL: should error" || echo "ok: rejected"
./flower project P1 show

hdr "details set/show/remove"
./flower project P1 details set Output "The project output are csv files."
./flower project P1 details set Stakeholders "Team A, Team B"
./flower project P1 details show
echo "-- field only:"
./flower project P1 details show Stakeholders
echo "-- after remove Output:"
./flower project P1 details remove Output
./flower project P1 details show
echo "-- unknown field (expect fail):"
./flower project P1 details set Outpt "x" && echo "FAIL" || echo "ok: rejected"

hdr "context add/list/edit/rename/remove"
./flower project P1 context add Fact "configuration location" "Configuration always happens in directory X"
./flower project P1 context add Rule "temporary files" "Temp files are discarded automatically at process termination"
echo "-- list:"
./flower project P1 context list
echo "-- edit body:"
./flower project P1 context edit Fact "configuration location" "New body text"
./flower project P1 context list
cat "$FLOWER_BASE/projects/P1/context.md"
echo "-- rename:"
./flower project P1 context rename Fact "configuration location" "config location"
./flower project P1 context list
echo "-- duplicate title across prefix (expect fail):"
./flower project P1 context add Risk "config location" "clash" && echo "FAIL" || echo "ok: rejected"
echo "-- duplicate prefix+title (expect fail):"
./flower project P1 context add Fact "config location" "clash" && echo "FAIL" || echo "ok: rejected"
echo "-- unknown prefix (expect fail):"
./flower project P1 context add Fct "x" "y" && echo "FAIL" || echo "ok: rejected"
echo "-- remove:"
./flower project P1 context remove Fact "config location"
./flower project P1 context list

hdr "context multiline body"
./flower project P1 context add Pattern "two lines" "first line
second line"
./flower project P1 context show 2>/dev/null || cat "$FLOWER_BASE/projects/P1/context.md"

hdr "resource add/set/list/remove"
./flower project P1 resource add "staging webapp url" "https://www.example.org/"
./flower project P1 resource add "bulk csv generator" "src/product/feeds/generator.py#generate_csv"
echo "-- list:"
./flower project P1 resource list
echo "-- set re-point:"
./flower project P1 resource set "staging webapp url" "https://staging.example.org/"
./flower project P1 resource list
echo "-- duplicate add (expect fail):"
./flower project P1 resource add "staging webapp url" "http://x" && echo "FAIL" || echo "ok: rejected"
echo "-- set missing (expect fail):"
./flower project P1 resource set "nope" "http://x" && echo "FAIL" || echo "ok: rejected"
echo "-- remove:"
./flower project P1 resource remove "bulk csv generator"
./flower project P1 resource list

hdr "task set/show"
./flower project P1 task T1 set Name "mix ingredients"
./flower project P1 task T1 set Description "Combine water and flour"
./flower project P1 task T1 show

hdr "task context"
./flower project P1 task T1 context add Fact "task fact" "Task level fact body"
./flower project P1 task T1 context list
./flower project P1 task T1 context edit Fact "task fact" "Edited task body"
./flower project P1 task T1 context list

hdr "nonexistent project/task guards (expect fails)"
./flower project P9 show && echo "FAIL" || echo "ok: rejected"
./flower project P9 details show && echo "FAIL" || echo "ok: rejected"
./flower project P1 task T9 show && echo "FAIL" || echo "ok: rejected"
./flower project P9 context list && echo "FAIL" || echo "ok: rejected"
./flower project P1 resource list 2>/dev/null && echo "(existing project, empty resources: ok)"
./flower project P1 context list >/dev/null && echo "(empty context list exits 0: ok)"

hdr "invalid ids (expect fails)"
./flower project flower set Name x && echo "FAIL" || echo "ok: rejected"
./flower project P1 task T set Name x && echo "FAIL" || echo "ok: rejected"

hdr "unbalanced quotes (expect fails)"
./flower project P1 set Name '"unclosed' && echo "FAIL" || echo "ok: rejected"

hdr "cwd project detection (<P> omitted)"
FLOWER_BIN="$(pwd)/flower"
PROJDIR="$(mktemp -d)"
./flower project P1 set Path "$PROJDIR"
mkdir -p "$PROJDIR/sub"
echo "-- project show from inside Path:"
( cd "$PROJDIR" && "$FLOWER_BIN" project show )
echo "-- details set from a subdir:"
( cd "$PROJDIR/sub" && "$FLOWER_BIN" project details set Output "set from subdir" )
cat "$FLOWER_BASE/projects/P1/details.md"
echo "-- tie-break: P2 shares the Path, lowest id (P1) wins:"
mkdir "$FLOWER_BASE/projects/P2"
printf '# Project\nName: dup\nPath: %s\n' "$PROJDIR" > "$FLOWER_BASE/projects/P2/config.md"
( cd "$PROJDIR" && "$FLOWER_BIN" project show | grep -c "Name: flower" )
rm -rf "$FLOWER_BASE/projects/P2"
echo "-- unrelated dir without P (expect fail):"
( cd / && "$FLOWER_BIN" project show ) && echo "FAIL" || echo "ok: rejected"
echo "-- malformed explicit id keeps old error (expect fail):"
./flower project p1 show && echo "FAIL" || echo "ok: rejected"
rm -rf "$PROJDIR"

hdr "parse error on corrupt file (expect fail, unchanged)"
printf '# Project\n## broken jump\n' > "$FLOWER_BASE/projects/P1/config.md"
./flower project P1 set Name "y" && echo "FAIL" || echo "ok: rejected"
echo "-- file after failed write:"
cat "$FLOWER_BASE/projects/P1/config.md"

hdr "duplicate chapter in file (md_parse error, expect fail)"
printf '# Task\nName: a\n\n# Task\nName: b\n' > "$FLOWER_BASE/projects/P1/tasks/T1/config.md"
./flower project P1 task T1 set Name "y" && echo "FAIL" || echo "ok: rejected"
rm "$FLOWER_BASE/projects/P1/tasks/T1/config.md"

hdr "final tree"
find "$FLOWER_BASE" -type f | sort

echo
echo "ALL SMOKE STEPS RAN"
