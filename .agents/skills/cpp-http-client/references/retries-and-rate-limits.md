# Outgoing HTTP retry, rate-limit and deadline contracts

## Retry is opt-in

A failed read or timeout can occur **after the server committed a mutation**. Retry safety is determined by the requested business operation and server's documented idempotency/deduplication contract, not by a transport error alone.

- Default `application retry: disabled` until target configuration selects explicit retryable operations. GET/HEAD are generally designed for safe repeats, but validate actual API behavior and side effects. PUT/DELETE may be idempotent by protocol yet have application-level preconditions. POST requires verified deduplication/idempotency semantics if failure is ambiguous.
- An idempotency key is only effective when the server recognizes and retains it for the required time. Reuse the same key across *attempts of one logical operation*, never mint a new key per attempt.
- Define response statuses (`429`, selected `502/503/504`) and error classes separately. `401/403` and invalid payloads are not generally fixed by retry. Non-2xx may be a valid domain response.
- Ensure upload/stream body replayability; bytes may already have been consumed or sent. Do not buffer arbitrarily large streams merely to enable retry.
- Limit attempts, per-attempt timeouts and **one total operation deadline** that includes connection waits, server response, backoff and downstream cancellation; no infinite jitter/sleep after shutdown.
- Honor documented `Retry-After` when applicable: it may be delay-seconds or an HTTP date. Validate parsing, clamp to the remaining total budget and project caps; no negative or unbounded waits. Use bounded exponential backoff with jitter when approved.
- Record attempt number, selected retry reason, elapsed/remaining budget and final result without double-counting a single logical operation as several business outcomes.

## Pool concurrency is not a quota limiter

Connection limit, request concurrency, per-user rate/quota, server 429, HTTP/2 concurrent-stream cap and queue bound are distinct controls. Use an already existing project mechanism when the upstream requires proactive throttling; do not introduce token buckets, queues, caches or a circuit breaker by default.

## SSE reconnect is different

A server-sent event subscription may reconnect after a stream closes and resume with a cursor. This is not a retry of a mutable HTTP operation; it uses EventSource-specific `retry:`, `Last-Event-ID` and terminal conditions. See `sse-protocol.md`. Do not stack a generic HTTP retry loop around an SSE reconnect loop.

## Test negative cases

- Transport reports EOF after the server applied a POST: do not send a second mutation without a verified idempotency agreement.
- GET 503 then 200: retry only when enabled, with observed delay and a cap.
- 429 with HTTP-date `Retry-After`: respect the budget, handle invalid header safely.
- Cancellation during backoff: no new attempt.
- Nonreplayable upload: fail/return according to contract rather than silently retrying.
