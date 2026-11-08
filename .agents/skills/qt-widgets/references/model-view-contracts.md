# Qt Widgets QAbstractItemModel and view contracts

Derived from Qt's model/view documentation and the useful A1 section of The Qt Company's qt-cpp-review review guide. Treat its regex lint results as investigation leads, not automatic defects.

## Model consistency

- Check rowCount/columnCount/parent/index and hasChildren for each valid and invalid QModelIndex. A non-tree model can intentionally return zero children for non-root indexes.
- data() must return meaningful QVariant for supported roles. roleNames and data should agree for custom roles, especially when exposed to QML; default Qt roles need not all be implemented.
- For editable models, flags() and setData() behavior/return code must match and dataChanged must describe actually modified indexes/roles.
- Structural changes require correct beginInsertRows/endInsertRows, beginRemoveRows/endRemoveRows, beginMoveRows/endMoveRows or reset; bracket operations around the actual mutation and validate parent/index ranges. layoutChanged does not replace insertion signals.
- Persistent QModelIndex and selection maps matter across move/reset operations. Proxies must mapToSource/mapFromSource instead of mixing indexes.
- Delegates can hold references to rows that are removed; validate editor commit/close, model reset and view interactions.

## Useful test oracles

- For a tree model with three children, check parent(index(row, col, parent)) round-trips when applicable.
- Removing a row emits a balanced begin/end notification; no stray dataChanged for a removed index.
- A proxy filtered/sorted view displays the correct source record after model edits.
- QAbstractItemModelTester can validate many standard invariants in tests where available; it is not a substitute for domain-specific assertions.

## Review counterexamples

Not every model needs setData or a nonempty roleNames. A read-only model returning false for setData is valid. No automatic "cache roleNames" requirement unless performance is relevant.
Sources: https://doc.qt.io/qt-6/model-view-programming.html ; https://doc.qt.io/qt-6/qabstractitemmodel.html ; https://doc.qt.io/qt-6/qabstractitemmodeltester.html
