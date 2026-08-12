#!/usr/bin/env python3
"""tests/fixtures/mock_server.py - minimal OpenAI-compatible mock for offline
demo/tests. Serves an SSE /v1/chat/completions stream:

  request #1 -> assistant tool_call to run_shell(/bin/echo)
  request #2 -> final assistant text

Use:  python3 tests/fixtures/mock_server.py  (listens on 127.0.0.1:8011)
"""

import json
from http.server import BaseHTTPRequestHandler, HTTPServer

PORT = 8011
REQUEST = {"count": 0}


def chunk(delta, finish_reason=None):
    obj = {
        "id": "mock-" + str(REQUEST["count"]),
        "object": "chat.completion.chunk",
        "choices": [{"index": 0, "delta": delta, "finish_reason": finish_reason}],
    }
    return "data: " + json.dumps(obj) + "\n\n"


def stream_v1():
    return (
        chunk({"role": "assistant"})
        + chunk(
            {
                "tool_calls": [
                    {
                        "index": 0,
                        "id": "mock_call_1",
                        "type": "function",
                        "function": {
                            "name": "run_shell",
                            "arguments": "",
                        },
                    }
                ]
            }
        )
        + chunk(
            {
                "tool_calls": [
                    {
                        "index": 0,
                        "function": {
                            "arguments": json.dumps(
                                {"command": "/bin/echo", "args": ["hello-from-puci-cli"]}
                            )
                        },
                    }
                ]
            }
        )
        + chunk({}, finish_reason="tool_calls")
        + "data: [DONE]\n\n"
    )


def stream_v2():
    return (
        chunk({"role": "assistant", "content": "Done. The tool ran and replied: "})
        + chunk({"content": "hello-from-puci-cli"})
        + chunk({}, finish_reason="stop")
        + "data: [DONE]\n\n"
    )


class Handler(BaseHTTPRequestHandler):
    def log_message(self, *args):  # silence
        pass

    def do_POST(self):
        length = int(self.headers.get("Content-Length", 0))
        self.rfile.read(length)
        REQUEST["count"] += 1
        body = stream_v1() if REQUEST["count"] == 1 else stream_v2()
        data = body.encode()
        self.send_response(200)
        self.send_header("Content-Type", "text/event-stream")
        self.send_header("Content-Length", str(len(data)))
        self.end_headers()
        self.wfile.write(data)


if __name__ == "__main__":
    print(f"mock server on 127.0.0.1:{PORT}", flush=True)
    HTTPServer(("127.0.0.1", PORT), Handler).serve_forever()