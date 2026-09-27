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
  renderConvPane();
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
  else if (act === "cancel") { draft = null; closePicker(); renderEditor(); }
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

/* ---------- conversations (column two) ----------
 *
 * A conversation is bound to one agent (user-defined or builtin) and
 * stored server-side under {config}/conversations/{ID}/ — the list is
 * rendered from memory and PUT whole, like projects. The bottom half
 * shows the details of the selected conversation: its id, the agent
 * binding, and the llm pick (an override — "the agent's" inherits the
 * agent's own llm; the builtins have none, so theirs must select one).
 * The selected conversation is the scope for column three. */

const convListEl = document.getElementById("conv-list");
const convDetailsEl = document.getElementById("conv-details");
const convCountEl = document.getElementById("conv-count");

let conversations = []; // [{id, title, agent, llm, created, agent_ok?, llm_ok?}]
let convSel = -1;       // index into conversations
let convDraft = null;   // {agent, title, llm} while creating
let agentsMeta = [];    // agents incl. builtins, for the selects
let llmsMeta = [];      // configured llms, for the selects
let convSaveTimer = 0;
let convSaveSeq = 0;

const convDisplayTitle = (c) => (c.title || "").trim() || "Untitled conversation";

function findAgentMeta(name) {
  return agentsMeta.find(
    (a) => (a.name || "").toLowerCase() === (name || "").toLowerCase());
}

/* the conversation's llm: its override, else the bound agent's */
function convEffectiveLlm(c) {
  if (c.llm) return c.llm;
  const a = findAgentMeta(c.agent);
  return a ? a.llm || "" : "";
}

function convFlagged(c) {
  return c.agent_ok === false || c.llm_ok === false;
}

function setConvStatus(text, kind) {
  const s = document.getElementById("conv-status");
  if (!s) return;
  s.textContent = text;
  s.className = kind === "ok" ? "cfg-ok" : kind === "warn" ? "cfg-warn" : "muted";
}

function showConvError(name, msg) {
  const err = document.getElementById(`err-${name}`);
  const input = convDetailsEl.querySelector(`[data-f="${name}"]`);
  if (err) err.textContent = msg || "";
  if (input) input.classList.toggle("invalid", !!msg);
}

async function refreshConvMeta() {
  try {
    const [ar, lr] = await Promise.all([fetch("api/agents"), fetch("api/llms")]);
    if (ar.ok) agentsMeta = await ar.json();
    if (lr.ok) llmsMeta = await lr.json();
  } catch (_) { /* selects stay with what they had */ }
}

