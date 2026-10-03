// conversations.js — the Conversations page: every llm conversation
// flower runs, past and present, in one readable place. The list and
// the open transcript follow the live store (src/conv.c): an SSE
// channel (api/conversations/stream) names a version that moves with
// every appended record, and each bump refetches what is on screen —
// a running conversation progresses in real time.
//
// Sub-agent conversations (a researcher invoked by the project scan)
// carry the spawning conversation's id in `parent`; the detail view
// links both ways. Records are (t, k, tool?, text?, err?) — see
// src/conv.h for the kinds.

/* ---------- helpers ---------- */

function el(tag, attrs = {}, ...children) {
  const node = document.createElement(tag);
  for (const [k, v] of Object.entries(attrs)) {
    if (k === "text") node.textContent = v;
    else if (v !== null && v !== undefined) node.setAttribute(k, v);
  }
  for (const c of children) if (c != null) node.append(c);
  return node;
}

const fmtTime = (t) =>
  t ? new Date(t * 1000).toLocaleTimeString() : "";

const fmtWhen = (t) =>
  t ? new Date(t * 1000).toLocaleString() : "";

function fmtDur(s) {
  if (!(s >= 0)) return "";
  s = Math.floor(s);
  const h = Math.floor(s / 3600), m = Math.floor((s % 3600) / 60);
  if (h) return `${h}h ${m}m`;
  if (m) return `${m}m ${s % 60}s`;
  return `${s}s`;
}

const STATE_BADGE = {
  running: "badge-accent",
  completed: "badge-success",
  failed: "badge-warning",
  stopped: "badge-muted",
};
const KIND_MARKS = {
  user: "❯", thinking: "…", response: "=",
  tool_call: "→", tool_result: "←", error: "⚠",
};

/* ---------- state ---------- */

let list = [];        // conversation metas, newest first (the wire order)
let projects = {};    // project id -> title
let selId = null;     // the open conversation's id
let detail = null;    // its full document (meta fields + records)
let tickTimer = 0;
const expanded = new Set(); // conversation ids whose sub-agents show
let delTimer = 0;     // the delete button's arm window

const listEl = document.getElementById("conv-list");
const countEl = document.getElementById("conv-count");
const detailEl = document.getElementById("conv-detail");

const convTitle = (c) => (c.title || "").trim() || c.agent || "Conversation";
const projectTitle = (id) =>
  id && projects[id] ? projects[id] : id || "";
const childrenOf = (id) => list.filter((c) => c.parent === id);

/* ---------- data ---------- */

async function loadProjects() {
  try {
    const r = await fetch("api/projects");
    if (r.ok)
      projects = Object.fromEntries((await r.json()).map((p) => [p.id, p.title]));
  } catch (_) { /* titles fall back to raw ids */ }
}

async function loadList() {
  try {
    const r = await fetch("api/conversations");
    if (r.ok) list = await r.json();
  } catch (_) { /* keep what we have */ }
  renderList();
}

async function loadDetail(id) {
  try {
    const r = await fetch("api/conversations/" + encodeURIComponent(id));
    detail = r.ok ? await r.json() : null;
  } catch (_) {
    detail = null;
  }
  if (!detail) selId = null;
  renderDetail();
}

/* open a conversation (from the list, a parent/child link, or the
 * page hash) */
async function openConv(id) {
  if (!id) return;
  selId = id;
  if (history.replaceState) history.replaceState(null, "", "#" + id);
  /* a sub-agent opens with its ancestors expanded, so its row shows */
  for (let cur = list.find((c) => c.id === id); cur && cur.parent;
       cur = list.find((c) => c.id === cur.parent))
    expanded.add(cur.parent);
  detail = null;
  renderDetail(); // the outline right away, records when they land
  renderList(); // the active row follows the selection
  await loadDetail(id);
}

/* ---------- rendering: the list ---------- */

const hasKids = (id) => list.some((c) => c.parent === id);

/* one row of the tree: a twistie when sub-agents sit below it, the
 * state badge (and the interactive flag), the title block, the time.
 * Children render under an expanded parent, indented by depth */
