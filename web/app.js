// app.js — flower's main screen: a three-column deck whose first column
// manages projects. The list is rendered from memory on every change and
// persisted whole via PUT api/projects; the server (src/projects.c)
// validates it (working dirs must exist on disk) and stores
// projects.json in the config dir. GET reports a live "exists" flag per
// project; a project whose directory vanished is flagged everywhere and
// blocks columns two and three until fixed.
//
// On narrow screens the deck becomes a swipeable carousel: CSS
// scroll-snap handles touch, this script adds mouse drags, dot clicks
// and arrow keys. Emoji data (EMOJI_GROUPS) comes from web/emoji.js,
// generated from Unicode's emoji-test.txt.

const DEFAULT_COLORS = [
  "#58a6ff", "#ff7b72", "#3fb950", "#d29922",
  "#bc8cff", "#39c5cf", "#f778ba", "#7ee787",
];
const DEFAULT_EMOJIS = ["🌸", "🐝", "🌿", "🪴", "🌻", "🍄", "🌊", "🌙"];
const FIELDS = ["dir", "title", "color", "emoji"];
// project details: free-text textareas in column one (name, label, hint)
const DETAILS = [
  ["description", "Description", "What this project is about"],
  ["objectives", "Output/Objectives", "What it should deliver"],
  ["scope", "Scope/Constraints/Resources", "What's in, what's out, what's available"],
  ["stakeholders", "Stakeholders", "Who is involved or affected"],
];
const ALL_FIELDS = [...FIELDS, ...DETAILS.map(([name]) => name)];
const DETAIL_MAX = 4095; // bytes, mirrors PROJECT_TEXT_MAX-1 in src/projects.c
const SAVE_DELAY = 500; // ms of quiet before a PUT

/* ---------- typed context items (projects + tasks) ----------
 *
 * Mirrors src/context.c: an item is one of seven types, a text and
 * the update time (refreshed on every edit, like the server fills
 * it when missing). Each row also shows its generated id — P<n> in
 * a project's list, T<n> in a task's — numbered by position, so it
 * is derived, never stored. The list rides inside the owning
 * project/task PUT — no endpoints of its own, exactly like the
 * action tree. */
const CONTEXT_TYPES = [
  ["fact", "Fact"],
  ["pattern", "Pattern"],
  ["risk", "Risk"],
  ["success_metric", "Success metric"],
  ["failure_sign", "Failure sign"],
  ["evaluation_method", "Evaluation method"],
  ["rule", "Rule"],
];
const CTX_PLACEHOLDER = {
  fact: "Something known to be true",
  pattern: "A recurring behavior or relationship",
  risk: "What could go wrong",
  success_metric: "How success is measured",
  failure_sign: "How failure shows up",
  evaluation_method: "How to evaluate the outcome",
  rule: "A constraint that must hold",
};
const CTX_TEXT_MAX = 4096; // mirrors CTX_TEXT_MAX-1 in src/context.c
const CTX_MAX = 64;        // mirrors CONTEXT_MAX in src/context.c

const ctxTextOk = (v) =>
  bytes(v || "") <= CTX_TEXT_MAX - 1 && noControlsMulti(v || "");
const nowSec = () => Math.floor(Date.now() / 1000);
const newContextItem = () => ({ type: "fact", text: "", updated: nowSec() });

/* the wire omits defaults ("fact", and the server always fills
 * updated); the client state is always complete. The server's
 * generated ids are dropped — rows derive them from position */
function normContext(list) {
  return (list || []).map((c) => ({
    type: CONTEXT_TYPES.some(([v]) => v === c.type) ? c.type : "fact",
    text: c.text || "",
    updated: c.updated || nowSec(),
  }));
}

function contextValid(list) {
  return (list || []).every((c) => (c.text || "").trim() && ctxTextOk(c.text));
}

const railEl = document.getElementById("rail");
const editorEl = document.getElementById("editor");
const countEl = document.getElementById("project-count");
const deckEl = document.getElementById("deck");
const dotsEl = document.getElementById("deck-dots");
const narrow = window.matchMedia("(max-width: 879px)");
const reducedMotion = window.matchMedia("(prefers-reduced-motion: reduce)");

let projects = [];   // [{dir, title, color, emoji, exists?}]
let selected = -1;   // index into projects
let draft = null;    // project being created, not yet in the list
let saveTimer = 0;
let saveSeq = 0;
let disarmTimer = 0;
let picker = null;   // open emoji picker element

/* ---------- helpers ---------- */

const bytes = (s) => new TextEncoder().encode(s).length;
const noControls = (s) => !/[\x00-\x1f\x7f]/.test(s);
// detail textareas are multi-line: newline and tab are fine (server-side
// valid_text(..., multiline) in src/projects.c agrees)
const noControlsMulti = (s) => !/[\x00-\x08\x0b-\x1f\x7f]/.test(s);

// mirrors the server-side rules in src/projects.c
const validators = {
  dir: (v) =>
    (v.startsWith("/") && v.length <= 512 && noControls(v)) ||
    "must be an absolute path, like /home/you/project",
  title: (v) => (bytes(v) <= 96 && noControls(v)) || "too long (96 bytes at most)",
  color: (v) => /^#[0-9a-fA-F]{6}$/.test(v) || "must be #rrggbb",
  emoji: (v) => (v.length > 0 && bytes(v) <= 31 && noControls(v)) || "pick one emoji",
};
for (const [name] of DETAILS)
  validators[name] = (v) =>
    (bytes(v || "") <= DETAIL_MAX && noControlsMulti(v || "")) ||
    `too long (${DETAIL_MAX} bytes at most)`;

const basename = (dir) => {
  const parts = dir.split("/").filter(Boolean);
  return parts.length ? parts[parts.length - 1] : dir;
};
const displayTitle = (p) => p.title.trim() || basename(p.dir) || "Untitled project";

function el(tag, attrs = {}, ...children) {
  const node = document.createElement(tag);
  for (const [k, v] of Object.entries(attrs)) {
    if (k === "text") node.textContent = v;
    else node.setAttribute(k, v);
  }
  for (const c of children) if (c != null) node.append(c);
  return node;
}

/* ---------- rendering ---------- */

function renderRail() {
  railEl.textContent = "";
  const add = el("button", {
    type: "button", class: "chip chip-add", id: "add-project",
    title: "Add a project", "aria-label": "Add a project", text: "+",
  });
  railEl.appendChild(add);

  projects.forEach((p, i) => {
    const chip = el("button", {
      type: "button",
      class: "chip" + (p.exists === false ? " missing" : ""),
      "data-idx": i,
      title: p.title, "aria-label": p.title, text: p.emoji, // alt text = title
    });
    chip.style.setProperty("--pc", p.color);
    if (i === selected) chip.setAttribute("aria-current", "true");
    railEl.appendChild(chip);
  });

  const n = projects.length;
  countEl.textContent = n ? `${n} project${n === 1 ? "" : "s"}` : "";
}

function field(label, name, opts = {}) {
  const input = el("input", { id: `f-${name}`, "data-f": name, spellcheck: "false" });
  input.type = opts.type || "text";
  if (opts.placeholder) input.placeholder = opts.placeholder;
  if (opts.mono) input.setAttribute("class", "mono");
  if (opts.width) input.style.width = opts.width;
  return el("div", { class: "field" },
    el("label", { for: `f-${name}`, text: label }),
    input,
    el("span", { class: "field-error", id: `err-${name}` }));
}

/* emoji: a button opening the Unicode-list picker (web/emoji.js) */
function emojiField() {
  return el("div", { class: "field field-emoji" },
    el("label", { for: "f-emoji", text: "Emoji" }),
    el("button", {
      type: "button", id: "f-emoji", class: "emoji-pick",
      "data-f": "emoji", "data-action": "pick-emoji",
      "aria-haspopup": "dialog", title: "Pick an emoji",
    }),
    el("span", { class: "field-error", id: "err-emoji" }));
}

/* a free-text details field: multi-line textarea */
function detailField(name, label, hint) {
  const ta = el("textarea", {
    id: `f-${name}`, "data-f": name, rows: "3", placeholder: hint,
  });
  return el("div", { class: "field" },
    el("label", { for: `f-${name}`, text: label }),
    ta,
    el("span", { class: "field-error", id: `err-${name}` }));
}

