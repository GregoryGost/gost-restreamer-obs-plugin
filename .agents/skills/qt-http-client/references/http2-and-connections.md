# Qt 6 HTTP/2, connection reuse and protocol verification

## Qt defaults

Qt 6 enables HTTP/2 by default for `QNetworkAccessManager` (subject to server/client negotiation). For HTTPS, TLS ALPN is normally used. If HTTP/2 cannot be negotiated and the transport permits it, Qt uses HTTP/1.1. Nothing here requires manually forcing HTTP/2 for every request.

- `QNetworkRequest::Http2AllowedAttribute`: request policy, default true.
- `QNetworkRequest::Http2WasUsedAttribute`: **reply** attribute. Check `reply->attribute(QNetworkRequest::Http2WasUsedAttribute).toBool()` after headers/connection negotiation when the actually used protocol matters.
- `Http2DirectAttribute` requests prior-knowledge HTTP/2 without initial negotiation; it **does not fall back** to HTTP/1.1 if the remote endpoint does not support it. Do not enable it speculatively.
- `Http2CleartextAllowedAttribute`: permits the explicit h2c cleartext path; default false. Avoid enabling h2c outside an approved endpoint/TLS policy.
- `QNetworkRequest::setHttp2Configuration` has documented semantics and its effective configuration can be based on the first request sharing a connection. Verify compatibility with the exact Qt minor/version.

## Connection and pool behavior

- HTTP/2 multiplexes independent requests/streams over a shared connection. A long-lived SSE reply may occupy a stream, not necessarily a whole TCP connection. Server max-concurrent-streams, flow-control windows and retries are real constraints but do not justify adding a separate connection manager by default.
- Qt may cache/reuse connections. Do not change connection expiry globally without measuring or verifying a real resource/latency requirement.
- An `abort()` on a reply stops that logical request; the effect on the pooled HTTP/2 connection must not be guessed or treated as a guarantee of peer-side cancellation/rollback.
- Redirect, proxy, TLS policy, HTTP status and application retries still need explicit handling. HTTP/2 does not guarantee safe mutation replays or perfect resume.
- Keep the selected upstream/API concurrency and backend quota policy separate from Qt's internal connection cache or HTTP/2 stream limits.

## Verify

Use a controlled test server with real HTTP/2/ALPN support and check `Http2WasUsedAttribute`. Test a separately configured HTTP/1.1-only endpoint for fallback when that matters. A fake reply bearing `Http2WasUsedAttribute=true` tests client branching only, not actual HTTP/2 negotiation.

Official: https://doc.qt.io/qt-6/network-changes-qt6.html ; https://doc.qt.io/qt-6/qnetworkrequest.html ; https://www.rfc-editor.org/rfc/rfc9113
