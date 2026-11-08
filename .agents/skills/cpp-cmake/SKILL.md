---
name: cpp-cmake
description: "Use when implementing or reviewing C++ CMake targets, toolchains, compiler flags, presets, generators and reproducible compilation databases."
---

# C++ CMake

## Required Dependencies

- cpp-core
- .codex/rules/cpp_core.md

Use cpp_cmake.md only when independently active. CMake is not required for projects using other build systems.

## Workflow

1. Read CMakeLists.txt, included .cmake files, presets, toolchain, dependency manager manifests, CI and real target graph. Resolve minimum CMake version and configured generator.
2. Determine source standard and actual target compiler flags, compile definitions/includes, configurations, platform SDK, architectures and link dependencies. Inspect public usage requirements in exported headers.
3. Apply the smallest authorized target-specific change. Prefer target_link_libraries with correct PRIVATE/PUBLIC/INTERFACE propagation; avoid global flag churn and accidental ABI or dependency changes.
4. For existing Qt targets, honor chosen Qt package, Qt 6 minor, target helpers, autogen and resource handling. Defer framework semantics to qt-core/qt-quick-qml only when active.
5. Execute only authorized configure/build commands. Configuration can execute code, fetch packages and write caches; it is not inherently read-only.
6. Recheck relevant compile/link and declared matrix. Do not call a successful Debug/Ninja build validation for a Release/MSVC target.

## Reference routing

- references/targets-and-presets.md: target graph, generators, SDKs, dependencies and multi-config.
- references/compilation-database.md: compile_commands.json, language server context and Windows generator limitations.
- references/qt-cmake-and-review.md: Qt helper APIs, moc/uic/rcc, real-version checks and common AI mistakes from The Qt Company.
- references/official-sources.md: CMake/Qt and toolchain documentation.

No automatic dependency install, package fetch, toolchain change, cross-build, deployment, installer or extra test target without authorization.