/* ---------- context editor (shared by project + task) ---------- */

/* live error display inside one item row (mirrors the server's
 * context rules in src/context.c) */
function showCtxErrors(c, row) {
  if (!row) return;
  const err = row.querySelector(".err-text");
  const ta = row.querySelector('[data-cf="text"]');
  const msg = (c.text || "").trim()
    ? (ctxTextOk(c.text) ? "" : `too long (${CTX_TEXT_MAX - 1} bytes at most)`)
    : "required";
  if (err) err.textContent = msg;
  if (ta) ta.classList.toggle("invalid", !!msg);
}

/* any edit — text or type — restamps the item as updated; the
 * row's stamp refreshes in place so typing never needs a re-render
 * (the server fills the same field when it arrives missing) */
function touchCtxItem(c, row) {
  c.updated = nowSec();
  const ts = row && row.querySelector(".ts");
  if (ts) ts.textContent = new Date(c.updated * 1000).toLocaleString();
}

function ctxItemRow(c, i, prefix) {
  const type = CONTEXT_TYPES.some(([v]) => v === c.type) ? c.type : "fact";
  const sel = el("select", {
    class: "ctx-type", "data-ci": i, "data-cf": "type",
    "aria-label": "Context item type",
  });
  for (const [v, label] of CONTEXT_TYPES) {
    const o = el("option", { value: v, text: label });
    if (v === type) o.selected = true;
    sel.appendChild(o);
  }
  const ta = el("textarea", {
    "data-ci": i, "data-cf": "text", rows: "2", spellcheck: "false",
    placeholder: CTX_PLACEHOLDER[type],
  });
  ta.value = c.text || "";
  return el("div", { class: "ctx-item", "data-ci": i },
    el("div", { class: "ctx-row" },
      el("span", {
        class: "ctx-id mono", title: "Context item id",
        text: `${prefix}${i + 1}`,
      }),
      sel,
      el("span", {
        class: "ts", title: "Updated",
        text: c.updated ? new Date(c.updated * 1000).toLocaleString() : "",
      }),
      el("button", {
        type: "button", class: "icon-btn", "data-action": "ctx-del",
        "data-ci": i, title: "Remove context item",
        "aria-label": "Remove context item", text: "✕",
      })),
    ta,
    el("span", { class: "field-error err-text" }));
}

/* the typed context list of a project or task: an add button on
 * top, one row per item (generated id, type select, update stamp,
 * remove), the text below. Items are addressed by index — the
 * owning list is PUT whole, like everything else. `prefix` is the
 * id letter: "P" for projects, "T" for tasks. */
function contextEditor(list = [], prefix = "P") {
  const sec = el("section", { class: "ctx-editor", "aria-label": "Context" },
    el("div", { class: "ctx-head" },
      el("h3", { text: "Context" }),
      el("button", {
        type: "button", class: "btn btn-ghost ctx-add",
        "data-action": "ctx-add", title: "Add a context item",
        text: "+ item",
      })));
  if (!list.length)
    sec.append(el("p", {
      class: "ctx-none muted",
      text: "No context items yet — facts, patterns, risks, rules…",
    }));
  list.forEach((c, i) => sec.append(ctxItemRow(c, i, prefix)));
  return sec;
}

/* ---------- project scan agent (project details) ----------
 *
 * A button in the project details runs the server-side scan agent:
 * POST api/scan {project, llm} starts it — the llm is picked from
 * the configured endpoints and remembered in localStorage. The scan
 * runs in the background: this section only says so and links to the
 * conversation that is being recorded (conversations.html — the
 * transcript, its sub-agents and the Stop button live there). The
 * agent writes the project's detail fields and context items through
 * its own tools, so the editor refreshes from the server as the
 * writes land. */

let llms = [];           // [{name, …}] — names for the scan pick
let scanState = null;    // the last GET api/scan answer
let scanSeenWrites = 0;  // writes at the last refresh from the server
let scanTimer = 0;

const scanEl = () => document.getElementById("scan-section");

async function loadLlms() {
  try {
    const r = await fetch("api/llms");
    if (r.ok) llms = await r.json();
  } catch (_) { /* the select shows the empty hint */ }
}

function scanLlName() {
  const sel = document.getElementById("scan-llm");
  return sel && sel.value ? sel.value : "";
}

function scanLlmSelect() {
  const sel = el("select", {
    id: "scan-llm", "aria-label": "LLM to scan with",
  });
  if (!llms.length) {
    const o = el("option", {
      value: "", text: "no llm endpoints — add one in Config",
    });
    o.selected = true;
    sel.append(o);
    return sel;
  }
  const remembered = localStorage.getItem("flower.scan.llm") || "";
  const chosen = llms.some((l) => l.name === remembered)
    ? remembered : llms[0].name;
  for (const l of llms) {
    const o = el("option", { value: l.name, text: l.name });
    if (l.name === chosen) o.selected = true;
    sel.append(o);
  }
  return sel;
}

/* the scan block, refreshed in place by updateScanUI() while the
 * agent runs (no re-render: it would steal focus from the fields) */
function scanSection() {
  const sec = el("div", { class: "scan-editor", id: "scan-section" },
    el("div", { class: "ctx-head" },
      el("h3", { text: "Project scan" })),
    el("div", { class: "scan-row" },
      scanLlmSelect(),
      el("button", {
        type: "button", class: "btn btn-accent",
        id: "scan-start", "data-action": "scan-start",
        text: "Scan project",
      })),
    el("p", { id: "scan-status", class: "muted", role: "status" }),
    el("a", {
      id: "scan-open", class: "btn btn-ghost scan-open", hidden: "",
      title: "Open the recorded conversation",
    }));
  return sec;
}

function updateScanUI() {
  const sec = scanEl();
  if (!sec) return;
  const status = document.getElementById("scan-status");
  const start = document.getElementById("scan-start");
  const open = document.getElementById("scan-open");
  const sel = document.getElementById("scan-llm");
  const st = scanState;
  const running = !!st && !!st.running;
  const p = projects[selected];
  /* the scan (or its result) belongs here only while it is this
   * project's — the conversation page keeps every project's history */
  const mine = !!st && !!p && st.project === p.id;

  if (start) {
    const can = !running && !draft && llms.length > 0 &&
                 !!p && p.exists !== false;
    start.toggleAttribute("disabled", !can);
    start.textContent = running ? "Scanning…" : "Scan project";
    start.title = can
      ? "Run the scan agent on this project's directory"
      : llms.length ? "" : "add an llm endpoint on the Config page first";
  }
  if (sel) sel.toggleAttribute("disabled", running);

  if (status) {
    if (running) {
      status.className = "muted";
      status.textContent = mine
        ? "Scanning in the background — the conversation is being recorded."
        : "A scan of another project is running in the background.";
    } else if (st && st.done && mine) {
      if (st.ok) {
        status.className = "cfg-ok";
        status.textContent = `Scan complete ✓ — ${st.writes} ` +
          `write${st.writes === 1 ? "" : "s"}`;
      } else {
        status.className = "cfg-warn";
        status.textContent = `Scan failed — ${st.error || "the runner exited"}`;
      }
    } else {
      status.textContent = "";
      status.className = "muted";
    }
  }
  if (open) {
    const target = mine && (running || (st && st.done)) && st.conversation;
    open.hidden = !target;
    if (target) {
      open.setAttribute("href", "conversations.html#" + st.conversation);
      open.textContent = running
        ? "Watch the conversation →" : "View the conversation →";
    }
  }
}

/* the scan writes through the server; adopt its version of the
 * project list without disturbing a field mid-edit */
async function refreshScanWrites() {
  if (draft) return;
  try {
    const r = await fetch("api/projects");
    if (!r.ok) return;
    const fresh = await r.json();
    projects = fresh.map((p) => ({ ...p, context: p.context || [] }));
    if (editingIn(editorEl)) syncEditorFields();
    else { renderRail(); renderEditor(); }
  } catch (_) { /* keep showing what we have */ }
}

