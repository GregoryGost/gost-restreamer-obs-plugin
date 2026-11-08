# C++ critical review checklist

## Evidence-first sequence

1. Restate requested behavior and allowed surface, then inspect the entire relevant implementation, public headers, callers, tests and build flags.
2. Identify changed API/ABI and compatibility constraints. Check if the observed symptom is caused by source or incorrect analyzer/build context.
3. Trace the actual object/resource owner, exception/error paths and any thread interactions. Try to refute each suspected issue with a valid alternative interpretation.
4. Separate mandatory defect/safety correction from an optional design improvement or unrelated refactor; the latter is outside scope without explicit approval.
5. Evaluate whether the minimal correction preserves the current contract, and whether tests will detect regression.
6. Inspect exact final compiler, language-server, static-analysis, sanitizer and test evidence. Never infer PASS from a command that did not run.

## Targeted prompts

- Can a borrowed pointer, string_view, span or iterator outlive its storage?
- Do constructors, move/copy/destructors and partial failures release exactly what they own?
- Can exceptions cross an FFI/plugin boundary or violate a declared noexcept contract?
- Did a public header or vtable change ABI, visibility or layout?
- Are platform macro branches actually tested in the supported matrix?
- Are all data accesses synchronized and is shutdown bounded?
- Did a format change rename a Qt property/signal or otherwise break reflection?
- Is a new abstraction, cache or startup self-healing logic actually demanded by a requirement?

## Output contract

Give category, file:line, failure condition, evidence, possible counterexample, smallest remedy, impact of change and checks performed. Use INCONCLUSIVE/NOT VERIFIED where diagnostics cannot reach source or runtime, not a fabricated PASS.
