import { test, expect } from "@playwright/test";
import { mkdirSync, writeFileSync } from "node:fs";

// The Conversations page against a real recorded scan: the server runs
// with tests/fakellmkit.py as its llmkit (playwright.config.ts), a
// scripted runner that plays a scan with one researcher round and one
// write-back — so the page has a main conversation, a sub-agent
// conversation linked both ways, and a live transcript to follow.
// The fake takes ~3s; the assertions that wait for its later records
// carry generous timeouts.

const WORK = "/tmp/flower-e2e";

// one scan at a time: wait until the server reports none running
// (a previous test's fake runner may still be playing its script)
async function waitScanIdle(request: import("@playwright/test").APIRequestContext) {
  for (let i = 0; i < 80; i++) {
    const d = await (await request.get("/api/scan")).json();
    if (!d.running) return;
    await new Promise((r) => setTimeout(r, 250));
  }
  throw new Error("a scan is still running");
}

// same for the task planner (the interactive fake waits for replies)
async function waitPlanIdle(request: import("@playwright/test").APIRequestContext) {
  for (let i = 0; i < 80; i++) {
    const d = await (await request.get("/api/plan")).json();
    if (!d.running) return;
    await new Promise((r) => setTimeout(r, 250));
  }
  throw new Error("a plan is still running");
}

test("top bar links to the conversations page", async ({ page }) => {
  await page.goto("/");
  const link = page.locator("header.top nav a", { hasText: "Conversations" });
  await expect(link).toHaveAttribute("href", "conversations.html");
});

