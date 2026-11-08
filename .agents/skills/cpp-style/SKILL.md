---
name: cpp-style
description: "Use for scoped C++ naming, clang-format, .editorconfig style, formatting reviews and safe public/API-preserving style changes."
---

# C++ Style

## Required Dependencies

- cpp-core
- .codex/rules/cpp_core.md

Use cpp_style.md only if independently active; its rule/skill pair is not activated by this skill.

## Workflow

1. Read local .clang-format, .editorconfig, style-related .clang-tidy, CODEX_PROJECT.md, file-local patterns and generated/vendored ownership.
2. Determine whether the request permits formatting or is review-only. The selected formatter version, workspace root and file/line scope must be established before execution.
3. Prefer repository config over portable style examples. Where silent, preserve consistent established conventions.
4. Format only authorized touched paths/ranges. Compare the actual diff; no automatic -i on an entire tree, fix-all or new config to force a preference.
5. Avoid altering exported symbols, macros, C callbacks, Qt property/signal names, QML bindings, reflection or serialization with aesthetic renames.
6. Confirm format check on final snapshot and distinguish STYLE findings from correctness and ABI issues.

For detailed precedence, C++/Qt naming and targeted checks use references/formatting-and-naming.md and references/official-sources.md. Do not claim clang-format enforces correctness, header hygiene or Qt meta-object contracts.
