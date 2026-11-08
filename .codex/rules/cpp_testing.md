# C++ testing rules

Activate only through the exact cpp_testing.md Active Rules entry, cpp-testing Active Stack Profiles entry, or an enabled C++ Testing profile naming the cpp_testing.md + cpp-testing pair.

## Required skills

- cpp-core
- cpp-testing

Required base rule: .codex/rules/cpp_core.md

## Test engineering

- Read the original requirement, production contracts, CODEX_PROJECT.md, active build/test presets, CTest registration or other runner, existing fixtures and CI. Test behavior independently of the implementer's preferred code structure.
- Match test boundary to behavior: isolated pure C++ unit, C API/plugin boundary, integration, process E2E, concurrency, platform/ABI or GUI. Never invent a test framework or install one by default.
- Check success, failures, boundaries, cancellation, allocation/cleanup, callback lifetime and race-relevant interleavings proportional to risk. Avoid non-deterministic sleeps and shared mutable fixtures.
- Prefer already-declared Catch2/GoogleTest/doctest/native framework and CTest where applicable; do not assume all C++ tests use CTest. Run test binaries using project-approved isolation and parameters.
- Coverage is project-declared, toolchain- and instrumentation-specific. Do NOT import a universal Go 85% threshold or compare coverage from incompatible configurations. Instrumented coverage does not prove correct or leak-free behavior.
- ASan, UBSan, TSan, LSan and related runtime tools require supported toolchain/platform and a separate approved build/run configuration. They are complementary, not interchangeable, and do not establish absence of defects from a clean run.
- On a reproducible failure, optional GDB/LLDB/CDB terminal debugging follows cpp-core/references/runtime-debugging.md; do not attach to a running shared process or require privileged debugging by default.
- Final acceptance references a stable code, test, fixture and build configuration snapshot, actual commands/results, failures, missing checks and risk gaps. If a mandatory check did not execute, mark NOT VERIFIED/BLOCKED, not PASS.

See cpp-testing references for deterministic fixtures, coverage/sanitizers, cross-platform matrix and review checklists.
