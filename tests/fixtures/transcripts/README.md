# Per-provider SSE transcripts

Each `.jsonl` file is a captured SSE stream with **one `data:` payload per line**.
They document the exact wire shapes `mock_server.py` serves and double as
regression fixtures for the SSE parser and the provider stream parsers.

- `openai_toolcall.jsonl` — OpenAI `chat.completion.chunk` shape over two
  requests: request #1 carries a `run_shell` tool call (`finish_reason:
  "tool_calls"`), request #2 carries the final text (`finish_reason: "stop"`).
- `openai_reasoning.jsonl` — OpenAI shape that also emits `reasoning_content`
  deltas before the answer, demonstrating that reasoning is surfaced separately
  from the final `content`.
- `anthropic.jsonl` — Anthropic Messages shape (`message_start`,
  `content_block_delta`, `message_delta`, `message_stop`).

The final non-JSON line `[DONE]` is SSE's terminal marker and is intentionally
left as-is; it is not part of any JSON payload.
