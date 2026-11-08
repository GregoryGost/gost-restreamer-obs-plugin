---
name: cpp-testing
description: "Use for C++ unit/integration tests, CTest, existing test frameworks, ABI boundaries, sanitizers, coverage and deterministic regression checks."
---

# C++ Testing

## Required Dependencies

- cpp-core
- .codex/rules/cpp_core.md

Use cpp_testing.md only when independently active; tests and program launches must be permitted by the current request and safe target policy.

## Workflow

1. Inspect original requirements, candidate source, exported API, existing tests, selected runner/framework, compilation configuration, environment and CI.
2. Independently derive expected behavior and negative/risk cases; select the narrowest real boundary: unit, C linkage/plugin, concurrent, integration, GUI or process E2E.
3. Write focused tests only in authorized paths. Avoid unrelated scaffolding, a new test framework, new dependencies and brittle private-implementation assertions.
4. Use CTest with existing project registrations where applicable; otherwise use the project's actual runner. Temporary files, DBs, ports, user state and worker lifetimes must be isolated and cleaned up.
5. Run focused tests then required broader checks against an identified final snapshot. Classify execution failures, environment BLOCKED, assertions and suspected flaky outcomes separately.
6. Use instrumented sanitizer or coverage configurations only if supported, selected and authorized. Debugger observations are diagnostic, not test acceptance.
7. Report test oracle, commands, platform/build config, version, fixture, what passed/failed and unverified areas.

## References

- references/test-boundaries-and-fixtures.md: framework choices, determinism, failure and boundary checks.
- references/sanitizers-and-coverage.md: ASan/UBSan/TSan/LSan, instrumented builds, coverage caveats.
- references/official-sources.md: CTest, C++ test frameworks and tools.
- cpp-core/references/runtime-debugging.md: bounded terminal GDB/LLDB/CDB investigation if needed.
- cpp-core/references/performance-and-resource-diagnostics.md: representative benchmarks and lifecycle regression.

Do not copy Go's 85% default statement-coverage gate; use project-selected baseline, platform/compiler and coverage measurement.
