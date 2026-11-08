---
name: qt-testing
description: "Use for Qt 6 QTest, QSignalSpy, asynchronous event-loop tests, GUI/model tests and isolated Widgets/QML test validation."
---

# Qt Testing

## Required Dependencies

- cpp-testing
- qt-core
- .codex/rules/cpp_testing.md
- .codex/rules/qt_core.md

Use qt_testing.md only when independently active. The C++ testing core remains responsible for ordinary C++ runner, sanitizer and coverage policies.

## Workflow

1. Inspect selected Qt Test version, CMake registration/CTest runner, fixture application class (QCoreApplication/QGuiApplication/QApplication), platform plugins and existing test paths.
2. Design checks from original behavior: signals and payloads, queued work, deletion timing, model/view contracts, widget events and failure/shutdown paths as applicable.
3. Choose real Qt event-loop assertions (QSignalSpy, QTRY_VERIFY, QTRY_COMPARE, QTest input events and QAbstractItemModelTester) where the version supports them; do not use arbitrary sleeps as a synchronization oracle.
4. For Qt Quick tests, test QML through the real QML runtime and its project-owned qmltestrunner/CTest boundary where selected; do not generate unwarranted harnesses.
5. Isolate GUI test processes from live user application state; verify documented headless/offscreen limitations rather than treating offscreen as equivalent to a display server.
6. Run accepted narrow and broad test commands on the final snapshot and report actual GUI runtime/platform evidence, unavailable setup and skipped checks.

Read references/qt-test-patterns.md, references/qml-and-gui-test-boundaries.md and references/official-sources.md.
