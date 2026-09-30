import path from "node:path";
import { defineConfig } from "@playwright/test";

// Browsers live in the project-local .playwright/ (gitignored; installed
// by `tools/e2e.sh setup`) so nothing is scattered into /tmp or $HOME.
// Set here as a fallback so a bare `bunx playwright test` works too.
process.env.PLAYWRIGHT_BROWSERS_PATH ??= path.resolve(".playwright");

// End-to-end suite. Firefox only (see AGENTS.md — chromium/webkit system
// deps are not guaranteed here). Build first: `make`. The server runs on
// :8120 with the throwaway config dir .e2e-config and the scripted
// llmkit stand-in (tests/fakellmkit.py) so scan conversations have
// something real to record.
export default defineConfig({
  testDir: "tests/e2e",
  timeout: 20_000,
  reporter: [["list"]],
  workers: 1, // all tests share one server + config dir
  projects: [{ name: "firefox", use: { browserName: "firefox" } }],
  webServer: {
    command:
      "./flower -p 8120 -b 127.0.0.1 -c .e2e-config -l tests/fakellmkit.py",
    url: "http://127.0.0.1:8120/",
    reuseExistingServer: false,
    stdout: "ignore",
    stderr: "ignore",
  },
  use: { baseURL: "http://127.0.0.1:8120" },
});
