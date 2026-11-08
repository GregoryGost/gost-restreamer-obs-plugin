# QML diagnostics and review

## Operational validation

Use only declared commands and already configured tools from CODEX_PROJECT.md. No initial scanning or installation process is prescribed here.

- qmllint: static QML syntax/type/binding checks; ensure configured import paths, generated qmldir, .qmltypes files and exact Qt version. A missing import because build outputs are absent is a tooling/configuration issue until verified otherwise.
- qmlls: language server diagnostics/navigation; VS Code may have unsaved buffers and state an agent CLI invocation cannot see. Do not claim parity with Problems without verified access.
- qmlformat: format only authorized files/ranges using the project config/version; no broad text churn.
- QML Test: run actual TestCase tests via project-approved qmltestrunner/CTest, with real imports and GUI environment where needed. A parsed-only QML file is not a passing UI test.
- QML Profiler: optional for measured performance regressions, not mandatory for ordinary changes.
- Qt QML Preview MCP: optional already connected tool. When this specific preview server is selected, verify its documented Qt 6.12+ runtime prerequisite against the actual SDK before using it; never impose Qt 6.12+ on ordinary QML development. Do not start a preview session without authorization.

## Review questions from the Qt Company

- Is a bound property overwritten by imperative assignment?
- Does Loader.item access account for null/loading?
- Does a delegate retain recycled stale state?
- Do anchors and Layout own the same geometry?
- Can a Connections callback outlive its target?
- Do import paths/URI/resource prefix and compiled type registration match the built target?
- Are font/translation, accessibility, control style and screen scaling behaviors required by the change?
- Are test assertions based on observed behavior rather than internal QML line-by-line structure?

Evidence must include actual component/module context; upstream regex linter output is not definitive. Recheck after edits and report missing test environment separately from static analysis.