async function pollScan() {
  clearTimeout(scanTimer);
  try {
    const r = await fetch("api/scan");
    if (r.ok) scanState = await r.json();
  } catch (_) { /* transient: try again on the next tick */ }
  if (scanState && scanState.writes > scanSeenWrites) {
    scanSeenWrites = scanState.writes;
    refreshScanWrites();
  }
  updateScanUI();
  if (scanState && scanState.running)
    scanTimer = setTimeout(pollScan, 1200);
  else {
    scanSeenWrites = 0;
    refreshScanWrites(); /* the final state, whatever it was */
  }
}

async function startScan() {
  const p = projects[selected];
  const name = scanLlName();
  const status = document.getElementById("scan-status");
  if (!p || !name || p.exists === false) return;
  try {
    const r = await fetch("api/scan", {
      method: "POST",
      headers: { "Content-Type": "application/json" },
      body: JSON.stringify({ project: p.id, llm: name }),
    });
    if (!r.ok) {
      let msg = `scan failed (${r.status})`;
      try { msg = (await r.json()).error || msg; } catch (_) {}
      if (status) { status.textContent = msg; status.className = "cfg-warn"; }
      return;
    }
    localStorage.setItem("flower.scan.llm", name);
    pollScan();
  } catch (_) {
    if (status) {
      status.textContent = "network error — the scan was not started";
      status.className = "cfg-warn";
    }
  }
}

/* context-item wiring shared by the project editor and the task
 * details — both panes own a context list edited in place. `owner`
 * finds the list's owner (or null), `save` autosaves after an edit,
 * `rerender` rebuilds the pane after a structural change. Each
 * helper returns true when the event was a context-item event. */
function onCtxInput(e, owner, save) {
  const ci = e.target.dataset.ci;
  if (ci == null || e.target.dataset.cf !== "text") return false;
  const t = owner();
  const c = t && t.context && t.context[+ci];
  if (!c) return true;
  c.text = e.target.value;
  const row = e.target.closest(".ctx-item");
  showCtxErrors(c, row);
  touchCtxItem(c, row);
  save();
  return true;
}

function onCtxTypeChange(e, owner, save) {
  if (!e.target.classList.contains("ctx-type")) return false;
  const t = owner();
  const c = t && t.context && t.context[+e.target.dataset.ci];
  if (!c) return true;
  c.type = e.target.value;
  const row = e.target.closest(".ctx-item");
  const ta = row?.querySelector('[data-cf="text"]');
  if (ta) ta.placeholder = CTX_PLACEHOLDER[c.type] || "";
  touchCtxItem(c, row);
  save();
  return true;
}

function onCtxClick(e, container, owner, save, rerender) {
  const btn = e.target.closest("[data-action]");
  const act = btn && btn.dataset.action;
  if (act !== "ctx-add" && act !== "ctx-del") return false;
  const t = owner();
  if (!t) return true;
  if (act === "ctx-add") {
    const list = t.context || (t.context = []);
    if (list.length >= CTX_MAX) return true;
    list.push(newContextItem());
    rerender();
    container.querySelector(".ctx-item:last-of-type [data-cf='text']")?.focus();
  } else {
    t.context?.splice(+btn.dataset.ci, 1);
    rerender();
    save();
  }
  return true;
}

function openPicker(anchor) {
  closePicker();
  const total = EMOJI_GROUPS.reduce((a, [, items]) => a + items.length, 0);
  picker = el("div", {
    class: "emoji-picker", role: "dialog", "aria-label": "Pick an emoji",
  },
    el("input", {
      type: "search", class: "emoji-search",
      placeholder: `Search ${total} emojis…`, "aria-label": "Search emojis",
    }),
    el("div", { class: "emoji-list" }));
  document.body.appendChild(picker); // fixed: escapes the scroll containers

  // open under the button, clamped to the viewport
  const r = anchor.getBoundingClientRect();
  picker.style.left =
    Math.max(8, Math.min(r.left, innerWidth - picker.offsetWidth - 8)) + "px";
  picker.style.top =
    Math.max(8, Math.min(r.bottom + 6, innerHeight - picker.offsetHeight - 8)) + "px";

  renderPickerList("");
  const search = picker.querySelector(".emoji-search");
  search.addEventListener("input", () =>
    renderPickerList(search.value.trim().toLowerCase()));
  picker.addEventListener("click", (e) => {
    const opt = e.target.closest(".emoji-opt");
    if (opt) applyEmoji(opt.dataset.emoji);
  });
  search.focus();
}

function closePicker() {
  if (picker) { picker.remove(); picker = null; }
}

function renderPickerList(query) {
  const list = picker.querySelector(".emoji-list");
  const current = (draft || projects[selected] || {}).emoji;
  list.textContent = "";
  let shown = 0;
  for (const [group, items] of EMOJI_GROUPS) {
    const matched = query ? items.filter(([, name]) => name.includes(query)) : items;
    if (!matched.length) continue;
    shown += matched.length;
    const grid = el("div", { class: "emoji-grid" });
    for (const [em, name] of matched) {
      const b = el("button", {
        type: "button", class: "emoji-opt", "data-emoji": em,
        title: name, "aria-label": `${name} ${em}`, text: em,
      });
      if (em === current) b.setAttribute("aria-current", "true");
      grid.appendChild(b);
    }
    list.append(el("div", { class: "emoji-group" },
      el("h3", { text: group }), grid));
  }
  if (!shown)
    list.append(el("p", { class: "emoji-none muted", text: "No emoji matches." }));
}

function applyEmoji(em) {
  const t = draft || projects[selected];
  if (!t) return;
  t.emoji = em;
  closePicker();
  editorValid();
  renderRail();
  updateEditorHead();
  syncEditorFields();
  if (!draft) scheduleSave();
}

function editorForm(creating) {
  const frag = document.createDocumentFragment();
  frag.append(el("div", { class: "editor-head" },
    el("span", { class: "mark", id: "eh-emoji" }),
    el("h2", { id: "eh-title" })));

  const p = projects[selected];
  if (!creating && p && p.exists === false)
    frag.append(el("div", { class: "editor-warn", id: "editor-warn", role: "alert" },
      el("strong", { text: "Directory missing." }),
      el("span", {
        text: `${p.dir} is not on disk. Fix the path or remove the project — ` +
              "columns two and three stay off-limits until then.",
      })));

  frag.append(
    field("Title", "title", { placeholder: "defaults to the directory name" }),
    field("Working directory", "dir", { placeholder: "/home/you/project", mono: true }),
    el("div", { class: "field-row" },
      field("Color", "color", { type: "color" }),
      emojiField()));

  const details = el("section",
    { class: "editor-details", "aria-label": "Project details" },
    el("h3", { text: "Project details" }));
  for (const [name, label, hint] of DETAILS)
    details.append(detailField(name, label, hint));
  if (!creating) details.append(scanSection()); // needs a saved project
  frag.append(details);

  frag.append(contextEditor(
    (creating ? draft : projects[selected]).context, "P"));

  frag.append(el("p", { id: "editor-status", class: "muted", role: "status" }));

  const actions = el("div", { class: "editor-actions" });
  if (creating) {
    actions.append(
      el("button", { class: "btn btn-accent", type: "button", "data-action": "create", text: "Create project" }),
      el("button", { class: "btn btn-ghost", type: "button", "data-action": "cancel", text: "Cancel" }));
  } else {
    actions.append(el("button", {
      class: "btn btn-danger", type: "button", id: "delete-btn",
      "data-action": "delete", text: "Delete project",
    }));
  }
  frag.append(actions);
  return frag;
}

function renderEditor() {
  editorEl.textContent = "";
  disarmDelete();

  if (draft) {
    editorEl.append(editorForm(true));
    syncEditorFields();
    return;
  }
  if (selected < 0 || !projects[selected]) {
    editorEl.append(el("div", { class: "editor-empty" },
      el("p", { class: "placeholder-title",
                text: projects.length ? "No project selected" : "No projects yet" }),
      el("p", { class: "placeholder-text", text: projects.length
        ? "Pick one from the rail on the left."
        : "Add your first project with the + button." }),
      el("button", { class: "btn btn-accent", type: "button", "data-action": "new", text: "Add project" })));
    return;
  }
  editorEl.append(editorForm(false));
  syncEditorFields();
  editorValid(); // show the missing-directory state right after load
  updateScanUI(); // the scan block reflects the live scan state
}

