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
  frag.append(details);

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
    if (r.ok) projects = await r.json();
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
    projects = saved.map((p) => ({ ...p, exists: true }));
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

editorEl.addEventListener("click", (e) => {
  const btn = e.target.closest("[data-action]");
  if (!btn) { disarmDelete(); return; }
  const act = btn.dataset.action;
  if (act === "new") startDraft();
  else if (act === "cancel") { draft = null; closePicker(); render(); }
  else if (act === "create") createProject();
  else if (act === "delete") deleteSelected(btn);
  else if (act === "pick-emoji") openPicker(btn);
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
 * A task is bound to one agent (user-defined or builtin) and
 * stored server-side under {config}/tasks/{ID}/ — the list is
 * rendered from memory and PUT whole, like projects. The bottom half
 * shows the details of the selected task: its id, the agent
 * binding, and the llm pick (an override — "the agent's" inherits the
 * agent's own llm; the builtins have none, so theirs must select one).
 * The selected task is the scope for column three. */

const taskListEl = document.getElementById("task-list");
const taskDetailsEl = document.getElementById("task-details");
const taskCountEl = document.getElementById("task-count");

let tasks = []; // [{id, title, agent, llm, created, agent_ok?, llm_ok?}]
let taskSel = -1;       // index into tasks
let taskDraft = null;   // {agent, title, llm} while creating
let agentsMeta = [];    // agents incl. builtins, for the selects
let llmsMeta = [];      // configured llms, for the selects
let taskSaveTimer = 0;
let taskSaveSeq = 0;

const taskDisplayTitle = (c) => (c.title || "").trim() || "Untitled task";

function findAgentMeta(name) {
  return agentsMeta.find(
    (a) => (a.name || "").toLowerCase() === (name || "").toLowerCase());
}

/* the task's llm: its override, else the bound agent's */
function taskEffectiveLlm(c) {
  if (c.llm) return c.llm;
  const a = findAgentMeta(c.agent);
  return a ? a.llm || "" : "";
}

function taskFlagged(c) {
  return c.project_ok === false || c.agent_ok === false || c.llm_ok === false;
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

async function refreshTaskMeta() {
  try {
    const [ar, lr] = await Promise.all([fetch("api/agents"), fetch("api/llms")]);
    if (ar.ok) agentsMeta = await ar.json();
    if (lr.ok) llmsMeta = await lr.json();
  } catch (_) { /* selects stay with what they had */ }
}

async function loadTasks() {
  try {
    const r = await fetch("api/tasks");
    if (r.ok) tasks = await r.json();
  } catch (_) { /* stay with the empty list */ }
}

/* ---------- rendering ---------- */

function selectField(label, name, options, value) {
  const sel = el("select", { id: `f-${name}`, "data-f": name });
  for (const [v, text] of options) {
    const o = el("option", { value: v, text });
    if (v === (value ?? "")) o.selected = true;
    sel.appendChild(o);
  }
  return el("div", { class: "field" },
    el("label", { for: `f-${name}`, text: label }),
    sel,
    el("span", { class: "field-error", id: `err-${name}` }));
}

function kvRow(label, value, opts = {}) {
  return el("div", { class: "task-kv" + (opts.warn ? " task-warn" : "") },
    el("label", { text: label }),
    el(opts.mono ? "code" : "p", {
      class: opts.mono ? "mono" : "",
      style: "margin:0", text: value,
    }));
}

function agentOptions() {
  const opts = [];
  for (const b of agentsMeta.filter((a) => a.builtin))
    opts.push([b.name, `${b.name} — builtin`]);
  for (const a of agentsMeta.filter((a) => !a.builtin))
    opts.push([a.name, a.name]);
  if (!opts.length) opts.push(["", "— no agents configured —"]);
  return opts;
}

function llmOptions(forBuiltin) {
  const opts = [];
  if (!forBuiltin) opts.push(["", "the agent's llm"]);
  for (const l of llmsMeta) opts.push([l.name, l.name]);
  if (!opts.length) opts.push(["", "— no llms configured —"]);
  return opts;
}

function renderTaskList() {
  taskListEl.textContent = "";
  const pid = currentProjectId();
  const vis = visibleTasks();
  vis.forEach((c) => {
    const i = tasks.indexOf(c);
    const a = findAgentMeta(c.agent);
    const sub = `${c.agent}${a && a.builtin ? " · builtin" : ""}` +
                ` · ${taskEffectiveLlm(c) || "no llm"}`;
    taskListEl.append(el("button", {
      type: "button",
      class: "task-row" + (i === taskSel ? " active" : "") +
             (taskFlagged(c) ? " missing" : ""),
      "data-idx": i,
      title: taskDisplayTitle(c),
    },
      el("span", { class: "name", text: taskDisplayTitle(c) }),
      el("span", { class: "sub", text: sub })));
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
  const a = taskDraft.agent ? findAgentMeta(taskDraft.agent) : null;
  const title = field("Title", "task-title", {
    placeholder: "optional, shown in the list",
  });
  title.querySelector("input").value = taskDraft.title;
  taskDetailsEl.append(
    el("h3", { class: "task-h3", text: "New task" }),
    selectField("Agent", "task-agent", agentOptions(), taskDraft.agent),
    title,
    selectField("LLM", "task-llm", llmOptions(a && !a.llm), taskDraft.llm),
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
          ? "Pick one from the list above — it becomes the scope for column three."
          : "Create one with the New task button above.",
      })));
    return;
  }

  const a = findAgentMeta(c.agent);
  const eff = taskEffectiveLlm(c);
  const title = field("Title", "task-title", { placeholder: "shown in the list" });
  title.querySelector("input").value = c.title;
  taskDetailsEl.append(
    el("h3", { class: "task-h3", text: taskDisplayTitle(c) }),
    kvRow("ID", c.id, { mono: true }),
    kvRow("Agent", c.agent + (a && a.builtin ? " — builtin" : ""),
          { warn: c.agent_ok === false }),
    kvRow("Created", new Date(c.created * 1000).toLocaleString()),
    title,
    selectField("LLM", "task-llm", llmOptions(a && !a.llm), c.llm));
  if (c.agent_ok === false)
    setTaskStatus(`unknown agent “${c.agent}” — it was deleted or renamed`, "warn");
  else if (c.llm_ok === false)
    setTaskStatus(`the llm “${eff || "…"}” is not configured anymore`, "warn");
  else {
    const s = el("p", { id: "task-status", class: "muted", role: "status" });
    taskDetailsEl.append(s);
  }
}

/* column three mirrors the task scope: named when a task is picked,
 * generic while creating or when nothing is selected */
function updateColumn3() {
  const p = document.getElementById("col3-text");
  if (!p) return;
  const c = taskDraft ? null : currentTask();
  p.textContent = c
    ? `The task “${taskDisplayTitle(c)}” opens here — the talk itself, ` +
      "once flower grows into it."
    : "This column keeps its place in the layout. flower grows into " +
      "it later.";
}

function renderTaskPane() {
  renderTaskList();
  renderTaskDetails();
  updateColumn3();
}

/* ---------- validation (mirrors src/tasks.c) ---------- */

function taskDraftValid() {
  const errs = {};
  if (!taskDraft.agent) errs["task-agent"] = "pick an agent";
  else {
    const a = findAgentMeta(taskDraft.agent);
    if (!a) errs["task-agent"] = "unknown agent";
    else if (!taskDraft.llm && !a.llm)
      errs["task-llm"] = `agent “${a.name}” has no llm of its own — select one`;
  }
  const titleErr = bytes(taskDraft.title || "") <= 95 && noControls(taskDraft.title || "")
    ? "" : "too long (95 bytes at most)";
  if (titleErr) errs["task-title"] = titleErr;
  for (const name of ["task-agent", "task-title", "task-llm"])
    showTaskError(name, errs[name] || "");
  return !Object.keys(errs).length;
}

/* ---------- server sync ---------- */

const taskWire = (c) => ({
  id: c.id, project: c.project, title: c.title, agent: c.agent,
  llm: c.llm, created: c.created,
});

function scheduleTaskSave() {
  clearTimeout(taskSaveTimer);
  setTaskStatus("editing…");
  taskSaveTimer = setTimeout(saveTasks, SAVE_DELAY);
}

async function putTasks(list, seq) {
  const r = await fetch("api/tasks", {
    method: "PUT",
    headers: { "Content-Type": "application/json" },
    body: JSON.stringify(list),
  });
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
  return r.json();
}

async function saveTasks() {
  clearTimeout(taskSaveTimer);
  if (taskDraft) return;
  const sent = tasks.map(taskWire);
  const seq = ++taskSaveSeq;
  setTaskStatus("saving…");
  const saved = await putTasks(sent, seq).catch(() => null);
  if (!saved) {
    if (seq === taskSaveSeq) setTaskStatus("network error — changes stay on this screen", "warn");
    return;
  }
  tasks = saved.map((sc) => {
    const s = sent.find((x) => x.id === sc.id);
    const cur = tasks.find((x) => x.id === sc.id) || sc;
    const merged = { ...cur };
    for (const f of ["title", "llm"])
      if (s && sc[f] !== s[f] && cur[f] === s[f]) merged[f] = sc[f];
    return merged;
  });
  renderTaskPane();
  setTaskStatus("saved ✓", "ok");
}

/* ---------- selection & creation ---------- */

function selectTask(i) {
  taskDraft = null;
  taskSel = i;
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
      const ao = f ? f.agent_ok !== false : true;
      const lo = f ? f.llm_ok !== false : true;
      if (c.project_ok !== po || c.agent_ok !== ao || c.llm_ok !== lo) {
        c.project_ok = po;
        c.agent_ok = ao;
        c.llm_ok = lo;
        changed = true;
      }
    }
    const typing = document.activeElement && taskDetailsEl.contains(document.activeElement);
    if (changed && !typing) renderTaskPane();
  } catch (_) {}
}

