# QML bindings, delegates, layout and event-driven state

Based on version-specific Qt QML documentation and selected useful questions from The Qt Company's qt-qml and qt-qml-review skills; treat lint regex rules as leads requiring semantic verification.

## Declarative data flow

- A binding defines a dependency graph. Imperative assignment to a bound property can replace its binding; check whether the change is deliberate and whether the original expression must continue to update.
- Use specific typed properties where the contract is known; property var is not universally wrong but loses static type checking where a stable type is available.
- Bound/aliased properties and NOTIFY signals must reflect the actual C++ model; missing NOTIFY can cause stale UI despite compiling.
- Connections, JS callbacks, Loader-created objects and Qt.createComponent objects have lifetime semantics; guard Loader.item access against absence/loading/error.
- Qt Quick Controls style-specific imports may override runtime style selection. Do not inject a style-specific import globally because one component customizes its control.

## Layout, delegates and states

- Mixing anchors with Qt Quick Layout size management on the same item can cause conflicts; verify the actual parent/layout context.
- Delegate instances may be recycled; state stored only in a delegate can survive reassignment. Put durable item state in the model or reset it on reuse, as required.
- ListView, Repeater and proxy model indices have different lifetimes and ownership; do not assume a visual index matches a stable domain identity.
- States, PropertyChanges and transitions can override bindings temporarily; test the actual transition and restoring behavior.
- Avoid using opacity/clip/layer or image decode optimization as universal rules. Profile expensive rendering paths or establish real constraints first.
- Verify accessible names, keyboard behavior, translation and UI scaling when they are part of the UI change.

## Cases

A Loader of a component with asynchronous loading returns no item immediately: check status before dereference.

A ListView delegate with reuseItems stores isSelected locally: an unrelated row can receive its stale state after recycling.

A property binding is replaced by an imperative assignment in a signal handler: later source changes stop propagating.

All cases require verification with real component context; these are failure hypotheses, not automatic lint verdicts.
