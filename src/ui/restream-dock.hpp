#pragma once

#include "text-lookup.hpp"

#include <QWidget>

class QLabel;

namespace gost {

class StateStore;

class RestreamDock final : public QWidget {
	Q_OBJECT

public:
	RestreamDock(StateStore &stateStore, TextLookup text, QWidget *parent = nullptr, QString version = {});

signals:
	void managementRequested();

private:
	void refresh();
	StateStore &stateStore_;
	TextLookup text_;
	QLabel *server_ = nullptr;
	QLabel *stream_ = nullptr;
	QLabel *notice_ = nullptr;
	QLabel *profile_ = nullptr;
	QLabel *streaming_ = nullptr;
};

} // namespace gost