async function startTaskDraft() {
  if (!currentProjectId()) return;
  await refreshTaskMeta();
  taskDraft = { agent: "", title: "", llm: "" };
  renderTaskPane();
  const agent = taskDetailsEl.querySelector('[data-f="task-agent"]');
  if (agent) agent.focus();
}

/* Creating goes through its own PUT: the server generates the id and
 * the directory, and a rejection leaves the draft in place. */
async function createTask() {
  const pid = currentProjectId();
  if (!taskDraft || !pid || !taskDraftValid()) return;
  const oldIds = new Set(tasks.map((c) => c.id));
  const next = [...tasks.map(taskWire), {
    project: pid,
    agent: taskDraft.agent,
    title: taskDraft.title.trim(),
    llm: taskDraft.llm,
  }];
  const seq = ++taskSaveSeq;
  setTaskStatus("saving…");
  const saved = await putTasks(next, seq).catch(() => null);
  if (!saved) {
    if (seq === taskSaveSeq) setTaskStatus("network error — the task was not created", "warn");
    return;
  }
  tasks = saved;
  const created = tasks.find((c) => !oldIds.has(c.id)) ||
                  tasks[0];
  taskDraft = null;
  taskSel = created ? tasks.indexOf(created) : -1;
  if (created) localStorage.setItem("flower.task", created.id);
  renderTaskPane();
  setTaskStatus("saved ✓", "ok");
}

