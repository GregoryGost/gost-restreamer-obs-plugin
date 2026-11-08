---
name: qt-quick-qml
description: "Use for Qt 6 QML/Qt Quick binding semantics, QML modules, C++/QML ownership, layouts, performance, linting and optional QML tests."
---

# Qt Quick / QML

Independent Qt 6 UI overlay; do not activate for Qt Core or Qt Widgets projects lacking QML.

## Required Dependencies

- qt-core
- .codex/rules/qt_core.md

Use qt_quick_qml.md only if independently active. Qt Testing is optional coordination for QML tests, not an implicit requirement.

## Workflow

1. Confirm exact Qt 6 minor, selected Quick/Controls modules, compiled QML module URIs, import paths, QML files/resources, QML language tooling and test/build command.
2. Review real declarative data flow: typed properties, binding evaluation, alias targets, imperative assignments that break bindings, states, delegates, JavaScript, Loader creation/destruction and translations.
3. Verify QML module resource prefix, URI and imports against version-specific qt_add_qml_module policy and actual CMake target.
4. For QML/C++ exposure, verify QObject/metaobject registration, NOTIFY signals, thread affinity, ownership, context properties and lifetime of referenced objects.
5. Prefer existing declarative components and targeted fixes over speculative QML architecture changes. Don't introduce async loaders, custom singletons, extra files or new QML modules without requirement.
6. Use available, configured qmllint/qmlformat/qmlls and build/test commands. QML Preview MCP, Qt Documentation MCP and profilers remain optional and require actual configured connection, tool-specific version prerequisites (for the upstream live QML Preview MCP: Qt 6.12+) and authorized execution. This prerequisite is not a minimum Qt version for ordinary QML projects.
7. Validate runtime behavior with real environment where necessary. Report analyzer/tool version, scope, bindings and results distinctly from C++ compilation.

## References

- references/bindings-delegates-and-layouts.md
- references/qml-cmake-and-cpp-integration.md
- references/validation-and-review.md
- references/official-sources.md
- qt-testing/references/qml-and-gui-test-boundaries.md (only if qt-testing active)

The Qt Company's qt-qml, qt-qml-review and qt-qml-test materials are curated external references, not automatically imported instruction or subagent protocols. See qt-core/references/qt-company-skill-assessment.md.
