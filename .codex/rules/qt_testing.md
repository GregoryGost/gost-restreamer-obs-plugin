# Qt testing rules

Activate only through the exact qt_testing.md Active Rules entry, qt-testing Active Stack Profiles entry, or a Qt Testing profile naming qt_testing.md + qt-testing.

## Required skills

- cpp-testing
- qt-core
- qt-testing

Required base rules:
- .codex/rules/cpp_testing.md
- .codex/rules/qt_core.md

## Test boundaries

- Use Qt Test (QTest), QSignalSpy and project-selected CTest or binary test runner for real Qt semantics; pure C++ behavior stays in the base cpp-testing profile where possible.
- Include parent/deletion ownership, signal emission/order, queued versus direct delivery, thread affinity, GUI event dispatch, model/view notification rules and asynchronous completion where affected by the task.
- Do not use blind event sleeps or assume QCoreApplication, QGuiApplication and QApplication are interchangeable. Use a suitable fixture and bounded QTRY_* / event-loop driven waiting where supported.
- Isolate tests from a user's live GUI/application session and platform state. For GUI display/server or Qt platform plugin requirements, use project-declared CI/headless setup; no automatic environment or display configuration changes.
- QSignalSpy validity and signal overload identity matter; count alone does not prove signal arguments or state transitions. Verify registered metatypes for queued calls when necessary.
- Keep teardown of QObjects, connections, timers and worker threads deterministic, including deferred deletion when the event loop is involved. Avoid shared singleton state and fixed ports.
- When Qt Quick is independently active, use the suitable QML test boundary; do not invent QML modules or rewrite test wiring without authorization.
- Report commands and exact Qt/platform/config snapshot; a missing Qt Test executable or GUI environment blocks that scope rather than becoming a PASS.

See qt-testing references for QtTest patterns, model testers, GUI and QML test boundaries.
