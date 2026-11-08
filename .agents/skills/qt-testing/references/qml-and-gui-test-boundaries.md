# Qt GUI and Qt Quick Test boundaries

## Qt Widgets

A real QWidget interaction requires QApplication and a suitable GUI platform plugin/display context. Offscreen/headless may be useful for logic but cannot prove window-manager, compositor, focus/IME or actual visual behavior without an appropriate environment. Avoid using a user's live GUI session for tests by default.

## Qt Quick

The Qt Company's qt-qml-test materials provide useful patterns: TestCase, SignalSpy, tryCompare, mouse/key helpers, typed QML properties and test fixture parenting. The runner skill separates writing tests from optional CMake harness generation and execution.

- A TestCase with visible: false may be an invalid default parent for pointer-interactive test items. Ensure tested items have suitable visual hierarchy and event delivery.
- Delegate recycling and dynamic Loader cases need tests for creation, reuse and destruction, not just initial property values.
- For signal tests, check the QML signal actually exists and is exposed; verify payload and count after event handling.
- qmltestrunner and CTest wiring depend on project Qt version, module URI, import paths and selected test binary. Do not invent new module/test scaffolding simply to make the runner work.
- A missing display server/platform plugin may BLOCK a GUI test; report this explicitly and don't claim success from simply parsing the QML.

## Execution

Use the target project's established test command, package paths and approved fixture effects. Do not mutate CMake or install a new runner to perform a read-only test request. If extra wiring is necessary, propose minimal changes for explicit approval.
