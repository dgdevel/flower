// llms.js — LLM endpoint configuration on the config page. Mirrors
// src/agents.c: llms.json keeps the named endpoints (url, key, headers)
// in one place. The list is edited whole in memory and PUT whole, like
// the projects column; client-side validators mirror the server's rules
// and the server has the final word.

/* ---------- helpers ---------- */

const PROTOCOLS = [
  ["openai", "OpenAI — chat completions"],
  ["openai_responses", "OpenAI — responses"],
  ["anthropic", "Anthropic"],
];

const bytes = (s) => new TextEncoder().encode(s || "").length;
const noControls = (s) => !/[\x00-\x1f\x7f]/.test(s);

function el(tag, attrs = {}, ...children) {
  const node = document.createElement(tag);
  for (const [k, v] of Object.entries(attrs)) {
    if (k === "text") node.textContent = v;
    else if (k === "value") node.value = v;
    else if (v !== undefined && v !== null) node.setAttribute(k, v);
  }
  for (const c of children) if (c != null) node.append(c);
  return node;
}

function fieldRow(label, b, opts = {}) {
  const input = el("input", { type: opts.type || "text", "data-b": b, spellcheck: "false" });
  if (opts.placeholder) input.placeholder = opts.placeholder;
  if (opts.value !== undefined) input.value = opts.value;
  if (opts.mono) input.classList.add("mono");
  return el("div", { class: "field" },
    el("label", { text: label }),
    input,
    el("span", { class: "field-error", "data-err": b }));
}

function selectRow(label, b, options, value, opts = {}) {
  const sel = el("select", { "data-b": b });
  for (const [v, text] of options) {
    const o = el("option", { value: v, text });
    if (v === value) o.selected = true;
    sel.appendChild(o);
  }
  return el("div", { class: "field" },
    el("label", { text: label }),
    sel,
    el("span", { class: "field-error", "data-err": b }),
    opts.hint ? el("p", { class: "muted", text: opts.hint }) : null);
}

function headerRows(list, prefix, addAction, delAction) {
  const wrap = el("div", { class: "kv-list" });
  list.forEach((h, i) => {
    wrap.append(el("div", { class: "kv-row" },
      el("input", {
        "data-b": `${prefix}.${i}.name`, value: h.name,
        placeholder: "Header name", spellcheck: "false",
      }),
      el("input", {
        "data-b": `${prefix}.${i}.value`, value: h.value,
        placeholder: "Value", spellcheck: "false", class: "mono",
      }),
      el("button", {
        type: "button", class: "btn btn-ghost kv-del", "data-action": delAction,
        "data-i": i, title: "Remove header", "aria-label": "Remove header", text: "✕",
      })));
  });
  wrap.append(el("button", {
    type: "button", class: "btn btn-ghost", "data-action": addAction, text: "+ Add header",
  }));
  return wrap;
}

function showErrors(root, errors) {
  for (const span of root.querySelectorAll(".field-error")) span.textContent = "";
  for (const input of root.querySelectorAll(".invalid")) {
    input.classList.remove("invalid");
    input.removeAttribute("title");
  }
  for (const [b, msg] of Object.entries(errors || {})) {
    const span = root.querySelector(`.field-error[data-err="${CSS.escape(b)}"]`);
    if (span) span.textContent = msg;
    // header inputs have no dedicated error span: red border + tooltip,
    // so nothing fails silently
    const input = root.querySelector(`[data-b="${CSS.escape(b)}"]`);
    if (input && !span) {
      input.classList.add("invalid");
      input.title = msg;
    }
  }
}

function setStatus(elm, text, kind) {
  elm.textContent = text;
  elm.className = kind === "ok" ? "cfg-ok" : kind === "warn" ? "cfg-warn" : "muted";
}

function uniqueName(list, base) {
  let name = base, n = 2;
  while (list.some((x) => x.name.toLowerCase() === name.toLowerCase()))
    name = `${base} ${n++}`;
  return name;
}

