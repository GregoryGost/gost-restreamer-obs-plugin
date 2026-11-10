# Qt HTTP retries — QTimer scheduling without event-loop blocking

Only apply retries when `CODEX_PROJECT.md` and the individual upstream operation explicitly select them. A normal HTTP retry and SSE subscription reconnect are different lifecycles.

## Workflow

1. Confirm method/business idempotency and server acknowledgement/deduplication contract. Lost response after POST may mean a successful server mutation. Do not retry an unsafe operation because the Qt network API reported EOF or timed out.
2. Keep the same logical operation/correlation/idempotency key across permitted attempts. Reconstruct the `QNetworkRequest` and request body as needed; a consumed streaming `QIODevice` is not automatically replayable.
3. Observe `QNetworkReply::error()`, `HttpStatusCodeAttribute`, `finished()` and any received body; classify network/TLS/HTTP status separately. Decide from a bounded allowed set (such as selected 429, 503 or timeout) and project policy, never all non-2xx.
4. Schedule an attempt with `QTimer` or `QTimer::singleShot` tied to the owning QObject/context. Add project-selected bounded exponential backoff/jitter and `Retry-After` in delay-seconds or HTTP-date form when supported; cap by remaining total operation budget.
5. On cancellation, stop any timer, mark operation terminal, abort active `QNetworkReply` and prohibit further attempts. A late `finished()` on an aborted reply is **not** a retry trigger.
6. Bound attempts, queues, overlapping requests and response bodies; do not start a second request while an earlier attempt is unresolved without an explicit deduplication/concurrency contract.
7. Report one logical outcome and attempt diagnostics separately. No new circuit breaker, retry middleware or rate limiter unless expressly required.

## Time budgets

Qt's transfer timeout is for inactivity. A per-operation absolute deadline, if needed, must cover retries and waiting as well as individual transfers. If the upstream sends a heartbeat forever, transfer timeout alone never enforces an overall business deadline.

## SSE distinction

SSE reconnect may be triggered after a normally completed stream, may need to send `Last-Event-ID`, and must stop on HTTP 204 and explicit cancellation. The standard `retry:` field updates subscription reconnect delay; it is not a general HTTP request retry header. Read `sse-streaming.md` plus the generic SSE specification; do not stack normal retry attempts on the SSE reconnect handler.
