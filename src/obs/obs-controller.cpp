#include "obs-controller.hpp"

#include "core/state-store.hpp"

#include <QThread>

#include <utility>

namespace gost {

OBSController::OBSController(Frontend &frontend, StateStore &stateStore, QObject *parent)
	: QObject(parent),
	  frontend_(frontend),
	  stateStore_(stateStore)
{
}

void OBSController::handleEvent(FrontendEvent event)
{
	Q_ASSERT(QThread::currentThread() == thread());
	if (event == FrontendEvent::FinishedLoading) {
		ready_ = true;
	}
	if (ready_ && event != FrontendEvent::Exit) {
		observe();
	}
}

void OBSController::observe()
{
	auto profile = frontend_.currentProfile();
	const bool active = frontend_.streamingActive();
	stateStore_.setObsState(std::move(profile), active);
}

} // namespace gost
