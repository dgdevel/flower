// config.js — theme editor. Field list mirrors src/theme.c; validators
// match the server-side rules so nothing invalid is ever sent.
const form = document.getElementById("theme-form");
const statusEl = document.getElementById("cfg-status");

const FIELDS = [
  { group: "Background", key: "background_primary",   kind: "color", label: "Primary — main canvas" },
  { group: "Background", key: "background_secondary", kind: "color", label: "Secondary — cards, panels, modals" },
  { group: "Background", key: "background_tertiary",  kind: "color", label: "Tertiary — hover state, active rows" },
  { group: "Text", key: "text_primary_color",   kind: "color", label: "Primary color — body text" },
  { group: "Text", key: "text_primary_font",    kind: "font",  label: "Primary font family" },
  { group: "Text", key: "text_primary_size",    kind: "size",  label: "Primary size" },
  { group: "Text", key: "text_secondary_color", kind: "color", label: "Secondary color — labels, timestamps" },
  { group: "Text", key: "text_secondary_font",  kind: "font",  label: "Secondary font family" },
  { group: "Text", key: "text_secondary_size",  kind: "size",  label: "Secondary size" },
  { group: "Accents", key: "accent",  kind: "color", label: "Accent / interactive — links, focus, the FLOW of the logo" },
  { group: "Accents", key: "success", kind: "color", label: "Success / positive — buttons, badges, the ER of the logo" },
  { group: "Accents", key: "warning", kind: "color", label: "Warning / negative — buttons, badges" },
];

const FONT_PLACEHOLDER = "system-ui, sans-serif";

const validators = {
  color: (v) => /^#[0-9a-fA-F]{6}$/.test(v) || "must be #rrggbb",
  font: (v) => /^[A-Za-z0-9 ,\-_']{1,95}$/.test(v) || "letters, digits, spaces and , - _ ' only",
  size: (v) => /^\d{1,3}(\.\d{1,2})?(px|rem|em|%)$/.test(v) || "e.g. 16px (px, rem, em, %)",
};

function setStatus(text, cls) {
  statusEl.textContent = text;
  statusEl.className = cls === "ok" ? "cfg-ok" : cls === "warn" ? "cfg-warn" : "muted";
}

function buildForm() {
  const groups = new Map();
  for (const f of FIELDS) {
    if (!groups.has(f.group)) {
      const fs = document.createElement("fieldset");
      fs.className = "card cfg-group";
      const legend = document.createElement("legend");
      legend.textContent = f.group;
      fs.appendChild(legend);
      form.appendChild(fs);
      groups.set(f.group, fs);
    }
    const row = document.createElement("div");
    row.className = "cfg-row";
    const label = document.createElement("label");
    label.htmlFor = "f-" + f.key;
    label.textContent = f.label;
    row.appendChild(label);
    const input = document.createElement("input");
    input.id = "f-" + f.key;
    input.name = f.key;
    input.type = f.kind === "color" ? "color" : "text";
    input.spellcheck = false;
    if (f.kind === "font") input.placeholder = FONT_PLACEHOLDER;
    if (f.kind === "size") input.placeholder = "16px";
    input.dataset.kind = f.kind;
    row.appendChild(input);
    const err = document.createElement("span");
    err.className = "field-error";
    row.appendChild(err);
    groups.get(f.group).appendChild(row);
  }
}

function fillForm(t) {
  for (const el of form.elements)
    if (el.name && t[el.name]) el.value = t[el.name];
}

function currentTheme() {
  const t = {};
  for (const el of form.elements) if (el.name) t[el.name] = el.value;
  return t;
}

function validateAll() {
  let ok = true;
  for (const el of form.elements) {
    if (!el.name) continue;
    const bad = validators[el.dataset.kind](el.value);
    const err = el.parentElement.querySelector(".field-error");
    if (bad !== true) {
      ok = false;
      err.textContent = bad;
      el.classList.add("invalid");
    } else {
      err.textContent = "";
      el.classList.remove("invalid");
    }
  }
  return ok;
}

form.addEventListener("input", (e) => {
  const el = e.target;
  if (!el.name) return;
  const bad = validators[el.dataset.kind](el.value);
  const err = el.parentElement.querySelector(".field-error");
  if (bad === true) {
    err.textContent = "";
    el.classList.remove("invalid");
    applyTheme({ [el.name]: el.value }); // instant preview
  } else {
    err.textContent = bad;
    el.classList.add("invalid");
  }
});

document.getElementById("save").addEventListener("click", async () => {
  if (!validateAll()) {
    setStatus("fix the highlighted fields first", "warn");
    return;
  }
  try {
    const r = await fetch("api/theme", {
      method: "PUT",
      headers: { "Content-Type": "application/json" },
      body: JSON.stringify(currentTheme()),
    });
    if (r.ok) {
      applyTheme(await r.json());
      setStatus("saved ✓", "ok");
    } else {
      let msg = "save failed (" + r.status + ")";
      try {
        const err = await r.json();
        msg = err.error + (err.field ? ` — “${err.field}”` : "");
      } catch (_) {}
      setStatus(msg, "warn");
    }
  } catch (_) {
    setStatus("network error", "warn");
  }
});

document.getElementById("reset").addEventListener("click", async () => {
  try {
    const r = await fetch("api/theme/reset", { method: "POST" });
    if (!r.ok) return;
    const t = await r.json();
    fillForm(t);
    applyTheme(t);
    setStatus("restored defaults ✓", "ok");
  } catch (_) {}
});

buildForm();
(async () => {
  try {
    const r = await fetch("api/theme");
    if (!r.ok) return;
    const t = await r.json();
    fillForm(t);
  } catch (_) {}
})();
