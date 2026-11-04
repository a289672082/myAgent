# Codex, Claude Code, and MCP client interoperability

Use **Streamable HTTP** (`POST /mcp`) for remote clients and **stdio** for locally launched servers. The SDK does **not** create a separate legacy HTTP+SSE endpoint. New MCP `2026-07-28` requests are stateless: one HTTP POST, per-request `_meta`, `MCP-Protocol-Version` / `Mcp-Method` / named request headers. Fast calls return JSON; calls carrying a `progressToken` stream SSE progress notifications and end with the matching JSON-RPC response. The `subscriptions/listen` request creates a long-lived SSE stream.

## Codex CLI

Start `run_http_server_demo` (loopback-only by default), then in another terminal:

```sh
codex mcp add myMcpTools --url http://127.0.0.1:33867/mcp
codex mcp list
```

Or register a local **stdio** executable:

```sh
codex mcp add myMcpTools-local -- /absolute/path/to/run_stdio_server_demo
```

For HTTPS deployments, use your real endpoint URL and properly configured OAuth 2.1 protected resource metadata; never expose a loopback-only plain HTTP demo to the Internet.

## Claude Code

```sh
claude mcp add --transport http myMcpTools http://127.0.0.1:33867/mcp
claude mcp list
```

To install for all projects (if permitted), use `--scope user`. For a local server:

```sh
claude mcp add --transport stdio myMcpTools-local /absolute/path/to/run_stdio_server_demo
```

For HTTP clients on a different machine, `127.0.0.1` refers to that machine, not the MCP host. Deploy behind an HTTPS reverse proxy with a trusted access-control layer; the library intentionally rejects public-facing plaintext HTTP bindings.

## Protocol compatibility

| Feature | Behavior |
| --- | --- |
| MCP `2026-07-28` | Native stateless requests, request-scoped SSE, subscriptions/listen |
| MCP `2025-11-25` / `2025-06-18` / `2025-03-26` | Compatibility adapter for `initialize`, `notifications/initialized`, basic Tools/Resources/Prompts over Streamable HTTP; no protocol-level session is created |
| Old GET `/mcp` SSE stream | Returns **405**, a valid response when the server does not offer the legacy notification stream |
| Legacy HTTP+SSE (2024-11-05) | Not implemented; not a Streamable HTTP client |
| OAuth | See `SECURITY.md`; demo tokens are never production credentials |

## Tests

- `http_streamable_compat.py`: 2026 JSON and request-scoped SSE, legacy HTTP handshakes and tools calls, GET 405.
- `legacy_http_client.py`: C++ `Client::http` automatically falls back when `server/discover` is unsupported.
- `run_http_progress_demo`: no-argument C++ client displays live HTTP SSE progress and final tool result.
- Existing stdio, pagination, MRTR, subscription, OAuth and security tests remain enabled.

The legacy tests *simulate protocol behavior*, not real Codex/Claude process launches. This repository does **not** claim verified compatibility with every released Codex or Claude version or full official MCP conformance. In particular, rich legacy server-to-client requests, resume/Last-Event-ID, OAuth issuer-specific provider differences, and disconnect cancellation of blocked tool handlers remain outside this test suite.

Reference: [MCP 2026-07-28 HTTP transport](https://modelcontextprotocol.io/specification/2026-07-28/basic/transports/streamable-http).
