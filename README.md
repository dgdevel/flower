# flower

A web application shipped as a **single static binary**, written in C11.
The frontend lives in ordinary files under `web/`; at build time a small
generator compiles them into the executable, so deployment is one file.

**Status: part 3 — the application UI.** Parts 1–2 proved the base
architecture (embedded assets, epoll HTTP server, SSE) and added the
config directory with a `theme.json` backend and config UI. The main
screen is now a three-column layout; the projects column is live,
columns two and three are placeholders. See *Roadmap*.

## Quick start

```sh
make run            # build + serve on http://0.0.0.0:8080/
./flower -p 9000    # or: custom port
./flower -b 127.0.0.1   # or: loopback only
make check          # end-to-end smoke tests (tests/smoke.sh)
```

Pages: `/` (main app — three columns, projects manager), `/config.html`
(theme editor), `/components.html` (themed component gallery).

Requires: Linux, a C11 compiler (`cc`), GNU make ≥ 4.3 (grouped targets),
and **cJSON** (system package `libcjson`, discovered via pkg-config).

## How it works

**Build time** — assets are compiled into the binary:

```
web/*.html ─┐
web/*.css   ─┼─ tools/embed ──> src/assets_gen.{c,h} ──> linked into `flower`
web/*.js    ─┘   (host tool,      (C arrays + MIME        (serves everything
                       run by make)     + served path)          from memory)
```

**Run time** — one thread, one `epoll` loop, all sockets non-blocking:

```
              ┌────────────────────────────────────────────────────┐
 client ─────►│ accept ─► parse head ─► [read body] ─► route       │
              │                                    │               │
              │   static asset   │ SSE /api/time   │ /api/theme    │
              │   ───────────    │ ─────────────   │ ───────────   │
              │   200 + body     │ 200 + stream,   │ GET/PUT/reset │
              │   (keep-alive)   │ tick every 1 s  │ (cJSON, file) │
              └────────────────────────────────────────────────────┘
```

Because nothing blocks, one slow SSE client can never stall the others
(verified: 3 concurrent SSE streams + interleaved requests at ~0.3 ms).

## Configuration (filesystem backend)

The config directory is `$XDG_CONFIG_HOME/flower/`, falling back to
`~/.config/flower/` when the variable is unset (override with `-c DIR`).
The directory is created with mode 0700 on first start.

`theme.json` — all 12 keys are optional on disk (missing/invalid values
fall back to defaults), pretty-printed, written atomically (tmp + rename):

```json
{
  "background_primary":   "#0d1117",
  "background_secondary": "#161b22",
  "background_tertiary":  "#21262d",
  "text_primary_color":   "#e6edf3",
  "text_primary_font":    "system-ui, -apple-system, 'Segoe UI', sans-serif",
  "text_primary_size":    "16px",
  "text_secondary_color": "#8b949e",
  "text_secondary_font":  "system-ui, -apple-system, 'Segoe UI', sans-serif",
  "text_secondary_size":  "13px",
  "accent":   "#58a6ff",
  "success":  "#3fb950",
  "warning":  "#d29922"
}
```

Validation (enforced server-side on PUT, mirrored client-side):
colors must be `#rrggbb`; sizes like `16px` (unit `px`, `rem`, `em`, `%`);
fonts limited to ASCII letters, digits, spaces and `, - _ '` (keeps values
injection-safe as CSS custom properties). The browser applies the theme as
CSS custom properties (`--bg-primary`, `--fg-primary`, …) the moment it is
saved — `web/style.css` carries the same defaults as fallback.

`projects.json` — the user's project list (main screen, column one),
same lenient/strict contract, one file per concern:

```json
[
  {
    "dir":   "/home/dev/flower",
    "title": "flower",
    "color": "#58a6ff",
    "emoji": "🌸"
  }
]
```

A project's identity is its `dir` (unique, must be an absolute path);
`title` defaults to the directory's basename when empty, `color` to a
cycling palette and `emoji` to 🌸. Colors are `#rrggbb`, emoji/titles
are valid UTF-8 without control characters (≤ 31 / 96 bytes), at most
64 projects.

Working directories must **exist on disk**: PUT validates each `dir`
with `stat()` (422 `directory does not exist` / `not a directory`).
If a directory vanishes after saving, the entry stays and
`GET /api/projects` reports it with `"exists": false` — the UI flags
the project (chip badge, editor warning) and blocks columns two and
three until the path is fixed or the project removed.

## Repository layout

```
Makefile              build: embed assets, compile (cJSON via pkg-config), link
tools/embed.c         asset compiler: web/** -> C arrays (deterministic, sorted,
                      MIME table, escaping-safe for any binary content)
tools/emoji.py        regenerates web/emoji.js from Unicode's emoji-test.txt
                      (the emoji picker data; run manually per Unicode release)
src/main.c            CLI entry point (-b addr, -p port, -c config dir, -h)
src/server.c          epoll event loop, connection lifecycle, routing, SSE ticks
src/http.{c,h}        HTTP/1.1 parser (GET/HEAD/PUT/POST, Content-Length bodies)
                      + response builders
src/theme.{c,h}       config dir resolution, theme.json load/save/validate (cJSON)
src/projects.{c,h}    projects.json load/save/validate (same pattern)
src/assets_gen.{c,h}  GENERATED — do not edit; regenerated by `make`
web/index.html        main app: three-column deck (projects + 2 placeholders)
web/config.html/.js   theme editor: instant preview, validate, save, reset
web/components.html   themed component gallery (palette, buttons, badges, rows…)
web/theme.js          shared: fetch /api/theme, apply as CSS custom properties
web/app.js            main app: projects rail/editor, swipeable column deck
web/emoji.js          GENERATED by tools/emoji.py — the full Unicode emoji
                      list for the picker (v18.0, 3,963 emojis)
web/style.css         styles driven entirely by theme variables
tests/smoke.sh        end-to-end smoke tests (make check)
tests/e2e/            playwright browser tests (firefox; config in
                      playwright.config.ts, server on :8120 with .e2e-config)
```