function syncEditorFields() {
  const t = draft || projects[selected];
  if (!t) return;
  for (const name of ALL_FIELDS) {
    const control = editorEl.querySelector(`[data-f="${name}"]`);
    if (!control || document.activeElement === control) continue;
    const v = t[name] ?? "";
    if (name === "emoji") {
      if (control.textContent !== v) control.textContent = v;
    } else if (control.value !== v) {
      control.value = v;
    }
  }
  updateEditorHead();
}

function updateEditorHead() {
  const t = draft || projects[selected];
  const emojiEl = document.getElementById("eh-emoji");
  const titleEl = document.getElementById("eh-title");
  if (!t || !emojiEl || !titleEl) return;
  emojiEl.textContent = t.emoji || "🌱";
  emojiEl.style.setProperty("--pc", t.color);
  titleEl.textContent = draft ? "New project" : displayTitle(t);
}

function setStatus(text, kind) {
  const s = document.getElementById("editor-status");
  if (!s) return;
  s.textContent = text;
  s.className = kind === "ok" ? "cfg-ok" : kind === "warn" ? "cfg-warn" : "muted";
}

function showFieldError(name, msg) {
  const err = document.getElementById(`err-${name}`);
  const input = editorEl.querySelector(`[data-f="${name}"]`);
  if (err) err.textContent = msg || "";
  if (input) input.classList.toggle("invalid", !!msg);
}

function editorValid() {
  const t = draft || projects[selected];
  if (!t) return true;
  let ok = true;
  for (const name of ALL_FIELDS) {
    let msg = validators[name](t[name] ?? "");
    if (msg === true) msg = "";
    if (!msg && name === "dir") {
      const others = draft ? projects : projects.filter((_, i) => i !== selected);
      if (others.some((p) => p.dir === t.dir))
        msg = "another project already uses this directory";
    }
    showFieldError(name, msg);
    if (msg) ok = false;
  }
  if (!draft && t.exists === false) {
    showFieldError("dir", "directory is missing on disk");
    ok = false;
  }
  /* context items gate the save like the fields above (their rows
   * carry their own live error display) */
  editorEl.querySelectorAll(".ctx-item").forEach((row) => {
    const c = t.context && t.context[+row.dataset.ci];
    if (c) showCtxErrors(c, row);
  });
  if (!contextValid(t.context)) ok = false;
  return ok;
}

/* columns two and three refuse to work while the selected project's
 * working directory is gone (see .pane-warn in index.html) */
function updateBlockState() {
  const p = projects[selected];
  const blocked = !draft && !!p && p.exists === false;
  for (const pane of deckEl.querySelectorAll(".pane[data-pane]"))
    if (pane.dataset.pane !== "0")
      pane.classList.toggle("blocked", blocked);
  if (blocked)
    for (const w of document.querySelectorAll(".pane-warn .warn-text"))
      w.textContent =
        `The working directory of “${displayTitle(p)}” is missing on disk. ` +
        "Fix or remove the project in the Projects column; these columns " +
        "stay off-limits until then.";
}

function render() {
  renderRail();
  renderEditor();
  renderTaskPane();
  updateBlockState();
}

/* ---------- server sync ---------- */

async function loadProjects() {
  try {
    const r = await fetch("api/projects");
    if (r.ok)
      projects = (await r.json()).map((p) => ({ ...p, context: p.context || [] }));
  } catch (_) {
    /* stay with the empty list; the editor still works, saves will fail loudly */
  }
}

function scheduleSave() {
  clearTimeout(saveTimer);
  setStatus("editing…");
  saveTimer = setTimeout(saveNow, SAVE_DELAY);
}

async function saveNow() {
  clearTimeout(saveTimer);
  if (draft) return;
  const missing = projects.find((p) => p.exists === false);
  if (missing) {
    setStatus(`not saved — the directory of “${displayTitle(missing)}” is ` +
              "missing; fix or remove that project first", "warn");
    return;
  }
  if (!editorValid()) {
    setStatus("not saved — fix the highlighted fields", "warn");
    return;
  }

  const sent = projects.map(({ exists, ...rest }) => rest); // flags stay client-side
  const seq = ++saveSeq;
  setStatus("saving…");
  try {
    const r = await fetch("api/projects", {
      method: "PUT",
      headers: { "Content-Type": "application/json" },
      body: JSON.stringify(sent),
    });
    if (seq !== saveSeq) return; // a newer change is already saving
    if (!r.ok) {
      let msg = `save failed (${r.status})`;
      let field = "";
      try {
        const e = await r.json();
        msg = e.error + (e.field ? ` — ${e.field}` : "");
        field = e.field || "";
      } catch (_) {}
      // a vanished directory: flag that project so the UI reflects it
      const m = field.match(/^projects\[(\d+)\]\.dir$/);
      if (m && projects[+m[1]]) {
        projects[+m[1]].exists = false;
        render();
      }
      setStatus(msg, "warn");
      return;
    }
    const saved = await r.json();
    // keep anything edited while the request was in flight; only adopt
    // the server-resolved defaults for values we still sent as-is
    projects = saved.map((sp, i) => {
      const lp = sent[i];
      const cur = projects[i] || sp;
      const merged = { ...cur };
      for (const f of ALL_FIELDS)
        if (lp && sp[f] !== lp[f] && cur[f] === lp[f]) merged[f] = sp[f];
      if (!merged.id && sp.id) merged.id = sp.id; // server-generated
      merged.exists = true; // saved == verified on disk
      return merged;
    });
    const warn = document.getElementById("editor-warn");
    if (warn) warn.hidden = true; // a saved project is verified on disk
    renderRail();
    syncEditorFields();
    updateBlockState();
    setStatus("saved ✓", "ok");
  } catch (_) {
    setStatus("network error — changes are still on this screen", "warn");
  }
}

/* ---------- actions ---------- */

function rememberSelection() {
  const p = projects[selected];
  if (p) localStorage.setItem("flower.selected", p.dir);
  else localStorage.removeItem("flower.selected");
}

function select(i) {
  draft = null;
  selected = i;
  rememberSelection();
  setStatus("");
  resetTaskScope(); // the task list follows the project: fresh scope
  render();
  refreshExists(); // flags go stale; re-check when a project takes focus
}

/* re-fetch the exists flags without disturbing local edits */
async function refreshExists() {
  if (draft) return;
  try {
    const r = await fetch("api/projects");
    if (!r.ok) return;
    const fresh = await r.json();
    let changed = false;
    for (const p of projects) {
      const f = fresh.find((x) => x.dir === p.dir);
      if (f && p.exists !== (f.exists !== false)) {
        p.exists = f.exists !== false;
        changed = true;
      }
    }
    const typing = document.activeElement &&
                   editorEl.contains(document.activeElement) &&
                   ["INPUT", "TEXTAREA"].includes(
                     document.activeElement.tagName);
    if (changed && !typing) render();
  } catch (_) {}
}

function startDraft() {
  const i = projects.length;
  draft = {
    dir: "",
    title: "",
    color: DEFAULT_COLORS[i % DEFAULT_COLORS.length],
    emoji: DEFAULT_EMOJIS[i % DEFAULT_EMOJIS.length],
    description: "",
    objectives: "",
    scope: "",
    stakeholders: "",
    context: [],
  };
  resetTaskScope(); // drafting a project: no scope for columns 2-3
  renderTaskPane();
  renderEditor();
  const dir = editorEl.querySelector('[data-f="dir"]');
  if (dir) dir.focus();
}

/* Creating goes through its own PUT: the server validates the directory,
 * and a rejection (typo'd path, vanished dir) leaves the draft in place
 * instead of planting an unsavable project in the list. */
