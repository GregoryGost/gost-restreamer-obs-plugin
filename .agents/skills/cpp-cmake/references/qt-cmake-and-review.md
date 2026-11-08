# Qt 6 CMake — targeted checks derived from official Qt guidance

Use only when Qt is independently active. The Qt Company qt-cmake-project SKILL.md and common-mistakes.md are valuable sources of *questions to check*, not a mandate to migrate every existing build.

## Guard against common incorrect generation

- Verify actual Qt6 components through find_package(Qt6 ... COMPONENTS Core Widgets Quick ...), linked Qt6:: / Qt:: targets and target name. Do not mix qt5_* macros into a Qt 6-only target.
- qt_standard_project_setup exists from Qt 6.3, commonly before target definition for typical applications. It enables AUTOMOC/AUTOUIC defaults, not AUTORCC or language standard. Some existing projects set these properties intentionally; don't force the helper into an unrelated subproject.
- qt_add_executable, qt_add_library, qt_add_plugin, qt_add_resources and qt_add_qml_module have different semantics; use the command matching the existing target and intended outputs, not a blanket 'always use Qt helper' rule.
- Check AUTOMOC/AUTOUIC/AUTORCC, Q_OBJECT in headers and moc generated artifacts. Do not list generated moc_*.cpp, ui_*.h, qrc_*.cpp as hand-maintained source unless the existing build explicitly requires it.
- CMake targets and their link visibility must match public Qt types in headers. Avoid needless PUBLIC propagation of Widgets/Quick internals.
- For QML modules use qt_add_qml_module and correct URI/resources/import path when available; Qt policies such as QTP0001 and QTP0005 depend on Qt version. Do not assume Qt 6.8 features in Qt 6.2–6.7.
- CMake install/deploy handling is not automatic permission to add packaging or deployment tasks.

## Evidence before enforcement

The upstream skill recommends an explicit minimum Qt 6.8, unconditional qt_standard_project_setup, and aggressive migration of suspected defects. Those are useful defaults for *new Qt 6.8+ projects*, not universal facts. The target SDK and project scope govern changes; verify exact docs, failure mode and smallest remedy.

Upstream: https://github.com/TheQtCompanyRnD/agent-skills/tree/main/skills/qt-cmake-project
Qt docs: https://doc.qt.io/qt-6/qt-standard-project-setup.html ; https://doc.qt.io/qt-6/qt-add-qml-module.html
