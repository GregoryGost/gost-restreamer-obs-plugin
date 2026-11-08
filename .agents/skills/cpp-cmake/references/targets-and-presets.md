# Modern CMake for C++ — targets, presets and reproducible builds

## Targets and usage requirements

- Inspect target graph and included modules before editing. PRIVATE dependencies belong to implementation; PUBLIC dependencies participate in both building the target and its consumers; INTERFACE requirements belong only to consumers.
- Public include directories should use BUILD_INTERFACE and INSTALL_INTERFACE where an exported installed library requires distinct layouts. Check generated install/export targets before changing transitive dependencies.
- Use target_compile_features or an agreed CXX_STANDARD with CXX_STANDARD_REQUIRED and CXX_EXTENSIONS according to target policy. The effective compiler command is authoritative, not a top-level variable alone.
- Platform-specific source selection can be clearer with target_sources and generators rather than preprocessor mazes, but only refactor when requested.
- Avoid global include_directories, global link flags and transitive dependency leaks in new code; preserve existing valid builds without broad rewrites.

## Build systems and presets

- Single-config generators such as Ninja use CMAKE_BUILD_TYPE; multi-config generators (Visual Studio, Xcode, Ninja Multi-Config) choose a configuration at build/test time. Do not claim a Debug compile database validates a Release binary.
- Resolve generator, target architecture, toolchain file, compiler, platform SDK, chosen Qt SDK and minimum CMake version as configured in CODEX_PROJECT.md, presets and CI.
- Commands with --preset are meaningful only when the project's preset exists; use exact names and authorized build directories. Configuration executes arbitrary project CMake, may fetch dependencies and writes cache/output.
- For CTest, use ctest --test-dir build-dir -C Debug (multi-config) when applicable; the registration, build and working directory must match.
- Do not create toolchain files, package managers, presets, install rules or new subprojects without a concrete requirement.

## Example (illustrative; not a forced retrofit)

    add_library(core STATIC core.cpp)
    target_compile_features(core PUBLIC cxx_std_17)
    set_target_properties(core PROPERTIES CXX_EXTENSIONS OFF)
    target_include_directories(core PUBLIC
      $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
      $<INSTALL_INTERFACE:include>)
    target_link_libraries(core PRIVATE dependency_target)

Verify actual upstream target names and platform/version availability.
