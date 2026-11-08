# Authoritative C++ and native-tool sources

Order: target source/build/CI and effective compiler > applicable versioned standard, SDK and tool manuals > maintained best-practice guidance > articles. A new online tutorial is not the target's language or ABI contract.

- C++ language reference and standard working drafts: https://isocpp.org/std/the-standard ; https://eel.is/c++draft/
- C++ Core Guidelines (guidance, not mandatory lint policy): https://isocpp.github.io/CppCoreGuidelines/CppCoreGuidelines
- Microsoft C++ conformance: https://learn.microsoft.com/en-us/cpp/overview/visual-cpp-language-conformance
- GCC C++ language standards: https://gcc.gnu.org/projects/cxx-status.html
- Clang C++ status: https://clang.llvm.org/cxx_status.html
- Clang diagnostics: https://clang.llvm.org/docs/DiagnosticsReference.html
- Clangd compilation database: https://clangd.llvm.org/design/compile-commands
- Clangd client and check mode: https://clangd.llvm.org/
- Clang-tidy: https://clang.llvm.org/extra/clang-tidy/
- Clang Static Analyzer: https://clang.llvm.org/docs/ClangStaticAnalyzer.html
- Clang-format: https://clang.llvm.org/docs/ClangFormat.html
- AddressSanitizer: https://clang.llvm.org/docs/AddressSanitizer.html
- UndefinedBehaviorSanitizer: https://clang.llvm.org/docs/UndefinedBehaviorSanitizer.html
- ThreadSanitizer: https://clang.llvm.org/docs/ThreadSanitizer.html
- GDB: https://sourceware.org/gdb/documentation/
- LLDB: https://lldb.llvm.org/
- CMake: https://cmake.org/cmake/help/latest/
- VS Code C/C++ documentation: https://code.visualstudio.com/docs/languages/cpp
- OBS 33.0.1 C++17/C17 source-of-truth: https://github.com/obsproject/obs-studio/blob/33.0.1/cmake/common/compiler_common.cmake and https://github.com/obsproject/obs-studio/blob/33.0.1/CODESTYLE.md

Never convert official guideline preferences into unqualified correctness findings. Confirm exact feature availability in the target's installed language and toolchain.
