# Native sanitizers, coverage and analysis matrix

Instrumented runs are opt-in test configurations requiring a verified compiler, linker/runtime and platform support, clean fixture boundary and actual test execution.

## Sanitizers

- AddressSanitizer (ASan): out-of-bounds, use-after-free and related memory corruption; optional leak detection varies by platform. Does not prove data-race freedom.
- UndefinedBehaviorSanitizer (UBSan): selected categories of undefined behavior, coverage depends on exact enabled checks and compiler.
- ThreadSanitizer (TSan): detects many data races in instrumented runs; generally not combined with ASan in one test binary and often platform/runtime constrained.
- LeakSanitizer (LSan): certain leaked allocations; suppression, fork, container and allocator behavior affects results.
- MSVC sanitizer support differs from Clang/GCC; report tool/host limits and do not paste compiler flags universally into project config.

Run the right workload and record configuration. Sanitizers can alter scheduling and performance; passing an instrumented test is not equivalent to a production build/test. Do not claim absence of memory leaks from a single success.

## Coverage

- Coverage source and toolchain may use llvm-cov/llvm-profdata, gcov/lcov or compatible approved equivalents. Builds require matching compiler flags and fresh counters.
- Distinguish line, region, branch and function coverage; do not equate them or compare arbitrary tools' percentages.
- Count coverage only over the declared production source set and successful complete test run. Exclude generated/vendored code by explicit policy, not to inflate results.
- No universal 85% minimum inherited from Go. A threshold is a project-owner decision recorded in CODEX_PROJECT.md with scope, metric and command.

## Acceptance

Report exact selected config, binary, tool versions, test cases run, actual findings and untested configurations; unavailable required instrumentation is NOT VERIFIED or BLOCKED, not PASS.
