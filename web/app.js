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

/* ---------- boot ---------- */

(async () => {
  await loadProjects();
  const remembered = localStorage.getItem("flower.selected");
  const i = projects.findIndex((p) => p.dir === remembered);
  selected = i >= 0 ? i : projects.length ? 0 : -1;
  render();

  const pane = Number(localStorage.getItem("flower.pane") || "0") || 0;
  if (narrow.matches && pane > 0) deckEl.scrollLeft = pane * deckEl.clientWidth;
  updateDots();
})();
