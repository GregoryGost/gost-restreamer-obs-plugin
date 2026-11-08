# Scoped C++ style and formatting

## Precedence

1. Effective scoped repository .clang-format, .editorconfig and established formatting commands.
2. Approved CODEX_PROJECT.md naming/style configuration.
3. Consistent nearby source conventions.
4. Portable guidance only where the project is unconfigured.

clang-format supports formatting, not semantic correctness or Qt meta-object compatibility. Follow its installed version's supported options; do not introduce latest keys to an older tool.

## Safe examples

Existing exported callback with C linkage: do not rename it for style. Existing Qt signal, Q_PROPERTY name or QML-exposed method: preserve the reflection/API contract. Formatting a changed CMake file is not governed by clang-format's C++ options.

Use check mode on authorized paths/ranges when possible. Typical CLI examples for an already selected formatter:

    clang-format --version
    clang-format --dry-run --Werror path/to/changed.cpp

--dry-run and --Werror support depend on version; verify the installed help. For a limited edit use -lines ranges or a project-owned diff-specific workflow; do not blindly run clang-format -i over all source or include generated code.

## Naming tradeoffs

Public API, Qt naming and OBS project style may legitimately differ from generic C++ Core Guidelines or Google guide. Local style is not a reason to convert every member, pointer or container. A style-only PR must not expand to mass include-order changes unless expressly requested. Distinguish formatting validation from clang-tidy readability hints and compiler warnings.
