#!/bin/sh
# smoke test for flower command layer part 2 (plan.md task 4b):
# actions, edit escape hatch, destructive remove. runs the
# docs/config_cli.md examples against a throwaway FLOWER_BASE.
set -e
cd "$(dirname "$0")/.."
export FLOWER_BASE="$(mktemp -d)"
echo "== base: $FLOWER_BASE"

hdr() { echo; echo "== $*"; }

hdr "project + task scaffold"
./flower project P1 set Name "flower"
./flower project P1 task T1 set Name "mix ingredients"

hdr "action add (from docs/config_cli.md example)"
./flower project P1 task T1 action add "take water"
./flower project P1 task T1 action add "take flour" --description "Flour is in the cupboard."
./flower project P1 task T1 action add "mix water and flour"
cat "$FLOWER_BASE/projects/P1/tasks/T1/actions.md"

hdr "action add creates task dir when missing"
./flower project P1 task T2 action add "side task action"
test -d "$FLOWER_BASE/projects/P1/tasks/T2" && echo "ok: tasks/T2 created"

hdr "action add duplicate title (expect fail, unchanged)"
./flower project P1 task T1 action add "take water" && echo "FAIL: should error" || echo "ok: rejected"

hdr "action add unknown flag (expect fail)"
./flower project P1 task T1 action add "x" --desc "y" && echo "FAIL" || echo "ok: rejected"

hdr "action set"
./flower project P1 task T1 action A3 set Description "put water and flour in a bowl."
./flower project P1 task T1 action A3 set Status "complete"
./flower project P1 task T1 action A3 set Depends "A1, A2"
cat "$FLOWER_BASE/projects/P1/tasks/T1/actions.md"

hdr "action set unknown key (expect fail)"
./flower project P1 task T1 action A3 set Descr "x" && echo "FAIL" || echo "ok: rejected"

hdr "action set bad Status (expect fail)"
./flower project P1 task T1 action A3 set Status "done" && echo "FAIL" || echo "ok: rejected"

hdr "action set Depends unknown id (expect fail)"
./flower project P1 task T1 action A3 set Depends "A1, A9" && echo "FAIL" || echo "ok: rejected"

hdr "action set Depends self reference (expect fail)"
./flower project P1 task T1 action A3 set Depends "A3" && echo "FAIL" || echo "ok: rejected"

hdr "action set on missing action (expect fail)"
./flower project P1 task T1 action A9 set Status "complete" && echo "FAIL" || echo "ok: rejected"

hdr "action list"
./flower project P1 task T1 action list
echo "-- pending only:"
./flower project P1 task T1 action list --pending
echo "-- complete only:"
./flower project P1 task T1 action list --complete

hdr "action remove blocked by dependent (expect fail)"
./flower project P1 task T1 action A1 remove && echo "FAIL: should error" || echo "ok: rejected"
echo "-- message above must be: action A3 depends on A1; remove or edit dependents first"

hdr "action remove dependents then target"
./flower project P1 task T1 action A3 remove
./flower project P1 task T1 action A1 remove
./flower project P1 task T1 action list

hdr "action list on missing task (expect fail)"
./flower project P1 task T9 action list && echo "FAIL" || echo "ok: rejected"

hdr "action invalid id (expect fail)"
./flower project P1 task T1 action A set Status "complete" && echo "FAIL" || echo "ok: rejected"

hdr "corrupt actions.md chapter (expect load fail)"
cp "$FLOWER_BASE/projects/P1/tasks/T1/actions.md" "$FLOWER_BASE/actions.bak"
printf '# A9: rogue\nDescription: no status here\n' \
    > "$FLOWER_BASE/projects/P1/tasks/T1/actions.md"
./flower project P1 task T1 action list && echo "FAIL" || echo "ok: rejected"
mv "$FLOWER_BASE/actions.bak" "$FLOWER_BASE/projects/P1/tasks/T1/actions.md"