function armDelete(btn, onConfirm) {
  if (!btn.classList.contains("armed")) {
    disarmDeletes(btn.closest(".entity"));
    btn.classList.add("armed");
    btn.dataset.label = btn.textContent;
    btn.textContent = "Really delete?";
    setTimeout(() => disarm(btn), 2500);
    return;
  }
  onConfirm();
}
function disarm(btn) {
  if (btn && btn.classList.contains("armed")) {
    btn.classList.remove("armed");
    btn.textContent = btn.dataset.label || "Delete";
  }
}
function disarmDeletes(root) {
  for (const b of root.querySelectorAll(".armed")) disarm(b);
}

/* ============================================================
 * llm endpoints
 * ============================================================ */

const llmsUI = document.getElementById("llms-ui");
let llms = [];          // [{name, endpoint_protocol, api_base, model, api_key, headers:[{name,value}]}]
let llmSel = -1;
let llmsSnapshot = "";

const PROTOCOL_VALUES = PROTOCOLS.map(([v]) => v);

const llmValidators = {
  name: (v) =>
    (v.trim() && bytes(v) <= 63 && noControls(v)) || "required, 1–63 bytes",
  endpoint_protocol: (v) =>
    PROTOCOL_VALUES.includes(v) || "pick a protocol",
  api_base: (v) =>
    (/^https?:\/\/\S+$/.test(v) && bytes(v) <= 511) ||
    "required, an http(s) URL like http://localhost:11434/v1",
  model: (v) => !v || (bytes(v) <= 127 && noControls(v)) || "max 127 bytes, single line",
  api_key: (v) => !v || (bytes(v) <= 255 && noControls(v)) || "max 255 bytes, single line",
};

function validateLlm(l, all) {
  const errs = {};
  for (const k of ["name", "endpoint_protocol", "api_base", "model", "api_key"]) {
    const bad = llmValidators[k](l[k]);
    if (bad !== true) errs[k] = bad;
  }
  if (!errs.name &&
      all.some((o, i) => i !== llmIndexOf(o) && o.name.toLowerCase() === l.name.toLowerCase()))
    errs.name = "another llm already uses this name";
  if (l.headers.length > 16) errs["headers"] = "too many headers (max 16)";
  const seen = new Set();
  l.headers.forEach((h, i) => {
    if (!h.name || !/^[\x21-\x7e]+$/.test(h.name))
      errs[`headers.${i}.name`] = "visible ASCII, no spaces";
    else if (seen.has(h.name.toLowerCase()))
      errs[`headers.${i}.name`] = "duplicate header";
    else seen.add(h.name.toLowerCase());
    if (!h.value || bytes(h.value) > 255 || !noControls(h.value))
      errs[`headers.${i}.value`] = "1–255 bytes, single line";
  });
  return errs;
}
function llmIndexOf(l) { return llms.indexOf(l); }

function llmToWire(l) {
  const out = { name: l.name, endpoint_protocol: l.endpoint_protocol, api_base: l.api_base };
  if (l.model) out.model = l.model;
  if (l.api_key) out.api_key = l.api_key;
  if (l.headers.length) out.headers = Object.fromEntries(l.headers.map((h) => [h.name, h.value]));
  return out;
}

function llmFromWire(l) {
  return {
    name: l.name || "",
    endpoint_protocol: l.endpoint_protocol || "openai",
    api_base: l.api_base || "",
    model: l.model || "",
    api_key: l.api_key || "",
    headers: Object.entries(l.headers || {}).map(([name, value]) => ({ name, value })),
  };
}

