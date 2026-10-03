#!/usr/bin/env python3
"""fakellmkit — a scripted `llmkit` stand-in for tests.

Two modes, picked like the real binary picks its subcommand:

  llmkit runner                the project-scan stand-in (below)
  llmkit agent-as-tool SEED    the researcher-debug stand-in: a stdio
                               mcp server whose single invoke tool
                               reports what the seed carries

The runner mode: flower hands the runner its records on stdin (llm,
tools, system, user, flush) and reads the conversation back from
stdout as jsonl records. This script plays a short, deterministic
project-scan conversation that exercises flower's whole capture
pipeline:

  - streamed thinking/response blocks (partials folded server-side)
  - a filesystem_researcher.invoke round, with the researcher's own
    tool call made against flower's /mcp surface while the invoke is
    open (that is what lands in the sub-conversation)
  - a write-back through flower's /scan/mcp surface
    (set_project_details), like the real scanner's own tool call

The base url and the project path are read from the stdin records,
so the same script serves any port and config dir:

    tests/smoke.sh   (copies it to $CFG/bin/llmkit, runs flower -l it)
    playwright.config.ts (runs flower -l tests/fakellmkit.py)

The transcript it prints is what the smoke and e2e tests assert on.
"""
import json
import os
import re
import sys
import time
import urllib.request

STEP = float(os.environ.get("FAKELLMKIT_STEP", "0.4"))


def read_records():
    """the runner's stdin: llm + tools + system + user records."""
    recs = []
    for line in sys.stdin.read().splitlines():
        line = line.strip()
        if not line:
            continue
        try:
            recs.append(json.loads(line))
        except ValueError:
            pass
    return recs


def base_url(recs):
    """flower's own address, from the tools record (the scanner's
    first tool server points at <base>/scan/mcp)."""
    for r in recs:
        if r.get("type") != "tools":
            continue
        for srv in r.get("tools", []):
            url = srv.get("url", "")
            if url.endswith("/scan/mcp"):
                return url[: -len("/scan/mcp")]
            if url.endswith("/mcp"):
                return url[: -len("/mcp")]
    return os.environ.get("FLOWER_URL", "http://127.0.0.1:8199")


def project_name(recs):
    """the scanned project's title, from the user record."""
    for r in recs:
        if r.get("type") != "user":
            continue
        for block in r.get("content", []):
            m = re.search(r'Scan the project "(.*?)" now',
                          block.get("text", ""))
            if m:
                return m.group(1)
    return None


def project_mcp(base, recs):
    """the project's own mcp surface: the researchers' fs tools are
    grounded in the project directory there (relative paths), so the
    fake looks the project up by title and uses its /projects/{seq}/mcp
    url — exactly what the real researcher seeds carry."""
    name = project_name(recs)
    try:
        req = urllib.request.Request(base + "/api/projects")
        with urllib.request.urlopen(req, timeout=10) as r:
            for p in json.loads(r.read()):
                if name is None or p.get("title") == name:
                    return "/projects/%d/mcp" % p["seq"]
    except Exception as exc:
        sys.stderr.write("fakellmkit: api/projects failed: %s\n" % exc)
    return "/mcp"  # fallback: the server-wide surface


def emit(rec):
    print(json.dumps(rec, separators=(",", ":")), flush=True)


def rpc(base, path, method, params):
    body = json.dumps(
        {"jsonrpc": "2.0", "id": 1, "method": method, "params": params}
    ).encode()
    req = urllib.request.Request(
        base + path, data=body, headers={"Content-Type": "application/json"}
    )
    with urllib.request.urlopen(req, timeout=10) as r:
        return json.loads(r.read())


