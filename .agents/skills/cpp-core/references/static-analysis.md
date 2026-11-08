# C++ static analysis — compiler, clangd, clang-tidy, Clazy

Each source is distinct. Follow the selected project diagnostics severity gate; suggestions are not necessarily defects.

## Compiler and build

Use the real configured compiler (GCC/Clang/MSVC), standard, target architecture and warning settings. -Wall/-Wextra and MSVC /W4 are not identical warning sets. Warnings-as-errors is project policy, not a universal portability requirement. A translation unit may compile under one build matrix cell and fail another.

Do not add -Werror or disable warnings merely to make CI green. Record baseline new/pre-existing warnings and exact invocation. Compiler diagnostics do not establish runtime safety.

## clangd

clangd is an LSP server for code navigation and diagnostics; it uses a compile command database or a verified equivalent driver/config. A CLI --check against one saved file differs from workspace LSP diagnostics. See vscode-diagnostics.md.

## clang-tidy / Clang Static Analyzer

- Use the configured .clang-tidy checks and any project per-target settings. Prefer -p pointing at the actual compilation database, not ad hoc defaults.
- The clang-analyzer-* checks in clang-tidy may expose control-flow bugs; do not double-count them as independent compiler failures.
- A source-only diff scope is insufficient to capture all defects triggered through consumers of changed public headers. Analyze affected translation units under the required project scope.
- clang-tidy -fix, clang-apply-replacements, automated rewrites and new NOLINT annotations are source mutations requiring authorization and review of the diff.
- A regex warning or third-party static-analysis rule is a lead, not proof. Trace ownership, behavior, correct conditions and counterexamples before classifying it.

## Qt Clazy and others

Qt Core/Widgets/Quick can select Clazy as an additional configured analyzer. Verify installed Clazy/Clang version compatibility, supported checks, build flags and paths; no automatic installation. Treat project-specific severity and its false positives just as other analyzers. Cppcheck or another third-party checker is optional and must not become an implicit hard dependency.

## Completion checklist

- Are relevant headers analyzed using real consuming translation units and correct generated/Qt includes?
- Do analyzer and compiler run over the intended target/preset with matching source revision?
- Were warning codes, line ranges and actual severity preserved, rather than replacing them with the agent's preference?
- Were all new actionable findings examined and correction/waiver traceable?
- Was final diagnostic re-run performed after fixes, or clearly marked NOT VERIFIED?

Sources: https://clang.llvm.org/extra/clang-tidy/ ; https://clang.llvm.org/docs/ClangStaticAnalyzer.html ; https://clangd.llvm.org/design/compile-commands ; https://github.com/KDE/clazy
