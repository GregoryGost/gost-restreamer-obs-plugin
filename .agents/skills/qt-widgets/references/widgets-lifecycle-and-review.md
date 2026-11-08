# Widgets lifecycle, dialogs, events and layout

- Verify QApplication lifetime precedes all GUI objects, and window/dialog ownership survives the event loop while needed.
- Parent-child destruction, Qt layouts and QObjects are interconnected. A QWidget added to a layout may be reparented; do not treat parent as static without inspecting the API.
- Modal versus modeless dialogs, WA_DeleteOnClose, closing events and deleteLater have different lifetime consequences; confirm if the caller keeps a pointer afterward.
- GUI thread rule: widget and GUI-facing model mutations belong on GUI thread; worker-to-widget updates should cross through queued events/signals with safe context.
- With .ui forms, CMake AUTOUIC generates ui_*.h files; edit the canonical .ui and relevant owning source, not generated files.
- Event handlers: distinguish paintEvent, resizeEvent, keyPressEvent, eventFilter and default propagation; avoid repeated heavy work in hot paths without evidence.
- Layout and DPI: check sizeHint, minimumSize, margins, font scaling, translations and platform/window behavior when the requested change affects them.
- Accessibility and keyboard focus are real functional requirements for interactive UI, not optional style-only facts to disregard, but don't add unrelated UI features.

## Small review matrix

Check creation → show → user interaction → close → delete for dialog lifetime, including cancellation and repeated opens. Test focus/keyboard as well as mouse when behavior requires it. For a model/view change, use model tester plus targeted QTest assertions.
