# Security Policy

## Reporting a Vulnerability

PUCI handles API keys and spawns subprocesses, so security reports are taken
seriously. **Please do not open public issues for security vulnerabilities.**

Instead, report privately via one of:

- GitHub's [private vulnerability reporting](https://github.com/uzunenes/puci/security/advisories/new)
- Email: open an issue asking for the security contact email

You should receive a response within 72 hours. If confirmed, a fix and CVE
(if applicable) will be coordinated before public disclosure.

## Scope

PUCI's security surface includes:

- **Credential handling** — API keys (`PUCI_API_KEY`, `OPENROUTER_API_KEY`)
  must never be logged, leaked into transcripts, or accepted in project-local
  config files (`.puci.toml`).
- **Subprocess spawning** — tools run via `fork+execvp` (never `system()`).
  Path traversal, argument injection, and shell metacharacter issues are in
  scope.
- **MCP stdio transport** — JSON-RPC 2.0 message parsing and external server
  lifecycle.
- **HTTP streaming** — provider response parsing (SSE, Anthropic Messages).

## Hardening already in place

- API keys in project-local config (`.puci.toml`) are **rejected with a fatal
  error** to prevent secret leakage in shared repos.
- Credential redaction in logs and transcripts.
- `fork+execvp` with `setpgid`/`killpg` for process group isolation.
- Atomic file writes (`tmp → fsync → rename`).
- Path guards via `realpath` for protected paths (`.git`, `build*`, `puci-good`).
- Plan mode (`--dry-run`) simulates file mutations without writing to disk.
- Tool-call approval callbacks with unified diffs before any `write_file`.

## Out of scope

- Vulnerabilities in third-party dependencies (libcurl, termbox2) — report to
  their respective projects.
- Issues requiring physical access to the user's machine.
- DoS via the LLM provider itself (rate limits are the provider's concern).
