# Qt 6 authoritative sources and precedence

1. Target's actual Qt SDK version and build/CI, project-owned selected modules and documented host compatibility.
2. Exact Qt 6 minor/patch API docs and source behavior. qt-6/ is often newest and may describe features unavailable in older 6.x.
3. Applicable Qt Company agent skills for useful review patterns, never a replacement for the two sources above.
4. Third-party articles or linters as corroboration, not sole proof.

- Qt versions / docs: https://doc.qt.io/qt-6/
- QObject: https://doc.qt.io/qt-6/qobject.html
- Object trees / ownership: https://doc.qt.io/qt-6/objecttrees.html
- Threading: https://doc.qt.io/qt-6/threads-qobject.html
- QThread: https://doc.qt.io/qt-6/qthread.html
- Signals and slots: https://doc.qt.io/qt-6/signalsandslots.html
- Properties / QMetaObject: https://doc.qt.io/qt-6/properties.html ; https://doc.qt.io/qt-6/metaobjects.html
- Qt CMake manual: https://doc.qt.io/qt-6/cmake-manual.html
- Qt Test: https://doc.qt.io/qt-6/qttest-index.html
- Clazy: https://github.com/KDE/clazy
- Official Qt AI skills: https://github.com/TheQtCompanyRnD/agent-skills
- Qt Documentation MCP: https://github.com/TheQtCompanyRnD/agent-skills/tree/main/mcp/qt-documentation-mcp
- OBS Studio 33.0.1 build reference only: https://github.com/obsproject/obs-studio/blob/33.0.1/cmake/common/compiler_common.cmake

No hard dependency on a Qt MCP connection, Qt Creator, or any Qt patch from OBS.
