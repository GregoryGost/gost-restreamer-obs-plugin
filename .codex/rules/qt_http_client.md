# Qt 6 outgoing HTTP client rules

Activate only through the exact `qt_http_client.md` Active Rules entry, `qt-http-client` Active Stack Profiles entry, or a Qt HTTP Client profile naming the `qt_http_client.md + qt-http-client` pair. A Qt project is not automatically a Qt HTTP client project.

## Required skills

- `cpp-http-client`
- `qt-core`
- `qt-http-client`

Required base rules:
- `.codex/rules/cpp_http_client.md`
- `.codex/rules/qt_core.md`

## Qt Network contract

- Check the actual Qt 6 minor/patch, enabled Network component, selected `QNetworkAccessManager` owner/thread, event loop, `QNetworkRequest` policy, `QNetworkReply` consumers, target API and shutdown contracts. Prefer existing Qt Network in a Qt project; never require libcurl/cpr or a second HTTP stack without a verified reason.
- `QNetworkAccessManager` is asynchronous and QObject-thread-affine. Do not issue network I/O from the wrong owning thread or introduce synchronous nested event loops / GUI-blocking waits for normal HTTP.
- Consume `QNetworkReply` incrementally when streaming, check HTTP status via reply attributes and network errors separately, and use `deleteLater()` or an already configured auto-delete policy at a safe point. Do not delete a reply directly inside its `finished()` or `errorOccurred()` handler; do not double-clean it.
- Use Qt Network `setTransferTimeout` as an **inactivity** timeout, not as a total operation deadline. A separately owned deadline covers attempts, waits and cancellations only when the application contract requires it. Respect Qt version-specific overload availability.
- HTTP/2 is enabled by default in Qt 6 where negotiated; confirm actual use through reply attributes. Treat cleartext upgrade, direct-mode fallback and multiplexed stream limits as separate version/transport choices. Do not force HTTP/2, disable it globally or assume any SSE-specific HTTP/2 requirement without evidence.
- `QNetworkRequestFactory` and `QRestAccessManager` first appeared in Qt 6.7. Use them only where the target SDK supports them and they simplify the existing client. Do not retrofit a valid `QNetworkAccessManager` implementation just to use a new API.
- For Qt SSE use `QNetworkReply::readyRead`, an incremental standards-aware parser and `QTimer`-driven *optional* reconnect with `Last-Event-ID`. No third-party SSE library. Distinguish network errors, HTTP 204 stop, unexpected status/MIME, server `retry:`, duplicate events and normal cancellation.
- Ensure callbacks, delayed timers and retries cannot outlive receiver/manager/plugin state. Disconnect/cancel/abort on approved shutdown; no stale lambda captures or retry after explicit close.

## Testing and change scope

- Qt-specific network testing requires the active project test policy, including actual event-loop fixtures. When selected, `qt-testing` owns independent GUI/Qt checks; `cpp-testing` owns language-level tests; neither is implicitly activated.
- Check status/timeout/cancellation, retry safety, HTTP/2 negotiation/fallback and SSE parser+reconnect separately on the final snapshot, using disposable controlled HTTP servers where authorized.
- No automatic Qt SDK/extension installation, live external HTTP calls, new cache/rate limiter, persistent background watchers, QML UI or broad refactors simply to implement a client.
