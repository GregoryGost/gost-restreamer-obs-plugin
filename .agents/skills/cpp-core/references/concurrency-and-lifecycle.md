# C++ concurrency, lifetime and shutdown

## Correctness before scheduling

Identify which threads own each object and who joins/cancels workers. The C++ memory model makes unsynchronized conflicting access a data race and undefined behavior; successful tests do not establish race freedom.

- Use mutex and lock_guard/unique_lock scoped to real protected invariants; avoid locks across blocking I/O, external callbacks or arbitrary user code unless protocol requires it.
- For condition_variable, wait with a predicate in a loop; wakeups can be spurious. Ensure lost-wakeup protection and shutdown notification.
- Atomics require a valid memory-order protocol. Do not upgrade everything to seq_cst or downgrade to relaxed without proving the publication/lifetime relationship.
- A detached std::thread with pointers into destroyed state is unsafe; define cancellation/stop, error reporting, thread join and shutdown budget. std::jthread is C++20, not a C++17 fallback.
- Beware task queues retaining callbacks after owner destruction, locks acquired recursively, and shutdown while shared libraries/unload callbacks are still in flight.
- Avoid thread-affinity assumptions for external toolkits: Qt event loops, GUI thread, driver callbacks and host plugin APIs have additional constraints in their own contracts.

## Test hypotheses

- Stop during initialization failure, active work, zero work, repeated start/stop and callback-unregister races.
- One producer and consumer under saturation; queue bounds and backpressure if the real requirement depends on it.
- Callback fires after owning scope ends; ensure disconnected/guarded lifetime or documented barrier.
- Concurrent read and write: identify happens-before proof or reproducible race; sanitizer availability is platform-specific.

## Reporting

Name specific reads/writes, lock/atomic ownership, failing schedule and why a test or documented guarantee supports it. Do not infer a deadlock from simply seeing two mutexes, or a leak from increased RSS alone.
