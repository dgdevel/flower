import { test, expect, APIRequestContext } from "@playwright/test";
import { existsSync, mkdirSync, rmSync } from "node:fs";

// Working directories must exist on disk (the server PUT-validates them),
// so the tests manage real ones under /tmp/flower-e2e.
const WORK = "/tmp/flower-e2e";

// All tests share one server + .e2e-config, so each starts from a clean
// slate via the API before touching the page.
async function resetProjects(request: APIRequestContext) {
  const r = await request.put("/api/projects", { data: [] });
  expect(r.ok()).toBeTruthy();
  const c = await request.put("/api/tasks", { data: [] });
  expect(c.ok()).toBeTruthy();
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

  await expect(panes.nth(1)).toContainText("Tasks");
  await expect(panes.nth(1)).toContainText("No tasks yet");
  await expect(panes.nth(2)).toContainText("Actions");
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

  // project details: free-text notes below the identity fields
  const details = page.locator(".editor-details");
  await expect(details.locator("> h3")).toHaveText("Project details");
  await page.fill('[data-f="description"]', "A home for bees\nand their honey.");
  await page.fill('[data-f="stakeholders"]', "the queen");
  await expect
    .poll(async () => {
      const [p] = await (await request.get("/api/projects")).json();
      return p?.description ?? "";
    })
    .toBe("A home for bees\nand their honey.");

  // typed context items below the details: add, type, edit, autosave
  const ctx = page.locator(".ctx-editor");
  await expect(ctx.locator("h3")).toHaveText("Context");
  await expect(ctx).toContainText("No context items yet");
  await ctx.locator('[data-action="ctx-add"]').click();
  await ctx.locator('[data-cf="text"]').fill("The server room floods in spring");
  await ctx.locator(".ctx-type").selectOption("risk");
  await ctx.locator('[data-action="ctx-add"]').click();
  await ctx.locator(".ctx-item").nth(1).locator('[data-cf="text"]')
    .fill("Two developers, one queen");
  await expect
    .poll(async () => {
      const [p] = await (await request.get("/api/projects")).json();
      return (p?.context ?? []).map((c) => `${c.type || "fact"}:${c.text}`);
    })
    .toEqual([
      "risk:The server room floods in spring",
      "fact:Two developers, one queen",   // fact default, updated filled
    ]);
  await expect(ctx.locator(".ctx-item .ts").first()).not.toBeEmpty();
  // generated ids: P-prefixed, sequential by position in a project
  await expect(ctx.locator(".ctx-id")).toHaveText(["P1", "P2"]);

  // an empty item blocks the save until filled or removed
  await ctx.locator('[data-action="ctx-add"]').click();
  await page.fill('[data-f="title"]', "Beekeeping 2"); // triggers a save
  await expect(page.locator("#editor-status")).toContainText("fix the highlighted fields");
  await ctx.locator('[data-action="ctx-del"]').last().click();
  await expect(page.locator("#editor-status")).toContainText("saved", { timeout: 5_000 });
  await expect
    .poll(async () => (await (await request.get("/api/projects")).json())[0].title)
    .toBe("Beekeeping 2");

  // everything persisted
  await page.reload();
  const chipAfter = page.locator(".chip[data-idx='0']");
  await expect(chipAfter).toHaveAttribute("aria-label", "Beekeeping 2");
  await expect(chipAfter).toHaveText("🐝");
  await expect(page.locator('[data-f="description"]'))
    .toHaveValue("A home for bees\nand their honey.");
  await expect(page.locator('[data-f="stakeholders"]')).toHaveValue("the queen");
  await expect(page.locator(".ctx-item")).toHaveCount(2);
  await expect(page.locator(".ctx-item .ctx-type").first())
    .toHaveValue("risk");
  await expect(page.locator(".ctx-item .ctx-id")).toHaveText(["P1", "P2"]);
  await expect(page.locator(".ctx-item [data-cf=\"text\"]").first())
    .toHaveValue("The server room floods in spring");

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

test("tasks: per-project scoping — create, action tree, switch resets", async ({ page, request }) => {
  await resetProjects(request);
  mkdirSync(`${WORK}/bees`, { recursive: true });
  mkdirSync(`${WORK}/hives`, { recursive: true });
  await page.goto("/");

  // a project first: tasks belong to one
  await page.click("#add-project");
  await page.fill('[data-f="dir"]', `${WORK}/bees`);
  await page.click('[data-action="create"]');
  await expect(page.locator("#editor-status")).toContainText("saved");

  const pane2 = page.locator('.pane[data-pane="1"]');
  const pane3 = page.locator('.pane[data-pane="2"]');
  await expect(pane2.locator("h2")).toHaveText("Tasks");
  await expect(pane2).toContainText("No tasks yet");
  await expect(pane2.locator("#new-task")).toBeVisible();
  await expect(pane3.locator("h2")).toHaveText("Actions");

  // creating a task is just a title — no agent or llm binding anymore
  await pane2.locator("#new-task").click();
  await pane2.locator('[data-f="task-title"]').fill("Bee talk");
  await pane2.locator('[data-action="task-create"]').click();

  // the list shows it, the details below carry the server-made id
  const row = pane2.locator(".task-row").first();
  await expect(row).toContainText("Bee talk");
  await expect(row).toContainText("no actions yet");
  await expect(pane2.locator("#task-count")).toHaveText("1 task");
  await expect(pane2.locator(".task-details code").first()).toHaveText(/^[0-9a-f]{32}$/);

  // column three: the selected task's action tree
  await expect(pane3).toContainText("No actions yet");
  await pane3.locator("#new-action").click();
  await pane3.locator(".action-editor input").fill("Plan the hive");
  await pane3.locator("#new-action").click();
  await pane3.locator(".action-editor input").fill("Stock frames");
  // a sub-action under the first, with a description
  await pane3.locator('.action[data-path="0"] > .action-row [data-action="action-sub"]').click();
  await pane3.locator(".action-editor input").fill("Pick a spot");
  await pane3.locator(".action-editor textarea").fill("somewhere sunny");
  // states: in progress on the parent, completed on the sub-action;
  // types: observe on the parent, validate on the sub-action (the
  // refinement loop an action sits in), "Stock frames" stays "act"
  await pane3.locator('.action[data-path="0"] > .action-row .action-state').selectOption("in_progress");
  await pane3.locator('.action[data-path="0"] > .action-row .action-type').selectOption("observe");
  await pane3.locator('.action[data-path="0.0"] > .action-row .action-state').selectOption("completed");
  await pane3.locator('.action[data-path="0.0"] > .action-row .action-type').selectOption("validate");
  await expect(pane3.locator("#action-count")).toHaveText("3 actions");
  await expect(pane2.locator(".task-row").first()).toContainText("3 actions");

  // the task's own context items: typed, edited below the details
  const tctx = pane2.locator(".task-details .ctx-editor");
  await tctx.locator('[data-action="ctx-add"]').click();
  await tctx.locator('[data-cf="text"]').first().fill("No deploys on fridays");
  await tctx.locator(".ctx-type").first().selectOption("rule");

  // the tree and context persist server-side, nested, with defaults
  // omitted (volatile ids/timestamps stripped before comparing)
  await expect
    .poll(async () => {
      const [t] = await (await request.get("/api/tasks")).json();
      if (!t) return null;
      return {
        title: t.title,
        actions: t.actions,
        context: (t.context ?? []).map(({ id, updated, ...r }) => r),
      };
    })
    .toEqual({
      title: "Bee talk",
      actions: [
        { title: "Plan the hive", type: "observe", state: "in_progress",
          children: [
            { title: "Pick a spot", description: "somewhere sunny",
              type: "validate", state: "completed" },
          ] },
        { title: "Stock frames" },   // act/pending defaults stay off the wire
      ],
      context: [{ text: "No deploys on fridays", type: "rule" }],
    });

  // reloaded: selection, tree and states come back
  await page.reload();
  const pane2b = page.locator('.pane[data-pane="1"]');
  const pane3b = page.locator('.pane[data-pane="2"]');
  await expect(pane2b.locator(".task-row").first()).toContainText("Bee talk");
  await expect(pane2b.locator(".task-row")).toHaveClass(/active/);
  await expect(pane3b.locator(".action")).toHaveCount(3);
  await expect(pane3b.locator('.action[data-path="0"] > .action-row .action-title')).toHaveText("Plan the hive");
  await expect(pane3b.locator('.action[data-path="0.0"] > .action-row .action-title')).toHaveText("Pick a spot");
  await expect(pane3b.locator('.action[data-path="0"]')).toHaveAttribute("data-state", "in_progress");
  await expect(pane3b.locator('.action[data-path="0"]')).toHaveAttribute("data-type", "observe");
  await expect(pane3b.locator('.action[data-path="0"] > .action-row .action-type')).toHaveValue("observe");
  // so does the task's context, typed, stamped and T-id'd
  await expect(pane2b.locator(".task-details .ctx-item")).toHaveCount(1);
  await expect(pane2b.locator(".task-details .ctx-type")).toHaveValue("rule");
  await expect(pane2b.locator(".task-details .ctx-item .ts").first()).not.toBeEmpty();
  await expect(pane2b.locator(".task-details .ctx-id")).toHaveText("T1");

  // the title click opens the inline editor; edits autosave
  await pane3b.locator('.action[data-path="0.0"] > .action-row .action-title').click();
  // typing across an autosave must not lose the field: a mid-edit
  // re-render of the pane would swallow the keystrokes after the pause
  const editor = pane3b.locator(".action-editor input");
  await editor.fill(""); // retype from scratch
  await editor.pressSequentially("Pick a sunnier ", { delay: 40 });
  await page.waitForTimeout(900); // the debounced save fires mid-edit
  await expect(editor).toBeFocused();
  await editor.pressSequentially("spot");
  await expect(editor).toHaveValue("Pick a sunnier spot");
  await expect
    .poll(async () => (await (await request.get("/api/tasks")).json())[0].actions[0].children[0].title)
    .toBe("Pick a sunnier spot");

  // removing drops the action and its sub-actions, and the row's
  // action count updates at once
  await pane3b.locator('.action[data-path="1"] > .action-row [data-action="action-del"]').click();
  await expect(pane3b.locator(".action")).toHaveCount(2);
  await expect(pane2b.locator(".task-row").first()).toContainText("2 actions");
  await expect
    .poll(async () => (await (await request.get("/api/tasks")).json())[0].actions)
    .toHaveLength(1);

  // New Task empties the column-three scope while drafting
  await pane2b.locator("#new-task").click();
  await expect(pane3b).toContainText("No task selected");
  await pane2b.locator('[data-action="task-cancel"]').click();
  await expect(pane3b.locator(".action")).toHaveCount(2);

  // switching project resets column two and three
  await page.click("#add-project");
  await page.fill('[data-f="dir"]', `${WORK}/hives`);
  await page.click('[data-action="create"]');
  await expect(page.locator("#editor-status")).toContainText("saved");
  await expect(pane2b).toContainText("No tasks yet");       // fresh project owns none
  await expect(pane3b).toContainText("Create a task in column two first");

  // the task is still there — under its own project
  const allTasks = await (await request.get("/api/tasks")).json();
  expect(allTasks).toHaveLength(1);
  const projects = await (await request.get("/api/projects")).json();
  const bees = projects.find((x) => x.dir.endsWith("/bees"));
  await page.reload();
  await page.locator(`.chip[title="${bees.title}"]`).click();
  await expect(pane2b.locator(".task-row").first()).toContainText("Bee talk");
  await pane2b.locator(".task-row").first().click(); // a fresh project scope starts unselected
  await expect(page.locator('.pane[data-pane="2"] .action')).toHaveCount(2);

  // drafting a New project empties columns two and three
  await page.click("#add-project");
  await expect(pane2b).toContainText("No project selected");
  await expect(page.locator('.pane[data-pane="2"]')).toContainText("Nothing open");
  await page.click('[data-action="cancel"]');

  // the lower half of column three stays reserved
  await expect(page.locator('.pane[data-pane="2"]')).toContainText("Nothing here yet");
});

test("tasks: the row's ✕ deletes after asking twice", async ({ page, request }) => {
  await resetProjects(request);
  mkdirSync(`${WORK}/bees`, { recursive: true });
  await page.goto("/");

  // a project with two tasks under it
  await page.click("#add-project");
  await page.fill('[data-f="dir"]', `${WORK}/bees`);
  await page.click('[data-action="create"]');
  await expect(page.locator("#editor-status")).toContainText("saved");
  const pane2 = page.locator('.pane[data-pane="1"]');
  for (const title of ["Bee talk", "Waggle dance"]) {
    await pane2.locator("#new-task").click();
    await pane2.locator('[data-f="task-title"]').fill(title);
    await pane2.locator('[data-action="task-create"]').click();
  }
  await expect(pane2.locator("#task-count")).toHaveText("2 tasks"); // newest first
  const ids = (await (await request.get("/api/tasks")).json()).map((t) => t.id);

  // the first ✕ click only arms: "sure?", nothing deleted
  // targeted by title: same-second creations tie-break by id, so the
  // newest task is not reliably row 0
  const del = pane2.locator(".task-row", { hasText: "Bee talk" }).locator(".task-del");
  await del.click();
  await expect(del).toHaveText("sure?");
  await expect(pane2.locator(".task-row")).toHaveCount(2);

  // the second one goes through — server-side too (directory dropped)
  await del.click();
  await expect(pane2.locator("#task-count")).toHaveText("1 task");
  await expect(pane2.locator(".task-row").first()).toContainText("Waggle dance");
  const left = await (await request.get("/api/tasks")).json();
  expect(left.map((t) => t.title)).toEqual(["Waggle dance"]);
  const gone = ids.find((id) => id !== left[0].id);
  await expect
    .poll(() => existsSync(`.e2e-config/tasks/${gone}`))
    .toBe(false);

  // arming is undone by selecting: the ✕ never fires on its own
  const last = pane2.locator(".task-row[data-idx='0']");
  await last.locator(".task-del").click();
  await expect(last.locator(".task-del")).toHaveText("sure?");
  await last.locator(".task-main").click();
  await expect(last).toHaveClass(/active/);
  await expect(last.locator(".task-del")).toHaveText("✕");
  await expect(pane2.locator("#task-count")).toHaveText("1 task");

  // deleting the selected task clears the scope of column three
  await last.locator(".task-del").click();
  await last.locator(".task-del").click();
  await expect(pane2).toContainText("No tasks yet");
  await expect(page.locator(".task-details")).toContainText("Nothing open");
  await expect(await (await request.get("/api/tasks")).json()).toEqual([]);
});

test("tasks: deleting a project must not block task creation afterwards", async ({ page, request }) => {
  await resetProjects(request);
  mkdirSync(`${WORK}/bees`, { recursive: true });
  mkdirSync(`${WORK}/hives`, { recursive: true });
  await page.goto("/");

  // a project with one task under it
  await page.click("#add-project");
  await page.fill('[data-f="dir"]', `${WORK}/bees`);
  await page.click('[data-action="create"]');
  const pane2 = page.locator('.pane[data-pane="1"]');
  await pane2.locator("#new-task").click();
  await pane2.locator('[data-f="task-title"]').fill("Orphaned");
  await pane2.locator('[data-action="task-create"]').click();
  await expect(pane2.locator("#task-count")).toHaveText("1 task");

  // delete the project: the task stays stored as a dangling reference
  await page.click("#delete-btn");
  await page.click("#delete-btn");
  const orphan = (await (await request.get("/api/tasks")).json())
    .find((t: { title: string }) => t.title === "Orphaned");
  expect(orphan.project_ok).toBe(false);

  // a new project: creating a task in it must still succeed (the
  // whole list — orphan included — is PUT with every save)
  await page.click("#add-project");
  await page.fill('[data-f="dir"]', `${WORK}/hives`);
  await page.click('[data-action="create"]');
  await pane2.locator("#new-task").click();
  await pane2.locator('[data-f="task-title"]').fill("Fresh");
  await pane2.locator('[data-action="task-create"]').click();
  await expect(pane2.locator("#task-count")).toHaveText("1 task");
  await expect(pane2.locator(".task-row").first()).toContainText("Fresh");
  const after = await (await request.get("/api/tasks")).json();
  expect(after.find((t: { title: string }) => t.title === "Orphaned").project_ok)
    .toBe(false);
  expect(after.find((t: { title: string }) => t.title === "Fresh").project_ok)
    .toBe(true);
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
  await expect(page.locator("#new-task")).toBeVisible();
  rmSync(`${WORK}/gone`, { recursive: true });
});

test("mobile: swipe and dots move between the three columns", async ({ page, request }) => {
  await resetProjects(request);
  await page.setViewportSize({ width: 390, height: 780 });
  await page.goto("/");

  const deck = page.locator("#deck");
  const pane2 = page.locator(".pane").nth(1);
  await expect(pane2).toContainText("No tasks yet");
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