function convRow(c, depth) {
  const kids = hasKids(c.id);
  const open = expanded.has(c.id);
  return el("div", {
    class: "conv-row" + (c.id === selId ? " active" : "") +
           (depth ? " sub" : ""),
    "data-id": c.id, role: "button", tabindex: "0",
    style: depth ? `padding-left:${0.8 + depth * 1.5}rem` : "",
    title: convTitle(c), "aria-label": convTitle(c),
  },
    el("span", {
      class: "conv-tw" + (kids ? "" : " leaf"),
      "data-tw": kids ? c.id : null,
      role: "button", tabindex: "-1",
      "aria-label": open ? "Hide the sub-agent conversations"
                         : "Show the sub-agent conversations",
      text: kids ? (open ? "▾" : "▸") : "",
    }),
    el("span", {
      class: "badge " + (STATE_BADGE[c.state] || "badge-muted"),
      text: c.state || "?",
    }),
    c.interactive
      ? el("span", { class: "conv-flag", text: "interactive",
                     title: "You can reply while it runs" })
      : null,
    el("div", { class: "conv-main" },
      el("span", { class: "name", text: convTitle(c) }),
      el("span", { class: "sub" },
        (c.parent ? "sub-agent · " : "") + c.agent +
        (projectTitle(c.project) ? " · " + projectTitle(c.project) : "") +
        (c.llm ? " · " + c.llm : ""))),
    el("span", { class: "conv-when ts" },
      c.state === "running"
        ? "since " + fmtTime(c.started)
        : fmtWhen(c.started)));
}

function renderList() {
  listEl.textContent = "";
  if (!list.length) {
    listEl.append(el("div", { class: "conv-empty" },
      el("p", { class: "placeholder-title", text: "No conversations yet" }),
      el("p", {
        class: "placeholder-text",
        text: "Start a project scan or a planned task — the " +
              "conversation (and every sub-agent it spawns) is recorded here.",
      })));
    countEl.textContent = "";
    return;
  }
  const walk = (c, depth) => {
    listEl.append(convRow(c, depth));
    if (expanded.has(c.id))
      for (const kid of childrenOf(c.id)) walk(kid, depth + 1);
  };
  for (const c of list) if (!c.parent) walk(c, 0);
  const running = list.filter((c) => c.state === "running").length;
  countEl.textContent = running
    ? `${list.length} conversation${list.length === 1 ? "" : "s"}, ` +
      `${running} running`
    : `${list.length} conversation${list.length === 1 ? "" : "s"}`;
}

/* ---------- rendering: the open conversation ---------- */

function kvRow(label, value, opts = {}) {
  return el("div", { class: "task-kv" + (opts.warn ? " task-warn" : "") },
    el("label", { text: label }),
    el(opts.mono ? "code" : "p", {
      class: opts.mono ? "mono" : "",
      style: "margin:0", text: value,
    }));
}

function recordRow(r) {
  const k = r.k || "response";
  return el("div", { class: "conv-entry k-" + k + (r.err ? " is-err" : "") },
    el("span", { class: "conv-mark", text: KIND_MARKS[k] || "·" }),
    el("span", { class: "conv-time ts", text: fmtTime(r.t) }),
    r.tool ? el("span", { class: "conv-tool mono", text: r.tool }) : null,
    r.text ? el("span", { class: "conv-text", text: r.text }) : null);
}