async function loadConversations() {
  try {
    const r = await fetch("api/conversations");
    if (r.ok) conversations = await r.json();
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
  return el("div", { class: "conv-kv" + (opts.warn ? " conv-warn" : "") },
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

function renderConvList() {
  convListEl.textContent = "";
  conversations.forEach((c, i) => {
    const a = findAgentMeta(c.agent);
    const sub = `${c.agent}${a && a.builtin ? " · builtin" : ""}` +
                ` · ${convEffectiveLlm(c) || "no llm"}`;
    convListEl.append(el("button", {
      type: "button",
      class: "conv-row" + (i === convSel ? " active" : "") +
             (convFlagged(c) ? " missing" : ""),
      "data-idx": i,
      title: convDisplayTitle(c),
    },
      el("span", { class: "name", text: convDisplayTitle(c) }),
      el("span", { class: "sub", text: sub })));
  });
  if (!conversations.length)
    convListEl.append(el("p", {
      class: "conv-none muted",
      text: convDraft ? "Creating the first one…" : "No conversations yet.",
    }));
  const n = conversations.length;
  convCountEl.textContent = n ? `${n} conversation${n === 1 ? "" : "s"}` : "";
}

function renderConvDraft() {
  const a = convDraft.agent ? findAgentMeta(convDraft.agent) : null;
  const title = field("Title", "conv-title", {
    placeholder: "optional, shown in the list",
  });
  title.querySelector("input").value = convDraft.title;
  convDetailsEl.append(
    el("h3", { class: "conv-h3", text: "New conversation" }),
    selectField("Agent", "conv-agent", agentOptions(), convDraft.agent),
    title,
    selectField("LLM", "conv-llm", llmOptions(a && !a.llm), convDraft.llm),
    el("p", { id: "conv-status", class: "muted", role: "status" }),
    el("div", { class: "conv-actions" },
      el("button", {
        class: "btn btn-accent", type: "button",
        "data-action": "conv-create", text: "Create conversation",
      }),
      el("button", {
        class: "btn btn-ghost", type: "button",
        "data-action": "conv-cancel", text: "Cancel",
      })));
}

function renderConvDetails() {
  convDetailsEl.textContent = "";
  if (convDraft) {
    renderConvDraft();
    convDraftValid(); // show required-field hints right away
    return;
  }
  const c = conversations[convSel];
  if (!c) {
    convDetailsEl.append(el("div", { class: "conv-empty" },
      el("p", {
        class: "placeholder-title",
        text: conversations.length ? "No conversation selected" : "Nothing open",
      }),
      el("p", {
        class: "placeholder-text",
        text: conversations.length
          ? "Pick one from the list above — it becomes the scope for column three."
          : "Create one with the New conversation button above.",
      })));
    return;
  }

  const a = findAgentMeta(c.agent);
  const eff = convEffectiveLlm(c);
  const title = field("Title", "conv-title", { placeholder: "shown in the list" });
  title.querySelector("input").value = c.title;
  convDetailsEl.append(
    el("h3", { class: "conv-h3", text: convDisplayTitle(c) }),
    kvRow("ID", c.id, { mono: true }),
    kvRow("Agent", c.agent + (a && a.builtin ? " — builtin" : ""),
          { warn: c.agent_ok === false }),
    kvRow("Created", new Date(c.created * 1000).toLocaleString()),
    title,
    selectField("LLM", "conv-llm", llmOptions(a && !a.llm), c.llm));
  if (c.agent_ok === false)
    setConvStatus(`unknown agent “${c.agent}” — it was deleted or renamed`, "warn");
  else if (c.llm_ok === false)
    setConvStatus(`the llm “${eff || "…"}” is not configured anymore`, "warn");
  else {
    const s = el("p", { id: "conv-status", class: "muted", role: "status" });
    convDetailsEl.append(s);
  }
}

function renderConvPane() {
  renderConvList();
  renderConvDetails();
}

/* ---------- validation (mirrors src/conversations.c) ---------- */

function convDraftValid() {
  const errs = {};
  if (!convDraft.agent) errs["conv-agent"] = "pick an agent";
  else {
    const a = findAgentMeta(convDraft.agent);
    if (!a) errs["conv-agent"] = "unknown agent";
    else if (!convDraft.llm && !a.llm)
      errs["conv-llm"] = `agent “${a.name}” has no llm of its own — select one`;
  }
  const titleErr = bytes(convDraft.title || "") <= 95 && noControls(convDraft.title || "")
    ? "" : "too long (95 bytes at most)";
  if (titleErr) errs["conv-title"] = titleErr;
  for (const name of ["conv-agent", "conv-title", "conv-llm"])
    showConvError(name, errs[name] || "");
  return !Object.keys(errs).length;
}

/* ---------- server sync ---------- */

const convWire = (c) => ({
  id: c.id, title: c.title, agent: c.agent, llm: c.llm, created: c.created,
});

function scheduleConvSave() {
  clearTimeout(convSaveTimer);
  setConvStatus("editing…");
  convSaveTimer = setTimeout(saveConversations, SAVE_DELAY);
}

async function putConversations(list, seq) {
  const r = await fetch("api/conversations", {
    method: "PUT",
    headers: { "Content-Type": "application/json" },
    body: JSON.stringify(list),
  });
  if (seq !== convSaveSeq) return null; // a newer change is already saving
  if (!r.ok) {
    let msg = `save failed (${r.status})`;
    try {
      const e = await r.json();
      msg = e.error + (e.field ? ` — ${e.field}` : "");
    } catch (_) {}
    setConvStatus(msg, "warn");
    return null;
  }
  return r.json();
}

async function saveConversations() {
  clearTimeout(convSaveTimer);
  if (convDraft) return;
  const sent = conversations.map(convWire);
  const seq = ++convSaveSeq;
  setConvStatus("saving…");
  const saved = await putConversations(sent, seq).catch(() => null);
  if (!saved) {
    if (seq === convSaveSeq) setConvStatus("network error — changes stay on this screen", "warn");
    return;
  }
  conversations = saved.map((sc) => {
    const s = sent.find((x) => x.id === sc.id);
    const cur = conversations.find((x) => x.id === sc.id) || sc;
    const merged = { ...cur };
    for (const f of ["title", "llm"])
      if (s && sc[f] !== s[f] && cur[f] === s[f]) merged[f] = sc[f];
    return merged;
  });
  renderConvPane();
  setConvStatus("saved ✓", "ok");
}

/* ---------- selection & creation ---------- */

function selectConversation(i) {
  convDraft = null;
  convSel = i;
  const c = conversations[i];
  if (c) localStorage.setItem("flower.conversation", c.id);
  else localStorage.removeItem("flower.conversation");
  renderConvPane();
  refreshConvFlags(); // flags go stale; re-check when one takes focus
}

/* re-fetch the reference flags without disturbing local edits */
async function refreshConvFlags() {
  if (convDraft) return;
  try {
    const r = await fetch("api/conversations");
    if (!r.ok) return;
    const fresh = await r.json();
    let changed = false;
    for (const c of conversations) {
      const f = fresh.find((x) => x.id === c.id);
      const ao = f ? f.agent_ok !== false : true;
      const lo = f ? f.llm_ok !== false : true;
      if (c.agent_ok !== ao || c.llm_ok !== lo) {
        c.agent_ok = ao;
        c.llm_ok = lo;
        changed = true;
      }
    }
    const typing = document.activeElement && convDetailsEl.contains(document.activeElement);
    if (changed && !typing) renderConvPane();
  } catch (_) {}
}

async function startConvDraft() {
  await refreshConvMeta();
  convDraft = { agent: "", title: "", llm: "" };
  renderConvPane();
  const agent = convDetailsEl.querySelector('[data-f="conv-agent"]');
  if (agent) agent.focus();
}

/* Creating goes through its own PUT: the server generates the id and
 * the directory, and a rejection leaves the draft in place. */
async function createConversation() {
  if (!convDraft || !convDraftValid()) return;
  const oldIds = new Set(conversations.map((c) => c.id));
  const next = [...conversations.map(convWire), {
    agent: convDraft.agent,
    title: convDraft.title.trim(),
    llm: convDraft.llm,
  }];
  const seq = ++convSaveSeq;
  setConvStatus("saving…");
  const saved = await putConversations(next, seq).catch(() => null);
  if (!saved) {
    if (seq === convSaveSeq) setConvStatus("network error — the conversation was not created", "warn");
    return;
  }
  conversations = saved;
  const created = conversations.find((c) => !oldIds.has(c.id)) ||
                  conversations[0];
  convDraft = null;
  convSel = created ? conversations.indexOf(created) : -1;
  if (created) localStorage.setItem("flower.conversation", created.id);
  renderConvPane();
  setConvStatus("saved ✓", "ok");
}

convListEl.addEventListener("click", (e) => {
  const row = e.target.closest(".conv-row[data-idx]");
  if (row) selectConversation(Number(row.dataset.idx));
});

document.getElementById("new-conversation").addEventListener("click", startConvDraft);

convDetailsEl.addEventListener("input", (e) => {
  const name = e.target.dataset.f;
  if (!name || name !== "conv-title") return;
  if (convDraft) {
    convDraft.title = e.target.value;
    convDraftValid();
    return;
  }
  const c = conversations[convSel];
  if (!c) return;
  c.title = e.target.value;
  showConvError("conv-title",
    bytes(c.title) <= 95 && noControls(c.title)
      ? "" : "too long (95 bytes at most)");
  scheduleConvSave();
});

convDetailsEl.addEventListener("change", (e) => {
  const name = e.target.dataset.f;
  if (!name) return;
  if (convDraft) {
    if (name === "conv-agent") {
      convDraft.agent = e.target.value;
      convDraft.llm = ""; // the llm list depends on the agent
      renderConvPane();
      const sel = convDetailsEl.querySelector('[data-f="conv-agent"]');
      if (sel) sel.focus();
      convDraftValid();
    } else if (name === "conv-llm") {
      convDraft.llm = e.target.value;
      convDraftValid();
    }
    return;
  }
  const c = conversations[convSel];
  if (!c) return;
  if (name === "conv-llm") {
    c.llm = e.target.value;
    renderConvList();
    scheduleConvSave();
  }
});

convDetailsEl.addEventListener("click", (e) => {
  const btn = e.target.closest("[data-action]");
  if (!btn) return;
  const act = btn.dataset.action;
  if (act === "conv-create") createConversation();
  else if (act === "conv-cancel") {
    convDraft = null;
    renderConvPane();
  }
});

/* ---------- boot ---------- */

(async () => {
  await loadProjects();
  await Promise.all([loadConversations(), refreshConvMeta()]);
  const remembered = localStorage.getItem("flower.selected");
  const i = projects.findIndex((p) => p.dir === remembered);
  selected = i >= 0 ? i : projects.length ? 0 : -1;
  const wantedConv = localStorage.getItem("flower.conversation");
  const ci = conversations.findIndex((c) => c.id === wantedConv);
  convSel = ci >= 0 ? ci : conversations.length ? 0 : -1;
  render();

  const pane = Number(localStorage.getItem("flower.pane") || "0") || 0;
  if (narrow.matches && pane > 0) deckEl.scrollLeft = pane * deckEl.clientWidth;
  updateDots();
})();
