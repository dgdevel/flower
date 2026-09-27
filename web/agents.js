// agents.js — LLM endpoints and agent configuration on the config page.
// Mirrors src/agents.c: llms.json (named endpoints — url, key, headers in
// one place) and one json file per agent under agents/ (an llm reference,
// inference options, system prompt, mcp servers — the llmkit record
// shapes). Both lists are edited whole in memory and PUT whole, like the
// projects column; client-side validators mirror the server's rules and
// the server has the final word. Saving agents first persists pending
// llm edits, so a freshly added endpoint can be referenced immediately.

/* ---------- shared helpers ---------- */

const PROTOCOLS = [
  ["openai", "OpenAI — chat completions"],
  ["openai_responses", "OpenAI — responses"],
  ["anthropic", "Anthropic"],
];
const MCP_REVISIONS = ["2024-11-05", "2025-03-26", "2025-06-18", "2025-11-25", "2026-07-28"];
const TRANSPORTS = [["stdio", "stdio — local command"], ["http", "http — streamable"], ["sse", "sse — legacy"]];
const EFFORTS = ["low", "medium", "high"];
const NUM_OPTS = [
  ["temperature", "Temperature"],
  ["top_p", "Top-p"],
  ["max_tokens", "Max tokens"],
  ["top_k", "Top-k (anthropic)"],
  ["thinking_budget", "Thinking budget (anthropic)"],
  ["presence_penalty", "Presence penalty"],
  ["frequency_penalty", "Frequency penalty"],
  ["seed", "Seed"],
];

const bytes = (s) => new TextEncoder().encode(s || "").length;
const noControls = (s, multiline = false) =>
  multiline ? !/[\x00-\x08\x0b-\x1f\x7f]/.test(s) : !/[\x00-\x1f\x7f]/.test(s);

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
  const input = opts.textarea
    ? el("textarea", { rows: opts.rows || 4, "data-b": b, spellcheck: "false" })
    : el("input", { type: opts.type || "text", "data-b": b, spellcheck: "false" });
  if (opts.placeholder) input.placeholder = opts.placeholder;
  if (opts.value !== undefined) input.value = opts.value;
  if (opts.mono) input.classList.add("mono");
  return el("div", { class: "field" + (opts.wide ? " field-wide" : "") },
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
    // inputs without a dedicated error span (header rows, option grid):
    // red border + tooltip, so nothing fails silently
    let key = b;
    if (key === "io.stop") key = "stop-text";
    if (key.startsWith("io.")) key = key.slice(3);
    const input =
      root.querySelector(`[data-b="${CSS.escape(b)}"]`) ||
      root.querySelector(`[data-io="${CSS.escape(key)}"]`);
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
  name: (v, all) =>
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
    const bad = llmValidators[k](l[k], all);
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
      applyServerError("llms", field, msg);
      if (status()) setStatus(status(), msg, "warn");
      return false;
    }
    const selName = llms[llmSel] && llms[llmSel].name;
    llms = (await r.json()).map(llmFromWire);
    llmsSnapshot = JSON.stringify(llms);
    llmSel = Math.max(0, llms.findIndex((l) => l.name === selName));
    renderLlms();
    renderAgents(); // agent llm selects and missing flags follow the llms
    if (status()) setStatus(status(), "saved ✓", "ok");
    return true;
  } catch (_) {
    if (status()) setStatus(status(), "network error — nothing saved", "warn");
    return false;
  }
}

function llmsDirty() { return JSON.stringify(llms) !== llmsSnapshot; }

/* ============================================================
 * agents
 * ============================================================ */

const agentsUI = document.getElementById("agents-ui");
let agents = [];       // user agents: [{name, llm, inference_options, system_prompt, tools}]
let builtins = [];     // builtin agents (read-only): [{name, llm:"", system_prompt, builtin:true}]
let agentSel = -1;
let agentsSnapshot = "";

const agentValidators = {
  name: (v) => (v.trim() && bytes(v) <= 63 && noControls(v)) || "required, 1–63 bytes",
  system_prompt: (v) => bytes(v) <= 16383 || "max 16383 bytes",
};

