# C++ tests — boundaries, oracles and determinism

## Choose the narrowest sufficient boundary

- Unit: pure algorithms/value models; fast deterministic inputs, explicit edge cases and failures.
- Integration: real library object, file I/O, network/test server or actual C API where mocking hides behavior.
- Plugin/shared library: ABI/load/unload, callback registration, host API contract and object ownership. Use a disposable authorized test host, not the live application.
- Process E2E: real executable and meaningful process/config boundary; use only when a unit/integration test cannot prove the observable contract.
- Concurrency: verify shutdown, callbacks-after-free, repeated start/stop and actual synchronization invariants, not elapsed wall clock alone.

Match the existing declared framework (GoogleTest, Catch2, doctest, Boost.Test, custom runner, CTest). Tests should not require installation of a new framework because it is fashionable. CTest is an orchestration/registration layer, not automatically the assertion framework.

## Example cases

A callback registering with a foreign C library: test first success, failed registration, callback after shutdown, multiple registrations/unregistrations, and whether the host is allowed to retain user_data.

A move-only resource handle: test transfer, old-handle invalidation, move assignment over an existing resource and cleanup on exception/return.

A worker process: test cancellation before work starts, during work, on producer error and repeated shutdown without hanging.

## Test isolation

Use separate temporary directories, fixture process cleanup, bounded timeouts and ephemeral ports. Avoid fixed sleeps, order-dependent globals, matching private log strings instead of behavior, tests that reach production databases or shared real user settings, and incorrect fake implementations.

When CMake tests are registered, use the selected target and ctest test-dir/config/filter; CTest exit status and test count must be recorded. A 'no tests found' run is not a PASS for a required suite.
