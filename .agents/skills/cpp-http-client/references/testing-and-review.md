# C++ HTTP client / SSE test and code-review matrix

Test with project-approved C++ testing tools and disposable local HTTP fixtures. Do not introduce a permanent network service, uncontrolled external requests or new test framework.

| Scope | Required evidence when behavior in scope |
| --- | --- |
| Request lifecycle | shared transport/session ownership, cancellation, cleanup, no late callback after exit/unload |
| HTTP statuses | 200/2xx success, non-2xx response status, 429/503, invalid content type, malformed/oversized body, read/parse errors |
| Time budgets | connect/inactivity/per-attempt/total differentiation, cancellation during request and backoff, no attempt after deadline |
| Retries | one retry owner, allowed method and operation, replay-safe body, jitter/budget, valid/invalid `Retry-After` |
| Ambiguous mutation | remote commit then lost response: no duplicate POST absent server idempotency guarantee |
| HTTP/2 | actual negotiated version in capable fixture, allowed HTTP/1.1 fallback, stream-level failures where selected |
| SSE parser | all byte splits and UTF-8, CRLF/CR/LF, BOM, comments, `data`, `event`, `id`, `retry`, EOF and size limits |
| SSE reconnect | cursor and empty ID reset, server delay, 204 terminal stop, explicit close, auth/non-SSE response |
| Security | TLS errors, proxy/redirect boundary, credential forwarding, user-influenced URLs and allowed log sinks |

## Oracle quality

Separate test assertions for transport errors from HTTP status; a mock that only returns status 200 does not prove a real HTTP/2 handshake or TLS trust handling. A simulated SSE parser test does not prove shutdown of an actual `QNetworkReply` and timer. Use both pure deterministic parser cases and a small real loopback network test when required and permitted.

Avoid fixed delays and fixed ports. Use bounded event-driven waits with disposable fixture server state; do not disable TLS verification or relax production assertions merely to make test cases pass.

After any material fix, re-run affected diagnostics and tests on the current source/build/configuration snapshot. Report tested target/platform, covered boundary, actual tool versions, PASS/FAIL/NOT VERIFIED and known gaps. Do not claim broad network protocol support from unit-only tests.