function renderLlms() {
  llmsUI.textContent = "";
  const list = el("div", { class: "entity-list card" },
    el("button", {
      type: "button", class: "entity-add", "data-action": "llm-add", text: "+ Add llm",
    }));
  llms.forEach((l, i) => {
    list.append(el("button", {
      type: "button", class: "entity-row" + (i === llmSel ? " active" : ""),
      "data-action": "llm-select", "data-i": i,
    },
      el("span", { class: "name", text: l.name || "(unnamed)" }),
      el("span", { class: "sub", text: `${l.endpoint_protocol} · ${l.model || l.api_base}` })));
  });

  const editor = el("div", { class: "entity-editor card" });
  if (llmSel < 0 || !llms[llmSel]) {
    editor.append(el("div", { class: "editor-empty" },
      el("p", { class: "placeholder-title", text: llms.length ? "No llm selected" : "No llms yet" }),
      el("p", { class: "placeholder-text", text: llms.length
        ? "Pick one from the list, or add another endpoint."
        : "Add the endpoint your models live behind — ollama, an openai-compatible server, anthropic…" })));
  } else {
    const l = llms[llmSel];
    const fs = el("fieldset", { class: "cfg-group" }, el("legend", { text: "Endpoint" }));
    fs.append(
      fieldRow("Name", "name", { value: l.name, placeholder: "e.g. ollama-local" }),
      selectRow("Protocol", "endpoint_protocol", PROTOCOLS, l.endpoint_protocol),
      fieldRow("API base", "api_base", {
        value: l.api_base, mono: true, placeholder: "http://localhost:11434/v1",
      }),
      fieldRow("Model", "model", { value: l.model, placeholder: "optional — llama.cpp style endpoints work without" }),
      fieldRow("API key", "api_key", {
        value: l.api_key, mono: true, placeholder: "optional — sent as Authorization: Bearer …",
      }));
    const details = el("details", {
      class: "opts",
      ...(l.headers.length ? { open: "" } : {}),
    },
      el("summary", { text: `HTTP headers (${l.headers.length})` }),
      el("p", {
        class: "muted",
        text: "Sent verbatim with every request. The anthropic api wants x-api-key here, not the api key field.",
      }),
      headerRows(l.headers, "headers", "llm-hdr-add", "llm-hdr-del"));
    fs.append(details);
    editor.append(fs,
      el("div", { class: "entity-del" },
        el("button", {
          type: "button", class: "btn btn-danger", "data-action": "llm-del", text: "Delete llm",
        })));
    showErrors(editor, validateLlm(l, llms));
  }
  llmsUI.append(list, editor,
    el("div", { class: "entity-save" },
      el("span", { id: "llm-status", class: "muted", role: "status" }),
      el("button", {
        type: "button", class: "btn btn-accent", "data-action": "llm-save", text: "Save llms",
      })));
}

async function saveLlms() {
  const status = () => llmsUI.querySelector("#llm-status");
  // every entry must be valid — jump to the first broken one
  for (let i = 0; i < llms.length; i++) {
    const errs = validateLlm(llms[i], llms);
    if (Object.keys(errs).length) {
      llmSel = i;
      renderLlms();
      status().textContent = `fix “${llms[i].name || "unnamed"}” before saving`;
      status().className = "cfg-warn";
      return false;
    }
  }
  if (status()) setStatus(status(), "saving…");
  try {
    const r = await fetch("api/llms", {
      method: "PUT",
      headers: { "Content-Type": "application/json" },
      body: JSON.stringify(llms.map(llmToWire)),
    });
    if (!r.ok) {
      let msg = `save failed (${r.status})`, field = "";
      try { const e = await r.json(); msg = e.error; field = e.field || ""; } catch (_) {}
      applyServerError(field, msg);
      if (status()) setStatus(status(), msg, "warn");
      return false;
    }
    const selName = llms[llmSel] && llms[llmSel].name;
    llms = (await r.json()).map(llmFromWire);
    llmsSnapshot = JSON.stringify(llms);
    llmSel = Math.max(0, llms.findIndex((l) => l.name === selName));
    renderLlms();
    if (status()) setStatus(status(), "saved ✓", "ok");
    return true;
  } catch (_) {
    if (status()) setStatus(status(), "network error — nothing saved", "warn");
    return false;
  }
}

