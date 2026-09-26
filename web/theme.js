// theme.js — fetch the server-side theme and apply it as CSS custom
// properties. Every page includes this; style.css holds the same values
// as fallback for the moment before (or in case) the fetch completes.
const THEME_FIELDS = [
  ["background_primary",   "--bg-primary"],
  ["background_secondary", "--bg-secondary"],
  ["background_tertiary",  "--bg-tertiary"],
  ["text_primary_color",   "--fg-primary"],
  ["text_primary_font",    "--font-primary"],
  ["text_primary_size",    "--size-primary"],
  ["text_secondary_color", "--fg-secondary"],
  ["text_secondary_font",  "--font-secondary"],
  ["text_secondary_size",  "--size-secondary"],
  ["accent",               "--accent"],
  ["success",              "--success"],
  ["warning",              "--warning"],
];

function applyTheme(t) {
  const style = document.documentElement.style;
  for (const [key, cssVar] of THEME_FIELDS)
    if (t[key]) style.setProperty(cssVar, t[key]);
}

async function loadTheme() {
  try {
    const r = await fetch("api/theme");
    if (!r.ok) return;
    applyTheme(await r.json());
  } catch (_) {
    /* keep the CSS defaults */
  }
}
loadTheme();
