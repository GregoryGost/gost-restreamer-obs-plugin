# Qt 6 meta-object, APIs and tooling

## Meta-object source boundaries

- Q_OBJECT is needed when the type uses QObject meta-object features such as declared signals/slots/properties; not every QObject subclass must automatically add it. Confirm moc participation for affected headers.
- Q_PROPERTY and Q_INVOKABLE create external reflection/QML contracts; renaming and altering NOTIFY signals can silently break consumers.
- Q_GADGET, Q_ENUM, Q_DECLARE_METATYPE and QMetaType registration have version-specific conditions. Check queued argument support and dynamic invocation against the active Qt minor.
- QString/QStringView/QByteArray have different encoding and lifetime behaviors; views into temporaries and implicit sharing detachment can invalidate assumptions. Verify API signatures.
- Prefer official Qt CMake targets and automatic moc/uic/rcc with the configured build, not ad hoc generated file edits.

## Diagnostics

- Clazy is a Qt-specific Clang checker; verify installed version/compatibility and selected check set before treating it as required. A Clazy message is a lead to read actual code, not a license to replace all Qt types with std or vice versa.
- Qt API deprecation and availability are minor-version-specific. QT_VERSION_CHECK, Qt compilation flags and source compatibility may differ across targets.
- Qt Documentation MCP (when already connected and authorized) is a good versioned API lookup; Context7 and official doc.qt.io remain alternatives. Neither tool automatically changes active profile or dependency policy.

## Cases requiring explicit compatibility

- A QML-facing property adds a getter but omits a NOTIFY contract required by bindings.
- A QObject type is moved to a worker thread while still parented.
- An existing UI form/class relies on moc/uic generated code, but a proposed CMake change omits the target's header.
- Qt type conversion or move/copy is refactored based only on generic C++ style preference, affecting an API's real ownership/lifetime.

Review actual consumers and generated output boundaries before editing.
Sources: https://doc.qt.io/qt-6/metaobjects.html ; https://doc.qt.io/qt-6/properties.html ; https://doc.qt.io/qt-6/cmake-manual.html ; https://github.com/KDE/clazy
