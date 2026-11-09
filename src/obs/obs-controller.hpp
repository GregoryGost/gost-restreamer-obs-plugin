#pragma once

#include "frontend.hpp"

#include <QObject>

namespace gost {

class StateStore;

class OBSController final : public QObject {
	Q_OBJECT

public:
	OBSController(Frontend &frontend, StateStore &stateStore, QObject *parent = nullptr);
	void handleEvent(FrontendEvent event);

private:
	void observe();
	Frontend &frontend_;
	StateStore &stateStore_;
	bool ready_ = false;
};

} // namespace gost