taskListEl.addEventListener("click", (e) => {
  const row = e.target.closest(".task-row[data-idx]");
  if (row) selectTask(Number(row.dataset.idx));
});

document.getElementById("new-task").addEventListener("click", startTaskDraft);

taskDetailsEl.addEventListener("input", (e) => {
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
  const name = e.target.dataset.f;
  if (!name) return;
  if (taskDraft) {
    if (name === "task-agent") {
      taskDraft.agent = e.target.value;
      taskDraft.llm = ""; // the llm list depends on the agent
      renderTaskPane();
      const sel = taskDetailsEl.querySelector('[data-f="task-agent"]');
      if (sel) sel.focus();
      taskDraftValid();
    } else if (name === "task-llm") {
      taskDraft.llm = e.target.value;
      taskDraftValid();
    }
    return;
  }
  const c = currentTask();
  if (!c) return;
  if (name === "task-llm") {
    c.llm = e.target.value;
    renderTaskList();
    scheduleTaskSave();
  }
});

taskDetailsEl.addEventListener("click", (e) => {
  const btn = e.target.closest("[data-action]");
  if (!btn) return;
  const act = btn.dataset.action;
  if (act === "task-create") createTask();
  else if (act === "task-cancel") {
    taskDraft = null;
    renderTaskPane();
  }
});

/* ---------- boot ---------- */

(async () => {
  await loadProjects();
  await Promise.all([loadTasks(), refreshTaskMeta()]);
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

  const pane = Number(localStorage.getItem("flower.pane") || "0") || 0;
  if (narrow.matches && pane > 0) deckEl.scrollLeft = pane * deckEl.clientWidth;
  updateDots();
})();
