---
name: cpp-http-client
description: "Use for C++17+ outgoing HTTP clients, libcurl/cpr or project-selected transports, HTTP/2, bounded retries, TLS/security, streaming and SSE protocol review."
---

# C++ HTTP Client — outgoing HTTP contracts

This is a **transport-neutral C++ HTTP client** skill. The C++ standard library does not provide a built-in high-level HTTP client. Preserve the target project's chosen implementation; libcurl and cpr are examples, not mandatory dependencies. Qt-specific mechanics belong to independently activated `qt-http-client`.

## Required Dependencies

- `cpp-core`
- `.codex/rules/cpp_core.md`

Apply `.codex/rules/cpp_http_client.md` only when independently active. This skill never automatically activates Qt, testing, an SSE library or a replacement HTTP transport.

## Investigation and implementation workflow

1. Read `CODEX_PROJECT.md`, the actual selected transport/library and versions, effective C++/compiler constraints, build/CI setup, authentication and external API contracts. Inspect the full affected HTTP client, lifecycle owner, callers and tests.
2. Separate individual operation contracts: method, host/origin, request body/stream ownership, expected HTTP statuses, content type, auth, idempotency/replay, size limits, timeout/cancellation, redirect/TLS/proxy policy and logging sinks.
3. Reuse the project's connection/session pool and RAII ownership; avoid constructing a new expensive transport per request or leaking native handles. Check callbacks/response buffers remain valid until completion or cancellation.
4. Treat HTTP/1.1 and HTTP/2 as versioned transport capabilities, not a guaranteed result of setting a flag. Check configured dependency/build/runtime support and negotiated protocol when the contract depends on it.
5. Do not invent retries by default. If approved, use the smallest client-supported or project-owned retry mechanism; one owner for attempts/backoff, cap overall budget, respect idempotency and replayable request bodies, `Retry-After` and cancellation.
6. When processing streaming responses, read incrementally under bounded memory, do not accumulate an unbounded reply into a single string. If the endpoint uses `text/event-stream`, read `references/sse-protocol.md` and do not equate SSE reconnect with ordinary HTTP retries.
7. Validate the actual project-selected build, analyzer and isolated test commands against the final source snapshot. Report configured versus observed protocol, negative cases, environment blockers and unexecuted checks.

## Focused references

| Need | Reference |
| --- | --- |
| libcurl/cpr, ownership, C++17 requirements and connection reuse | `references/client-and-transports.md` |
| HTTP/1.1 vs HTTP/2 negotiation, multiplexing and transport boundaries | `references/http-protocols.md` |
| Idempotency, retryable errors, rate limits, deadlines and backoff | `references/retries-and-rate-limits.md` |
| TLS, redirects, SSRF, auth, timeouts, response size and logging | `references/security-and-timeouts.md` |
| SSE stream framing, event IDs, reconnect and standards compliance | `references/sse-protocol.md` |
| HTTP, streaming and SSE deterministic test/review matrix | `references/testing-and-review.md` |
| Versioned official standards and transport documentation | `references/official-sources.md` |

## Optional coordination and non-goals

- `cpp-testing` for tests, `cpp-cmake` for build/dependency edits, `qt-core` and `qt-http-client` for Qt-native clients, only when independently active.
- No automatic new dependencies, retry/circuit-breaker frameworks, rate-limiter, SSE client libraries or HTTP server features. Do not impose one logging redaction policy in conflict with an explicitly selected target policy; distinguish application-owned logging from agent output and third-party telemetry.
- Never run uncontrolled live external requests or install/use services just to prove an example.
