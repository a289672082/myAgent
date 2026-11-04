# Security and production limits

**Do not expose the built-in HTTP listener to the public Internet.** It binds to loopback deliberately. This code is an experimental MCP SDK, not an audited security boundary and not a fully conformant MCP implementation.

- `Server::protect_http` adds protected-resource metadata, a Bearer challenge and a token validation callback. **Your verifier must authenticate tokens cryptographically or through trusted introspection and verify issuer, audience/resource, expiry and per-operation scopes.** A token string comparison is used solely in the demo/test mode and is not OAuth 2.1 validation.
- For remote deployments place the service behind a correctly configured HTTPS authentication/authorization gateway. Check whether the gateway forwards authenticated identity securely and rejects unauthenticated traffic.
- HTTPS client connections validate certificates and hostnames using the OS/OpenSSL trust store. Never bypass verification in production.
- `oauth::begin` creates PKCE S256 and CSRF state. `oauth::validate_callback` checks state and issuer before token exchange. The application still must securely discover and validate AS metadata, manage registration, store tokens by issuer, exchange/refresh tokens and handle scope step-up.
- In-memory Tasks IDs have CSPRNG entropy and expire by TTL, but no crash recovery, owner-specific isolation or persisted authorization. Do not use the task registry in a multi-tenant deployment without identity-based isolation and durable storage.
- `validate_schema` is a **subset** of JSON Schema 2020-12. Unsupported assertions are not enforced. Always validate business rules within tools and do not rely on this validator as a security boundary.
- stdio tools can check cooperative `Server::is_cancelled()` and report progress. Arbitrary blocking callbacks cannot be preempted safely.
- Rate limiting, comprehensive request timeouts, server resource quotas, HTTP SSE disconnect cancellation, native TLS hosting and cross-SDK conformance are not implemented.
- Tool prompts, URI templates, extensions, and tool annotations should be treated as untrusted application input. Limit filesystem and system privileges.

### Migration OAuth security constraints

The new `oauth::Session` uses HTTPS for external discovery and token endpoints; only
explicit loopback is permitted for development. It disables cross-origin redirects for
token exchanges and requires exact issuer, resource and state checks. Do not print
`Authorization::code_verifier`, authorization codes, access tokens or refresh tokens.
Browser-based authorization is interactive and application-managed. Do not reuse a
session for a different resource/audience. Treat OAuth tokens as secrets, and implement
revocation, secure OS storage and client registration policy in the host application.