async function createProject() {
  if (!draft || !editorValid()) return;
  const next = [...projects.map(({ exists, ...rest }) => rest), { ...draft }];
  const seq = ++saveSeq;
  setStatus("saving…");
  try {
    const r = await fetch("api/projects", {
      method: "PUT",
      headers: { "Content-Type": "application/json" },
      body: JSON.stringify(next),
    });
    if (seq !== saveSeq) return; // a newer change is already saving
    if (!r.ok) {
      let msg = `save failed (${r.status})`, field = "";
      try {
        const e = await r.json();
        msg = e.error + (e.field ? ` — ${e.field}` : "");
        field = e.field || "";
      } catch (_) {}
      if (field.endsWith(".dir")) showFieldError("dir", msg.split(" — ")[0]);
      setStatus(msg, "warn");
      return; // stay in create mode, nothing applied
    }
    const saved = await r.json();
    projects = saved.map((p) => ({ ...p, exists: true, context: p.context || [] }));
    selected = projects.length - 1;
    draft = null;
    rememberSelection();
    resetTaskScope(); // a brand-new project owns no tasks yet
    render();
    setStatus("saved ✓", "ok");
  } catch (_) {
    setStatus("network error — the project was not created", "warn");
  }
}

function deleteSelected(btn) {
  if (!btn.classList.contains("armed")) {
    disarmDelete();
    btn.classList.add("armed");
    btn.textContent = "Really delete?";
    disarmTimer = setTimeout(disarmDelete, 2500);
    return;
  }
  projects.splice(selected, 1);
  selected = Math.min(selected, projects.length - 1);
  rememberSelection();
  resetTaskScope(); // a different (or no) project takes over
  render();
  saveNow();
}

function disarmDelete() {
  clearTimeout(disarmTimer);
  const btn = document.getElementById("delete-btn");
  if (btn && btn.classList.contains("armed")) {
    btn.classList.remove("armed");
    btn.textContent = "Delete project";
  }
}

railEl.addEventListener("click", (e) => {
  if (e.target.closest("#add-project")) { startDraft(); return; }
  const chip = e.target.closest(".chip[data-idx]");
  if (chip) select(Number(chip.dataset.idx));
});

editorEl.addEventListener("input", (e) => {
  /* context item text: live state + live errors, autosave below */
  if (onCtxInput(e, () => draft || projects[selected],
                 () => { if (!draft) scheduleSave(); }))
    return;
  const name = e.target.dataset.f;
  if (!name) return;
  const t = draft || projects[selected];
  if (!t) return;
  t[name] = e.target.value;
  if (name === "dir" && t.exists === false) {
    t.exists = undefined; // unknown again until the server checks on save
    const warn = document.getElementById("editor-warn");
    if (warn) warn.hidden = true;
    updateBlockState();
  }
  editorValid();
  renderRail();
  updateEditorHead();
  if (!draft) scheduleSave();
});

editorEl.addEventListener("change", (e) => {
  if (onCtxTypeChange(e, () => draft || projects[selected],
                      () => { if (!draft) scheduleSave(); }))
    return;
});

editorEl.addEventListener("click", (e) => {
  if (onCtxClick(e, editorEl, () => draft || projects[selected],
                 () => { if (!draft) scheduleSave(); }, renderEditor))
    return;
  const btn = e.target.closest("[data-action]");
  if (!btn) { disarmDelete(); return; }
  const act = btn.dataset.action;
  if (act === "new") startDraft();
  else if (act === "cancel") { draft = null; closePicker(); render(); }
  else if (act === "create") createProject();
  else if (act === "delete") deleteSelected(btn);
  else if (act === "pick-emoji") openPicker(btn);
  else if (act === "scan-start") startScan();
});

// close the picker when clicking elsewhere or pressing Escape
document.addEventListener("click", (e) => {
  if (picker && !e.target.closest(".emoji-picker, .emoji-pick")) closePicker();
});
document.addEventListener("keydown", (e) => {
  if (e.key === "Escape" && picker) closePicker();
});

/* ---------- deck navigation (mobile) ---------- */

function paneIndex() {
  return Math.round(deckEl.scrollLeft / Math.max(1, deckEl.clientWidth));
}

function goTo(i) {
  const max = deckEl.children.length - 1;
  deckEl.scrollTo({
    left: Math.max(0, Math.min(i, max)) * deckEl.clientWidth,
    behavior: reducedMotion.matches ? "auto" : "smooth",
  });
}

function updateDots() {
  const i = paneIndex();
  localStorage.setItem("flower.pane", String(i));
  for (const dot of dotsEl.children)
    dot.setAttribute("aria-current", String(Number(dot.dataset.pane) === i));
}

deckEl.addEventListener("scroll", updateDots, { passive: true });

[...deckEl.children].forEach((pane, i) => {
  const dot = el("button", {
    type: "button", class: "dot", "data-pane": i,
    "aria-label": `Column ${i + 1} of 3`,
  });
  dot.addEventListener("click", () => goTo(i));
  dotsEl.appendChild(dot);
});

// mouse drag swipes (touch is native scroll-snap)
let swipe = null;
deckEl.addEventListener("pointerdown", (e) => {
  if (e.pointerType !== "mouse" || !narrow.matches) return;
  if (e.target.closest("button, a, input, select, textarea, label")) return;
  swipe = { x: e.clientX, y: e.clientY };
});
window.addEventListener("pointerup", (e) => {
  if (!swipe) return;
  const dx = e.clientX - swipe.x, dy = e.clientY - swipe.y;
  swipe = null;
  if (Math.abs(dx) < 56 || Math.abs(dx) < Math.abs(dy)) return;
  goTo(paneIndex() + (dx < 0 ? 1 : -1));
});

window.addEventListener("keydown", (e) => {
  if (!narrow.matches || e.target.closest("input, textarea, select")) return;
  if (e.key === "ArrowRight") { e.preventDefault(); goTo(paneIndex() + 1); }
  else if (e.key === "ArrowLeft") { e.preventDefault(); goTo(paneIndex() - 1); }
});

/* ---------- tasks (column two) ----------
 *
 * A task belongs to one project and is stored server-side under
 * {config}/tasks/{ID}/ — the list is rendered from memory and PUT
 * whole, like projects. The bottom half shows the details of the
 * selected task: its id and title. The selected task is the scope
 * for column three. */

const taskListEl = document.getElementById("task-list");
const taskDetailsEl = document.getElementById("task-details");
const taskCountEl = document.getElementById("task-count");

let tasks = []; // [{id, project, title, created, actions, project_ok?}]
let taskSel = -1;       // index into tasks
let taskDraft = null;   // {title} while creating
let taskSaveTimer = 0;
let taskSaveSeq = 0;
let taskDelArm = -1;    // index of the row whose delete click is armed
let taskDelTimer = 0;

const taskDisplayTitle = (c) => (c.title || "").trim() || "Untitled task";

function taskFlagged(c) {
  return c.project_ok === false;
}

/* the hierarchy: tasks belong to the selected project. Column two
 * lists only that project's tasks; switching project (or drafting a
 * new one) resets the task scope, and with it column three. */
function currentProjectId() {
  return draft ? null : (projects[selected] || {}).id || null;
}

function visibleTasks() {
  const pid = currentProjectId();
  return pid ? tasks.filter((t) => t.project === pid) : [];
}

/* the selected task, but only while it belongs to the project in
 * focus — a stale selection reads as "none" */
function currentTask() {
  const c = tasks[taskSel];
  return c && c.project === currentProjectId() ? c : undefined;
}

function resetTaskScope() {
  taskDraft = null;
  taskSel = -1;
  disarmTaskDelete(); // these rows are gone
  localStorage.removeItem("flower.task");
}

function setTaskStatus(text, kind) {
  const s = document.getElementById("task-status");
  if (!s) return;
  s.textContent = text;
  s.className = kind === "ok" ? "cfg-ok" : kind === "warn" ? "cfg-warn" : "muted";
}

function showTaskError(name, msg) {
  const err = document.getElementById(`err-${name}`);
  const input = taskDetailsEl.querySelector(`[data-f="${name}"]`);
  if (err) err.textContent = msg || "";
  if (input) input.classList.toggle("invalid", !!msg);
}

async function loadTasks() {
  try {
    const r = await fetch("api/tasks");
    if (r.ok) tasks = (await r.json()).map(taskFromWire);
  } catch (_) { /* stay with the empty list */ }
}

/* ---------- rendering ---------- */

function kvRow(label, value, opts = {}) {
  return el("div", { class: "task-kv" + (opts.warn ? " task-warn" : "") },
    el("label", { text: label }),
    el(opts.mono ? "code" : "p", {
      class: opts.mono ? "mono" : "",
      style: "margin:0", text: value,
    }));
}

