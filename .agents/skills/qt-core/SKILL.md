---
name: qt-core
description: "Use for Qt 6 C++ QObject ownership, signals and slots, event loops, QThread, meta-object APIs, Qt CMake integration and Qt-specific review."
---

# Qt 6 Core

Independent Qt 6 overlay on cpp-core. It is not an OBS-specific, Qt Creator or Qt Quick mandatory profile.

## Required Dependencies

- cpp-core
- .codex/rules/cpp_core.md

Use qt_core.md only when independently active; the skill does not automatically activate Qt Widgets, QML or extra tooling.

## Workflow

1. Verify active Qt major/minor/patch from actual SDK/CMake/CI and loaded Qt components, alongside effective C++ standard and target ABI.
2. Read the involved QObject types, parent/child relationships, connections, callbacks, event loops, thread ownership, callers and real lifetime across shutdown/errors.
3. Identify authoritatively owned, borrowed, guarded and JavaScript-owned objects rather than inferring ownership from raw pointer syntax. Check QObject parent transfer and deleteLater dispatch.
4. Validate signal delivery, connection context, receiver destruction, queued argument types and thread affinity. A QThread instance is usually owned by its creating thread, not the worker execution thread.
5. Preserve Q_OBJECT, Q_PROPERTY, invokables, meta-types, translations and generated header/moc contracts.
6. Apply optional Qt-oriented Clazy and chosen language-server diagnostics only where installed and project-selected; investigate results rather than treating regex/lint output as conclusive.
7. Test with appropriate Qt Test / Qt Widgets / QML overlay only when independently active; report missing mandatory gates.

## Reference routing

| Concern | Reference |
| --- | --- |
| QObject ownership, QPointer, deleteLater and signal/slot lifetime | references/qobject-lifecycle.md |
| QThread, event loop, signals/slots and UI thread | references/threads-and-events.md |
| Meta-object, Qt types, version-specific API, CMake integration and Clazy | references/metaobject-api-and-tooling.md |
| Upstream Qt Company skills: reusable material, portability and conflict handling | references/qt-company-skill-assessment.md |
| Review oracle and version-aware authoritative sources | references/official-sources.md |

Official The Qt Company qt-cpp-review and qt-cmake-project provide useful structured checks, but their automatic subagent fan-out, fixed thresholds, forced upgrades and 'linter is authoritative' statements do not override this repository's request routing, independent subagents, scope and evidence policy. Do not automatically install or vendor upstream packages. Follow verified Qt API contracts and the target's existing configuration.
