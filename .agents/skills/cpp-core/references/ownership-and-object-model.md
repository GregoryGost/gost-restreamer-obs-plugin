# C++ ownership, object model and exception safety

## Model the actual owner

Distinguish owned resources, borrowed references/pointers, observer pointers, weak handles, external C handles, custom deleters and parent-managed Qt objects. Ownership can transfer on successful API calls; establish when the transfer occurs and what happens on failure.

- Prefer Rule of Zero with RAII members. Use a destructor/copy/move special members only when the type owns a real resource or has deliberate value semantics.
- std::unique_ptr is appropriate for sole heap ownership; std::shared_ptr implies an explicit shared lifetime contract and reference-count overhead. Neither is a replacement for framework or host ownership.
- Keep borrowed pointers/references alive for the whole use; reject returning views to temporaries, lambda reference captures outliving locals, strings/views invalidated by reallocation, iterator use after erase, and self-referential state invalidated by move.
- For C handles, prefer an appropriate custom deleter when an owning wrapper is justified. Preserve the API's matching release function and allocator domain.
- Containers and algorithms: check move/copy semantics, invalidated iterators, mutation during iteration, aliasing, const-correctness and exception behavior, not merely syntactic preference.

## Exception safety

Document whether the function guarantees no-throw, strong guarantee, or basic invariant preservation as warranted by callers. If constructor/build steps fail, already-acquired resources must be released. Check noexcept against actual potentially throwing operations and allocation; an incorrect noexcept can terminate the process. Standard container choices may have different relocation behavior based on noexcept move.

Prefer scope-bound cleanup to ad hoc early-return cleanup. Avoid unnecessary try/catch wrappers that swallow faults or modify error contracts.

## Typical defects

Bad: return a std::string_view into a local std::string; the view dangles.
Better: return an owning std::string or a view into an object whose lifetime is guaranteed and documented.

Bad: store a raw observer to a Qt QObject and dereference it after deleteLater without a guard.
Better: verify owning parent/connection lifetime or use QPointer where it matches the required semantics.

Bad: use shared_ptr for every object solely to 'avoid leaks'.
Better: express explicit single ownership and borrowed access, leaving shared ownership only where required.

## Review checklist

- Can the object be destroyed while any callback, view, reference or timer can still use it?
- Is cleanup correct for partial initialization, error paths, repeat shutdown and plugin unload?
- Does move/copy preserve each resource's ownership and invariants?
- Is a raw pointer borrowed, owned or unknown? Do not assert leak without tracing it.
- Can allocator, CRT, foreign library or Qt parent delete this memory? Verify real contract.
