import { test, expect, APIRequestContext } from "@playwright/test";
import { mkdirSync, rmSync } from "node:fs";

// Working directories must exist on disk (the server PUT-validates them),
// so the tests manage real ones under /tmp/flower-e2e.
const WORK = "/tmp/flower-e2e";

// All tests share one server + .e2e-config, so each starts from a clean
// slate via the API before touching the page.
async function resetProjects(request: APIRequestContext) {
  const r = await request.put("/api/projects", { data: [] });
  expect(r.ok()).toBeTruthy();
}

test.beforeAll(() => mkdirSync(WORK, { recursive: true }));

test("desktop: three columns side by side, placeholders visible", async ({ page, request }) => {
  await resetProjects(request);
  await page.goto("/");

  const panes = page.locator(".pane");
  await expect(panes).toHaveCount(3);

  const [a, b, c] = await Promise.all([
    panes.nth(0).boundingBox(),
    panes.nth(1).boundingBox(),
    panes.nth(2).boundingBox(),
  ]);
  expect(a.y).toBe(b.y);
  expect(b.y).toBe(c.y);
  expect(b!.x).toBeGreaterThan(a!.x);
  expect(c!.x).toBeGreaterThan(b!.x);

  await expect(panes.nth(1)).toContainText("Nothing here yet");
  await expect(panes.nth(2)).toContainText("Nothing here yet");
  await expect(panes.nth(0)).toContainText("Projects");
});

test("projects: create, live edits, persistence, two-step delete", async ({ page, request }) => {
  await resetProjects(request);
  mkdirSync(`${WORK}/bees`, { recursive: true });
  await page.goto("/");

  await expect(page.locator("#editor")).toContainText("No projects yet");

  await page.click("#add-project");
  await expect(page.locator("#eh-title")).toHaveText("New project");
  await page.fill('[data-f="dir"]', `${WORK}/bees`);
  await page.click('[data-action="create"]');

  // chip appears immediately, labeled with the directory name (default title)
  const chip = page.locator(".chip[data-idx='0']");
  await expect(chip).toHaveCount(1);
  await expect(chip).toHaveAttribute("aria-label", "bees");
  await expect(page.locator("#project-count")).toHaveText("1 project");
  await expect(page.locator("#editor-status")).toContainText("saved");

  // title edits reach the chip (alt text) and the editor head immediately
  await page.fill('[data-f="title"]', "Beekeeping");
  await expect(chip).toHaveAttribute("aria-label", "Beekeeping");
  await expect(page.locator("#eh-title")).toHaveText("Beekeeping");

  // so do color and emoji (picked from the full-Unicode emoji picker)
  await page.locator('[data-f="color"]').evaluate((el) => {
    el.value = "#f778ba";
    el.dispatchEvent(new Event("input", { bubbles: true }));
  });
  await expect(chip).toHaveCSS("background-color", "rgb(247, 120, 186)");

  await page.click(".emoji-pick");
  const picker = page.locator(".emoji-picker");
  await expect(picker).toBeVisible();
  await expect
    .poll(async () => await picker.locator(".emoji-opt").count())
    .toBeGreaterThan(3000); // the full Unicode v18 list is loaded
  await picker.locator(".emoji-search").fill("honeybee");
  await expect(picker.locator(".emoji-opt")).toHaveCount(1);
  await picker.locator(".emoji-opt").first().click();
  await expect(chip).toHaveText("🐝");
  await expect(page.locator(".emoji-picker")).toHaveCount(0); // closed on pick
  await expect(page.locator("#editor-status")).toContainText("saved");

  // everything persisted
  await page.reload();
  const chipAfter = page.locator(".chip[data-idx='0']");
  await expect(chipAfter).toHaveAttribute("aria-label", "Beekeeping");
  await expect(chipAfter).toHaveText("🐝");

  // delete asks twice
  await page.click('[data-action="delete"]');
  await expect(page.locator("#delete-btn")).toHaveText("Really delete?");
  await expect(chipAfter).toHaveCount(1); // first click only arms
  await page.click('[data-action="delete"]');
  await expect(page.locator(".chip[data-idx='0']")).toHaveCount(0);
  await expect(page.locator("#editor")).toContainText("No projects yet");
  await page.reload();
  await expect(page.locator("#editor")).toContainText("No projects yet");
});

test("emoji picker: search narrows the Unicode list, Escape closes", async ({ page, request }) => {
  await resetProjects(request);
  mkdirSync(`${WORK}/picker`, { recursive: true });
  await page.goto("/");

  await page.click("#add-project");
  await page.fill('[data-f="dir"]', `${WORK}/picker`);

  await page.click(".emoji-pick");
  const picker = page.locator(".emoji-picker");
  await expect(picker.locator(".emoji-opt").first()).toBeVisible();
  await picker.locator(".emoji-search").fill("cactus");
  await expect(picker.locator(".emoji-opt")).toHaveCount(1);
  await page.keyboard.press("Escape");
  await expect(page.locator(".emoji-picker")).toHaveCount(0);

  // picking after search still applies the clicked emoji
  await page.click(".emoji-pick");
  await page.locator(".emoji-picker .emoji-search").fill("cactus");
  await expect(page.locator(".emoji-picker .emoji-opt")).toHaveCount(1);
  await page.locator(".emoji-picker .emoji-opt").first().click();
  await expect(page.locator("#f-emoji")).toHaveText("🌵");
});

