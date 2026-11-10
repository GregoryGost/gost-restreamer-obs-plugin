# Outgoing HTTP security, timeouts, redirects and bounded responses

## Connection and origin boundaries

Resolve the actual configured upstream origins, trust model and credential owner. When a user or remote payload influences URLs, validate scheme, origin, target-address range, DNS rebinding risks and redirect destination according to the application threat model. Do not make broad live requests to discover behavior.

- Never disable certificate/hostname verification to fix a test. Respect the project's existing CA roots, TLS versions, proxy/mTLS and client-cert policies.
- Redirects can change method and body semantics, leak credentials or cause SSRF. Review 301/302/303 versus 307/308 behavior in the chosen client. Check every redirect target before forwarding sensitive headers when trust boundaries matter.
- Classify 4xx/5xx HTTP response as HTTP outcomes, distinct from TLS/connect/read failures, parse/validation failures and cancellation. Restrict error body size to a configured maximum.
- Authentication headers, cookies and body logging follow the **project's explicitly approved application logging policy**. Do not substitute a blanket new mask/record rule. Agent/PR/CI outputs and external telemetry need their own disclosure authorization.
- A connection-pool timeout is not an upstream response timeout. A per-attempt connect timeout, inactivity/read timeout, absolute operation deadline, and SSE liveness/heartbeat deadline are not interchangeable.
- Bound response bytes, decompressed size, line/event accumulation, queue length and redirects as appropriate to real response contracts. Choose observed values or project policy; do not invent universal limits for every client or buffer.
- For streaming, account for gradual bytes arriving forever: an inactivity timeout alone cannot bound elapsed total operation time. SSE may intentionally have no finite total connection duration, in which case bound idle/reconnect/resource budgets instead.

## Practical review

1. Identify which user-controlled values can influence request destinations.
2. Trace redirect graph and credential forwarding, selected proxy/TLS and certificate errors.
3. Verify timeout categories with real library APIs and selected version; mark unavailable categories not supported by library rather than fabricating them.
4. Ensure cancellation releases handles/replies, ends scheduled retries and prevents callbacks after shutdown.
5. Exercise failures with isolated local servers and approved fixture credentials; do not point tests at production or log real secrets.
