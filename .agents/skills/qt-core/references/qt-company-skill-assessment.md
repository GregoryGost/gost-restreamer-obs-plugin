# The Qt Company's agent skills — reviewed and selectively adopted

Primary upstream: https://github.com/TheQtCompanyRnD/agent-skills (Qt Company Ltd; BSD-3-Clause alternative; read per-file license). Reviewed skill entrypoints and relevant checklists: qt-cpp-review (agent-rules, qt-review-checklist, qt-framework-checklist, qt-deprecated-classes), qt-cmake-project (common-mistakes), qt-qml, qt-qml-review, qt-qml-test and qt-qml-test-run.

## Ideas explicitly incorporated

- qt-cpp-review A1..A6 domains: model contracts, ownership, threading, API/C++ correctness, error paths, performance. These are applied through qt-core/qt-widgets and ordinary code_reviewer, not a second independent fan-out topology.
- qt-cmake-project: targeted checks for target-based dependencies, AUTOMOC/AUTOUIC, versionless Qt 6 functions, QML URIs/resources and versioned policies. See cpp-cmake/references/qt-cmake-and-review.md.
- qt-qml: scrutinize declarative bindings, Loader and delegate lifetimes, alias/property typing, layouts, states and JS interop. See qt-quick-qml references.
- qt-qml-test / qt-qml-test-run: real TestCase/SignalSpy/waits and bounded runner wiring with explicit authorization. See qt-testing references.

## What is NOT automatically adopted

- Upstream qt-cpp-review spawns its own shard/review-agent workflow and tells the reader its deterministic linter is authoritative. Our code_reviewer, test_engineer and primary must follow subagent_coordination.md, current request routing and independent evidence; lint findings require real verification.
- Upstream confidence thresholds, line-count agent fan-out and auto-generated reports are optional review-method choices, not replacements for our existing validation gate.
- Upstream qt-cmake-project suggests Qt 6.8 minimum and automatic qt_standard_project_setup; Qt 6.2-6.7, legacy valid targets and existing build policy need separate treatment. Qt's command arrived in Qt 6.3 and Qt policies evolved later; version matters.
- Upstream qt-cpp-review deprecation advice (for example universal QMutex over std::mutex, blanket Q_DISABLE_COPY_MOVE, raw QObject ownership heuristics) may reflect upstream Qt implementation conventions, not a universal requirement for third-party Qt applications.
- Upstream QML preview, test runner harness generation and self-install flows can mutate local/editor/project state. They are opt-in and not implied by selecting Qt.
- Upstream docs/writing conventions and framework internals review are separate from application ownership; there is no new Qt-specific subagent and no Qt Creator assumption.

## Reuse and licensing

These files paraphrase and adapt ideas and link to original materials; no upstream script, source file or checklist is copied verbatim into this repository. If future work vendors an unchanged or modified upstream skill/script, preserve its original BSD-3-Clause notice (and assess any dual-license terms) and review it against the repository's scope/delegation policy. Do not silently download or install it.
