import { test, expect } from "@playwright/test";

// The config page manages two stores (src/agents.c): llms.json (named
// endpoints) and one json file per agent under agents/. Both are edited
// whole in memory and PUT whole. These tests drive the real UI against
// the shared .e2e-config server; each starts from a clean slate.
async function resetLlmConfig(request: import("@playwright/test").APIRequestContext) {
  expect((await request.put("/api/llms", { data: [] })).ok()).toBeTruthy();
  expect((await request.put("/api/agents", { data: [] })).ok()).toBeTruthy();
}

test("config: llm endpoint and agent — create, reference, persist", async ({ page, request }) => {
  await resetLlmConfig(request);
  await page.goto("/config.html");

  await expect(page.locator("#llms-ui")).toContainText("No llms yet");
  await expect(page.locator("#agents-ui")).toContainText("No agents yet");

  // llm endpoint: name + protocol + url + model, saved as llms.json
  await page.click('#llms-ui [data-action="llm-add"]');
  await page.fill('#llms-ui [data-b="name"]', "ollama");
  await page.fill('#llms-ui [data-b="api_base"]', "http://localhost:11434/v1");
  await page.fill('#llms-ui [data-b="model"]', "llama3.1");
  await page.click('#llms-ui [data-action="llm-save"]');
  await expect(page.locator("#llm-status")).toContainText("saved");
  const llms = await (await request.get("/api/llms")).json();
  expect(llms).toHaveLength(1);
  expect(llms[0]).toMatchObject({
    name: "ollama",
    endpoint_protocol: "openai",
    api_base: "http://localhost:11434/v1",
    model: "llama3.1",
  });

  // agent referencing it: system prompt, inference options, one mcp server
  await page.click('#agents-ui [data-action="agent-add"]');
  await page.fill('#agents-ui [data-b="name"]', "gardener");
  await page.selectOption('#agents-ui [data-b="llm"]', "ollama");
  await page.fill('#agents-ui [data-b="system_prompt"]', "You tend flowers.");
  await page.fill('#agents-ui [data-io="temperature"]', "0.7");
  await page.fill('#agents-ui [data-io="max_tokens"]', "2048");
  await page.click('#agents-ui [data-action="tool-add"]');
  await page.fill('#agents-ui [data-b="tools.0.name"]', "fs");
  await page.fill('#agents-ui [data-b="tools.0.command_line"]', "npx -y @mcp/fs /tmp");
  await page.check('#agents-ui [data-b="tools.0.required"]');
  await page.click('#agents-ui [data-action="agent-save"]');
  await expect(page.locator("#agent-status")).toContainText("saved");

  const agents = await (await request.get("/api/agents")).json();
  expect(agents).toHaveLength(1);
  expect(agents[0]).toMatchObject({
    name: "gardener",
    llm: "ollama",
    system_prompt: "You tend flowers.",
    llm_ok: true,
  });
  expect(agents[0].inference_options).toMatchObject({ temperature: 0.7, max_tokens: 2048 });
  expect(agents[0].tools[0]).toMatchObject({ type: "stdio", name: "fs", required: true });

  // everything persists across a reload
  await page.reload();
  await expect(page.locator('#llms-ui .entity-row .name')).toHaveText("ollama");
  await expect(page.locator('#agents-ui .entity-row .name')).toHaveText("gardener");
  await expect(page.locator('#agents-ui .entity-row .sub')).toContainText("1 mcp server");
  await expect(page.locator('#agents-ui [data-b="system_prompt"]')).toHaveValue("You tend flowers.");
});

test("config: invalid input flagged client-side, dangling llm reference blocks saving", async ({ page, request }) => {
  await resetLlmConfig(request);
  await page.goto("/config.html");

  // an invalid api_base never reaches the server
  await page.click('#llms-ui [data-action="llm-add"]');
  await page.fill('#llms-ui [data-b="name"]', "ollama");
  await page.fill('#llms-ui [data-b="api_base"]', "notaurl");
  await page.click('#llms-ui [data-action="llm-save"]');
  await expect(page.locator('#llms-ui [data-err="api_base"]')).toContainText("http(s)");
  await expect(page.locator("#llm-status")).toContainText("fix");
  expect(await (await request.get("/api/llms")).json()).toEqual([]);

  // fix it, then build an agent on top
  await page.fill('#llms-ui [data-b="api_base"]', "http://localhost:11434/v1");
  await page.click('#llms-ui [data-action="llm-save"]');
  await expect(page.locator("#llm-status")).toContainText("saved");
  await page.click('#agents-ui [data-action="agent-add"]');
  await page.selectOption('#agents-ui [data-b="llm"]', "ollama");
  await page.click('#agents-ui [data-action="agent-save"]');
  await expect(page.locator("#agent-status")).toContainText("saved");

  // deleting the llm (two-step) flags the referencing agent…
  await page.click('#llms-ui .entity-row[data-i="0"]');
  await page.click('#llms-ui [data-action="llm-del"]');
  await expect(page.locator('#llms-ui [data-action="llm-del"]')).toHaveText("Really delete?");
  await page.click('#llms-ui [data-action="llm-del"]'); // second click confirms
  await page.click('#llms-ui [data-action="llm-save"]');
  await expect(page.locator("#llm-status")).toContainText("saved");
  await expect(page.locator('#agents-ui .entity-row.missing')).toHaveCount(1);

  // …and the agent cannot be saved while the reference dangles
  await page.click('#agents-ui .entity-row[data-i="0"]');
  await expect(page.locator('#agents-ui [data-err="llm"]')).toContainText("unknown llm");
  await page.click('#agents-ui [data-action="agent-save"]');
  await expect(page.locator("#agent-status")).toContainText("fix");
  expect((await (await request.get("/api/agents")).json())[0].llm_ok).toBe(false);
});
