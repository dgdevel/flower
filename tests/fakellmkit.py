#!/usr/bin/env python3
"""fakellmkit — a scripted `llmkit runner` stand-in for tests.

flower hands the runner its records on stdin (llm, tools, system,
user, flush) and reads the conversation back from stdout as jsonl
records. This script plays a short, deterministic project-scan
conversation that exercises flower's whole capture pipeline:

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


def project_path(recs):
    """the scanned project's directory, from the user record."""
    for r in recs:
        if r.get("type") != "user":
            continue
        for block in r.get("content", []):
            m = re.search(r'at (\S+) now', block.get("text", ""))
            if m:
                return m.group(1)
    return "/etc"


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


def main():
    recs = read_records()
    base = base_url(recs)
    proj = project_path(recs)

    emit({"type": "thinking", "text": "Planning the scan", "partial": True})
    time.sleep(STEP)
    emit({"type": "thinking", "text": " — reading the files first.",
          "partial": False})
    emit({"type": "response", "text": "I will explore the files and " \
          "summarize the project.", "partial": False})
    time.sleep(STEP)

    # one researcher round: the invoke opens a sub-conversation; the
    # researcher's own read_file (below) is folded into it
    emit({"type": "tool_request", "tool": "filesystem_researcher.invoke",
          "arguments": {"input": "Find the build files and entry points."}})
    time.sleep(STEP)
    try:
        rpc(base, "/mcp", "tools/call",
            {"name": "read_file", "arguments": {"path": proj + "/README.md"}})
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
