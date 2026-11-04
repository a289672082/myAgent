# MCP 2026-07-28 compatibility matrix

Updated October 31, 2026. **An actively developed subset implementation, not a fully conformant SDK.** A green CTest result only certifies the tested local behavior, not official protocol conformance.

| Protocol area | Current implementation | Remaining work |
|---|---|---|
| Core JSON-RPC | Request IDs, errors, typed tools, basic notifications | Strict wire-schema testing and malformed packet fuzzing |
| Stateless modern lifecycle | `server/discover`, required version + client identity/capabilities in request `_meta`, server response metadata, `resultType` | Official conformance tests and interoperability with TS v2/Kotlin/C# |
| Legacy 2025-11-25 | stdio and basic Streamable HTTP `initialize` fallback, stateless tool calls | GET-based notifications, session resume, complete historical feature parity |
| Tools | Typed registration, jsoncons JSON Schema validation, context tools, async task tools, HTTP header annotations | Remote schema references, structured outputs/outputSchema validation; icons and annotations |
| Resources | Plain text resources, basic URI template variables, update notification | Binary resources and full RFC6570 URI templates |
| Prompts/completions | Rich prompt callback, arguments, completion callback | Schema validation and comprehensive field coverage |
| List pagination/cache | Deterministic in-memory pagination, `ttlMs`, `cacheScope`; authenticated HTTP forces private scope | Snapshot cursors and cache revalidation |
| MRTR | Basic `input_required` and retry with requestState | Full validation of sampling/elicitation, state security, all behaviors |
| Tasks extension | Opt-in capability gate, cryptographic task IDs, poll, input-required/update, cooperative cancel, TTL expiry, HTTP routing headers | Durable persistence across restart; `taskIds` subscriptions/`notifications/tasks`; external task backend; interoperable conformance |
| Request progress/cancellation | Concurrent stdio work, `notifications/progress`, cooperative cancellation; HTTP request-scoped progress SSE and incremental client callback | Prompt cancellation when a blocked HTTP handler receives disconnects, retries/timeouts, robust stdio streaming API |
| Subscriptions | Explicit `subscriptions/listen`, acknowledge, filters, HTTP SSE, stdio cancellation | Task status subscriptions, reconnect/resume, graceful close API integration |
| HTTP transport | 2026 header validation, 4MiB request limit, loopback listener, metadata and bearer verification callback | Network-facing native HTTPS server, authenticated reverse proxy deployment guides, load/DoS tuning, legacy HTTP sessions |
| HTTPS client | TLS with peer/hostname verification (OpenSSL), configurable Bearer token | HTTPS SSE subscription path, timeouts/retries/cert pinning options |
| OAuth resource server | RFC9728-style metadata endpoint + bearer challenge; caller-supplied callback for token validation (MUST validate token/scope/audience/issuer) | Built-in JWT validation/JWKS discovery, OAuth scope step-up, authorization server interoperability |
| OAuth client | PRM/AS discovery, PKCE S256, state+issuer verification, DCR registration, code exchange, refresh, Bearer attachment | CIMD content validation, interactive browser redirect receiver, durable secret storage, scope escalation/re-authorization |
| Extensions | Namespaced capability advertising + custom method registry; Tasks subset | Apps, non-core extensions, full negotiation and independent extension conformance |
| Official conformance | Linux and Windows CTest suite | Official conformance suite, TS v2/Kotlin integration, CI across OSes |

## Build and test

```sh
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j
ctest --test-dir build --output-on-failure
```

Dependencies: C++20, cpp-httplib, jsoncons, OpenSSL (SSL and Crypto), Threads. CMake FetchContent uses pinned upstream commits.

## Security and scope notes

This repository must not be advertised as full MCP 2026-07-28 compatible. OAuth has **partial** implementation: a token verification callback is an integration hook, not a substitute for a trusted JWT/introspection verifier. The built-in HTTP listener binds only to loopback; a production resource server needs an authenticated TLS gateway and a security review. Long-running tasks are in-memory only, with finite retention and no cross-process durability.

## cpp-httplib / jsoncons migration workstream

- Replaces Boost.Asio/Beast with cpp-httplib and Boost.Process with Windows/POSIX native process APIs.
- Replaces Boost.JSON with jsoncons and compiles JSON Schema validation with jsoncons 2020-12.
- Adds OAuth Session: protected-resource discovery, AS metadata discovery (RFC 8414 and OIDC),
  authorization code + PKCE S256, callback state/issuer validation, token exchange, dynamic client
  registration (when advertised), and refresh; clients can attach a Session using `Client::http_oauth`.
- The OAuth server is a protected resource, not an authorization server; host apps supply a token
  validator and the AS endpoints separately.
- No official MCP conformance, native TLS hosting, durable Tasks, SSE replay/resumption, or secure
  automatic browser redirect receiver claims are made at this stage.


## Streamable HTTP compatibility addition

- **New:** HTTP `tools/call` with `progressToken` returns request-scoped `text/event-stream` containing progress notifications followed by the final JSON-RPC result; `Client::http` accepts an event callback and parses events incrementally.
- **New:** Basic legacy HTTP initialize, list/call/read support (`2025-03-26`, `2025-06-18`, `2025-11-25`) with no server-issued session ID. Legacy GET `/mcp` explicitly returns 405 (no GET SSE notification stream).
- **Remaining limitations:** Some legacy clients need server-to-client sampling/elicitation, GET-based resource change streams, session resumption, or complete 2025 feature parity; those are not provided. A dropped HTTP connection only marks a cooperative tool cancelled once an SSE write fails. Actual Codex and Claude Code application-level integration is not part of CI.