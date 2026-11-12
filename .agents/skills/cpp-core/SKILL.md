---
name: cpp-core
description: "Use for C++17+ language correctness, ownership/RAII, memory, threading, C API/ABI boundaries, VS Code diagnostics and expert C++ review."
---

# C++ Core — version-aware native engineering

Language-level guidance for standalone C++ libraries, applications and native plugins. OBS Studio 33.0.1 supplies a verified C++17/C17 reference, not a dependency or the scope of this skill. Support another standard only when the active project's configuration establishes it.

## Activation

- Inspect CODEX_PROJECT.md and the relevant CMake/build/CI source; follow AGENTS.md and material_dependencies.md for activation and required rules.
- Use cpp_core.md only when independently active. This skill alone does not activate rules or optional overlays.
- Preserve the user's authorized scope; never install tools, change editor settings, run debugger attachments, migrate build systems or add diagnostic suppression solely on advice here.

## Expert investigation

1. Identify whether the task concerns correctness, architecture, a public API, a compiler warning, diagnostics, concurrency, performance or a version mismatch.
2. Read entire relevant source sections, public headers, callers, tests, compilation flags and generated/build boundaries. Search results are locators only.
3. Resolve C++ standard (e.g. C++17), compiler/toolset, standard library, architecture, exceptions/RTTI, target and source configuration, shared-library ABI and C-language boundary independently.
4. Reconstruct ownership, nullability, invariants, destructors, copy/move and callbacks; identify thread and event-loop lifetimes when present.
5. For meaningful design choices compare the smallest simpler approach. Try to falsify each finding with callers and documented contracts.
6. Use the actual project-selected compiler, formatter, diagnostics and test commands on the final stable snapshot. Missing mandatory checks are NOT VERIFIED, not clean.
7. Report exact path/range, configuration and category; distinguish compiler/UB evidence from preference or potential risk.

## Reference routing

| Task | Reference |
| --- | --- |
| Standards, feature availability, build ABI and C17 interop | references/standards-abi-and-c-interop.md |
| RAII, value categories, move/copy, resource and pointer lifetimes | references/ownership-and-object-model.md |
| Thread, synchronization, atomics, callbacks, resource cleanup | references/concurrency-and-lifecycle.md |
| VS Code runtime diagnostics and use of configured language servers | references/vscode-diagnostics.md |
| Compiler, clang-tidy, clangd, other static analyzers | references/static-analysis.md |
| GDB, LLDB, CDB terminal diagnostics on isolated targets | references/runtime-debugging.md |
| Sanitizers, profiling, performance and leak claims | references/performance-and-resource-diagnostics.md |
| General code audit, tradeoffs, evidence and review | references/review-checklist.md |
| Disputed, version-sensitive or tool-specific behavior | references/official-sources.md |

Load focused references, not every file for a small change. Formatting belongs to cpp-style, CMake to cpp-cmake, tests to cpp-testing and Qt to independently active Qt overlays.

## Defaults, not dogma

- Prefer RAII and explicit owned/borrowed semantics; a raw pointer is not inherently incorrect and a shared_ptr is not a universal owner.
- Preserve exception safety and observable error contracts. Never unwind C++ exceptions across an unsupported C/plugin boundary.
- Treat object lifetime, iterator invalidation, race freedom, ABI and symbol visibility as separate questions.
- Do not add a generic abstraction, threading subsystem, health check, startup verifier, cache or fallback solely because it might be helpful.
- Check compiler warnings and IDE diagnostics according to the project's selected gate, not a hypothetical global strict mode.
- A successful compilation does not imply a clean analyzer or a correct runtime; a sanitizer run is not proof that all resource defects are absent.

## Code documentation when the comment-quality pair is active

When `code_comment_quality.md` and `code-comment-quality` are both independently active, ensure every authored affected function/method (including constructors, destructors, callbacks and short helpers), field, constant, and local/loop variable has the required source documentation. A group block must identify each variable and explain its role; use the project's C++/Qt documentation conventions and preserve accurate lifetime and ownership explanations. No exception arises from brevity or symbol names. This skill does not activate the pair.

## Review handoff

Classify CORRECTNESS, SECURITY, COMPATIBILITY/ABI, CONCURRENCY, RESOURCE, DESIGN, PERFORMANCE or STYLE. Give source path/line, reproduced or documented failing path, smallest correction, tests/checks performed and explicit uncertainty.
