# VS Code C++ diagnostics — using the already selected integration

This is an **operational** reference for prepared projects, not a first-time discovery or installation procedure. The universal environment-discovery process belongs ONLY in this repository's README and runs during preinitialization. CODEX_PROJECT.md supplies the confirmed active C++ diagnostic provider, executable/connection mode, build context, analyzer configuration, severities, workspace and validation commands.

## Distinct diagnostic sources

- Microsoft C/C++ (cpptools) IntelliSense diagnostics are IDE/LSP-client state; they are not identical to clangd and are not automatically exposed as a terminal command.
- VS Code clangd extension uses LLVM clangd, with its own version, settings and compilation context. When an accessible LSP connection is approved and verified, prefer its semantic references and published diagnostics; do not assume Codex has the VS Code LSP socket or Problems API.
- Headless clangd --check against a saved C++ translation unit can provide a useful fallback when the project config permits it, but cannot prove equality with open editor buffers, workspace diagnostics or other providers.
- clang-tidy and compiler warnings are separate inputs. clangd may run selected clang-tidy checks; it does not guarantee the complete project .clang-tidy set, nor replace an explicit project gate.
- Qt Clazy is a separate optional analyzer; not an inherent part of cpptools/clangd.

## Exact build context matters

C/C++ analysis requires the correct compile command: -std, target triple, defines, include roots, resource path, sysroot, Qt-generated headers, configuration (Debug/Release), platform and source file. For header diagnostics choose a matching translation unit; one header may be consumed under several macro configurations. A database generated for Linux Clang cannot validate Windows MSVC semantics.

A compile_commands.json file and selected CMake target/preset are sources of configuration, not magical evidence that the IDE currently uses that file. Native Visual Studio generators do not generate a compilation database through CMAKE_EXPORT_COMPILE_COMMANDS; the approved provider/build alternative must be used. Never fabricate compile commands by guessing include paths.

## Use and revalidate

1. From the prepared project profile, select the actual declared interface (connected LSP, verified CLI, project compiler/analyzer invocation). If disabled/unavailable, do not invent a replacement gate.
2. Check diagnostics against the saved final source snapshot, selected target/preset and provider settings. Record exact file, line, code, severity, origin, scope and diagnostic text.
3. If results conflict with a saved file, investigate stale state, unsaved buffer, wrong target or missing generated header before editing functional source.
4. Inspect relevant code and callers. Fix proven, authorized actionable findings only; no global fix-all, blanket suppression, analyzer disable, or unsolicited refactor.
5. Re-run the same selected diagnostic gate after the final source/configuration change, plus independent compiler/build/tests. Existing warnings and nonblocking hints are separately classified.
6. Report PASS/FAIL/NOT VERIFIED or BLOCKED for each required check, including provider, version, build context and observed limitations. Do not equate an empty terminal output with a clean VS Code Problems pane.

## Illustrative read-oriented commands

Commands below are examples for an already configured, allowed build and tool path, NOT instructions to create a new configuration or install an extension:

    clangd --version
    clangd --check=/absolute/path/to/source.cpp --compile-commands-dir=/approved/build
    clang-tidy -p /approved/build /absolute/path/to/source.cpp

Verify supported flags with the installed tool's help. Clangd check and clang-tidy may access caches, compile configurations and generated files; any actual configure/build execution requires the current request to permit it.

Official guides: https://clangd.llvm.org/design/compile-commands ; https://clangd.llvm.org/installation ; https://code.visualstudio.com/docs/cpp/cpp-ide ; https://code.visualstudio.com/docs/cpp/cmake-linux