function renderTaskList() {
  taskListEl.textContent = "";
  const pid = currentProjectId();
  const vis = visibleTasks();
  vis.forEach((c) => {
    const i = tasks.indexOf(c);
    const n = countActions(c.actions);
    const armed = i === taskDelArm;
    /* a div, not a button: the row carries a delete button, and
     * buttons cannot nest */
    taskListEl.append(el("div", {
      class: "task-row" + (i === taskSel ? " active" : "") +
             (taskFlagged(c) ? " missing" : ""),
      "data-idx": i,
      role: "button", tabindex: "0",
      "aria-label": taskDisplayTitle(c),
      title: taskDisplayTitle(c),
    },
      el("div", { class: "task-main" },
        el("span", { class: "name", text: taskDisplayTitle(c) }),
        el("span", { class: "sub",
          text: n ? `${n} action${n === 1 ? "" : "s"}` : "no actions yet" })),
      el("button", {
        type: "button", class: "icon-btn task-del" + (armed ? " armed" : ""),
        "data-action": "task-del", "data-idx": i,
        title: armed ? "Click again to delete" : "Delete task",
        "aria-label": `Delete task ${taskDisplayTitle(c)}`,
        text: armed ? "sure?" : "✕",
      })));
  });
  if (!vis.length)
    taskListEl.append(el("p", {
      class: "task-none muted",
      text: taskDraft ? "Creating the first one…"
        : !pid && projects.length ? "No project selected."
        : "No tasks yet.",
    }));
  taskCountEl.textContent =
    vis.length ? `${vis.length} task${vis.length === 1 ? "" : "s"}` : "";
  document.getElementById("new-task").hidden = !pid;
}

function renderTaskDraft() {
  const title = field("Title", "task-title", {
    placeholder: "optional, shown in the list",
  });
  title.querySelector("input").value = taskDraft.title;
  taskDetailsEl.append(
    el("h3", { class: "task-h3", text: "New task" }),
    title,
    el("p", { id: "task-status", class: "muted", role: "status" }),
    el("div", { class: "task-actions" },
      el("button", {
        class: "btn btn-accent", type: "button",
        "data-action": "task-create", text: "Create task",
      }),
      el("button", {
        class: "btn btn-ghost", type: "button",
        "data-action": "task-cancel", text: "Cancel",
      })));
}

function renderTaskDetails() {
  taskDetailsEl.textContent = "";
  if (taskDraft) {
    renderTaskDraft();
    taskDraftValid(); // show required-field hints right away
    return;
  }
  const c = currentTask();
  if (!c) {
    const pid = currentProjectId();
    const vis = visibleTasks();
    taskDetailsEl.append(el("div", { class: "task-empty" },
      el("p", {
        class: "placeholder-title",
        text: !pid ? "No project selected" : vis.length ? "No task selected" : "Nothing open",
      }),
      el("p", {
        class: "placeholder-text",
        text: !pid
          ? "Column two follows the project picked in column one."
          : vis.length
          ? "Pick one from the list above — its actions open in column three."
          : "Create one with the New task button above.",
      })));
    return;
  }

  const title = field("Title", "task-title", { placeholder: "shown in the list" });
  title.querySelector("input").value = c.title;
  taskDetailsEl.append(
    el("h3", { class: "task-h3", text: taskDisplayTitle(c) }),
    kvRow("ID", c.id, { mono: true }),
    kvRow("Project", c.project, { mono: true, warn: c.project_ok === false }),
    kvRow("Created", new Date(c.created * 1000).toLocaleString()),
    title);
  taskDetailsEl.append(contextEditor(c.context, "T"));
  if (c.project_ok === false)
    setTaskStatus("unknown project — it was deleted", "warn");
  else {
    const s = el("p", { id: "task-status", class: "muted", role: "status" });
    taskDetailsEl.append(s);
  }
}

function renderTaskPane() {
  renderTaskList();
  renderTaskDetails();
  renderActions();
}

/* ---------- validation (mirrors src/tasks.c) ---------- */

function taskDraftValid() {
  const t = taskDraft.title || "";
  const msg = bytes(t) <= 95 && noControls(t)
    ? "" : "too long (95 bytes at most)";
  showTaskError("task-title", msg);
  return !msg;
}

/* ---------- server sync ---------- */

/* the wire shape: the server omits defaults, the client state is
 * always complete (normalized via taskFromWire) */
const taskWire = (c) => ({
  id: c.id, project: c.project, title: c.title, created: c.created,
  actions: c.actions, context: c.context,
});

function taskFromWire(t) {
  return { ...t, actions: normActions(t.actions), context: normContext(t.context) };
}

function normActions(list) {
  return (list || []).map((a) => ({
    title: a.title || "",
    description: a.description || "",
    state: a.state || "pending",
    type: a.type || "act",
    children: normActions(a.children),
  }));
}

/* is the user typing into a field under `root`? A re-render would
 * replace that field and silently swallow the next keystrokes */
const editingIn = (root) => {
  const ae = document.activeElement;
  return !!ae && root.contains(ae) &&
    ["INPUT", "TEXTAREA", "SELECT"].includes(ae.tagName);
};

function scheduleTaskSave() {
  clearTimeout(taskSaveTimer);
  setTaskStatus("editing…");
  taskSaveTimer = setTimeout(saveTasks, SAVE_DELAY);
}

/* PUT the whole task list. Resolves to the server's saved list, or
 * null when nothing was saved — after saying why in the status line
 * (server rejection with its field, or netMsg on a network error;
 * a superseded save stays silent, a newer one is already talking). */
async function putTasks(list, seq, netMsg) {
  let r;
  try {
    r = await fetch("api/tasks", {
      method: "PUT",
      headers: { "Content-Type": "application/json" },
      body: JSON.stringify(list),
    });
  } catch (_) {
    if (seq === taskSaveSeq)
      setTaskStatus(netMsg || "network error — changes stay on this screen", "warn");
    return null;
  }
  if (seq !== taskSaveSeq) return null; // a newer change is already saving
  if (!r.ok) {
    let msg = `save failed (${r.status})`;
    try {
      const e = await r.json();
      msg = e.error + (e.field ? ` — ${e.field}` : "");
    } catch (_) {}
    setTaskStatus(msg, "warn");
    return null;
  }
  try {
    return await r.json();
  } catch (_) {
    setTaskStatus("bad response — changes stay on this screen", "warn");
    return null;
  }
}

/* every action and context item in every task must be valid before a
 * PUT (mirrors the server: one broken item would reject the list) */
function actionsValid(list) {
  for (const a of list || []) {
    if (!(a.title || "").trim() || !actionTitleOk(a.title) ||
        !actionDescOk(a.description))
      return false;
    if (!actionsValid(a.children)) return false;
  }
  return true;
}

async function saveTasks() {
  clearTimeout(taskSaveTimer);
  if (taskDraft) return;
  if (!tasks.every((t) => actionsValid(t.actions) && contextValid(t.context))) {
    setTaskStatus("not saved — fix the highlighted action or context fields first", "warn");
    return;
  }
  const sent = tasks.map(taskWire);
  const seq = ++taskSaveSeq;
  setTaskStatus("saving…");
  const saved = await putTasks(sent, seq);
  if (!saved) return; // putTasks said why
  tasks = saved.map((sc) => {
    const s = sent.find((x) => x.id === sc.id);
    const cur = tasks.find((x) => x.id === sc.id) || sc;
    const merged = { ...cur };
    for (const f of ["title"])
      if (s && sc[f] !== s[f] && cur[f] === s[f]) merged[f] = sc[f];
    merged.actions = normActions(sc.actions); // server-normalized tree
    merged.context = normContext(sc.context); // server-normalized items
    return merged;
  });
  /* refresh without disturbing a field mid-edit: the editors keep
   * their own DOM in sync while typing, and a rebuild would steal
   * the focus and the caret right after every autosave */
  renderTaskList(); // the counts — no editable fields inside
  if (!editingIn(taskDetailsEl)) renderTaskDetails();
  if (!editingIn(actionListEl)) renderActions();
  setTaskStatus("saved ✓", "ok");
}

/* ---------- selection & creation ---------- */

