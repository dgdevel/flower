# flower

A web application shipped as a **single static binary**, written in C11.
The frontend lives in ordinary files under `web/`; at build time a small
generator compiles them into the executable, so deployment is one file.

**Status: part 4 — LLM interaction.** Parts 1–3 built the base
architecture (embedded assets, epoll HTTP server, SSE), the config
directory with a `theme.json` backend and config UI, and the main
three-column screen with a live projects column. The config page
also manages **llm endpoints** (`llms.json`); the config directory
additionally holds **agents** (one json file per agent under
`agents/`, plus the compiled-in **builtin agents**, managed via the
API) — the llmkit-shaped settings for the conversation runner a
later part adds (the agent config UI will return with it). Column two keeps the **tasks**: a list with a
create button on top, the selected task's details (id, project,
title) and its typed **context items** below. A task is a **list of
actions** (nested without a depth limit, each with a title,
description, state and refinement type) edited in column three;
projects carry context items of the same shape. Part 4's web side
has landed: flower offers its own **mcp server** at `POST /mcp`
(`web_search`, `web_fetch`), builtin agents get their prompts from
tweakable, template-capable **prompt files** under `prompts/`, and
**`online_researcher`** — a research agent built on the two tools —
ships compiled in. See *Roadmap*.

## Quick start

```sh
make run            # build + serve on http://0.0.0.0:8080/
./flower -p 9000    # or: custom port
./flower -b 127.0.0.1   # or: loopback only
make check          # end-to-end smoke tests (tests/smoke.sh)
```

Pages: `/` (main app — three columns, projects manager), `/config.html`
(theme editor + llm endpoints), `/components.html` (themed
component gallery).

Requires: Linux, a C11 compiler (`cc`), GNU make ≥ 4.3 (grouped targets),
and **cJSON** (system package `libcjson`, discovered via pkg-config)
and **libcurl** (package `libcurl`, also via pkg-config — it powers the
web tools behind `/mcp`).

## How it works

**Build time** — assets are compiled into the binary:

```
web/*.html  ─┐                 prompts/agents/…/system_prompt.txt
web/*.css   ─┼─ tools/embed ──> src/assets_gen.{c,h}   ─┐
web/*.js    ─┘   (host tool,   (C arrays + mime +       ├─ linked into `flower`
                              served path)              │  (serves everything
prompts/**/*.txt ─ tools/embed ─> src/prompts_gen.{c,h}─┘   from memory)
                   (same tool, "prompt" prefix; never served over http)
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
    "id":   "9f2c4a1b0d7e6358",
    "dir":   "/home/dev/flower",
    "title": "flower",
    "color": "#58a6ff",
    "emoji": "🌸",
    "description":  "A single-binary webapp in C11.",
    "objectives":   "Ship part 4: llm-driven tasks.",
    "scope":        "No TLS; POSIX only; evenings and weekends.",
    "stakeholders": "me, the bees",
    "context": [
      { "type": "risk", "text": "one dev, many evenings",
        "updated": 1790529738 },
      { "text": "single binary = one file to ship" }
    ]
  }
]
```

A project's identity is its `dir` (unique, must be an absolute path)
plus a server-assigned `id` (16 hex chars, unique, stable across
saves; a PUT may omit it — e.g. for a new project — and the server
generates one, migrating files that predate ids on load). `title`
defaults to the directory's basename when empty, `color` to a
cycling palette and `emoji` to 🌸. Colors are `#rrggbb`, emoji/titles
are valid UTF-8 without control characters (≤ 31 / 96 bytes), at most
64 projects. The four **detail fields** (`description`, `objectives`,
`scope`, `stakeholders`) are free multi-line text (≤ 4095 bytes each,
valid UTF-8, no control characters but newlines), edited in column one
under "Project details". They are optional, default to empty, and are
the project context the llm side will draw on later.

The **context items** are the structured half of that context, shared
with tasks (`src/context.c`): each item is a `type` — `fact`,
`pattern`, `risk`, `success_metric`, `failure_sign`,
`evaluation_method` or `rule` (the `fact` default is omitted on the
wire) — a free `text` (required, multi-line, ≤ 4095 bytes, valid
UTF-8) and its update time (`updated`, unix seconds — the client
restamps an item on every edit; the server fills it in when
missing, and loads pre-update files that still say `created`).
`GET` also gives every item a generated **id**: `P1`, `P2`, … in a
project's list, `T1`, `T2`, … in a task's — numbered by position,
regenerated on every read (a delete renumbers the rest) and never
persisted; a PUT may echo ids back, the server ignores them. They
are edited as a list under "Context" in both the project editor
and the task details, autosaved with the owning PUT; at most 64
items per project/task.

