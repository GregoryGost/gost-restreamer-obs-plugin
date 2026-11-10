# Qt Network / REST client semantics and version boundaries

## Core Qt 6 API

- The supported Qt CMake target for these APIs is `Qt6::Network` (`find_package(Qt6 REQUIRED COMPONENTS Network)`, then `target_link_libraries(target PRIVATE Qt6::Network)` as appropriate). A plugin/library may already acquire that target transitively; preserve its real build contract.
- `QNetworkAccessManager` is an asynchronous, QObject-thread-affine manager for requests. One appropriately owned manager may serve related integrations; it owns proxy/cache/cookie settings as configured, but it does **not** imply one universal auth/policy for all endpoints.
- `QNetworkReply` is a `QIODevice`: `readyRead()` signals incremental bytes, `finished()` ends updates and `errorOccurred()` identifies an error condition. HTTP status is available from `reply->attribute(QNetworkRequest::HttpStatusCodeAttribute)`; inspect it **alongside**, not instead of, `reply->error()`. A 404/503 may be accompanied by a network error classification but still has an HTTP response.
- Read any final buffered bytes at `finished()` when needed, avoid `readAll()` on unlimited responses and choose explicit maximum expected payload sizes. Processing the entire response in a GUI update callback can block the event loop.
- Qt owns no universal application-level retry policy. Prefer an already chosen small per-integration policy only when needed, not a new generic retry framework.
- Qt `QNetworkRequestFactory` (since Qt 6.7) groups request defaults; `QRestAccessManager` and `QRestReply` (since Qt 6.7) provide REST conveniences on top of an existing `QNetworkAccessManager`. `QRestAccessManager` does **not** own the wrapped manager. They do not replace `QNetworkReply` for long-lived SSE parsing.
- Qt minor-specific APIs evolve; for example factory attribute setters and header convenience types arrived later than Qt 6.7. Confirm the **exact** minor before copying examples. No blanket minimum Qt 6.7/6.8 requirement for basic Qt Network.

## Valid request and response lifecycle

1. Create request with the actual URL, auth, headers, accepted content, redirect/TLS and timeout policy. Decide trust boundaries before starting.
2. Issue manager `get/post/put/deleteResource` in its owning thread and connect handlers with a suitable receiver/context lifetime.
3. Observe status/header metadata and incremental body; distinguish completed HTTP response, transport error, validation failure and explicit cancellation.
4. Defer reply deletion with `deleteLater()` after processing, or use an **already configured** `AutoDeleteReplyOnFinishAttribute`/`setAutoDeleteReplies` without also deleting it. Never delete directly in `finished()`/`errorOccurred()` handler.
5. Revoke/abort active replies and timers during authorized shutdown, ensuring no callbacks can refer to the destroyed client/plugin.

## Anti-patterns

**Bad:** new manager per request with a temporary parent; or synchronous `QEventLoop::exec()` inside a button handler to 'await' the response.

**Better:** manager with explicit lifetime, async callbacks or Qt-native event mechanism and safe receiver context.

**Bad:** treat `errorOccurred` alone as the HTTP status decision, or use unbounded `reply->readAll()` for an SSE connection.

**Better:** examine `HttpStatusCodeAttribute`, transport state and streaming buffer limits independently.

Official: https://doc.qt.io/qt-6/qnetworkaccessmanager.html ; https://doc.qt.io/qt-6/qnetworkreply.html ; https://doc.qt.io/qt-6/qrestaccessmanager.html ; https://doc.qt.io/qt-6/qnetworkrequestfactory.html