function inferenceErrors(opts, protocol) {
  const errs = {};
  for (const [k] of NUM_OPTS) {
    const v = opts[k];
    if (v === undefined || v === "" || v === null) continue;
    if (typeof v !== "number" || !Number.isFinite(v)) errs[`io.${k}`] = "must be a number";
  }
  const stop = opts.stop || [];
  if (stop.length > 8) errs["io.stop"] = "too many (max 8)";
  stop.forEach((s, i) => {
    if (!s || bytes(s) > 127 || !noControls(s, true)) errs["io.stop"] = "entries are text, max 127 bytes";
  });
  if (opts.reasoning_effort && (bytes(opts.reasoning_effort) > 23 || !noControls(opts.reasoning_effort)))
    errs["io.reasoning_effort"] = "max 23 bytes";
  if (protocol === "anthropic") {
    if (!opts.max_tokens) errs["io.max_tokens"] = "anthropic endpoints require max_tokens";
    else if (opts.thinking_budget && Number(opts.thinking_budget) >= Number(opts.max_tokens))
      errs["io.thinking_budget"] = "must be lower than max_tokens";
  }
  return errs;
}

function toolErrors(t, i) {
  const errs = {};
  if (!["stdio", "http", "sse"].includes(t.type)) errs[`tools.${i}.type`] = "pick a transport";
  if (!t.name || !/^[A-Za-z0-9._-]{1,63}$/.test(t.name))
    errs[`tools.${i}.name`] = "letters/digits/dot/underscore/hyphen, e.g. fs";
  if (t.type === "stdio" && !t.command_line.trim())
    errs[`tools.${i}.command_line`] = "required — the full shell command";
  if (t.type !== "stdio" && !/^https?:\/\/\S+$/.test(t.url || ""))
    errs[`tools.${i}.url`] = "required, an http(s) URL";
  const terms = t.terminal_tools || [];
  if (terms.length > 16) errs[`tools.${i}.terminal_tools`] = "too many (max 16)";
  if (terms.some((x) => !/^[A-Za-z0-9._-]{1,63}$/.test(x)))
    errs[`tools.${i}.terminal_tools`] = "tool names, comma-separated";
  if (t.headers.length > 16) errs[`tools.${i}.headers`] = "too many headers (max 16)";
  const seen = new Set();
  t.headers.forEach((h, j) => {
    if (!h.name || !/^[\x21-\x7e]+$/.test(h.name)) errs[`tools.${i}.headers.${j}.name`] = "visible ASCII, no spaces";
    else if (seen.has(h.name.toLowerCase())) errs[`tools.${i}.headers.${j}.name`] = "duplicate header";
    else seen.add(h.name.toLowerCase());
    if (!h.value || bytes(h.value) > 255 || !noControls(h.value))
      errs[`tools.${i}.headers.${j}.value`] = "1–255 bytes, single line";
  });
  return errs;
}

function validateAgent(a, all) {
  const errs = {};
  for (const k of ["name", "system_prompt"]) {
    const bad = agentValidators[k](a[k]);
    if (bad !== true) errs[k] = bad;
  }
  if (!errs.name &&
      all.some((o) => o !== a && o.name.toLowerCase() === a.name.toLowerCase()))
    errs.name = "another agent already uses this name";
  if (!errs.name &&
      builtins.some((b) => b.name.toLowerCase() === a.name.toLowerCase()))
    errs.name = "a builtin agent already uses this name";
  if (!a.llm) errs.llm = "pick the llm this agent talks to";
  else if (!llms.some((l) => l.name.toLowerCase() === a.llm.toLowerCase()))
    errs.llm = `unknown llm “${a.llm}” — add or save it in the llm section first`;
  const protocol = (llms.find((l) => l.name.toLowerCase() === (a.llm || "").toLowerCase()) || {}).endpoint_protocol;
  Object.assign(errs, inferenceErrors(a.inference_options || {}, protocol));
  const names = new Set();
  a.tools.forEach((t, i) => {
    if (!errs[`tools.${i}.name`] && names.has(t.name))
      errs[`tools.${i}.name`] = "duplicate server name in this agent";
    names.add(t.name);
    Object.assign(errs, toolErrors(t, i));
  });
  if (a.tools.length > 16) errs.tools = "too many mcp servers (max 16)";
  return errs;
}

