# SSE over Qt Network — incremental parser, reconnect and shutdown

Qt Network already transports `text/event-stream` over HTTP/1.1 or negotiated HTTP/2. No additional SSE library is required or selected by this skill. This is an **implementation review specification**, not an instruction to create an SSE subsystem for every Qt application.

**Read first:** `.agents/skills/cpp-http-client/references/sse-protocol.md` for normative parsing of bytes, UTF-8/BOM, CR/LF/CRLF, empty data and id-only blocks. Using raw network fragments as complete events violates the SSE wire format.

## One subscription lifecycle

1. Create a `QNetworkRequest` for a verified SSE endpoint, normally with `Accept: text/event-stream` and configured auth/TLS/redirect policy. On reconnect send `Last-Event-ID` only if the cursor is nonempty; do not synthesize IDs.
2. Use the existing long-lived `QNetworkAccessManager` in its owning thread and `get()` a `QNetworkReply`. Do not synchronously wait or add a new network stack.
3. Once response metadata is available, validate HTTP status and compatible `Content-Type`; handle 200 SSE, **204 terminal stop**, auth/non-SSE responses and redirects under the target's contract. Separate HTTP errors from network failure.
4. On each `readyRead()`, read available bytes incrementally into a **bounded** streaming parser. Preserve line/UTF-8/CRLF state across reads. Parse and emit complete events only at blank-line boundaries; an incomplete trailing event at EOF is **not** dispatched.
5. Maintain a persistent last-event-ID cursor independently of event data. `id:` with empty value clears it, id-only events can update it, and invalid NUL-containing IDs are ignored. Deduplication/gap recovery is **not** guaranteed by sending the cursor; delegate it to the selected upstream/domain contract.
6. Accept server `retry:` only for ASCII decimal digits, with integer-overflow and maximum-wait protection. Use a `QTimer` scheduled in the subscription owner's event loop, never blocking sleep. The project decides bounded backoff/jitter where applicable; do not override a valid server delay arbitrarily.
7. On stream `finished()` or `errorOccurred()`, consolidate the terminal result **once**. Read remaining buffered bytes where appropriate, release the reply via `deleteLater()` (or the already configured auto-delete policy), then schedule **at most one** permitted reconnect. Avoid double reconnect if both signals fire.
8. On HTTP 204, explicit stop, unauthorized fatal condition or closing plugin, mark state terminal, stop timers, abort request, clear guarded callbacks and prevent any future reconnect or callbacks after QObject/plugin destruction.
9. Bound accumulated line/event bytes and pending consumer messages. Slow Qt GUI consumers must not cause unbounded memory growth. Read buffering and backpressure behavior should follow actual traffic/GUI requirements; don't invent a new queue if no queue is needed.

## Specific failure hypotheses

- `QNetworkReply::readyRead()` returns only half a UTF-8 sequence or a CR without LF: keep decoder/line state until the rest arrives.
- First signal `errorOccurred()` and later `finished()`: no duplicate timer or reconnect.
- `stop()` aborts the reply: later finished callback must not reconnect.
- 204 on reconnect after a successful stream: stop, even if the Qt error classification reports non-success.
- Server sends `id:\n\n` without data: clear cursor; next reconnect omits header.
- Server sends `data:\n\n`: dispatch an empty-message event.
- An HTTP/2 stream is closed while other requests share the same connection: do not assume entire TCP session is gone.
- Event consumer is destroyed during a pending QTimer callback: no use-after-free.

## Test strategy

Use a controlled Qt Test fixture with QCoreApplication/QGuiApplication as required, a disposable loopback server and deterministic event-loop waits. Pure parser tests should feed every byte split; integration tests must separately exercise real `readyRead/finished/errorOccurred`, 204, lost connection, no duplicate reconnect, stop during backoff and resource cleanup. HTTP/2 requires a true capable server plus reply-attribute verification; a parser test alone proves no HTTP/2 transport behavior.

Official: https://doc.qt.io/qt-6/qnetworkreply.html ; https://doc.qt.io/qt-6/qnetworkaccessmanager.html ; https://html.spec.whatwg.org/multipage/server-sent-events.html