function renderDetail() {
  clearInterval(tickTimer);
  detailEl.textContent = "";
  if (!selId) {
    detailEl.append(el("div", { class: "conv-empty" },
      el("p", { class: "placeholder-title", text: "No conversation open" }),
      el("p", {
        class: "placeholder-text",
        text: list.length
          ? "Pick one from the list above — running ones update live."
          : "Conversations appear here as soon as flower runs one.",
      })));
    return;
  }
  if (!detail) {
    detailEl.append(el("p", { class: "muted", text: "loading…" }));
    return;
  }

  const c = detail;
  const running = c.state === "running";
  const head = el("div", { class: "conv-head" },
    el("span", {
      class: "badge " + (STATE_BADGE[c.state] || "badge-muted"),
      text: c.state,
    }),
    c.interactive
      ? el("span", { class: "conv-flag", text: "interactive",
                     title: "You can reply while it runs" })
      : null,
    el("h2", { text: convTitle(c) }),
    running ? el("button", {
      type: "button", class: "btn btn-ghost conv-stop",
      id: "conv-stop",
      title: "Ask the runner to stop (click again to force)",
      text: "Stop",
    }) : null,
    el("button", {
      type: "button", class: "btn btn-danger conv-delete",
      id: "conv-delete", disabled: running ? "" : null,
      title: running ? "Stop the conversation before deleting it"
                     : "Delete this conversation and its sub-agents",
      text: "Delete",
    }));
  detailEl.append(head);

  const meta = el("div", { class: "conv-meta" },
    kvRow("Agent", c.agent),
    kvRow("LLM", c.llm || "—", { mono: true }),
    c.project ? kvRow("Project", projectTitle(c.project) || c.project,
                      { mono: !projects[c.project] }) : null,
    kvRow("Started", fmtWhen(c.started)),
    kvRow(running ? "Elapsed" : "Duration", "", { mono: false }));
  const durEl = meta.lastChild.querySelector("p, code");
  const dur = () => fmtDur(
    (running ? Date.now() / 1000 : c.ended || c.started) - c.started);
  durEl.textContent = dur();
  durEl.className = running ? "conv-elapsed" : "";
  detailEl.append(meta);
  if (running) tickTimer = setInterval(() => {
    durEl.textContent = dur();
  }, 1000);

  /* the parent link and the spawned sub-conversations */
  if (c.parent) {
    const parent = list.find((x) => x.id === c.parent);
    detailEl.append(el("button", {
      type: "button", class: "btn btn-ghost conv-parent",
      "data-id": c.parent, title: "Open the spawning conversation",
      text: "↩ part of " + (parent ? convTitle(parent) : c.parent.slice(0, 8)),
    }));
  }
  const kids = childrenOf(c.id);
  if (kids.length) {
    const box = el("div", { class: "conv-kids" },
      el("span", { class: "muted", text: "spawned:" }));
    for (const kid of kids)
      box.append(el("button", {
        type: "button", class: "chip chip-kid", "data-id": kid.id,
        title: convTitle(kid), text: (kid.agent || "?") + " ▸",
      }));
    detailEl.append(box);
  }

  const records = c.records || [];
  const log = el("div", { class: "conv-log", id: "conv-log" });
  if (c.records_dropped)
    log.append(el("p", {
      class: "muted",
      text: `… ${c.records_dropped} earlier record` +
            (c.records_dropped === 1 ? "" : "s") + " not shown",
    }));
  for (const r of records) log.append(recordRow(r));
  if (!records.length && !c.records_dropped)
    log.append(el("p", { class: "muted", text: "no records yet" }));
  detailEl.append(log);
  log.scrollTop = log.scrollHeight; /* live: follow the newest record */

  /* an interactive conversation takes replies while it runs: the
   * user's turn goes to the transcript and to the waiting planner */
  if (c.interactive && running) {
    detailEl.append(el("div", { class: "conv-reply" },
      el("textarea", {
        id: "conv-reply-text", rows: "3",
        placeholder: "Reply to refine the plan — the agent adjusts " +
                     "the task title and its action tree",
        "aria-label": "Reply to the planner",
      }),
      el("div", { class: "conv-reply-row" },
        el("button", {
          type: "button", class: "btn btn-accent",
          id: "conv-reply-send", text: "Send",
        }),
        el("span", { class: "muted", id: "conv-reply-err", role: "status" }))));
    const ta = document.getElementById("conv-reply-text");
    ta.addEventListener("keydown", (e) => {
      if (e.key === "Enter" && (e.ctrlKey || e.metaKey)) {
        e.preventDefault();
        sendReply();
      }
    });
  }
}

async function sendReply() {
  const ta = document.getElementById("conv-reply-text");
  const err = document.getElementById("conv-reply-err");
  const btn = document.getElementById("conv-reply-send");
  const text = (ta && ta.value || "").trim();
  if (!ta || !text || !selId) return;
  if (btn) btn.disabled = true;
  try {
    const r = await fetch("api/conversations/" +
                          encodeURIComponent(selId) + "/reply", {
      method: "POST",
      headers: { "Content-Type": "application/json" },
      body: JSON.stringify({ text }),
    });
    if (!r.ok) {
      let msg = `reply failed (${r.status})`;
      try { msg = (await r.json()).error || msg; } catch (_) {}
      if (err) err.textContent = msg;
      return;
    }
    if (err) err.textContent = "";
    ta.value = "";
    await refresh(); // the user record (and the model's turn) follow
  } catch (_) {
    if (err) err.textContent = "network error — the reply was not sent";
  } finally {
    if (btn) btn.disabled = false;
    const again = document.getElementById("conv-reply-text");
    if (again) again.focus();
  }
}