test("invalid input is flagged and never saved", async ({ page, request }) => {
  await resetProjects(request);
  await page.goto("/");

  await page.click("#add-project");
  await page.fill('[data-f="dir"]', "not/absolute");
  await page.click('[data-action="create"]');

  await expect(page.locator("#err-dir")).toContainText("absolute path");
  await expect(page.locator(".chip[data-idx='0']")).toHaveCount(0);

  // a well-formed but non-existent directory is refused by the server,
  // and the create rolls back (no phantom chip)
  await page.fill('[data-f="dir"]', `${WORK}/never-created`);
  await page.click('[data-action="create"]');
  await expect(page.locator("#editor-status")).toContainText("does not exist");
  await expect(page.locator("#err-dir")).toContainText("does not exist");
  await expect(page.locator(".chip[data-idx='0']")).toHaveCount(0);
  expect(await (await request.get("/api/projects")).json()).toEqual([]);
});

test("vanished directory: warning, blocked columns, refuses to save, recovers", async ({ page, request }) => {
  await resetProjects(request);
  mkdirSync(`${WORK}/gone`, { recursive: true });
  await page.goto("/");

  await page.click("#add-project");
  await page.fill('[data-f="dir"]', `${WORK}/gone`);
  await page.click('[data-action="create"]');
  await expect(page.locator("#editor-status")).toContainText("saved");

  // the folder disappears behind our back
  rmSync(`${WORK}/gone`, { recursive: true });
  await page.reload();

  const chip = page.locator(".chip[data-idx='0']");
  await expect(chip).toHaveClass(/missing/);
  await expect(page.locator("#editor-warn")).toContainText("not on disk");
  await expect(page.locator("#err-dir")).toContainText("missing on disk");
  await expect(page.locator('.pane[data-pane="1"]')).toHaveClass(/blocked/);
  await expect(page.locator('.pane[data-pane="2"]')).toHaveClass(/blocked/);
  await expect(page.locator('.pane[data-pane="1"] .warn-text')).toContainText("missing on disk");
  await expect(page.locator('.pane[data-pane="1"]'))
    .not.toContainText("Nothing here yet", { useInnerText: true });

  // saving is refused while the directory is gone
  await page.fill('[data-f="title"]', "Still broken");
  await expect(page.locator("#editor-status")).toContainText("missing", { timeout: 5_000 });

  // fixing the path lets everything through again
  mkdirSync(`${WORK}/gone`);
  await page.fill('[data-f="dir"]', `${WORK}/gone`);
  await expect(page.locator("#editor-status")).toContainText("saved ✓", { timeout: 5_000 });
  await expect(page.locator("#editor-warn")).toBeHidden();
  await expect(page.locator('.pane[data-pane="1"]')).not.toHaveClass(/blocked/);
  await expect(page.locator('.pane[data-pane="1"]')).toContainText("Nothing here yet");
  rmSync(`${WORK}/gone`, { recursive: true });
});

test("mobile: swipe and dots move between the three columns", async ({ page, request }) => {
  await resetProjects(request);
  await page.setViewportSize({ width: 390, height: 780 });
  await page.goto("/");

  const deck = page.locator("#deck");
  const pane2 = page.locator(".pane").nth(1);
  await expect(pane2).toContainText("Nothing here yet");
  expect((await pane2.boundingBox())!.x).toBeGreaterThanOrEqual(390); // off-screen

  // swipe left -> column two
  await page.mouse.move(340, 150);
  await page.mouse.down();
  await page.mouse.move(40, 155, { steps: 10 });
  await page.mouse.up();
  await expect.poll(() => deck.evaluate((el) => el.scrollLeft)).toBeGreaterThan(300);
  expect((await pane2.boundingBox())!.x).toBeLessThan(60);
  await expect(page.locator(".dot").nth(1)).toHaveAttribute("aria-current", "true");

  // arrow key -> column three
  await page.keyboard.press("ArrowRight");
  await expect.poll(() => deck.evaluate((el) => el.scrollLeft)).toBeGreaterThan(700);
  await expect(page.locator(".dot").nth(2)).toHaveAttribute("aria-current", "true");

  // swipe right -> column two
  await page.mouse.move(40, 150);
  await page.mouse.down();
  await page.mouse.move(340, 150, { steps: 10 });
  await page.mouse.up();
  await expect.poll(() => deck.evaluate((el) => el.scrollLeft)).toBeGreaterThan(320);
  await expect.poll(() => deck.evaluate((el) => el.scrollLeft)).toBeLessThan(460);
  await expect(page.locator(".dot").nth(1)).toHaveAttribute("aria-current", "true");

  // arrow key -> back to the projects column
  await page.keyboard.press("ArrowLeft");
  await expect.poll(() => deck.evaluate((el) => el.scrollLeft)).toBeLessThan(60);
  await expect(page.locator(".dot").nth(0)).toHaveAttribute("aria-current", "true");
});
