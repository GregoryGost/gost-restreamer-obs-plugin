---
name: qt-http-client
description: "Use for Qt 6 QNetworkAccessManager/QNetworkReply HTTP clients, HTTP/2, Qt REST, async cancellation, retries and SSE streaming without SSE dependencies."
---

# Qt 6 HTTP Client — Qt Network and SSE

This is an **optional** Qt 6 outgoing HTTP profile layered on the generic C++ HTTP contract and Qt event-loop/lifetime rules. Prefer Qt Network when the project already uses Qt. Neither OBS-specific behavior nor a Qt Creator workflow is implied.

## Required Dependencies

- `cpp-http-client`
- `qt-core`
- `.codex/rules/cpp_http_client.md`
- `.codex/rules/qt_core.md`

Apply `.codex/rules/qt_http_client.md` only when independently active; selecting this skill does not activate a new library, tests or QML.

## Workflow

1. Verify `CODEX_PROJECT.md`, actual Qt 6 minor, Network module, CMake target, selected Qt network manager/thread, event loop, reply ownership and API endpoint contract. Inspect source, callers, tests and shutdown.
2. Use `QNetworkAccessManager` asynchronously, with a long-lived owner appropriate to the application. Distinguish reply `errorOccurred`, HTTP response status, final `finished` and the presence of partial body data.
3. Use `QNetworkRequest` for headers, TLS/proxy/redirect and timeout policy; `QNetworkReply` for incremental reads and status. Release at an appropriate time through `deleteLater` or already selected auto-delete; track cancellation and callback safety.
4. Confirm the **actually negotiated** HTTP/2 on the reply only when required; Qt 6 enables HTTP/2 by default but may fall back to HTTP/1.1. Avoid h2c/direct mode unless the endpoint explicitly requires it.
5. For Qt 6.7+ projects consider `QNetworkRequestFactory`/`QRestAccessManager` when already adopted or clearly useful. They are convenience APIs, not compulsory replacements for `QNetworkAccessManager` or applicable to every Qt 6 minor.
6. Retry normal requests only under the `cpp-http-client` idempotency/replay/budget policy, using a bounded `QTimer` approach where needed; never block the Qt event loop with sleeps or nested synchronous loops.
7. When SSE is requested, read both `.agents/skills/cpp-http-client/references/sse-protocol.md` and `references/sse-streaming.md`. Implement SSE over `QNetworkReply::readyRead()` with an incremental parser, optional subscription reconnect, event cursor and lifecycle-safe stop. **No separate SSE library.**
8. Validate a saved final build/diagnostic/test snapshot and report the actual Qt version, HTTP protocol used when material, test fixture and any blocked/unavailable checks.

## Reference routing

- `references/qt-network-and-rest.md` — QObject/thread ownership, Qt Network/REST and response errors.
- `references/http2-and-connections.md` — ALPN, fallback, h2c/direct, multiplexing, actual reply attributes.
- `references/async-lifecycle-and-cancellation.md` — replies, Qt timers, inactivity and total deadlines, teardown.
- `references/retries-and-timeouts.md` — allowed per-operation retry contract and nonblocking Qt scheduling.
- `references/sse-streaming.md` — event stream parser integration, reconnect, Last-Event-ID and stop conditions.
- `references/testing-and-review.md` — deterministic isolated Qt HTTP/SSE scenarios.
- `references/official-sources.md` — versioned Qt and SSE normative sources.

## Boundaries

No additional SSE dependency, second network stack or new background service by default. No automatic SDK installations, renderer/UI work, unapproved HTTP calls or blanket error suppressions. Do not use general `cpp-http-client`/Qt logging permissions as permission to disclose credentials through agent responses.
