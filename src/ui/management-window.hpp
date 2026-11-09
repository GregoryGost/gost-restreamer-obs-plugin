#pragma once

#include "text-lookup.hpp"

#include <QWidget>

class QLabel;

namespace gost {

class StateStore;

class ManagementWindow final : public QWidget {
	Q_OBJECT

public:
	ManagementWindow(StateStore &stateStore, TextLookup text, QWidget *parent = nullptr);

private:
	void refresh();
	StateStore &stateStore_;
	TextLookup text_;
	QLabel *profile_ = nullptr;
	QLabel *streaming_ = nullptr;
};

} // namespace gost