function agentToWire(a) {
  const io = {};
  for (const [k] of NUM_OPTS) {
    const v = a.inference_options[k];
    if (v !== "" && v !== undefined) io[k] = Number(v);
  }
  if (a.inference_options.reasoning_effort) io.reasoning_effort = a.inference_options.reasoning_effort;
  if ((a.inference_options.stop || []).length) io.stop = a.inference_options.stop;
  if (a.inference_options.stream === false) io.stream = false;
  const out = { name: a.name, llm: a.llm };
  if (Object.keys(io).length) out.inference_options = io;
  if (a.system_prompt) out.system_prompt = a.system_prompt;
  if (a.tools.length)
    out.tools = a.tools.map((t) => {
      const w = { type: t.type, name: t.name };
      if (t.type === "stdio") w.command_line = t.command_line;
      else {
        w.url = t.url;
        if (t.headers.length) w.headers = Object.fromEntries(t.headers.map((h) => [h.name, h.value]));
      }
      if (t.protocol) w.protocol = t.protocol;
      if (t.required) w.required = true;
      if ((t.terminal_tools || []).length) w.terminal_tools = t.terminal_tools;
      return w;
    });
  return out;
}

/* live reference check: the server's llm_ok flag at load, recomputed
 * from the current llms state after that (an llm deleted or renamed in
 * this session flags the agent immediately) */
function llmOk(a) {
  return !!a.llm && llms.some((l) => l.name.toLowerCase() === a.llm.toLowerCase());
}

function agentFromWire(a) {
  const io = { ...(a.inference_options || {}) };
  if (io.stop !== undefined && !Array.isArray(io.stop)) io.stop = [io.stop];
  return {
    name: a.name || "",
    llm: a.llm || "",
    inference_options: io,
    system_prompt: a.system_prompt || "",
    tools: (a.tools || []).map((t) => ({
      type: t.type || "stdio",
      name: t.name || "",
      command_line: t.command_line || "",
      url: t.url || "",
      headers: Object.entries(t.headers || {}).map(([name, value]) => ({ name, value })),
      protocol: t.protocol || "",
      required: !!t.required,
      terminal_tools: t.terminal_tools || [],
    })),
  };
}

function inferenceFields(io) {
  const grid = el("div", { class: "opt-grid" });
  for (const [k, label] of NUM_OPTS) {
    const v = io[k];
    grid.append(el("div", { class: "field" },
      el("label", { text: label }),
      el("input", {
        type: "number", step: "any", "data-io": k,
        value: v === undefined ? "" : v, placeholder: "default",
        spellcheck: "false",
      }),
      el("span", { class: "field-error", "data-err": `io.${k}` })));
  }
  grid.append(
    el("div", { class: "field" },
      el("label", { text: "Reasoning effort" }),
      (() => {
        const sel = el("select", { "data-io": "reasoning_effort" });
        sel.append(el("option", { value: "", text: "default" }));
        for (const e of EFFORTS) sel.append(el("option", { value: e, text: e }));
        sel.value = io.reasoning_effort || "";
        return sel;
      })(),
      el("span", { class: "field-error", "data-err": "io.reasoning_effort" })),
    el("div", { class: "field" },
      el("label", { text: "Stop sequences" }),
      el("input", {
        type: "text", "data-io": "stop-text", spellcheck: "false",
        value: (io.stop || []).join(", "),
        placeholder: "comma-separated",
      }),
      el("span", { class: "field-error", "data-err": "io.stop" })),
    el("div", { class: "field field-check" },
      el("label", { text: "Stream responses" }),
      el("input", { type: "checkbox", "data-io": "stream" }),
      el("span", { class: "field-error", "data-err": "io.stream" })));
  grid.querySelector('[data-io="stream"]').checked = io.stream !== false;
  return grid;
}