def agent_mode(seed_path):
    """the `llmkit agent-as-tool SEED` stand-in: flower's `researcher`
    subcommand execs it, so tests drive the real seed through a real
    mcp handshake. The single invoke tool answers with what the seed
    carries (the llm's model and the invoke input); a copy of the
    seed goes to $FAKE_AGENT_SEED_OUT when set, for assertions."""
    model, servers = "?", []
    try:
        with open(seed_path) as f:
            recs = [json.loads(l) for l in f if l.strip()]
        for r in recs:
            if r.get("type") == "llm":
                model = r.get("model") or r.get("api_base") or "?"
            if r.get("type") == "tools":
                servers = [s.get("name", "?") for s in r.get("tools", [])]
        sys.stderr.write("fakellmkit: agent seed %s: %d records, model %s,"
                         " servers %s\n" % (seed_path, len(recs), model,
                                            servers))
        out = os.environ.get("FAKE_AGENT_SEED_OUT")
        if out:
            with open(out, "w") as f:
                f.write(json.dumps({"model": model, "servers": servers}))
    except OSError as exc:
        sys.stderr.write("fakellmkit: cannot read the seed: %s\n" % exc)

    for line in sys.stdin:
        line = line.strip()
        if not line:
            continue
        try:
            msg = json.loads(line)
        except ValueError:
            continue
        if not isinstance(msg, dict) or "id" not in msg:
            continue  # a notification: no reply
        i, m = msg["id"], msg.get("method")
        if m == "initialize":
            emit({"jsonrpc": "2.0", "id": i,
                  "result": {"protocolVersion": "2025-11-25",
                             "capabilities": {"tools": {}},
                             "serverInfo": {"name": "fake-agent-as-tool"}}})
        elif m == "tools/list":
            emit({"jsonrpc": "2.0", "id": i,
                  "result": {"tools": [
                      {"name": "invoke",
                       "description": "tools: %s" % ",".join(servers),
                       "inputSchema": {
                           "type": "object",
                           "properties": {"input": {"type": "string"}},
                           "required": ["input"]}}]}})
        elif m == "tools/call":
            params = msg.get("params") or {}
            if params.get("name") != "invoke":
                emit({"jsonrpc": "2.0", "id": i,
                      "error": {"code": -32602,
                                "message": "unknown tool"}})
                continue
            inp = (params.get("arguments") or {}).get("input", "")
            emit({"jsonrpc": "2.0", "id": i,
                  "result": {"content": [
                      {"type": "text",
                       "text": "fake researcher (%s) ran: %s"
                               % (model, inp)}]}})
        elif m == "ping":
            emit({"jsonrpc": "2.0", "id": i, "result": {}})
        else:
            emit({"jsonrpc": "2.0", "id": i,
                  "error": {"code": -32601, "message": "method not found"}})
    return 0


def main():
    if len(sys.argv) > 1 and sys.argv[1] == "agent-as-tool":
        return agent_mode(sys.argv[2] if len(sys.argv) > 2 else "")
    recs = read_records()
    base = base_url(recs)
    surface = project_mcp(base, recs)

    emit({"type": "thinking", "text": "Planning the scan", "partial": True})
    time.sleep(STEP)
    emit({"type": "thinking", "text": " — reading the files first.",
          "partial": False})
    emit({"type": "response", "text": "I will explore the files and " \
          "summarize the project.", "partial": False})
    time.sleep(STEP)

    # one researcher round: the invoke opens a sub-conversation; the
    # researcher's own read_file (below, project-relative) is folded
    # into it
    emit({"type": "tool_request", "tool": "filesystem_researcher.invoke",
          "arguments": {"input": "Find the build files and entry points."}})
    time.sleep(STEP)
    try:
        rpc(base, surface, "tools/call",
            {"name": "read_file", "arguments": {"path": "README.md"}})
    except Exception as exc:  # the show goes on; tests assert the text
        sys.stderr.write("fakellmkit: read_file failed: %s\n" % exc)
    time.sleep(STEP)
    emit({"type": "tool_response",
          "text": "Found: one Makefile and a single main.c entry point."})
    time.sleep(STEP)

    # the scanner's own write-back tool, executed over /scan/mcp
    emit({"type": "tool_request", "tool": "flower.set_project_details",
          "arguments": {"description": "A single-binary webapp in C11."}})
    time.sleep(STEP)
    try:
        rpc(base, "/scan/mcp", "tools/call",
            {"name": "set_project_details",
             "arguments": {"description": "A single-binary webapp in C11."}})
    except Exception as exc:
        sys.stderr.write("fakellmkit: set_project_details failed: %s\n" % exc)
    time.sleep(STEP)
    emit({"type": "tool_response", "text": "saved 1 field: description"})
    emit({"type": "response", "text": "Scan complete.", "partial": False})
    return 0


if __name__ == "__main__":
    sys.exit(main())
