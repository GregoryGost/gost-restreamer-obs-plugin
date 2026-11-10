# C++ outgoing HTTP client rules

Activate only through the exact `cpp_http_client.md` Active Rules entry, `cpp-http-client` Active Stack Profiles entry, or a C++ HTTP Client profile naming the `cpp_http_client.md + cpp-http-client` pair. Mentioning HTTP in a task does not independently activate this rule.

## Required skills

- `cpp-core`
- `cpp-http-client`

Required base rule: `.codex/rules/cpp_core.md`

## Ownership and source of truth

- Read `CODEX_PROJECT.md`, the selected HTTP client/transport and versions, compiler/C++ standard, dependency/build configuration, external API contract, lifecycle owner and the affected callers/tests. This is an **outgoing** HTTP client profile, not an inbound HTTP server or an implicit networking framework.
- Preserve the selected implementation (such as libcurl, cpr or another verified client); C++17 has no universal standard-library HTTP client. Do not add, replace or upgrade a library solely because another is familiar or newer.
- Treat client/session/connection pool, callbacks, request bodies, response objects and background work as owned resources. Document shutdown and plugin/unload boundaries where relevant. Blocking transport calls must not block a UI or other latency-sensitive thread.
- Classify HTTP status, transport/TLS failure, parse/validation failure, timeout and cancellation separately; non-2xx status is not necessarily a transport failure. Bound request/response and streaming memory and account for redirects/proxies, certificate verification and authentication scope.

## Retries, protocols and SSE

- **No application retries by default.** A retry must be enabled by the already selected operation-level policy: replay-safe/idempotent semantics, verified server deduplication if needed, replayable request body, attempt and total budgets, explicit retryable statuses/errors, backoff/jitter, `Retry-After` and cancellation.
- A lost response may follow a successfully committed remote operation. Never retry a mutating POST merely after an ambiguous failure or merely because the client generated an idempotency-key header.
- HTTP/1.1 versus HTTP/2 capabilities depend on the actual transport binary/build and server negotiation. Do not claim HTTP/2 from configuration alone, or assume a protocol fallback is always guaranteed. Streaming/SSE works over either version; application parsing is independent of HTTP framing.
- When SSE is in scope, load `.agents/skills/cpp-http-client/references/sse-protocol.md`. SSE **reconnection** is a separate subscription lifecycle from ordinary HTTP retries. Always parse the SSE framing and field semantics correctly; use `Last-Event-ID` resume and server `retry:` in accordance with the EventSource protocol, while treating actual replay/retention guarantees as **server-specific**.
- Do not install an SSE library, create a generic retry/cache/circuit-breaker/limiter subsystem, or introduce broad fallback behavior without an actual requirement. Rate limiting and admission control are distinct from connection-pool limits and must reflect the upstream contract.

## Validation boundary

- Apply independently active `cpp-testing` when its profile is selected; otherwise use project-declared tests. Prefer disposable local HTTP endpoints, approved transport doubles and bounded streaming fixtures rather than real production APIs.
- Confirm final saved source/build snapshot, required compiler/diagnostic checks and test outcome. Failed or unavailable mandatory checks are FAIL/BLOCKED/NOT VERIFIED, never PASS.
- Read task-relevant `cpp-http-client` references for transport/version, retries, security, HTTP/2, SSE and testing. Preserve authorized scope, external-system gate and disclosure policy; project logging permissions never authorize leaking secrets to agent output or GitHub.