Working directories must **exist on disk**: PUT validates each `dir`
with `stat()` (422 `directory does not exist` / `not a directory`).
If a directory vanishes after saving, the entry stays and
`GET /api/projects` reports it with `"exists": false` — the UI flags
the project (chip badge, editor warning) and blocks columns two and
three until the path is fixed or the project removed.

`llms.json` — the named **llm endpoints** (config page): connection
details live in exactly one place, agents reference them by name.
Same lenient/strict contract, one file:

```json
[
  {
    "name": "ollama-local",
    "endpoint_protocol": "openai",
    "api_base": "http://localhost:11434/v1",
    "model": "llama3.1"
  },
  {
    "name": "anthropic",
    "endpoint_protocol": "anthropic",
    "api_base": "https://api.anthropic.com/v1",
    "api_key": "sk-ant-…",
    "headers": { "anthropic-version": "2023-06-01" }
  }
]
```

`agents/` — one json file per **agent** (managed by the agents API,
no config UI yet; the file name is a slug of the agent name). An agent is an llm
reference plus everything llmkit-shaped it adds on top, so feeding it
to `llmkit runner` later is a mechanical translation: the `llm` record
is the referenced endpoint's fields plus the agent's
`inference_options`, the `tools` record is the agent's `tools` array,
and `system_prompt` becomes the `system` record's text block:

```json
{
  "name": "gardener",
  "llm": "ollama-local",
  "inference_options": { "temperature": 0.7, "max_tokens": 2048 },
  "system_prompt": "You tend flowers.",
  "tools": [
    {
      "type": "stdio",
      "name": "fs",
      "command_line": "npx -y @modelcontextprotocol/server-filesystem /tmp",
      "required": true,
      "terminal_tools": ["read_text_file"]
    }
  ]
}
```

Validation mirrors llmkit's own record rules (`endpoint_protocol` one
of `openai`/`openai_responses`/`anthropic`; mcp transports
`stdio`/`http`/`sse` with revisions up to `2026-07-28`; anthropic
requires `max_tokens` and `thinking_budget < max_tokens`; names are
unique per list). PUT `/api/agents` checks every `llm` reference
against the stored endpoints; if one dangles (an llm was deleted),
`GET /api/agents` reports it with `"llm_ok": false` — like a
vanished project directory. A PUT replaces the whole
list: files of removed agents are deleted.

**Builtin agents** are compiled into the binary (currently:
`assistant` and `online_researcher`) and merged into
`GET /api/agents`, sorted by name and flagged `"builtin": true` —
never written to `agents/`, their names are reserved (PUT rejects a
user agent taking one). Their system prompts are not C strings but
compiled-in **prompt files** (see below), tweakable without touching
code. `online_researcher` carries flower's own mcp server as its
tool (`{"type":"http","name":"flower","url":"/mcp"}` — the url
means "this flower instance" and is resolved when a task is handed
to the runner). Tasks no longer reference agents or llms (see
below); the stores remain for the conversation runner later parts
will add.

**Prompts** live as plain `.txt` files under `prompts/` in the repo,
compiled into the binary at build time by the same embedder that
compiles `web/` (into a second, private table — they are never
served over http):

```
prompts/
  agents/<name>/system_prompt.txt        builtin agents' prompts
  mcp/<tool>/description.txt             tool description (tools/list)
  mcp/<tool>/arguments/<arg>.txt         per-argument hint
```

They support a small `{{variable}}` template language
(`src/prompts.c`), meant to inject task/project context into a
prompt when the runner lands:

```
{{project_path}}        the project's working directory
{{project_name}}        the project's title
{{project_attributes}}  its detail fields, "Field: text" per line
{{project_context}}     its typed context items, "- [type] text" per line
```

Unknown tokens stay verbatim so typos remain visible; a NULL
project renders the known variables as empty strings.