## HTTP surface

| Path              | Method       | Response                                        |
|-------------------|--------------|-------------------------------------------------|
| `/`               | GET/HEAD     | embedded `index.html` (`/` → `/index.html`)     |
| `/config.html`, `/components.html`, `/style.css`, `/app.js`, … | GET/HEAD | any file under `web/`, exact path match |
| `/api/time`       | GET          | `text/event-stream`; one JSON tick per second   |
| `/api/theme`      | GET/HEAD     | current theme as JSON                           |
| `/api/theme`      | PUT          | body: full/partial theme JSON; validates, saves |
|                   |              | 200 + saved theme; 400 bad JSON; 422 invalid    |
|                   |              | value/unknown key (`{"error","field"}`)         |
| `/api/theme/reset`| POST         | restore defaults, save, return theme            |
| `/api/projects`   | GET/HEAD     | project array + live `exists` flag per entry    |
| `/api/projects`   | PUT          | body: the whole array (see projects.json);      |
|                   |              | validates, saves, 200 + saved list; 400 bad     |
|                   |              | JSON; 422 invalid entry/duplicate dir/dir not   |
|                   |              | on disk/too many                               |
| anything else     | GET/HEAD     | 404; other methods → 405 (with `Allow`)         |

Request bodies: `Content-Length` only (≤ 64 KB); `Transfer-Encoding:
chunked` and duplicate `Content-Length` are rejected (400/413).

SSE event format (`retry` hint at stream start, `: ping` comment every
15 s; `X-Accel-Buffering: no` for proxies):

```
data: {"unix":1790414077,"iso":"2026-09-26T09:14:37Z"}
```

Adding a static file = drop it anywhere under `web/` and rebuild; it is
served automatically with the right MIME type. Dotfiles are skipped.

## Design decisions

- **Build-time embedding via a generator tool** (`tools/embed`), not
  `xxd -i` or preprocessor tricks: no external tool dependency,
  deterministic output (sorted), MIME types resolved once at build time,
  handles arbitrary binary bytes, and refuses unsafe filenames.
- **Hand-rolled HTTP/1.1 subset** rather than vendoring a library
  (e.g. mongoose): zero dependencies beyond libc/cJSON, full control.
  Trade-off: we own correctness for parsing/keep-alive. Swapping in a
  library later is contained behind `src/http.{c,h}`.
- **cJSON from the system** (`libcjson` via pkg-config) for all JSON:
  parsing PUTs, serializing responses, writing theme.json. One small,
  ubiquitous dependency; the JSON seams live only in `src/theme.c`.
- **Single-threaded epoll loop**: SSE broadcast is one loop over
  connections, no locking. Slow clients are dropped once > 256 KB is
  pending.
- **Theme as CSS custom properties**: server stores plain values, the
  browser applies them (`--bg-primary`, `--accent`, …). Instant preview
  is one `setProperty` call per field; no CSS re-generation server-side.
- **Lenient load, strict save**: a hand-edited theme.json with a bad
  value still boots (that field falls back to default), but the API and
  UI never let invalid data in. projects.json follows the same contract.
- **Projects as one replaceable list**: the client renders from memory
  and PUTs the whole array; the server validates, fills defaults (title
  from the directory name, palette color, emoji) and writes it back.
  No per-item endpoints to keep in sync with the UI state.
- **Mobile columns via CSS scroll-snap**: the deck is a grid on wide
  screens and a snap-scrolling carousel on narrow ones; touch swipes are
  native, mouse drags/arrows/dots are a few lines of JS on top.

## Known limits

- No TLS, no URL percent-decoding (exact path match only).
- No idle keep-alive timeouts.
- Access log goes to stderr, one line per request.
- Linux-only for now (`epoll`, POSIX sockets). Windows later needs:
  `epoll` → `select`/IOCP, `winsock2` init + `closesocket`, and a
  directory-walk replacement in `tools/embed` — the seams are isolated
  in `server.c` (event loop) and `tools/embed.c`.

## Roadmap

- [x] part 1: base architecture (embedded assets, epoll, SSE)
- [x] part 2: config directory + theme.json + config UI + gallery
- [x] automated smoke tests (`make check`) and e2e browser tests (playwright)
- [x] part 3a: main screen shell — three-column layout, swipeable on
      mobile; projects column (projects.json + rail/editor UI) complete
- [ ] part 3b: give columns two and three their purpose
- [ ] more settings in the config directory (beyond theme.json/projects.json)
- [ ] idle connection timeouts
- [ ] Windows build (winsock + select/IOCP)
- [ ] Dockerfile / packaging
- [ ] TLS (optional reverse-proxy or built-in)

## Development notes

```sh
make          # build ./flower (regenerates assets if web/ changed)
make run      # build + run with defaults
make check    # build + run the smoke tests (isolated tmp config dir)
make clean    # remove binary, objects, generated asset files

tools/e2e.sh setup    # one-time: firefox -> project-local .playwright/
make && tools/e2e.sh test --project=firefox
              # browser e2e suite: firefox against ./flower on :8120
              # (throwaway config dir .e2e-config; browsers and the few
              # dev-only npm deps in package.json stay inside the repo —
              # the web frontend itself has no dependencies)
```

The binary is ~62 KB, dynamically linked against libc and libcjson.
