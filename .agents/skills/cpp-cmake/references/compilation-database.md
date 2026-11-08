# CMake compilation databases and diagnostic contexts

Compilation databases encode concrete compile commands per source file. clangd/clang-tidy and similar tools need matching flags, includes, Qt moc output directories, sysroot/target architecture and preprocessor defines.

## Supported mechanisms

- CMAKE_EXPORT_COMPILE_COMMANDS is implemented for Makefile and Ninja generators. Native Visual Studio and Xcode generators do not provide this variable's output. Ninja Multi-Config and generator behavior should be verified with the installed CMake version and target.
- Alternative ways to supply clangd configuration depend on the *existing project/IDE build provider*. Never invent an incomplete compile_commands.json by rewriting flags and claim parity with cpptools.
- A header file usually has no standalone compile command. clangd may infer a command from a related source, but a macro-sensitive header consumed by multiple targets needs each relevant configuration examined.
- Check whether the database describes the current target/configuration, not an old build directory or the host rather than a cross-compiled target.
- Generated headers must exist in the approved build directory for accurate tool diagnostics. A failed compile caused by missing autogen files is not automatically a source defect.

## Readiness and reporting

An already configured profile may declare a known database path, generator and safe commands for consumers. On failure: report unknown/missing/stale path, provider and scope; do not configure a new build tree without authorization.

References:
https://cmake.org/cmake/help/latest/variable/CMAKE_EXPORT_COMPILE_COMMANDS.html
https://clang.llvm.org/docs/JSONCompilationDatabase.html
https://clangd.llvm.org/design/compile-commands
https://code.visualstudio.com/docs/cpp/cmake-linux
