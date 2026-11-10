# HTTP/1.1 and HTTP/2 behavior

## Distinguish capability, negotiation and actual traffic

- HTTP/1.1 and HTTP/2 convey the same HTTP methods, status and content semantics but have different framing and connection concurrency. HTTP/2 multiplexes independent streams on a connection and performs flow control; application code should not parse HTTP/2 frames when the chosen client already owns them.
- For HTTPS, ALPN usually negotiates the protocol; whether HTTP/2 is actually available depends on the transport's TLS backend, library build and remote server. A requested protocol or installed header is not evidence of use.
- If negotiation cannot select HTTP/2, a client may use HTTP/1.1 when its contract allows. Direct/prior-knowledge modes need not fall back. Cleartext HTTP/2 (h2c) has separate policy, proxy and security implications; do not enable it by default.
- libcurl capabilities can be inspected with `curl_version_info()` and the selected build flags; request options such as `CURLOPT_HTTP_VERSION` alone do not prove negotiation. Observe `CURLINFO_HTTP_VERSION` or the transport's supported equivalent when the used protocol matters.
- HTTP/2 streams can fail independently of a transport connection; safe retry still requires an operation contract. GOAWAY/REFUSED_STREAM behavior is transport-specific; do not infer exactly-once delivery or blanket replay safety from the HTTP/2 layer.
- A long-lived SSE response can occupy a stream in HTTP/2, or a connection in HTTP/1.1. Confirm server stream/concurrency limits and that ordinary HTTP requests are not starved. Don't add a second connection pool without a measured or contractual reason.

## Contract and payload remain independent

- Status, redirect, `Retry-After`, authentication, content type and body size are HTTP semantics in both versions. HTTP/2 does not make retries automatically safe.
- SSE is `text/event-stream` encoded as UTF-8; HTTP/1.1 chunking and HTTP/2 DATA frames are not SSE event boundaries. Application parsing works on arbitrary received byte fragments.
- A 204 response is a specific stop-reconnect signal for SSE EventSource behavior, not a generic HTTP success with SSE data.

## Verify

Where protocol choice matters, record client build/version, server endpoint/fixture, requested policy and negotiated result from an actual request. Test HTTP/1.1 fallback and HTTP/2 with a truly capable controlled server; a mock that only labels a response HTTP/2 does not prove negotiation.

Sources: https://httpwg.org/specs/rfc9113.html ; https://curl.se/libcurl/c/CURLOPT_HTTP_VERSION.html ; https://curl.se/libcurl/c/CURLINFO_HTTP_VERSION.html
