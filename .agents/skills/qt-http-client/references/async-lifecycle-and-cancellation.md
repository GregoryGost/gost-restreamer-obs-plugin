# Qt HTTP response lifetime, cancellation and timeout categories

## Ownership and thread affinity

- `QNetworkAccessManager`, `QNetworkReply`, `QTimer` and associated receivers are QObjects whose thread affinity and event-loop availability matter. Do not access them concurrently from unrelated threads; queue signals/operations to the correct owner.
- Prefer a manager whose lifetime matches the integration service/plugin, not a per-request manager or an unnecessary app-global singleton. The caller/client owns the logical request state, including timers and cancellation.
- Connect request handlers using an appropriate live context object. A lambda capturing a raw `this`, temporary reference, request body or plugin callback after unload can dangle even if the network reply still exists.
- `deleteLater()` is the usual way to release a reply safely; if auto-delete is enabled, avoid duplicate deletion. A late `finished()` after `abort()` must be treated as cancellation rather than scheduling a reconnect unconditionally.
- On explicit stop, disable retries/reconnect, cancel waiting timers, abort active replies, drain/close work within the authorized shutdown boundary and prevent queued callbacks from acting on freed state.
- Use a clear current-request token/generation, QPointer/guarded context or other project-owned state as needed to distinguish stale completion from the latest connection. This is a targeted lifetime mechanism, not a mandate for a new state-machine framework.

## Distinct timeout meanings

- `QNetworkRequest::setTransferTimeout(int)` aborts transfer when **no data is exchanged** for the configured period; it is not a total deadline. The std::chrono overload was introduced in Qt 6.7.
- `QNetworkAccessManager::setTransferTimeout` was introduced in Qt 6.7 and applies a manager-level default subject to request overrides. Verify exact target Qt minor/behavior before calling it.
- A total request budget covering connect, queue, server processing and retries needs a separately scoped policy/timer if the contract requires it; a long-lived SSE subscription may intentionally have no finite overall deadline.
- A heartbeat may be an SSE `:` comment: it counts as transferred bytes but is **not** necessarily a business event. Define liveness in terms of actual bytes or events as appropriate.
- Avoid nested `QEventLoop::exec()`, blocking `sleep()` or busy waits for network completion. These can freeze a GUI, trigger reentrancy or violate orderly plugin shutdown.

## Review and regression

Trigger stop during connect, between readyRead calls, while a timer waits to reconnect, inside a response callback and during plugin unload. Verify no callback after teardown and exactly one cleanup owner, irrespective of `finished()/errorOccurred()` signal order.

Official: https://doc.qt.io/qt-6/qnetworkrequest.html ; https://doc.qt.io/qt-6/qnetworkaccessmanager.html ; https://doc.qt.io/qt-6/qnetworkreply.html ; https://doc.qt.io/qt-6/qtimer.html