/* ---------- live updates ---------- */

/* one refresh at a time; callers that land mid-refresh await the run
 * already in flight — the boot's hash-open needs a list that was
 * actually fetched, not a skipped call (the focus handler can beat
 * the boot to the first refresh). A bump landing mid-refresh is not
 * lost either: one trailing re-run. */
let refreshRun = null;
let refreshAgain = false;

function refresh() {
  if (refreshRun) {
    refreshAgain = true;
    return refreshRun;
  }
  const run = (async () => {
    try {
      await loadList();
      if (selId) await loadDetail(selId);
    } finally {
      refreshRun = null;
      if (refreshAgain) {
        refreshAgain = false;
        refresh();
      }
    }
  })();
  refreshRun = run;
  return run;
}

function watch() {
  try {
    const es = new EventSource("api/conversations/stream");
    es.addEventListener("conv", () => refresh());
    /* the clock ticks ride the same stream (proxy keep-alive); the
     * page ignores the unnamed events */
  } catch (_) { /* no SSE: the focus/visibility handlers carry it */ }
}

document.addEventListener("visibilitychange", () => {
  if (!document.hidden) refresh();
});
window.addEventListener("focus", refresh);

/* ---------- events ---------- */

listEl.addEventListener("click", (e) => {
  const tw = e.target.closest("[data-tw]");
  if (tw) { // expand/collapse the sub-agents, not open the row
    if (expanded.has(tw.dataset.tw)) expanded.delete(tw.dataset.tw);
    else expanded.add(tw.dataset.tw);
    renderList();
    return;
  }
  const row = e.target.closest(".conv-row[data-id]");
  if (row) openConv(row.dataset.id);
});
listEl.addEventListener("keydown", (e) => {
  if (e.key !== "Enter" && e.key !== " ") return;
  const row = e.target.closest(".conv-row[data-id]");
  if (!row || e.target !== row) return;
  e.preventDefault();
  openConv(row.dataset.id);
});

detailEl.addEventListener("click", async (e) => {
  const send = e.target.closest("#conv-reply-send");
  if (send) {
    sendReply();
    return;
  }
  const stop = e.target.closest("#conv-stop");
  if (stop) {
    /* interactive conversations stop through their engine; the scan
     * keeps its own endpoint */
    const url = detail && detail.interactive
      ? "api/plan/stop" : "api/scan/stop";
    try { await fetch(url, { method: "POST" }); } catch (_) {}
    setTimeout(refresh, 800); /* the version bump usually arrives first */
    return;
  }
  const del = e.target.closest("#conv-delete");
  if (del) {
    if (del.classList.contains("armed")) {
      clearTimeout(delTimer);
      try {
        const r = await fetch("api/conversations/" +
                              encodeURIComponent(selId), { method: "DELETE" });
        if (!r.ok) {
          let msg = `delete failed (${r.status})`;
          try { msg = (await r.json()).error || msg; } catch (_) {}
          del.textContent = "Delete";
          del.title = msg;
          return;
        }
      } catch (_) { del.textContent = "Delete"; return; }
      selId = null;
      detail = null;
      if (history.replaceState) history.replaceState(null, "", "#");
      await refresh();
      renderDetail();
      return;
    }
    /* ask twice, like every destructive button here */
    clearTimeout(delTimer);
    del.classList.add("armed");
    del.textContent = "sure?";
    delTimer = setTimeout(() => {
      del.classList.remove("armed");
      del.textContent = "Delete";
    }, 2500);
    return;
  }
  const link = e.target.closest("[data-id]");
  if (link) openConv(link.dataset.id);
});

/* ---------- boot ---------- */

(async () => {
  await loadProjects();
  await refresh();
  watch();
  const hash = decodeURIComponent(location.hash.slice(1));
  if (hash && list.some((c) => c.id === hash)) await openConv(hash);
})();
