import { test, expect } from "@playwright/test";

// The config page edits llms.json (src/agents.c): named endpoints,
// managed whole in memory and PUT whole. These tests drive the real UI
// against the shared .e2e-config server; each starts from a clean slate.
async function resetLlmConfig(request: import("@playwright/test").APIRequestContext) {
  expect((await request.put("/api/llms", { data: [] })).ok()).toBeTruthy();
  expect((await request.put("/api/agents", { data: [] })).ok()).toBeTruthy();
}

test("config: llm endpoint — create, persist", async ({ page, request }) => {
  await resetLlmConfig(request);
  await page.goto("/config.html");

  await expect(page.locator("#llms-ui")).toContainText("No llms yet");

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

  // everything persists across a reload
  await page.reload();
  await expect(page.locator('#llms-ui .entity-row .name')).toHaveText("ollama");
  await expect(page.locator('#llms-ui [data-b="model"]')).toHaveValue("llama3.1");
});

test("config: invalid input flagged client-side, nothing saved", async ({ page, request }) => {
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

  // fix it and save
  await page.fill('#llms-ui [data-b="api_base"]', "http://localhost:11434/v1");
  await page.click('#llms-ui [data-action="llm-save"]');
  await expect(page.locator("#llm-status")).toContainText("saved");
  expect(await (await request.get("/api/llms")).json()).toHaveLength(1);

  // deleting (two-step) removes the endpoint
  await page.click('#llms-ui [data-action="llm-del"]');
  await expect(page.locator('#llms-ui [data-action="llm-del"]')).toHaveText("Really delete?");
  await page.click('#llms-ui [data-action="llm-del"]'); // second click confirms
  await page.click('#llms-ui [data-action="llm-save"]');
  await expect(page.locator("#llm-status")).toContainText("saved");
  expect(await (await request.get("/api/llms")).json()).toEqual([]);
});
