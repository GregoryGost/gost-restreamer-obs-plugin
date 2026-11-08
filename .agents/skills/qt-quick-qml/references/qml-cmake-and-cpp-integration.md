# QML module packaging, CMake and C++ bridge

## QML module and CMake

- Inspect Qt version, QML URI, qmldir metadata, import paths, resource prefix, build target, generated QML type registration and actual CMake packaging.
- qt_add_qml_module is the primary Qt 6 QML module CMake command from Qt 6.2; its flags, properties and policy defaults evolve by Qt minor. Use the exact target SDK API docs.
- A QML file registered via arbitrary resource code may still load at runtime but not participate in QML type tooling/compilation as intended. For a new module use the project-selected Qt module mechanism; do not automatically migrate working legacy resources.
- Qt policy QTP0001 changes default prefix and QTP0005 target-based dependency/import options arrive later; do not assume these features are present in Qt 6.2–6.7.
- Check CMake target visibility and PUBLIC headers exposing Qt types, as well as installation/deployment/import availability. Never add packaging outside authorized task scope.

## C++/QML interop

- QML_ELEMENT, QML_SINGLETON, qmlRegisterType, context properties and QQmlEngine ownership policies have different registration/lifecycle models. Preserve target's established mechanism unless change requested.
- QObject-based properties and methods require appropriate meta-object information; NOTIFY and signal arguments must match QML expectations. Expose stable value types intentionally.
- CppOwnership and JavaScriptOwnership govern whether the engine may delete an object; verify when and why an object is transferred to the engine.
- Qt GUI operations must run on the GUI thread even when triggered by worker-backed C++ signals.
- Avoid exposing a raw native plugin handle or borrowed C++ pointer to QML without an explicit lifetime plan.

Sources: https://doc.qt.io/qt-6/qt-add-qml-module.html ; https://doc.qt.io/qt-6/qtqml-cppintegration-overview.html ; https://doc.qt.io/qt-6/qqmlengine.html
