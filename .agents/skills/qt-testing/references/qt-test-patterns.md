# Qt Test — signals, models, asynchronous events

## Qt Test essentials

- Use QTest and project-owned Qt Test setup. QTEST_MAIN / QTEST_GUILESS_MAIN and custom main have different application/event-loop needs. Choose the fixture matching the actual objects.
- QSignalSpy must bind to a valid signal; check isValid, argument arity/types and actual payload as relevant. Signal count alone is insufficient for a state-transition contract.
- Use QTRY_VERIFY and QTRY_COMPARE or other bounded event-loop aware waits for queued/async behavior. Avoid QTest::qWait as a blanket substitute for proper synchronization or infinite waiting.
- Test signal delivery from worker threads with registered argument types and receiver/context lifetimes where necessary.
- QAbstractItemModelTester can catch structural model violations; add application-specific tests for roles, edits, proxy mapping and view behavior.
- Verify deletion after deleteLater when the event loop has processed deferred deletes. A weak pointer becoming null is observable evidence; do not assume immediate destruction at scheduling time.

## Example scenarios

A timer-driven worker emits update(value): expect one signal within a bounded window, verify value and final state, then stop worker and ensure no callbacks after receiver destruction.

A model insert: attach spies before the call, assert rowsInserted signals and new model data; model tester checks index structure.

## Isolation

Cleanup connections, threads, timers and global Qt state in teardown, use unique temp paths and no production user profile. Preserve exact Qt version, platform plugin, GUI display configuration and CTest invocation in the report.
