#pragma once

#include "frontend.hpp"

#include <QObject>

namespace gost {

class StateStore;

class OBSController final : public QObject {
	Q_OBJECT

public:
	OBSController(Frontend &frontend, StateStore &stateStore, QObject *parent = nullptr);
	void handleEvent(FrontendEvent event, bool reliableProfile = true);

signals:
	void profileObserved(gost::FrontendEvent event, const gost::ProfileDescriptor &profile);

private:
	Frontend &frontend_;
	StateStore &stateStore_;
	bool ready_ = false;
};

} // namespace gost
