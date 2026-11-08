# C++ CMake rules

Activate only through the exact cpp_cmake.md Active Rules entry, cpp-cmake Active Stack Profiles entry, or an enabled C++ CMake profile naming the cpp_cmake.md + cpp-cmake pair. CMake is not imposed on a non-CMake project.

## Required skills

- cpp-core
- cpp-cmake

Required base rule: .codex/rules/cpp_core.md

## Source of truth and constraints

- Inspect complete affected CMakeLists.txt, included modules, presets, toolchain files, cache/settings, target graph and CI build matrix. Identify actual CMake version, generator, compiler, architecture, single- versus multi-config behavior and dependency resolution.
- Use target-scoped modern CMake commands and PUBLIC/PRIVATE/INTERFACE propagation according to real header/consumer contracts. Do not rewrite an existing valid project into a new template for style alone.
- Separate CMAKE_CXX_STANDARD and effective per-target features, extensions, C standard, platform APIs, ABI and generator/build config. Never silently raise minimum versions.
- A compile_commands.json file is valid only for the configuration that produced it; CMAKE_EXPORT_COMPILE_COMMANDS is supported by Ninja and Makefile generators, not all generators (including native Visual Studio). Do not manufacture a fake compilation database or diagnose a target with unrelated flags.
- Do not silently run configuring operations that download dependencies, execute arbitrary CMake code, replace build artifacts or modify caches. Verify scope, directory, generator and side effects before any command.
- For Qt CMake projects, follow the separately active qt-core / qt-quick-qml references and project-owned Qt SDK/version, not an assumed Qt 6.8+ minimum. Respect AUTOMOC/AUTOUIC/AUTORCC, generated sources, Qt targets and target-finalization requirements.
- Revalidate affected configure/build presets, target linkage, generated code, install/deploy only within task scope and project-declared commands. Builds for one platform/configuration are not proof for another.

See cpp-cmake references for build and compilation-database workflows, review checklist and official sources.