function selectTask(i) {
  taskDraft = null;
  editingPath = null;
  taskSel = i;
  disarmTaskDelete(); // selecting is not deleting
  const c = tasks[i];
  if (c) localStorage.setItem("flower.task", c.id);
  else localStorage.removeItem("flower.task");
  renderTaskPane();
  refreshTaskFlags(); // flags go stale; re-check when one takes focus
}

/* re-fetch the reference flags without disturbing local edits */
async function refreshTaskFlags() {
  if (taskDraft) return;
  try {
    const r = await fetch("api/tasks");
    if (!r.ok) return;
    const fresh = await r.json();
    let changed = false;
    for (const c of tasks) {
      const f = fresh.find((x) => x.id === c.id);
      const po = f ? f.project_ok !== false : true;
      if (c.project_ok !== po) {
        c.project_ok = po;
        changed = true;
      }
    }
    const typing = editingIn(taskDetailsEl) || editingIn(actionListEl);
    if (changed && !typing) renderTaskPane();
  } catch (_) {}
}

function startTaskDraft() {
  if (!currentProjectId()) return;
  taskDraft = { title: "" };
  renderTaskPane();
  const title = taskDetailsEl.querySelector('[data-f="task-title"]');
  if (title) title.focus();
}

/* Creating goes through its own PUT: the server generates the id and
 * the directory, and a rejection leaves the draft in place. */
async function createTask() {
  const pid = currentProjectId();
  if (!taskDraft || !pid || !taskDraftValid()) return;
  const oldIds = new Set(tasks.map((c) => c.id));
  const next = [...tasks.map(taskWire), {
    project: pid,
    title: taskDraft.title.trim(),
    actions: [],
    context: [],
  }];
  const seq = ++taskSaveSeq;
  setTaskStatus("saving…");
  const saved = await putTasks(next, seq, "network error — the task was not created");
  if (!saved) return; // putTasks said why
  tasks = saved.map(taskFromWire);
  const created = tasks.find((c) => !oldIds.has(c.id)) ||
                  tasks[0];
  taskDraft = null;
  editingPath = null;
  taskSel = created ? tasks.indexOf(created) : -1;
  if (created) localStorage.setItem("flower.task", created.id);
  renderTaskPane();
  setTaskStatus("saved ✓", "ok");
}

/* Deleting asks twice, like the project delete: the first click arms
 * the row's ✕ ("sure?", warning colors) for 2.5s, the second one
 * goes through. */
function disarmTaskDelete() {
  clearTimeout(taskDelTimer);
  taskDelArm = -1;
  taskListEl.querySelectorAll(".task-del.armed").forEach((b) => {
    b.classList.remove("armed");
    b.textContent = "✕";
    b.title = "Delete task";
  });
}

function armTaskDelete(btn, i) {
  disarmTaskDelete(); // one armed row at a time
  taskDelArm = i;
  taskDelTimer = setTimeout(disarmTaskDelete, 2500);
  btn.classList.add("armed");
  btn.textContent = "sure?";
  btn.title = "Click again to delete";
}

/* Deleting goes through the same whole-list PUT: with the id gone
 * from the list, the server drops the task's directory. */
async function deleteTask(i) {
  disarmTaskDelete();
  const c = tasks[i];
  if (!c) return;
  const next = tasks.filter((_, k) => k !== i).map(taskWire);
  /* one broken item elsewhere would reject the list, like saveTasks */
  if (!next.every((t) => actionsValid(t.actions) && contextValid(t.context))) {
    setTaskStatus("not deleted — fix the highlighted action or context fields first", "warn");
    return;
  }
  const seq = ++taskSaveSeq;
  setTaskStatus("deleting…");
  const saved = await putTasks(next, seq, "network error — the task was not deleted");
  if (!saved) return; // putTasks said why
  tasks = saved.map(taskFromWire);
  if (taskSel === i) {
    taskSel = -1;           // the selected task is gone
    editingPath = null;
    localStorage.removeItem("flower.task");
  } else if (taskSel > i) {
    taskSel--;              // indexes shifted by one
  }
  renderTaskPane();
  setTaskStatus("deleted ✓", "ok");
}

taskListEl.addEventListener("click", (e) => {
  const del = e.target.closest("[data-action='task-del']");
  if (del) {
    const i = Number(del.dataset.idx);
    if (i === taskDelArm) deleteTask(i);
    else armTaskDelete(del, i);
    return; // the ✕ never selects
  }
  const row = e.target.closest(".task-row[data-idx]");
  if (row) selectTask(Number(row.dataset.idx));
});

/* the rows are divs (they hold a button), so Enter/Space select
 * here instead of coming for free */
taskListEl.addEventListener("keydown", (e) => {
  if (e.key !== "Enter" && e.key !== " ") return;
  const row = e.target.closest(".task-row[data-idx]");
  if (!row || e.target !== row) return;
  e.preventDefault();
  selectTask(Number(row.dataset.idx));
});

document.getElementById("new-task").addEventListener("click", startTaskDraft);

taskDetailsEl.addEventListener("input", (e) => {
  /* context item text: live state + live errors, autosave below */
  if (onCtxInput(e, currentTask, scheduleTaskSave)) return;
  const name = e.target.dataset.f;
  if (!name || name !== "task-title") return;
  if (taskDraft) {
    taskDraft.title = e.target.value;
    taskDraftValid();
    return;
  }
  const c = currentTask();
  if (!c) return;
  c.title = e.target.value;
  showTaskError("task-title",
    bytes(c.title) <= 95 && noControls(c.title)
      ? "" : "too long (95 bytes at most)");
  scheduleTaskSave();
});

taskDetailsEl.addEventListener("change", (e) => {
  onCtxTypeChange(e, currentTask, scheduleTaskSave);
});

taskDetailsEl.addEventListener("click", (e) => {
  if (onCtxClick(e, taskDetailsEl, currentTask, scheduleTaskSave,
                 renderTaskPane))
    return;
  const btn = e.target.closest("[data-action]");
  if (!btn) return;
  const act = btn.dataset.action;
  if (act === "task-create") createTask();
  else if (act === "task-cancel") {
    taskDraft = null;
    renderTaskPane();
  }
});

/* ---------- actions (column three) ----------
 *
 * The selected task's action tree: one row per action (a state
 * select, the title, add-sub-action and remove buttons), children
 * indented under their parent. Clicking a title opens the inline
 * editor (title + description). Actions are addressed by their path
 * — the child indexes from the task's root list ("0.2.1"); typing
 * mutates the in-memory tree and autosaves the whole task list,
 * structural changes (add/remove/state) re-render the pane. */

const ACTION_STATES = [
  ["pending", "Pending"],
  ["in_progress", "In progress"],
  ["completed", "Completed"],
  ["partial", "Partial"],
  ["failed", "Failed"],
];
/* the refinement loop an action sits in (mirrors src/tasks.c;
 * "act" is the default and omitted on the wire) */
const ACTION_TYPES = [
  ["observe", "Observe"],
  ["analyze", "Analyze"],
  ["find_root_cause", "Root cause"],
  ["act", "Act"],
  ["validate", "Validate"],
  ["improve", "Improve"],
];
const ACTION_TITLE_MAX = 96;   // mirrors ACTION_TITLE_MAX in src/tasks.c
const ACTION_DESC_MAX = 4096;  // mirrors ACTION_DESC_MAX in src/tasks.c

const actionListEl = document.getElementById("action-list");
const actionCountEl = document.getElementById("action-count");

let editingPath = null; // array of indexes, or null

const actionTitleOk = (v) => bytes(v || "") <= ACTION_TITLE_MAX - 1 && noControls(v || "");
const actionDescOk = (v) =>
  bytes(v || "") <= ACTION_DESC_MAX - 1 && noControlsMulti(v || "");

const newAction = () =>
  ({ title: "", description: "", state: "pending", type: "act", children: [] });

function countActions(list) {
  let n = 0;
  for (const a of list || []) n += 1 + countActions(a.children);
  return n;
}

/* the sibling list that holds the action at `path` */
function actionParentList(path) {
  const c = currentTask();
  if (!c) return null;
  let list = c.actions;
  for (let i = 0; i < path.length - 1; i++) {
    const a = list[path[i]];
    if (!a) return null;
    list = a.children;
  }
  return list;
}

