# C++ style rules

Activate only through the exact cpp_style.md Active Rules entry, cpp-style Active Stack Profiles entry, or an enabled C++ Style profile naming the cpp_style.md + cpp-style pair.

## Required skills

- cpp-core
- cpp-style

Required base rule: .codex/rules/cpp_core.md

## Policy

1. Read existing .clang-format, .editorconfig, .clang-tidy (for relevant stylistic diagnostics), repository conventions and CODEX_PROJECT.md. Precedence: local scoped config > approved project policy > consistently established code > optional portable examples.
2. Apply the configured formatter version and check scope only when the task authorizes edits; otherwise inspect/report. For touched lines, distinguish the formatter's actual output from human style preference.
3. Do not impose Google, LLVM, Microsoft, Qt or OBS coding style universally. Their public guides are reference material; target-specific config wins unless it violates a verified contract.
4. Do not reformat unrelated files or generated, vendored or third-party sources; preserve encoding, line endings, public names, macros and C/C++ header conventions.
5. Style or naming refactors must not silently change ABI, serialized or meta-object names, QObject signals/slots/properties, QML-exposed identifiers, plugin entry points or reflection contracts.
6. A stylistic analyzer hint does not justify suppressions, project-wide changes, new rules or extra tools. Report unsupported, missing or failing formatter checks rather than claiming format validation.

See cpp-style references for examples and the version-aware review checklist.
