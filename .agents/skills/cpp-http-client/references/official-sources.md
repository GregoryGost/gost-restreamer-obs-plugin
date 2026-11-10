# C++ HTTP and SSE primary sources

## Precedence

1. `CODEX_PROJECT.md`, chosen transport's actual version/build features, target API documentation and client code/tests.
2. Standards for HTTP, SSE and TLS semantics.
3. Versioned official documentation for the selected library.
4. Examples and community snippets only as leads; never assume options exist across library versions.

## Sources

- HTTP Semantics (RFC 9110): https://www.rfc-editor.org/rfc/rfc9110
- HTTP/1.1 (RFC 9112): https://www.rfc-editor.org/rfc/rfc9112
- HTTP/2 (RFC 9113): https://www.rfc-editor.org/rfc/rfc9113
- EventSource/SSE normative spec: https://html.spec.whatwg.org/multipage/server-sent-events.html
- libcurl overview and programming guide: https://curl.se/libcurl/c/libcurl-tutorial.html
- libcurl multi interface: https://curl.se/libcurl/c/libcurl-multi.html
- libcurl HTTP version selection: https://curl.se/libcurl/c/CURLOPT_HTTP_VERSION.html
- libcurl observed HTTP version: https://curl.se/libcurl/c/CURLINFO_HTTP_VERSION.html
- libcurl runtime features: https://curl.se/libcurl/c/curl_version_info.html
- cpr official repository/docs: https://github.com/libcpr/cpr ; https://docs.libcpr.dev/
- Boost.Beast overview: https://www.boost.org/doc/libs/release/libs/beast/
- POCO Net: https://docs.pocoproject.org/current/group__Net.html
- cpp-httplib: https://github.com/yhirose/cpp-httplib

These are alternatives, not a shopping list or implicit library dependencies.