**flower as an mcp server** — `POST /mcp` speaks mcp's json-rpc
subset over the streamable-http transport with plain json replies
(revision `2025-11-25`, matching what llmkit's client speaks):
`initialize`, `notifications/*` (answered `202`), `tools/list`,
`tools/call`, `ping`. The tools are flower's own web readers
(`src/web.c`, libcurl + `src/html.c`):

- **`web_search`** — queries DuckDuckGo's html endpoint and returns
  the top ten results as `Url: …` / `Description: title — snippet`
  records.
- **`web_fetch`** — fetches one page, picks its main content
  (readability-lite: obvious boilerplate stripped, paragraph
  containers scored) and returns it as markdown (headings, lists,
  links, code fences survive).

Any mcp client can use them — the llmkit runner included — by
registering `{"type":"http","name":"flower","url":"http://host:port/mcp"}`
as a tool server; the model then sees `flower.web_search` and
`flower.web_fetch`.

`tasks/` — one directory per **task**, named by its
server-generated 32-hex-char id, holding `task.json` with the
details (column two lists the **selected project's** tasks with a
create button on top and the selected task's details — id, project,
title — below; the selected task is the scope for column three):

```json
{
  "id":      "375b2ec7c276687f07218394e04a38c9",
  "project": "9f2c4a1b0d7e6358",
  "title":   "write the report",
  "created": 1790529738,
  "actions": [
    {
      "title": "gather sources",
      "type": "observe",
      "state": "completed",
      "children": [{ "title": "ask the archive" }]
    },
    { "title": "draft", "state": "in_progress" }
  ],
  "context": [
    { "id": "T1", "type": "success_metric", "text": "shipped by friday",
      "updated": 1790529738 }
  ]
}
```

A task is a list of **actions** to be done. Actions nest without a
fixed depth limit (a recursion cap of 64 protects the parser); each
carries a `title` (required), a `description` (optional,
multi-line), a `state`: `pending` (the default, omitted on the
wire), `in_progress`, `completed`, `partial` (completed partially)
or `failed` (completed unsuccessfully) — and a `type` saying where
it sits in the refinement loop: `observe`, `analyze`,
`find_root_cause`, `act` (the default, omitted on the wire),
`validate` or `improve`. Column three edits the selected task's
tree: the top half lists the actions — a type and a state select,
an inline title/description editor, add-sub-action and remove per
row — with children indented; the lower half stays reserved. The
task's own typed context items (same shape as a project's, edited
in the task details) complete the picture. The whole task list is
PUT after every edit, so the tree autosaves like the rest of the UI.

The main screen is a hierarchy: a task belongs to one `project`
(by id; the format is checked on PUT — existence is not, since the
whole list is PUT after every edit and a deleted project's tasks
must stay savable). Switching project — or drafting a new
one — resets the task list, the task selection and column three;
drafting a task empties column three. Same replaceable-list
contract as projects: PUT the whole array, the server writes one
`tasks/{ID}/` per entry and deletes the directories of removed
ones; GET adds a live `"project_ok"` flag for dangling references
(the project deleted), which the UI flags like a missing project
directory — tasks of a deleted project stay stored but are listed
under no project. The list is kept newest-first; later parts add
the conversation (the message log) inside the same `{ID}`
directory.

## Repository layout

```
Makefile              build: embed assets + prompts, compile (cJSON and
                      libcurl via pkg-config), selftest, link
tools/embed.c         asset compiler: web/** and prompts/** -> C arrays
                      (deterministic, sorted, escaping-safe for any
                      binary content; second table under the "prompt" prefix)
tools/emoji.py        regenerates web/emoji.js from Unicode's emoji-test.txt
                      (the emoji picker data; run manually per Unicode release)
prompts/              tweakable prompt files, compiled in (see Prompts):
                      agents/<name>/system_prompt.txt, mcp/<tool>/*.txt
tools/emoji.py        regenerates web/emoji.js from Unicode's emoji-test.txt
                      (the emoji picker data; run manually per Unicode release)
src/main.c            CLI entry point (-b addr, -p port, -c config dir, -h)
src/server.c          epoll event loop, connection lifecycle, routing, SSE ticks
src/http.{c,h}        HTTP/1.1 parser (GET/HEAD/PUT/POST, Content-Length bodies)
                      + response builders
src/theme.{c,h}       config dir resolution, theme.json load/save/validate (cJSON)
src/projects.{c,h}    projects.json load/save/validate (same pattern,
                      incl. the four free-text detail fields)
src/agents.{c,h}      llms.json + agents/ backends: named llm endpoints,
                      one file per agent (llm reference, inference
                      options, system prompt, mcp servers) and the
                      compiled-in builtin agents
src/tasks.{c,h} tasks/ backend: one directory per
                      task (project reference, title, nested
                      action tree, context items)
src/context.{c,h}    typed context items shared by projects and
                      tasks (fact/pattern/risk/… — parse/emit
                      rules live once for both stores)
src/prompts.{c,h}     prompt-file lookup + the {{project_*}} template renderer
src/html.{c,h}        tolerant html tokenizer/DOM, readability-lite and the
                      markdown emitter (src/web.c's reading engine)
src/web.{c,h}         outbound fetching (libcurl) + the web tools: ddg search
                      record extraction, page-to-markdown
src/mcp.{c,h}         flower's own mcp server: json-rpc dispatch for POST /mcp
src/assets_gen.{c,h}  GENERATED — do not edit; regenerated by `make`
src/prompts_gen.{c,h} GENERATED — do not edit; regenerated by `make`
web/index.html        main app: three-column deck (projects,
                      tasks, actions)
web/config.html       config page: theme, llm endpoints
web/config.js         theme editor: instant preview, validate, save, reset
web/llms.js           llm endpoints editor (list/editor pair,
                      client-side validation mirroring src/agents.c)
web/components.html   themed component gallery (palette, buttons, badges, rows…)
web/theme.js          shared: fetch /api/theme, apply as CSS custom properties
web/app.js            main app: projects rail/editor, tasks
                      column, action-tree column, swipeable column deck
web/emoji.js          GENERATED by tools/emoji.py — the full Unicode emoji
                      list for the picker (v18.0, 3,963 emojis)
web/style.css         styles driven entirely by theme variables
tests/smoke.sh        end-to-end smoke tests (make check)
tests/selftest.c      offline parser checks: ddg extraction, readability/
                      markdown, prompt templating (fixtures in tests/fixtures)
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
| `/api/llms`       | GET/HEAD     | llm endpoint array (sorted by name)             |
| `/api/llms`       | PUT          | body: the whole array (see llms.json); same     |
|                   |              | error contract as /api/projects                 |
| `/api/agents`     | GET/HEAD     | agent array (user + builtin, sorted) + live   |
|                   |              | `llm_ok` reference flag per user agent        |
| `/api/agents`     | PUT          | body: the whole array (see agents/); validates |
|                   |              | llm references, writes one file per agent,     |
|                   |              | deletes removed agents' files; 400 bad JSON;   |
|                   |              | 422 invalid entry/unknown llm/duplicate or    |
|                   |              | builtin-reserved name                          |
| `/mcp`            | POST         | flower's own mcp server (see above):         |
|                   |              | initialize/tools/list/tools/call json-rpc;   |
|                   |              | notifications 202; parse error -32700        |
| `/api/tasks` | GET/HEAD  | task array (newest first) + live      |
|                   |              | `project_ok` flag per entry                    |
| `/api/tasks` | PUT       | body: the whole array (see tasks/);   |
|                   |              | validates project reference format,   |
|                   |              | the action trees (states, types) and  |
|                   |              | the context items (dangling project   |
|                   |              | refs stay savable — GET flags them),  |
|                   |              | writes one `{ID}/task.json` per       |
|                   |              | entry, deletes removed directories;   |
|                   |              | 400 bad JSON; 422 invalid entry/bad   |
|                   |              | action/over-deep nesting               |
| anything else     | GET/HEAD     | 404; other methods → 405 (with `Allow`)         |

Request bodies: `Content-Length` only (≤ 256 KB); `Transfer-Encoding:
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
  No per-item endpoints to keep in sync with the UI state. llms.json
  and the agent list follow the same replaceable-list contract.