function toolCard(t, i) {
  const card = el("div", { class: "tool-card", "data-tool": i });
  card.append(
    el("div", { class: "field-row" },
      selectRow("Transport", `tools.${i}.type`, TRANSPORTS, t.type),
      fieldRow("Server name", `tools.${i}.name`, {
        value: t.name, placeholder: "tool names become name.tool",
      })));
  if (t.type === "stdio") {
    card.append(fieldRow("Command line", `tools.${i}.command_line`, {
      value: t.command_line, mono: true, placeholder: "npx -y @modelcontextprotocol/server-filesystem /tmp",
    }));
  } else {
    card.append(
      fieldRow("URL", `tools.${i}.url`, { value: t.url, mono: true, placeholder: "https://example.com/mcp" }),
      el("details", {
        class: "opts",
        ...(t.headers.length ? { open: "" } : {}),
      },
        el("summary", { text: `HTTP headers (${t.headers.length})` }),
        headerRows(t.headers, `tools.${i}.headers`, `tool-hdr-add`, `tool-hdr-del`)));
  }
  card.append(
    el("div", { class: "field-row" },
      selectRow("MCP protocol", `tools.${i}.protocol`,
        [["", "2025-11-25 (default)"], ...MCP_REVISIONS.filter((r) => r !== "2025-11-25").map((r) => [r, r])],
        t.protocol),
      fieldRow("Terminal tools", `tools.${i}.terminal_tools`, {
        value: (t.terminal_tools || []).join(", "),
        placeholder: "comma-separated; calling one ends the conversation",
      }),
      el("div", { class: "field field-check" },
        el("label", { text: "Required" }),
        el("input", { type: "checkbox", "data-b": `tools.${i}.required` }),
        el("span", { class: "field-error", "data-err": `tools.${i}.required` }))),
    el("button", {
      type: "button", class: "btn btn-ghost", "data-action": "tool-del", "data-i": i,
      text: "Remove server",
    }));
  card.querySelector(`[data-b="tools.${i}.required"]`).checked = !!t.required;
  return card;
}

function renderAgents() {
  agentsUI.textContent = "";
  const list = el("div", { class: "entity-list card" },
    el("button", { type: "button", class: "entity-add", "data-action": "agent-add", text: "+ Add agent" }));
  agents.forEach((a, i) => {
    const sub = `${a.llm || "no llm"} · ${a.tools.length} mcp server${a.tools.length === 1 ? "" : "s"}`;
    list.append(el("button", {
      type: "button",
      class: "entity-row" + (i === agentSel ? " active" : "") + (llmOk(a) ? "" : " missing"),
      "data-action": "agent-select", "data-i": i,
    },
      el("span", { class: "name", text: a.name || "(unnamed)" }),
      el("span", { class: "sub", text: sub })));
  });
  // the builtins: shipped with flower, selectable in conversations,
  // never editable here
  builtins.forEach((b) => {
    list.append(el("button", {
      type: "button", class: "entity-row builtin", disabled: "",
      title: "builtin agent — shipped with flower, not editable",
    },
      el("span", { class: "name", text: b.name }),
      el("span", { class: "sub", text: "builtin · llm picked per conversation" })));
  });

  const editor = el("div", { class: "entity-editor card" });
  if (agentSel < 0 || !agents[agentSel]) {
    editor.append(el("div", { class: "editor-empty" },
      el("p", { class: "placeholder-title", text: agents.length ? "No agent selected" : "No agents yet" }),
      el("p", { class: "placeholder-text", text: agents.length
        ? "Pick one from the list, or add another agent."
        : "An agent is an llm plus a personality: system prompt, inference options and mcp servers." })));
  } else {
    const a = agents[agentSel];
    const llmOptions = llms.map((l) => [l.name, l.name]);
    if (a.llm && !llms.some((l) => l.name.toLowerCase() === a.llm.toLowerCase()))
      llmOptions.push([a.llm, `${a.llm} — missing`]);
    if (!llmOptions.length) llmOptions.push(["", "— add an llm first —"]);

    const fs = el("fieldset", { class: "cfg-group" }, el("legend", { text: "Agent" }));
    fs.append(
      fieldRow("Name", "name", { value: a.name, placeholder: "e.g. gardener" }),
      selectRow("LLM", "llm", llmOptions, a.llm, {
        hint: a.llm && !llmOk(a) ? "the referenced llm is gone — pick another or add it back" : null,
      }));
    const io = el("details", { class: "opts", open: Object.keys(a.inference_options).some((k) => k !== "stop") },
      el("summary", { text: "Inference options" }),
      el("p", {
        class: "muted",
        text: "The llmkit inference options, sent with every request. Empty fields use the endpoint defaults.",
      }),
      inferenceFields(a.inference_options));
    fs.append(io);
    fs.append(fieldRow("System prompt", "system_prompt", {
      textarea: true, rows: 6, value: a.system_prompt,
      placeholder: "You are a concise unix expert…",
    }));
    editor.append(fs);

    const tools = el("fieldset", { class: "cfg-group" }, el("legend", { text: `MCP servers (${a.tools.length})` }));
    a.tools.forEach((t, i) => tools.append(toolCard(t, i)));
    tools.append(el("button", {
      type: "button", class: "btn btn-ghost", "data-action": "tool-add", text: "+ Add mcp server",
    }));
    editor.append(tools,
      el("div", { class: "entity-del" },
        el("button", { type: "button", class: "btn btn-danger", "data-action": "agent-del", text: "Delete agent" })));
    showErrors(editor, validateAgent(a, agents));
  }
  agentsUI.append(list, editor,
    el("div", { class: "entity-save" },
      el("span", { id: "agent-status", class: "muted", role: "status" }),
      el("button", { type: "button", class: "btn btn-accent", "data-action": "agent-save", text: "Save agents" })));
}