function actionByPath(path) {
  const list = actionParentList(path);
  return list ? list[path[path.length - 1]] : undefined;
}

function pathKey(path) { return path.join("."); }

function renderActions() {
  actionListEl.textContent = "";
  const c = taskDraft ? null : currentTask();
  document.getElementById("new-action").hidden = !c;
  if (!c) {
    actionCountEl.textContent = "";
    actionListEl.append(el("div", { class: "task-empty" },
      el("p", {
        class: "placeholder-title",
        text: visibleTasks().length ? "No task selected" : "Nothing open",
      }),
      el("p", {
        class: "placeholder-text",
        text: visibleTasks().length
          ? "Pick one in column two — its action tree opens here."
          : "Create a task in column two first, then plan it out here.",
      })));
    return;
  }
  const n = countActions(c.actions);
  actionCountEl.textContent = n ? `${n} action${n === 1 ? "" : "s"}` : "";
  if (!n)
    actionListEl.append(el("p", {
      class: "task-none muted",
      text: "No actions yet — add the first thing to be done.",
    }));
  renderActionList(actionListEl, c.actions, []);
}

function renderActionList(container, actions, path) {
  actions.forEach((a, i) => container.append(actionRow(a, [...path, i])));
}

function actionRow(a, path) {
  const key = pathKey(path);
  const editing = editingPath && pathKey(editingPath) === key;
  const state = a.state || "pending";
  const type = a.type || "act";

  const tsel = el("select", {
    class: "action-type", "data-path": key, "aria-label": "Type",
  });
  for (const [v, label] of ACTION_TYPES) {
    const o = el("option", { value: v, text: label });
    if (v === type) o.selected = true;
    tsel.appendChild(o);
  }

  const sel = el("select", {
    class: "action-state", "data-path": key, "aria-label": "State",
  });
  for (const [v, label] of ACTION_STATES) {
    const o = el("option", { value: v, text: label });
    if (v === state) o.selected = true;
    sel.appendChild(o);
  }

  const row = el("div", {
    class: "action", "data-path": key, "data-state": state, "data-type": type,
  },
    el("div", { class: "action-row" },
      tsel,
      sel,
      el("span", {
        class: "action-title",
        text: a.title.trim() || "",
        title: "Click to edit",
      }),
      el("button", {
        type: "button", class: "icon-btn", "data-action": "action-sub",
        "data-path": key, title: "Add sub-action",
        "aria-label": "Add sub-action", text: "+",
      }),
      el("button", {
        type: "button", class: "icon-btn", "data-action": "action-del",
        "data-path": key, title: "Remove action and its sub-actions",
        "aria-label": "Remove action", text: "✕",
      })));

  if (editing) row.append(actionEditor(a));
  if (a.children.length)
    row.append(el("div", { class: "action-children" },
      ...a.children.map((_, i) => actionRow(a.children[i], [...path, i]))));
  return row;
}

function actionEditor(a) {
  const wrap = el("div", { class: "action-editor" });
  const tf = el("div", { class: "field" },
    el("label", { text: "Title" }),
    el("input", { "data-af": "title", spellcheck: "false" }),
    el("span", { class: "field-error err-title" }));
  const df = el("div", { class: "field" },
    el("label", { text: "Description" }),
    el("textarea", { "data-af": "description", rows: "3", spellcheck: "false" }),
    el("span", { class: "field-error err-desc" }));
  wrap.append(tf, df);
  tf.querySelector("input").value = a.title;
  df.querySelector("textarea").value = a.description;
  return wrap;
}

/* live error display inside an open editor (mirrors the server's
 * action rules) */
function showActionErrors(a, editor) {
  const t = editor.querySelector(".err-title");
  const d = editor.querySelector(".err-desc");
  const ti = editor.querySelector('[data-af="title"]');
  const di = editor.querySelector('[data-af="description"]');
  const terr = (a.title || "").trim() ? "" : "required";
  const derr = actionDescOk(a.description) ? "" : `too long (${ACTION_DESC_MAX - 1} bytes at most)`;
  t.textContent = terr;
  d.textContent = derr;
  ti.classList.toggle("invalid", !!terr);
  di.classList.toggle("invalid", !!derr);
}

document.getElementById("new-action").addEventListener("click", () => {
  const c = currentTask();
  if (!c) return;
  c.actions.push(newAction());
  editingPath = [c.actions.length - 1];
  renderActions();
  renderTaskList(); // the row's action count changed
  scheduleTaskSave();
  actionListEl.querySelector(".action-editor input")?.focus();
});

actionListEl.addEventListener("click", (e) => {
  const btn = e.target.closest("[data-action]");
  if (btn) {
    const path = btn.dataset.path.split(".").map(Number);
    if (btn.dataset.action === "action-sub") {
      const a = actionByPath(path);
      if (!a) return;
      a.children.push(newAction());
      editingPath = [...path, a.children.length - 1];
      renderActions();
      renderTaskList(); // the row's action count changed
      scheduleTaskSave();
      actionListEl.querySelector(".action-editor input")?.focus();
    } else if (btn.dataset.action === "action-del") {
      const list = actionParentList(path);
      if (!list) return;
      const key = pathKey(path);
      list.splice(path[path.length - 1], 1);
      if (editingPath &&
          (pathKey(editingPath) === key ||
           pathKey(editingPath).startsWith(key + ".")))
        editingPath = null;
      renderActions();
      renderTaskList(); // the row's action count changed
      scheduleTaskSave();
    }
    return;
  }
  const title = e.target.closest(".action-title");
  if (title) { // toggle the inline editor
    const key = title.closest(".action").dataset.path;
    editingPath = editingPath && pathKey(editingPath) === key
      ? null : key.split(".").map(Number);
    renderActions();
    if (editingPath)
      actionListEl.querySelector(".action-editor input")?.focus();
  }
});

actionListEl.addEventListener("change", (e) => {
  if (!e.target.classList.contains("action-state") &&
      !e.target.classList.contains("action-type"))
    return;
  const path = e.target.dataset.path.split(".").map(Number);
  const a = actionByPath(path);
  if (!a) return;
  if (e.target.classList.contains("action-state")) {
    a.state = e.target.value;
    renderActions(); // refresh row state styling
    renderTaskList(); // nothing depends on state, but keep counts fresh
  } else {
    a.type = e.target.value;
    const row = e.target.closest(".action");
    if (row) row.dataset.type = a.type;
  }
  scheduleTaskSave();
});

actionListEl.addEventListener("input", (e) => {
  const f = e.target.dataset.af;
  if (!f) return;
  const editor = e.target.closest(".action-editor");
  const row = editor.closest(".action");
  const a = actionByPath(row.dataset.path.split(".").map(Number));
  if (!a) return;
  a[f] = e.target.value;
  if (f === "title") // keep the row label in sync without a re-render
    row.querySelector(".action-title").textContent = a.title.trim();
  showActionErrors(a, editor);
  scheduleTaskSave();
});

/* ---------- boot ---------- */

(async () => {
  await loadProjects();
  await loadTasks();
  await loadLlms(); // the scan agent's llm pick
  const remembered = localStorage.getItem("flower.selected");
  const i = projects.findIndex((p) => p.dir === remembered);
  selected = i >= 0 ? i : projects.length ? 0 : -1;
  const pid = currentProjectId();
  const wantedTask = localStorage.getItem("flower.task");
  let ci = tasks.findIndex((c) => c.id === wantedTask && c.project === pid);
  if (ci < 0 && pid) {
    const first = visibleTasks()[0];
    ci = first ? tasks.indexOf(first) : -1;
  }
  taskSel = ci;
  render();

  /* a scan may outlive a page reload: pick up its progress */
  try {
    const r = await fetch("api/scan");
    if (r.ok) {
      scanState = await r.json();
      updateScanUI(); // show a finished scan's result too
      if (scanState.running) pollScan();
    }
  } catch (_) { /* no scan state, no polling */ }

  const pane = Number(localStorage.getItem("flower.pane") || "0") || 0;
  if (narrow.matches && pane > 0) deckEl.scrollLeft = pane * deckEl.clientWidth;
  updateDots();
})();
