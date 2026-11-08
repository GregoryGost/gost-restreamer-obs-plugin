# Qt 6 core rules

Activate only through the exact qt_core.md Active Rules entry, qt-core Active Stack Profiles entry, or an enabled Qt Core profile naming qt_core.md + qt-core. This is a Qt 6 overlay, not an OBS or Qt Creator profile.

## Required skills

- cpp-core
- qt-core

Required base rule: .codex/rules/cpp_core.md

## Source of truth

- Resolve exact Qt 6 version and modules from target CMake, configured SDK, generated metadata, source and CI. OBS Studio 33.0.1 is a C++17/C17 compatibility reference only, not an application target and not a universal Qt patch/version requirement.
- Read actual QObject/QMetaObject contracts, ownership, signal/slot and thread/event-loop behavior across complete relevant callers. Check Qt minor-version availability; do not treat a newer documentation page as proof for an older target.
- Preserve meta-object and binary contracts: Q_OBJECT/Q_GADGET, Q_PROPERTY, signals, slots, Q_INVOKABLE, translation, C++/QML exposure, generated moc interfaces and exported symbols.
- Treat parent-child ownership and RAII as intersecting systems; no blanket prohibition on raw QObject pointers and no automatic smart-pointer conversion. Verify owner, parent transfers, deletion order, QPointer/observer use and deferred delete timing.
- Verify QObject thread affinity, event-loop availability, connection delivery (auto/direct/queued), worker lifetime and UI-thread restrictions. QThread object affinity differs from the thread it manages; running a signal callback on the wrong thread is a correctness defect.
- Qt QString/QStringView, QByteArray, implicit sharing, QDataStream and container conversions have version- and lifetime-specific semantics. Do not infer zero-copy or safe pointer lifetime by type name alone.
- Use project-owned CMake Qt targets, autogen tooling, Clazy and compiler/analyzer configurations only when selected and available. Never generate moc/uic/rcc outputs by hand or rewrite an existing build absent a requirement.
- Clazy, official Qt AI lint scripts and third-party heuristics are evidence sources, not authority to apply fixes blindly. Confirm each relevant result against full code and the active Qt version.
- Preserve existing log/privacy policy and bounded resource/event lifetime. No Qt SDK/IDE/plugin installation, Qt Creator workflow, debugger attach or extra runtime feature without authorization.

## Review and validation

Use qt-core references for ownership/threading/meta-object APIs and official Qt-source precedence. Route Widgets, Qt Test and QML details to independently active profiles. Validate chosen final build/diagnostics/test snapshot; report unavailable required checks as NOT VERIFIED.