function llmsDirty() { return JSON.stringify(llms) !== llmsSnapshot; }

/* map a server error field ("llms[1].api_base") onto the editor */
function applyServerError(field, msg) {
  const m = field && field.match(/^llms\[(\d+)\](?:\.(.*))?$/);
  if (!m) return;
  const i = Number(m[1]);
  const rest = (m[2] || "").replace(/\\/g, ".");
  if (llmSel !== i) { llmSel = i; renderLlms(); }
  const span = llmsUI.querySelector(`.field-error[data-err="${CSS.escape(rest)}"]`);
  if (span) span.textContent = msg;
}

/* ============================================================
 * event wiring
 * ============================================================ */

function bindInputExact(target) {
  const b = target.dataset.b;
  if (!b) return false;
  const l = llms[llmSel];
  if (!l) return true;

  let m;
  if ((m = b.match(/^headers\.(\d+)\.(name|value)$/))) {
    l.headers[+m[1]][m[2]] = target.value;
  } else if (b in l) {
    l[b] = target.value;
  }
  return true;
}

document.addEventListener("input", (e) => {
  const t = e.target;
  if (!t.closest("#llms-ui")) return;
  bindInputExact(t);
  liveValidate(t);
});
document.addEventListener("change", (e) => {
  const t = e.target;
  if (!t.closest("#llms-ui")) return;
  if (t.tagName === "SELECT") bindInputExact(t);
  liveValidate(t);
});

function liveValidate(t) {
  const root = t.closest(".entity-editor");
  if (!root || !llms[llmSel]) return;
  showErrors(root, validateLlm(llms[llmSel], llms));
  updateRowText();
  markDirty();
}

function updateRowText() {
  const row = llmsUI.querySelector(`.entity-row[data-i="${llmSel}"]`);
  if (!row) return;
  const l = llms[llmSel];
  row.querySelector(".name").textContent = l.name || "(unnamed)";
  row.querySelector(".sub").textContent = `${l.endpoint_protocol} · ${l.model || l.api_base}`;
}

function markDirty() {
  const s = llmsUI.querySelector("#llm-status");
  if (!s) return;
  if (llmsDirty() && !s.classList.contains("cfg-warn") && !s.classList.contains("cfg-ok"))
    setStatus(s, "unsaved changes");
  else if (!llmsDirty() && s.textContent === "unsaved changes") s.textContent = "";
}

document.addEventListener("click", (e) => {
  const btn = e.target.closest("[data-action]");
  if (!btn || !btn.closest("#llms-ui")) return;
  const act = btn.dataset.action;
  const i = +btn.dataset.i;

  if (act === "llm-select") { llmSel = i; renderLlms(); }
  else if (act === "llm-add") {
    llms.push({ name: uniqueName(llms, "llm"), endpoint_protocol: "openai", api_base: "", model: "", api_key: "", headers: [] });
    llmSel = llms.length - 1;
    renderLlms();
    llmsUI.querySelector('[data-b="api_base"]')?.focus();
  }
  else if (act === "llm-del") {
    armDelete(btn, () => {
      llms.splice(llmSel, 1);
      llmSel = Math.min(llmSel, llms.length - 1);
      renderLlms();
      markDirty();
    });
  }
  else if (act === "llm-save") saveLlms();
  else if (act === "llm-hdr-add") {
    llms[llmSel].headers.push({ name: "", value: "" });
    renderLlms();
    llmsUI.querySelector(".kv-list:last-of-type input")?.focus();
  }
  else if (act === "llm-hdr-del") {
    llms[llmSel].headers.splice(i, 1);
    renderLlms();
  }
});

/* ---------- boot ---------- */

(async () => {
  try {
    const r = await fetch("api/llms");
    if (r.ok) llms = (await r.json()).map(llmFromWire);
  } catch (_) { /* editor still works; saves will fail loudly */ }
  llmsSnapshot = JSON.stringify(llms);
  llmSel = llms.length ? 0 : -1;
  renderLlms();
})();
