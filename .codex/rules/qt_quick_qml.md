# Qt Quick / QML rules

Activate only through the exact qt_quick_qml.md Active Rules entry, qt-quick-qml Active Stack Profiles entry, or a Qt Quick/QML profile naming qt_quick_qml.md + qt-quick-qml. Do NOT activate for Qt Widgets alone.

## Required skills

- qt-core
- qt-quick-qml

Required base rule: .codex/rules/qt_core.md

## Constraints

- Resolve the exact Qt 6 minor version, QML modules, import paths, CMake targets, Qt Quick Controls and runtime/platform configuration. Validate availability of QML language-server, qmllint, qmlformat, qmltestrunner and profiler only through the declared target workflow.
- Prefer declarative bindings, explicit property types and scoped imports; validate alias/property lifecycle, binding loops, JS interop, Loader asynchronous creation, delegate recycling, states/animations, layouts, accessibility and translations as relevant.
- For new Qt 6 CMake QML modules use actual version-supported qt_add_qml_module API, URI, resource and compiled type registration; never copy Qt 6.8+ examples to an earlier SDK without checking. Do not forcibly migrate working legacy module layouts without an explicit request.
- C++/QML boundary requires explicit ownership (CppOwnership/JavaScriptOwnership when applicable), thread affinity, lifetime and property notification; never expose non-thread-safe C++ state directly to arbitrary QML callbacks.
- Treat qmllint output as a diagnostic input alongside compilation, runtime checks and tests. Do not auto-silence warnings, apply bulk format or claim a QML preview tool is present because it is mentioned in official external skills.
- Qt Quick Test and qmltestrunner are opt-in through project-owned test commands and, when active, qt-testing. Never silently add test scaffolding or start a persistent preview session.

Consult qt-quick-qml references for binding/import/runtime and integration/test pitfalls.