test("scan runs in the background: the editor links to its conversation", async ({ page, request }) => {
  mkdirSync(`${WORK}/bgscan`, { recursive: true });
  await request.put("/api/llms", {
    data: [{ name: "ollama", endpoint_protocol: "openai",
             api_base: "http://localhost:11434/v1", model: "m" }],
  });
  await request.put("/api/projects", {
    data: [{ dir: `${WORK}/bgscan`, title: "background scan" }],
  });

  await page.goto("/");
  await expect(page.locator("#scan-section")).toBeVisible();
  await page.locator("#scan-start").click();

  // no chat window opens here — just the background note and the link
  await expect(page.locator("#scan-status")).toContainText(
    "Scanning in the background", { timeout: 5000 });
  await expect(page.locator("#scan-log")).toHaveCount(0);
  const open = page.locator("#scan-open");
  await expect(open).toBeVisible();
  await expect(open).toHaveAttribute("href", /^conversations\.html#[0-9a-f]{32}$/);

  // the link opens the recorded conversation
  await open.click();
  await expect(page.locator("#conv-detail h2")).toContainText(
    "Project scan — background scan");
  // …and this test's scan is done before the next one starts
  await expect(page.locator(".conv-log")).toContainText("Scan complete.", {
    timeout: 15000,
  });
});

test("conversations: a scan is recorded live, sub-agents linked", async ({ page, request }) => {
  mkdirSync(`${WORK}/conv`, { recursive: true });
  writeFileSync(`${WORK}/conv/README.md`, "e2e fixture\n");

  await request.put("/api/llms", {
    data: [{ name: "ollama", endpoint_protocol: "openai",
             api_base: "http://localhost:11434/v1", model: "m" }],
  });
  await request.put("/api/projects", {
    data: [{ dir: `${WORK}/conv`, title: "conv garden" }],
  });
  const projects = await (await request.get("/api/projects")).json();
  await waitScanIdle(request);
  const r = await request.post("/api/scan", {
    data: { project: projects[0].id, llm: "ollama" },
  });
  expect(r.ok()).toBeTruthy();

  await page.goto("/conversations.html");

  // the scan's conversation appears and finishes while we watch
  const main = page
    .locator(".conv-row", { hasText: "Project scan — conv garden" })
    .first();
  await expect(main).toBeVisible({ timeout: 15000 });
  await main.click();
  await expect(page.locator("#conv-detail h2")).toContainText("Project scan");
  await expect(page.locator(".conv-log")).toContainText("Scan complete.", {
    timeout: 15000,
  });
  await expect(page.locator(".conv-log")).toContainText(
    "flower.set_project_details");

  // the sub-agent conversation: collapsed under its parent's row,
  // revealed by the twistie, with its own transcript
  await main.locator(".conv-tw").click();
  const sub = page
    .locator(".conv-row.sub", { hasText: "filesystem_researcher" })
    .first();
  await expect(sub).toBeVisible();
  await sub.click();
  await expect(page.locator("#conv-detail h2")).toContainText(
    "build files");
  await expect(page.locator(".conv-log")).toContainText("e2e fixture");
  await expect(page.locator(".conv-log")).toContainText("Found:");

  // …and it links back to the conversation that spawned it
  await page.locator(".conv-parent").click();
  await expect(page.locator("#conv-detail h2")).toContainText("Project scan");
});

test("a task plan is interactive: collapsed tree, replies, delete", async ({ page, request }) => {
  mkdirSync(`${WORK}/planconv`, { recursive: true });
  writeFileSync(`${WORK}/planconv/README.md`, "e2e fixture\n");
  await waitScanIdle(request);
  await waitPlanIdle(request);

  await request.put("/api/llms", {
    data: [{ name: "ollama", endpoint_protocol: "openai",
             api_base: "http://localhost:11434/v1", model: "m" }],
  });
  await request.put("/api/projects", {
    data: [{ dir: `${WORK}/planconv`, title: "plan garden" }],
  });
  // earlier runs' plans (stopped) would collide with the row counts
  const old = await (await request.get("/api/conversations")).json();
  for (const c of old.filter((x) => x.agent === "task_planner"))
    await request.delete("/api/conversations/" + c.id);
  const projects = await (await request.get("/api/projects")).json();
  const r = await request.post("/api/plan", {
    data: { project: projects[0].id, llm: "ollama",
            prompt: "Plan the fixture cleanup" },
  });
  expect(r.ok()).toBeTruthy();

  await page.goto("/conversations.html");

  // the plan's conversation appears, flagged interactive; its
  // sub-agents stay collapsed until the twistie opens them
  const root = page
    .locator(".conv-row", { hasText: "Task plan — Plan the fixture cleanup" })
    .first();
  await expect(root).toBeVisible({ timeout: 15000 });
  await expect(root.locator(".conv-flag")).toHaveText("interactive");
  await expect(page.locator(".conv-row.sub")).toHaveCount(0);
  await root.locator(".conv-tw").click();
  const sub = page
    .locator(".conv-row.sub", { hasText: "filesystem_researcher" })
    .first();
  await expect(sub).toBeVisible();

  // the open conversation offers the reply box while it runs
  await root.click();
  await expect(page.locator("#conv-detail h2"))
    .toContainText("Task plan — Plan the fixture cleanup");
  await expect(page.locator("#conv-reply-text")).toBeVisible();
  await expect(page.locator(".conv-log")).toContainText(
    "Plan ready.", { timeout: 15000 });

  // one turn at a time: the reply only lands once the turn is over
  // (the conversation stays running between turns)
  await expect.poll(async () =>
    (await (await request.get("/api/plan")).json()).awaiting_reply,
    { timeout: 15000 }).toBe(true);

  // a reply is a turn: the fake adds an action and answers
  await page.locator("#conv-reply-text").fill("add a follow-up action");
  await page.locator("#conv-reply-send").click();
  await expect(page.locator(".conv-log")).toContainText(
    "add a follow-up action", { timeout: 15000 });
  await expect(page.locator(".conv-log")).toContainText(
    "Plan updated.", { timeout: 15000 });

  // stop, then delete: the whole tree leaves with its root
  await page.locator("#conv-stop").click();
  await expect(page.locator("#conv-detail .badge"))
    .toHaveText("stopped", { timeout: 15000 });
  const del = page.locator("#conv-delete");
  await del.click(); // arms…
  await expect(del).toHaveText("sure?");
  await del.click(); // …and confirms
  await expect(page.locator("#conv-detail .placeholder-title"))
    .toHaveText("No conversation open");
  await expect(
    page.locator(".conv-row", { hasText: "Task plan — Plan the fixture cleanup" }))
    .toHaveCount(0);
  await expect(page.locator(".conv-row.sub")).toHaveCount(0);
});

test("a finished run stays linked to its project and can be continued", async ({ page, request }) => {
  mkdirSync(`${WORK}/followup`, { recursive: true });
  writeFileSync(`${WORK}/followup/README.md`, "follow-up fixture\n");

  await request.put("/api/llms", {
    data: [{ name: "ollama", endpoint_protocol: "openai",
             api_base: "http://localhost:11434/v1", model: "m" }],
  });
  await request.put("/api/projects", {
    data: [{ dir: `${WORK}/followup`, title: "follow-up garden" }],
  });
  const projects = await (await request.get("/api/projects")).json();
  await waitScanIdle(request);
  const started = await request.post("/api/scan", {
    data: { project: projects[0].id, llm: "ollama" },
  });
  expect(started.ok()).toBeTruthy();
  await waitScanIdle(request);

  // the finished run is still listed under its project, one click away
  await page.goto("/");
  const runs = page.locator("#scan-section .scan-run");
  await expect(runs).toHaveCount(1);
  await expect(runs.first()).toContainText("completed");
  await expect(runs.first()).toContainText("Project scan — follow-up garden");
  await expect(runs.first()).toHaveAttribute(
    "href", /^conversations\.html#[0-9a-f]{32}$/);

  // …and the follow-up box asks for another round on top of it
  await page.locator("#scan-note").fill("now cover the tests");
  await expect(page.locator("#scan-start")).toHaveText("Ask the scanner");
  await page.locator("#scan-start").click();
  await expect(runs).toHaveCount(2, { timeout: 10000 });
  await expect(runs.first()).toContainText("completed", { timeout: 15000 });

  // the new transcript carries the previous run and the request
  await runs.first().click();
  const log = page.locator(".conv-log");
  await expect(log).toContainText("Another round on the project");
  await expect(log).toContainText("# Previous Run");
  await expect(log).toContainText("# Request");
  await expect(log).toContainText("now cover the tests");
  await expect(log).toContainText(
    'Scan the project "follow-up garden" now.');
});