hdr "edit escape hatch: valid rewrite via sed wrapper"
./flower project P1 context add Fact "configuration location" "Configuration always happens in directory X"
ed_ok="$FLOWER_BASE/ed_ok.sh"
printf '#!/bin/sh\nsed -i "s/directory X/directory Y/" "$1"\n' > "$ed_ok"
chmod +x "$ed_ok"
EDITOR="$ed_ok" ./flower edit projects/P1/context.md
cat "$FLOWER_BASE/projects/P1/context.md"
test -f "$FLOWER_BASE/projects/P1/context.md.tmp" && echo "FAIL: tmp left behind" || echo "ok: no tmp left"

hdr "edit: invalid chapter via wrapper (expect reject, original unchanged)"
ed_bad="$FLOWER_BASE/ed_bad.sh"
printf '#!/bin/sh\nprintf "# Nope\nbody text\n" > "$1"\n' > "$ed_bad"
chmod +x "$ed_bad"
cp "$FLOWER_BASE/projects/P1/context.md" "$FLOWER_BASE/context.bak"
EDITOR="$ed_bad" ./flower edit projects/P1/context.md && echo "FAIL" || echo "ok: rejected"
cmp "$FLOWER_BASE/context.bak" "$FLOWER_BASE/projects/P1/context.md" \
    && echo "ok: original unchanged"
rm -f "$FLOWER_BASE/context.bak" "$FLOWER_BASE/projects/P1/context.md.tmp"

hdr "edit: duplicate chapter via wrapper (expect reject)"
ed_dup="$FLOWER_BASE/ed_dup.sh"
printf '#!/bin/sh\nprintf "# Fact: a\nx\n\n# Fact: a\ny\n" > "$1"\n' > "$ed_dup"
chmod +x "$ed_dup"
EDITOR="$ed_dup" ./flower edit projects/P1/context.md && echo "FAIL" || echo "ok: rejected"
rm -f "$FLOWER_BASE/projects/P1/context.md.tmp"

hdr "edit: editor exits nonzero (expect abort, no changes)"
ed_fail="$FLOWER_BASE/ed_fail.sh"
printf '#!/bin/sh\nexit 3\n' > "$ed_fail"
chmod +x "$ed_fail"
cp "$FLOWER_BASE/projects/P1/context.md" "$FLOWER_BASE/context.bak"
EDITOR="$ed_fail" ./flower edit projects/P1/context.md && echo "FAIL" || echo "ok: aborted"
cmp "$FLOWER_BASE/context.bak" "$FLOWER_BASE/projects/P1/context.md" \
    && echo "ok: original unchanged"
rm -f "$FLOWER_BASE/context.bak" "$FLOWER_BASE/projects/P1/context.md.tmp"

hdr "edit: path escaping base (expect fail)"
./flower edit "../etc/passwd" && echo "FAIL" || echo "ok: rejected"
./flower edit "/etc/passwd" && echo "FAIL" || echo "ok: rejected"

hdr "edit: unknown config file (expect fail)"
./flower edit projects/P1/nope.md && echo "FAIL" || echo "ok: rejected"

hdr "edit: actions.md schema (bad Status via wrapper, expect reject)"
ed_st="$FLOWER_BASE/ed_st.sh"
printf '#!/bin/sh\nsed -i "s/Status: pending/Status: done/" "$1"\n' > "$ed_st"
chmod +x "$ed_st"
EDITOR="$ed_st" ./flower edit projects/P1/tasks/T1/actions.md && echo "FAIL" || echo "ok: rejected"
rm -f "$FLOWER_BASE/projects/P1/tasks/T1/actions.md.tmp"

hdr "task remove (confirmed)"
echo y | ./flower project P1 task T1 remove
test ! -d "$FLOWER_BASE/projects/P1/tasks/T1" && echo "ok: task dir gone"

hdr "task remove aborted on N"
./flower project P1 task T2 action list
echo n | ./flower project P1 task T2 remove && echo "FAIL: should abort" || echo "ok: aborted"
test -d "$FLOWER_BASE/projects/P1/tasks/T2" && echo "ok: task dir still there"

hdr "project remove (confirmed)"
echo y | ./flower project P1 remove
test ! -d "$FLOWER_BASE/projects/P1" && echo "ok: project dir gone"

hdr "project remove on missing project (expect fail)"
./flower project P1 remove && echo "FAIL" || echo "ok: rejected"

hdr "final tree"
find "$FLOWER_BASE" -type f | sort

echo
echo "ALL SMOKE STEPS RAN"
