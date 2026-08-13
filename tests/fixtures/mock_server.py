#!/usr/bin/env python3
"""tests/fixtures/mock_server.py - per-provider offline mock server.

Serves OpenAI-compatible SSE completions plus alternate provider shapes so the
agent engine can be exercised offline. Routing is by request path prefix, so a
live `puci run` target can be pointed at any of them:

  /v1/chat/completions         OpenAI: tool_call then final text (default)
  /v1-reasoning/chat/...       OpenAI + reasoning_content deltas
  /v1-anthropic/v1/messages    Anthropic-style content_block_delta stream

Use:  python3 tests/fixtures/mock_server.py   (listens on 127.0.0.1:8011)

The event sequences are also captured verbatim under
tests/fixtures/transcripts/*.jsonl (one SSE event per line) as documentation
and regression fixtures for the SSE parser.
"""

import json
from http.server import BaseHTTPRequestHandler, HTTPServer

PORT = 8011
REQUEST = {"count": 0}


def chunk(delta, finish_reason=None, extra=None):
    obj = {
        "id": "mock-" + str(REQUEST["count"]),
        "object": "chat.completion.chunk",
        "choices": [{"index": 0, "delta": delta, "finish_reason": finish_reason}],
    }
    if extra:
        obj.update(extra)
    return "data: " + json.dumps(obj) + "\n\n"


def stream_openai():
    """Tool_call on request #1, final text on request #2. (default)"""
    return (
        chunk({"role": "assistant"})
        + chunk(
            {
                "tool_calls": [
                    {
                        "index": 0,
                        "id": "mock_call_1",
                        "type": "function",
                        "function": {"name": "run_shell", "arguments": ""},
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
                                {
                                    "command": "/bin/echo",
                                    "args": ["hello-from-puci-cli"],
                                }
                            )
                        },
                    }
                ]
            }
        )
        + chunk({}, finish_reason="tool_calls")
        + "data: [DONE]\n\n"
    )


def stream_openai_final():
    return (
        chunk({"role": "assistant",
               "content": "Done. The tool ran and replied: "})
        + chunk({"content": "hello-from-puci-cli"})
        + chunk({}, finish_reason="stop")
        + "data: [DONE]\n\n"
    )


def stream_reasoning():
    """OpenAI-compatible shape that also emits reasoning_content first, then
    the answer, proving reasoning stays separate from the final text."""
    return (
        chunk({"role": "assistant"})
        + chunk({"reasoning_content": "Let me compute this. "},
                extra={"id": "mock-reas-1"})
        + chunk({"reasoning_content": "2 plus 3 is 5."},
                extra={"id": "mock-reas-1"})
        + chunk({"content": "The answer is 5."})
        + chunk({}, finish_reason="stop")
        + "data: [DONE]\n\n"
    )


def sse(**fields):
    return "data: " + json.dumps(fields, separators=(",", ":")) + "\n\n"


def stream_anthropic():
    """Anthropic Messages wire shape (message_start / content_block_delta /
    message_delta / message_stop) with [DONE] trailer."""
    return (
        sse(type="message_start",
            message={"id": "msg_mock", "type": "message", "role": "assistant",
                     "content": [], "model": "mock-claude", "stop_reason": None})
        + sse(type="content_block_start", index=0,
              content_block={"type": "text", "text": ""})
        + sse(type="content_block_delta", index=0,
              delta={"type": "text_delta", "text": "Hello from the An"})
        + sse(type="content_block_delta", index=0,
              delta={"type": "text_delta", "text": "thropic-shaped mock."})
        + sse(type="content_block_stop", index=0)
        + sse(type="message_delta",
              delta={"stop_reason": "end_turn", "stop_sequence": None},
              usage={"output_tokens": 12})
        + sse(type="message_stop")
        + "data: [DONE]\n\n"
    )


def serve(path):
    REQUEST["count"] += 1
    if "reasoning" in path:
        return stream_reasoning()
    if "anthropic" in path:
        return stream_anthropic()
    # Default OpenAI: request #1 tool call, #2 final text.
    return stream_openai() if REQUEST["count"] == 1 else stream_openai_final()


class Handler(BaseHTTPRequestHandler):
    def log_message(self, *args):  # silence
        pass

    def do_POST(self):
        length = int(self.headers.get("Content-Length", 0))
        self.rfile.read(length)
        body = serve(self.path).encode()
        self.send_response(200)
        self.send_header("Content-Type", "text/event-stream")
        self.send_header("Content-Length", str(len(body)))
        self.end_headers()
        self.wfile.write(body)


if __name__ == "__main__":
    print(f"per-provider mock server on 127.0.0.1:{PORT}", flush=True)
    HTTPServer(("127.0.0.1", PORT), Handler).serve_forever()