- **Agents as llm references, not copies**: connection details (url,
  key, headers) live once in llms.json; each agent stores a name
  reference plus only what is its own (inference options, prompt,
  tools). The stored shapes are llmkit's record shapes, so driving the
  runner is assembly, not translation. Reference problems surface the
  same way missing project directories do: a live flag on GET and a
  refused save until fixed.
- **Mobile columns via CSS scroll-snap**: the deck is a grid on wide
  screens and a snap-scrolling carousel on narrow ones; touch swipes are
  native, mouse drags/arrows/dots are a few lines of JS on top.

## Known limits

- No TLS, no URL percent-decoding (exact path match only).
- No idle keep-alive timeouts.
- A `tools/call` on `/mcp` fetches its web page synchronously in the
  event loop, so the whole server waits (capped at ~25 s). Fine for
  the runner's rare, serial calls; revisit if tools get chatty.
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
- [x] part 3b: column two is the tasks column, scoped hierarchically —
      it lists the selected project's tasks (list + create on top,
      selected task's details below); switching project or drafting
      one resets columns two and three, drafting a task empties
      column three
- [ ] part 4: llm interaction via llmkit
      - [x] llm endpoints (llms.json) with a config UI; agents
            (agents/, one json file per agent: llm reference,
            inference options, system prompt, mcp servers) via the
            API — their config UI is deferred to the runner work
      - [x] builtin agents (compiled in, read-only, selectable
            like user agents)
      - [x] task store: {config}/tasks/{ID}/ with the details
            (project reference, title, nested action tree,
            context items), replaceable-list API, project ids as
            stable identities
      - [x] action trees: column three edits the selected task's
            actions (add/edit/remove, sub-actions, five states, six
            types); the agent/llm task binding was removed — how a
            task reaches a model is decided when the runner lands
      - [x] typed context items on projects and tasks (fact,
            pattern, risk, success_metric, failure_sign,
            evaluation_method, rule — text + creation time), with
            editors in column one and column two
      - [x] custom-made mcp servers offered by flower itself —
            POST /mcp (streamable-http json-rpc) with the web tools
            web_search (DuckDuckGo html) and web_fetch (readability +
            markdown), prompt files under prompts/mcp/, and the
            builtin online_researcher agent using them
      - [ ] drive `llmkit runner` conversations from a task (renders
            the selected agent's prompt with the project's context)
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
