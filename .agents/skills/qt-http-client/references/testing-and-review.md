# Qt HTTP, retries and SSE — deterministic checks and review

Use only approved project test commands with actual Qt 6 Network and the configured compiler/platform. `qt-testing` or `cpp-testing` is applied only if independently active; this reference does not add a hard dependency.

## Test boundaries

- Pure SSE parser: no network, input bytes arbitrarily split; use a deterministic corpus for CR/LF, UTF-8, BOM, empty data, comments, multi-line, id-only, empty ID, bad retry and oversize buffer.
- Real Qt Network reply: local QTcpServer/HTTP fixture or existing test server sending real incremental bytes; assert `QNetworkReply` connection lifecycle, status, MIME, bounded reading and shutdown.
- Qt GUI: a QCoreApplication event loop is usually sufficient for network-only tests; use QApplication and GUI-specific fixtures only when the actual code requires widgets. Avoid testing against a user's live OBS process.
- HTTP/2: only a real h2/ALPN-capable fixture proves protocol negotiation; check `Http2WasUsedAttribute`, and test HTTP/1.1 fallback when material.
- Auth/TLS: fixture certificates, redirect origins and proxy configuration should be isolated and authorized. Do not disable certificate validation in production to pass a test.

## Scenario matrix

| Scenario | Required oracle when in scope |
| --- | --- |
| 200 JSON / HTTP 404 | proper status/error separation and body validation |
| 429 / 503 with `Retry-After` | bounded operation-level retries only when enabled; correct header/date handling |
| POST committed, response lost | no duplicate mutation without verified server idempotency |
| Timed out or aborted QNetworkReply | cancellation, deferred cleanup, no late user callback |
| SSE event split across readyRead chunks | no premature dispatch; correct UTF-8 and multiline payload |
| SSE heartbeat comments | correct idle/liveness policy and no phantom data events |
| SSE invalid MIME / HTTP 204 | fail/stop according to contract; 204 never reconnect |
| SSE id-only / empty id | cursor retained or cleared, as specified |
| SSE reconnect after server close | at most one timer; optional Last-Event-ID |
| stop during retry backoff / plugin unload | no reconnect, no dangling QObject reference or pending callback |
| slow subscriber / huge field | bounded memory, fail/close/backpressure according to declared policy |

## Review output

Report the real source line/caller, actual Qt minor/version and C++ target, chosen policy, precise failure, proof/test, possible counterexample, smallest correction and what was NOT VERIFIED. UI-not-blocked, thread-affinity and no resource leaks need actual evidence; neither compilation nor parser unit tests prove them alone.
