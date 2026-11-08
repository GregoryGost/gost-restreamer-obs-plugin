# C++ core rules

Activate only through the exact cpp_core.md Active Rules entry, cpp-core Active Stack Profiles entry, or an enabled C++ profile naming the cpp_core.md + cpp-core pair. Merely mentioning C++ does not override target-project activation. Template maintenance may edit this rule directly.

## Required skills

- cpp-core

## Source of truth

1. Read CODEX_PROJECT.md, the relevant CMakeLists.txt / CMakePresets.json / toolchain or other actual build configuration, public headers, implementation, callers, tests and CI.
2. Establish independently the effective C++ standard, compiler/version, standard library, architecture, build configuration, macros, exception/RTTI settings, ABI and C/C++ linkage. The portable reference baseline is C++17, NOT an instruction to downgrade or upgrade a target.
3. Use the already selected VS Code diagnostic pathway and project validation commands. No assumption of direct access to editor Problems, unsaved buffers, or an installed extension merely from its name.
4. Read only task-relevant cpp-core references. Add CMake/style/testing/Qt overlays only when independently active and dependency-valid.

## Engineering constraints

- Preserve observable contracts, public API/ABI, cross-module allocation/deallocation ownership, object lifetimes and resource cleanup. Prefer explicit RAII ownership and Rule of Zero; use raw pointers/references legitimately for borrowed/observer relationships. Do not mass-convert borrowed or Qt-owned pointers to smart pointers.
- Check copy/move special members, iterator/reference invalidation, value categories, constructor/destructor behavior, exception safety, noexcept, thread shutdown, atomics, synchronization and lock scope when relevant.
- Detect unsupported C++20/23 syntax or library features in a C++17 target; do not assume compiler extensions are portable. Keep C and C++ standards distinct.
- Preserve C API/extern "C" and plugin boundaries: callbacks, lifetime, calling convention, binary symbols, visibility, ABI, exception/nonexception crossings and compiler/CRT compatibility. Do not introduce breaking ABI or exported APIs without authorization.
- Distinguish a proven correctness issue from a convention, speculative performance gain, noisy analyzer hint or unverified warning. Investigate against complete relevant code and interfaces; compare the smallest sufficient fix with plausible alternatives.
- Use project-declared compilation, diagnostics and tests. Compiler build success is not a substitute for chosen clangd/clang-tidy checks; editor diagnostics do not replace a real build or run.
- Inspect diagnostic baseline and analyzer severity. Recheck changed files and impacted translation units on the final saved snapshot. A required missing, stale, mismatched or unreachable diagnostic check is NOT VERIFIED, never PASS.
- No automatic installation, upgrade, debugging process attach, build-tool migration, new dependencies, compile database generation with unapproved side effects, mass format, blanket suppressions or fix-all actions.

## Validation and review

- Classify findings as CORRECTNESS, SECURITY, COMPATIBILITY/ABI, CONCURRENCY, RESOURCE, DESIGN, PERFORMANCE or STYLE, with path/line, evidence, build context, smallest safe correction and verification status.
- For runtime debugging, see cpp-core/references/runtime-debugging.md; interactive GDB/LLDB/CDB is optional and only for authorized isolated targets with actual input support.
- For performance or resource claims, see cpp-core/references/performance-and-resource-diagnostics.md; sanitizers detect certain failures, not all leaks/races, and not every target/platform supports them.
- Test and review the final source/build snapshot, report checks skipped and distinguish new defects from pre-existing findings.
