# Qt Widgets rules

Activate only through the exact qt_widgets.md Active Rules entry, qt-widgets Active Stack Profiles entry, or a Qt Widgets profile naming qt_widgets.md + qt-widgets.

## Required skills

- qt-core
- qt-widgets

Required base rule: .codex/rules/qt_core.md

## Constraints

- Inspect active Qt 6 version, widgets/forms/layout implementation, model/view data contract, event/painting and UI thread restrictions, translation and .ui AUTOUIC generation.
- QWidget and GUI-facing QAbstractItemModel access normally belongs on the GUI thread. Never invoke widget mutators from worker threads; use appropriate queued handoff and safe receiver/context lifetime.
- Verify parenting, layout ownership, modal/modeless dialog lifetime, connections and shutdown; do not blindly assign new parents, call deleteLater or convert owner types without tracing ownership.
- For QAbstractItemModel verify index parent/row/column, data/roleNames, setData/dataChanged, begin/end insert/remove/reset, persistent indexes and any proxy model mapping. A working view screenshot does not prove model contract correctness.
- Keep rendering, event handling, input, DPI, accessibility and translation behavior version-/platform-aware. Avoid premature GUI architecture, automatic Qt Quick conversion or extra UI features.
- Test UI-sensitive behavior using the separately active qt-testing / cpp-testing profiles and project-declared headless/display/CI policy. A Qt Widgets application does not imply QML activation.

Consult qt-widgets references for model/view/event-loop review.
