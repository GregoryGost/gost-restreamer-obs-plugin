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

void OBSController::handleEvent(FrontendEvent event, bool reliableProfile)
{
	Q_ASSERT(QThread::currentThread() == thread());
	if (event == FrontendEvent::FinishedLoading) {
		ready_ = true;
	}
	if (!ready_ || event == FrontendEvent::Exit) {
		return;
	}
	const auto profile = frontend_.currentProfile();
	const bool active = frontend_.streamingActive();
	stateStore_.setObsState(profile, active);
	if (event == FrontendEvent::FinishedLoading || event == FrontendEvent::ProfileChanging ||
	    event == FrontendEvent::ProfileChanged || event == FrontendEvent::ProfileRenamed ||
	    event == FrontendEvent::ProfileListChanged) {
		ProfileDescriptor descriptor;
		descriptor.name = profile;
		if (reliableProfile) {
			descriptor.path = frontend_.currentProfilePath();
			descriptor.inventory = frontend_.profiles();
			descriptor.valid = !descriptor.name.isEmpty() && !descriptor.path.isEmpty() &&
					   descriptor.inventory.contains(descriptor.name);
		}
		emit profileObserved(event, descriptor);
	}
}

} // namespace gost