async function saveAgents() {
  const status = () => agentsUI.querySelector("#agent-status");
  // pending llm edits go first, so new references resolve server-side
  if (llmsDirty()) {
    if (status()) setStatus(status(), "saving llms…");
    if (!(await saveLlms())) {
      if (status()) setStatus(status(), "fix the llm section first", "warn");
      return;
    }
  }
  for (let i = 0; i < agents.length; i++) {
    const errs = validateAgent(agents[i], agents);
    if (Object.keys(errs).length) {
      agentSel = i;
      renderAgents();
      status().textContent = `fix “${agents[i].name || "unnamed"}” before saving`;
      status().className = "cfg-warn";
      return;
    }
  }
  if (status()) setStatus(status(), "saving…");
  const selName = agents[agentSel] && agents[agentSel].name;
  try {
    const r = await fetch("api/agents", {
      method: "PUT",
      headers: { "Content-Type": "application/json" },
      body: JSON.stringify(agents.map(agentToWire)),
    });
    if (!r.ok) {
      let msg = `save failed (${r.status})`, field = "";
      try { const e = await r.json(); msg = e.error; field = e.field || ""; } catch (_) {}
      applyServerError("agents", field, msg);
      if (status()) setStatus(status(), msg, "warn");
      return;
    }
    agents = (await r.json()).map(agentFromWire);
    agentsSnapshot = JSON.stringify(agents);
    agentSel = Math.max(0, agents.findIndex((a) => a.name === selName));
    renderAgents();
    if (status()) setStatus(status(), "saved ✓", "ok");
  } catch (_) {
    if (status()) setStatus(status(), "network error — nothing saved", "warn");
  }
}

/* map a server error field ("agents[2].tools[0].url") onto the editor */
function applyServerError(kind, field, msg) {
  const m = field && field.match(new RegExp(`^${kind}\\[(\\d+)\\](?:\\.(.*))?$`));
  if (!m) return;
  const i = Number(m[1]);
  const rest = (m[2] || "").replace(/\\/g, ".");
  if (kind === "llms") {
    if (llmSel !== i) { llmSel = i; renderLlms(); }
    const span = llmsUI.querySelector(`.field-error[data-err="${CSS.escape(rest)}"]`);
    if (span) { span.textContent = msg; }
  } else {
    if (agentSel !== i) { agentSel = i; renderAgents(); }
    const span = agentsUI.querySelector(`.field-error[data-err="${CSS.escape(rest)}"]`);
    if (span) span.textContent = msg;
  }
}

/* ============================================================
 * event wiring
 * ============================================================ */

