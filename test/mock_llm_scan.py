#!/usr/bin/env python3
"""mock openai endpoint for the scan retry test.

turn 1: update_project with invalid args -> flower mcp-scan answers a
        json-rpc error
turn 2: update_project with valid args -> scan succeeds
"""
import json
import sys
from http.server import BaseHTTPRequestHandler, HTTPServer

TURN = 0


def chunk(cid, delta, finish=None):
    choice = {"index": 0, "delta": delta}
    if finish:
        choice["finish_reason"] = finish
    return {"id": cid, "object": "chat.completion.chunk",
            "choices": [choice]}


def tool_call(cid, name, args):
    return {"role": "assistant",
            "tool_calls": [{"index": 0, "id": cid, "type": "function",
                            "function": {"name": name,
                                         "arguments": json.dumps(args)}}]}


BAD_ARGS = {"details": {"Output": "x"},
            "contextItems": [{"bad": 1}],
            "resources": []}
GOOD_ARGS = {"details": {"Output": "the output"},
             "contextItems": [{"type": "Fact", "content": "f1"}],
             "resources": [{"type": "path", "content": "/a"}]}


class H(BaseHTTPRequestHandler):
    def log_message(self, *a):
        pass

    def do_POST(self):
        global TURN
        self.rfile.read(int(self.headers.get("Content-Length", 0)))
        TURN += 1
        args = BAD_ARGS if TURN == 1 else GOOD_ARGS
        cid = f"call_{TURN}"
        out = [chunk(f"c{TURN}", tool_call(cid, "project.update_project",
                                           args)),
               chunk(f"c{TURN}", {}, "tool_calls")]
        payload = ""
        for o in out:
            payload += f"data: {json.dumps(o)}\n\n"
        payload += "data: [DONE]\n\n"
        data = payload.encode()
        self.send_response(200)
        self.send_header("Content-Type", "text/event-stream")
        self.send_header("Content-Length", str(len(data)))
        self.end_headers()
        self.wfile.write(data)
        sys.stderr.write(f"mock: served turn {TURN}\n")
        sys.stderr.flush()


if __name__ == "__main__":
    HTTPServer(("127.0.0.1", int(sys.argv[1])), H).serve_forever()
