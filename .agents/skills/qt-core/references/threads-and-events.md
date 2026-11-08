# Qt event loops and thread affinity

## Invariants

- Each QObject has a thread affinity. Queued events/signals require a running event loop in the receiver object's thread.
- A QThread object itself normally lives in the thread that created it; the managed thread executes run(). Do not put worker slots on QThread assuming they execute inside run().
- QWidget and other GUI-facing objects must be used from the GUI thread.
- DirectConnection invokes the receiver synchronously in the emitting thread, regardless of receiver affinity; investigate cross-thread direct calls to non-reentrant objects.
- AutoConnection decision depends on current delivery context. QueuedConnection dispatches only if receiver thread can process the event; queued signal argument metatypes and lifetimes must be valid.
- moveToThread restrictions: parent cannot be non-null; member pointers not parented to the object do not move automatically.
- Timers and sockets tied to an event loop should be created/started/stopped in appropriate thread context.
- Worker shutdown must stop dispatch, cancel queued requests as required and join/finish workers before destroying their dependencies.

## Practical review steps

1. Name sender, receiver, context and which thread emits/receives; inspect how it is constructed, parented and moved.
2. Trace connection type and when the event loop starts/stops (including during process shutdown).
3. Check lambda captures and receiver contexts for objects destroyed before queued events arrive.
4. Verify cross-thread data access and actual synchronization; a queued signal is not universal protection for shared mutable native state.
5. For UI/model mutations verify GUI-thread delivery and test representative asynchronous state.

Useful documents: https://doc.qt.io/qt-6/threads-qobject.html ; https://doc.qt.io/qt-6/qthread.html ; https://doc.qt.io/qt-6/qobject.html