function bindInputExact(target) {
  const b = target.dataset.b;
  if (!b) return false;
  const isLlm = !!target.closest("#llms-ui");
  const e = isLlm ? llms[llmSel] : agents[agentSel];
  if (!e) return true;

  let m;
  if ((m = b.match(/^headers\.(\d+)\.(name|value)$/))) {           // llm headers
    e.headers[+m[1]][m[2]] = target.value;
  } else if ((m = b.match(/^tools\.(\d+)\.headers\.(\d+)\.(name|value)$/))) { // tool headers
    const t = e.tools[+m[1]];
    if (t) t.headers[+m[2]][m[3]] = target.value;
  } else if ((m = b.match(/^tools\.(\d+)\.(name|command_line|url|terminal_tools)$/))) {
    const t = e.tools[+m[1]];
    if (!t) return true;
    if (m[2] === "terminal_tools")
      t.terminal_tools = target.value.split(",").map((s) => s.trim()).filter(Boolean);
    else t[m[2]] = target.value;
  } else if ((m = b.match(/^tools\.(\d+)\.required$/))) {
    const t = e.tools[+m[1]];
    if (t) t.required = target.checked;
  } else if ((m = b.match(/^tools\.(\d+)\.type$/))) {
    const t = e.tools[+m[1]];
    if (t) { t.type = target.value; renderAgents(); return true; }
  } else if (b in e) {
    e[b] = target.type === "checkbox" ? target.checked : target.value;
  }
  return true;
}

function bindInference(target, agent) {
  const k = target.dataset.io;
  const io = agent.inference_options;
  if (k === "stop-text") {
    io.stop = target.value.split(",").map((s) => s.trim()).filter(Boolean);
  } else if (k === "stream") {
    if (target.checked) delete io.stream;
    else io.stream = false;
  } else if (k === "reasoning_effort") {
    if (target.value) io.reasoning_effort = target.value;
    else delete io.reasoning_effort;
  } else {
    if (target.value === "" || target.value === null) delete io[k];
    else {
      const n = Number(target.value);
      io[k] = Number.isFinite(n) && target.value.trim() !== "" ? n : target.value;
    }
  }
}

document.addEventListener("input", (e) => {
  const t = e.target;
  if (!t.closest("#llms-ui") && !t.closest("#agents-ui")) return;
  if (t.dataset.io) {
    const a = agents[agentSel];
    if (a) bindInference(t, a);
  } else {
    bindInputExact(t);
  }
  liveValidate(t);
});
document.addEventListener("change", (e) => {
  const t = e.target;
  if (!t.closest("#llms-ui") && !t.closest("#agents-ui")) return;
  if (t.tagName === "SELECT" || t.type === "checkbox") {
    if (t.dataset.io) {
      const a = agents[agentSel];
      if (a) bindInference(t, a);
    } else {
      bindInputExact(t);
    }
  }
  liveValidate(t);
});

function liveValidate(t) {
  const root = t.closest(".entity-editor");
  if (!root) return;
  if (root.closest("#llms-ui") && llms[llmSel]) {
    showErrors(root, validateLlm(llms[llmSel], llms));
    updateRowText("llms");
  } else if (root.closest("#agents-ui") && agents[agentSel]) {
    showErrors(root, validateAgent(agents[agentSel], agents));
    updateRowText("agents");
  }
  markDirty();
}

function updateRowText(kind) {
  const ui = kind === "llms" ? llmsUI : agentsUI;
  const i = kind === "llms" ? llmSel : agentSel;
  const row = ui.querySelector(`.entity-row[data-i="${i}"]`);
  if (!row) return;
  if (kind === "llms") {
    const l = llms[i];
    row.querySelector(".name").textContent = l.name || "(unnamed)";
    row.querySelector(".sub").textContent = `${l.endpoint_protocol} · ${l.model || l.api_base}`;
  } else {
    const a = agents[i];
    row.querySelector(".name").textContent = a.name || "(unnamed)";
    row.querySelector(".sub").textContent = `${a.llm || "no llm"} · ${a.tools.length} mcp server${a.tools.length === 1 ? "" : "s"}`;
  }
}

