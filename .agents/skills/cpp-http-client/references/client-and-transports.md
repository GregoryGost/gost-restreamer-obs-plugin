# C++ HTTP transports, ownership and lifecycle

C++17 does **not** include an HTTP client in the standard library. Use the exact transport chosen by the target project. This reference describes ways to review existing libraries, not a new library selection requirement.

## Transport options and their different contracts

| Transport | What it provides | Integration caution |
| --- | --- | --- |
| libcurl | C API, synchronous easy and event-driven multi interfaces, TLS/proxy/HTTP/2 depending on build | Own and release CURL easy/multi/share handles and header lists; check callback userdata lifetime, error codes, version features, CPU/event integration |
| cpr | High-level C++ wrapper over libcurl, request/session options and asynchronous APIs depending on version | Match the supported cpr C++ standard and its bundled/system libcurl; do not infer that a generic Retry-After/jitter middleware exists |
| Boost.Beast / Asio | HTTP/network primitives including asynchronous flows | More explicit protocol/stream/lifetime mechanics; not an automatic REST client or a ready-made retry policy |
| POCO Net | HTTP session/request/response abstractions | Check actual SSL, pool, async mode and selected POCO version |
| cpp-httplib | Convenient HTTP/1.1 client/server, typically synchronous operations | Avoid blocking Qt UI thread; do not claim HTTP/2 support by inference |

A project already using Qt Network should normally keep it; see `qt-http-client`. Adding libcurl/cpr to a Qt program just to support SSE or HTTP/2 is not justified without requirements.

## Review state and resources

- Identify who constructs, shares and closes sessions, native handles, callback contexts, connection pools, socket/event resources, uploaded data, response streams and scheduler threads.
- `libcurl` easy transfers are blocking; the multi interface drives concurrent transfers via event polling and requires correct cleanup. Not all cpr APIs have the same cancellation/reuse semantics. Do not wrap a blocking API in the GUI event loop accidentally.
- Keep request headers and bodies valid until the transport has finished consuming them. In callback APIs track whether received bytes are borrowed during the callback or retained by the client; copy only when needed.
- Distinguish HTTP status from transport error: a server may return an HTTP 404/503 with successful transport completion. Response size limits and streaming read limits are application contracts, not assumptions from a library's default.
- Link/runtime TLS backend, ALPN/HTTP2 support and certificate storage may differ across target OS and build; verify both compile/link and deployed runtime before claiming a protocol is available.
- Safe shutdown means no callback can refer to destroyed application/plugin code, released session memory, unloaded modules, cancelled operations or thread-affine objects.

## Good / bad

**Bad:** allocate a fresh libcurl easy/multi subsystem for every UI repaint or leave a callback pointing at a destroyed Qt dialog.

**Better:** own a transport per appropriate integration lifetime, preserve callback contexts, perform clean cancel/drain at shutdown and avoid blocking the UI thread.

**Bad:** assume cpr's higher-level API automatically supplies bounded retries or that a curl CLI `--retry` option applies to libcurl API calls.

**Better:** check the installed library's actual API and implement only the selected operation-level retry policy.

See `official-sources.md` for version-matched transport sources.
