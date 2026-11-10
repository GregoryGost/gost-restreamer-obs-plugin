# Server-Sent Events (SSE) — protocol contract for C++ clients

Use this reference **only** for a confirmed SSE endpoint. No separate SSE library is assumed or installed. This is a transport-neutral protocol reference; Qt's `QNetworkReply` wiring belongs to `.agents/skills/qt-http-client/references/sse-streaming.md`.

Authoritative specification: https://html.spec.whatwg.org/multipage/server-sent-events.html

## Event-stream framing (not HTTP chunk framing)

- A server streams `text/event-stream` UTF-8 text over ordinary HTTP/1.1 or HTTP/2. Received read chunks, TCP packets and HTTP/2 DATA frames can split or combine SSE lines/events arbitrarily.
- The parser must persist its **byte/UTF-8 decoder, line state and event buffers across reads**. Recognize CRLF (including CR and LF split between reads), lone LF and lone CR. Ignore at most one leading UTF-8 BOM. Follow UTF-8 replacement behavior for invalid sequences as required by the contract.
- Empty line dispatches an accumulated SSE event; a line beginning with `:` is a comment/heartbeat. Parse field at the first colon; strip **one** optional leading space after colon, not all spaces. Unknown fields are ignored and names are case-sensitive.
- `data:` appends the value and a newline. For multiple data lines, combine with newline; on dispatch remove **one** final newline. `event:` selects the event type, with empty meaning the default `message`.
- `id:` sets the persistent last-event-ID buffer unless the value contains a NUL; an **empty id resets the cursor**. An id-only block can update the cursor even if no data event is dispatched. `Last-Event-ID` is sent on reconnect **only when nonempty**. When serializing the header, use the cursor's UTF-8 bytes and reject invalid CR/LF/NUL bytes rather than injecting or normalizing arbitrary field names; a cursor is opaque text, not necessarily an integer.
- `retry:` with only ASCII digits updates the reconnect time; malformed/non-digit fields are ignored. Do not crash, overflow integers or schedule unbounded delays; apply the project's maximum and budget policy.
- A block with no `data` is not an application event. A `data` field with an empty value **does** dispatch an event whose data is empty. An EOF without a terminating blank line must not implicitly dispatch a partial event.
- Do not parse a JSON object as an SSE event just because a `data` field may contain JSON; JSON parsing is a separate application contract.

## Subscription and reconnect semantics

- Accept a valid SSE response (normally HTTP 200 and compatible `text/event-stream` content type). Unknown status/media types, invalid redirects or auth failures must be classified and handled according to the selected server contract.
- HTTP **204 No Content** stops reconnection. An explicit user close/shutdown also stops. Other disconnects may trigger a policy-approved reconnect with `Last-Event-ID` and bounded wait. Do not automatically reconnect endlessly to an unauthorized or invalid endpoint.
- `Last-Event-ID` is a **resume hint**; server retention, deduplication, ordering and gap handling are domain contracts, not guarantees provided by SSE. An event ID is not necessarily an integer.
- Server `retry:` is a reconnect delay, not an instruction to retry a previously committed HTTP POST. Combine it with optional bounded backoff/jitter only as the subscribed endpoint policy allows.
- Heartbeat comments may keep a connection active without application events. Distinguish transfer inactivity from no application message. Use bounded per-stream parser memory and consumer queue/backpressure handling.
- For a large continuous stream choose explicit maximum line, event and queued message sizes and a clear error/reconnect/disconnect policy; values belong to the target project, not this reusable skill.

## Test corpus

Feed the same valid stream in different arbitrary byte splits, including between UTF-8 code-unit bytes, CR and LF, colons, and two blank-line terminators. Check: BOM, blank `data`, multiline data, comments, unknown fields, empty ID reset, id-only event, NUL ID ignored, malformed `retry:`, oversized lines, EOF without blank, HTTP 204 no reconnect, cursor resume, reconnect and duplicate/gap policy. Compare emitted event sequence to the standard.

## Review gotchas

Bad: `split("\n\n")` on each `readyRead` and `QString::fromUtf8` per arbitrary chunk, dropping boundary bytes.

Good: maintain a bounded streaming state machine and process complete lines/events only, carrying decoder and CRLF state across reads.

Bad: update cursor only when `data` was nonempty; violates id-only / empty-id reset behavior.

Good: maintain cursor independently of whether a dispatchable data event occurs.