function markDirty() {
  const s1 = llmsUI.querySelector("#llm-status");
  if (s1 && llmsDirty() && !s1.classList.contains("cfg-warn") && !s1.classList.contains("cfg-ok"))
    setStatus(s1, "unsaved changes");
  else if (s1 && !llmsDirty() && s1.textContent === "unsaved changes") s1.textContent = "";
  const s2 = agentsUI.querySelector("#agent-status");
  if (s2 && (llmsDirty() || JSON.stringify(agents) !== agentsSnapshot) &&
      !s2.classList.contains("cfg-warn") && !s2.classList.contains("cfg-ok"))
    setStatus(s2, "unsaved changes");
  else if (s2 && !(llmsDirty() || JSON.stringify(agents) !== agentsSnapshot) &&
           s2.textContent === "unsaved changes") s2.textContent = "";
}

document.addEventListener("click", async (e) => {
  const btn = e.target.closest("[data-action]");
  if (!btn) return;
  const inLlms = !!btn.closest("#llms-ui");
  const inAgents = !!btn.closest("#agents-ui");
  if (!inLlms && !inAgents) return;
  const act = btn.dataset.action;
  const i = +btn.dataset.i;

  if (act === "llm-select") { llmSel = i; renderLlms(); }
  else if (act === "agent-select") { agentSel = i; renderAgents(); }
  else if (act === "llm-add") {
    llms.push({ name: uniqueName(llms, "llm"), endpoint_protocol: "openai", api_base: "", model: "", api_key: "", headers: [] });
    llmSel = llms.length - 1;
    renderLlms();
    renderAgents(); // the new endpoint is referenceable before it is saved
    llmsUI.querySelector('[data-b="api_base"]')?.focus();
  }
  else if (act === "agent-add") {
    agents.push({
      name: uniqueName(agents, "agent"),
      llm: (llms[0] || {}).name || "",
      inference_options: {},
      system_prompt: "",
      tools: [],
    });
    agentSel = agents.length - 1;
    renderAgents();
    agentsUI.querySelector('[data-b="name"]')?.focus();
  }
  else if (act === "llm-del") {
    armDelete(btn, () => {
      llms.splice(llmSel, 1);
      llmSel = Math.min(llmSel, llms.length - 1);
      renderLlms();
      renderAgents();
      markDirty();
    });
  }
  else if (act === "agent-del") {
    armDelete(btn, () => {
      agents.splice(agentSel, 1);
      agentSel = Math.min(agentSel, agents.length - 1);
      renderAgents();
      markDirty();
    });
  }
  else if (act === "llm-save") saveLlms();
  else if (act === "agent-save") saveAgents();
  else if (act === "llm-hdr-add") {
    llms[llmSel].headers.push({ name: "", value: "" });
    renderLlms();
    llmsUI.querySelector(".kv-list:last-of-type input")?.focus();
  }
  else if (act === "llm-hdr-del") {
    llms[llmSel].headers.splice(i, 1);
    renderLlms();
  }
  else if (act === "tool-hdr-add") {
    agents[agentSel].tools[+btn.closest("[data-tool]")?.dataset.tool].headers.push({ name: "", value: "" });
    renderAgents();
  }
  else if (act === "tool-hdr-del") {
    const ti = +btn.closest("[data-tool]").dataset.tool;
    agents[agentSel].tools[ti].headers.splice(i, 1);
    renderAgents();
  }
  else if (act === "tool-add") {
    agents[agentSel].tools.push({
      type: "stdio", name: "", command_line: "", url: "", headers: [], protocol: "", required: false, terminal_tools: [],
    });
    renderAgents();
    agentsUI.querySelector(".tool-card:last-of-type [data-b$='.name']")?.focus();
  }
  else if (act === "tool-del") {
    agents[agentSel].tools.splice(i, 1);
    renderAgents();
  }
});

/* ---------- boot ---------- */

(async () => {
  try {
    const [lr, ar] = await Promise.all([fetch("api/llms"), fetch("api/agents")]);
    if (lr.ok) llms = (await lr.json()).map(llmFromWire);
    if (ar.ok) {
      const all = await ar.json();
      agents = all.filter((a) => !a.builtin).map(agentFromWire);
      builtins = all.filter((a) => a.builtin);
    }
  } catch (_) { /* editors still work; saves will fail loudly */ }
  llmsSnapshot = JSON.stringify(llms);
  agentsSnapshot = JSON.stringify(agents);
  llmSel = llms.length ? 0 : -1;
  agentSel = agents.length ? 0 : -1;
  renderLlms();
  renderAgents();
})();
