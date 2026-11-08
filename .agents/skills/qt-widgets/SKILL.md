---
name: qt-widgets
description: "Use for Qt 6 Widgets GUI engineering, QAbstractItemModel/view contracts, layouts, dialogs, paint/events and safe UI-thread updates."
---

# Qt Widgets

## Required Dependencies

- qt-core
- .codex/rules/qt_core.md

Use qt_widgets.md only when independently active. Widgets does not activate Qt Quick/QML by implication.

## Workflow

1. Read the actual QWidget, forms, layout hierarchy, parent ownership, active event loop, model/view and UI consumers; resolve the Qt minor and relevant platforms.
2. Keep UI updates on the GUI thread and workers behind safe queued boundaries. Verify receiver lifetime and shutdown for asynchronous operations.
3. On layout/dialog changes preserve ownership, event semantics, show/hide/close, modality and focus. Avoid unasked GUI restructuring.
4. For QAbstractItemModel and proxies check index validity, parent/child shape, data/roles, flags, setData, structural begin/end notifications, source/proxy index mapping, persistent indexes and reset behavior.
5. For view delegates and paint/event handlers verify size, DPI, event propagation and performance against concrete behavioral requirements.
6. Select an authorized Qt Test / GUI integration boundary with qt-testing only when active; do not claim a headless CLI test reflects a real-display interaction without appropriate evidence.

Read references/model-view-contracts.md and references/widgets-lifecycle-and-review.md; use qt-core references for QObject and event-loop foundations and references/official-sources.md when Qt version matters.
