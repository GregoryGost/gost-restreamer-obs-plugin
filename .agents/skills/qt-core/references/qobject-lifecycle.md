# QObject ownership, connections and destruction

## Parent/child and native C++ ownership

- QObject parents own children and delete them with parent destruction. QObject allocated on the stack or owned by unique_ptr may also participate in QObject parent relationships; prove creation/deletion order before adding both owners.
- A parent is not proof that any raw QObject* is safe to retain; objects can be deleted independently and deleteLater defers destruction to an appropriate event loop.
- QPointer tracks deletion of a QObject but does not keep it alive; a QSharedPointer/std::shared_ptr is not universally safe with parent ownership. Determine explicit transfer/borrowing and API-specific ownership.
- QObject cannot be copied. Do not offer move/copy constructors mechanically to a QObject subclass or store QObjects by value in containers requiring copies.
- Connections: prefer typed signal/slot forms when compatible. A receiver/context QObject helps auto-disconnect on destruction; a contextless lambda capturing this/reference is potentially dangerous after owner destruction.
- Worker callbacks, timers and foreign plugin contexts may outlive widgets or the plugin module. Check cancellation, disconnection, callback drain and unload order.

## Signal/destroyed lifetime subtleties

- deleteLater schedules deferred deletion; without a suitable running event loop, timely cleanup may not occur as expected. Distinguish scheduling from actual destruction.
- AutoConnection selects direct/queued behavior using the emitting thread vs receiver affinity when delivered. Do not assert sender and receiver creation-thread equality alone guarantees delivery mode.
- When a QObject is moved between threads, all children move together; a parented QObject cannot be moved directly with moveToThread.
- Destroying a QThread while its worker still runs is unsafe; stop/wait according to actual contract and shutdown budget.

## Example investigation

A dialog creates a QObject with parent this and stores it also in an owning unique_ptr member. Verify member and QObject parent destruction ordering and whether the member calls delete before or after the parent; avoid blanket statements 'always double free' without examining the exact path.

Review: parent creation, transfers, observers, signal connections, lambdas, deferred deletion, thread affinity and shutdown.
Sources: https://doc.qt.io/qt-6/objecttrees.html ; https://doc.qt.io/qt-6/qobject.html ; https://doc.qt.io/qt-6/qpointer.html
