#include "obs-controller.hpp"

#include "core/state-store.hpp"

#include <QThread>

#include <utility>

namespace gost {

OBSController::OBSController(Frontend &frontend, StateStore &stateStore, QObject *parent)
	: QObject(parent),
	  frontend_(frontend),
	  stateStore_(stateStore),
	  statsTimer_(this)
{
	statsTimer_.setInterval(std::chrono::seconds(1));
	connect(&statsTimer_, &QTimer::timeout, this, [this] {
		if (lifecycle_ == Lifecycle::Ready)
			publishStatistics(frontend_.streamingActive());
	});
}

OBSController::~OBSController()
{
	shutdown();
}

void OBSController::handleEvent(FrontendEvent event, bool reliableProfile)
{
	Q_ASSERT(QThread::currentThread() == thread());
	if (event == FrontendEvent::Exit) {
		shutdown();
		return;
	}
	if (event == FrontendEvent::FinishedLoading && lifecycle_ == Lifecycle::AwaitingFrontend) {
		// До готовности интерфейса OBS допустима регистрация плагина, но чтение профиля и выхода отложено.
		lifecycle_ = Lifecycle::Ready;
		statsTimer_.start();
	}
	if (lifecycle_ != Lifecycle::Ready) {
		return;
	}
	const auto profile = frontend_.currentProfile();
	const bool active = frontend_.streamingActive();
	stateStore_.setObsState(profile, active);
	publishStatistics(active, event);
	if (event == FrontendEvent::FinishedLoading || event == FrontendEvent::ProfileChanging ||
	    event == FrontendEvent::ProfileChanged || event == FrontendEvent::ProfileRenamed ||
	    event == FrontendEvent::ProfileListChanged) {
		ProfileDescriptor descriptor;
		descriptor.name = profile;
		// При перенесённом событии путь и перечень профилей уже могут относиться к другому переходу.
		if (reliableProfile) {
			descriptor.path = frontend_.currentProfilePath();
			descriptor.inventory = frontend_.profiles();
			descriptor.valid = !descriptor.name.isEmpty() && !descriptor.path.isEmpty() &&
					   descriptor.inventory.contains(descriptor.name);
		}
		emit profileObserved(event, descriptor);
	}
}

void OBSController::shutdown() noexcept
{
	// До releaseStatistics переводим lifecycle_ в Stopped. Повторный shutdown, в том числе из
	// деструктора после EXIT, завершится без вызова адаптера: Frontend API к этому моменту уже может быть недоступен.
	Q_ASSERT(QThread::currentThread() == thread());
	// Состояние Stopped устанавливается и до первого FinishedLoading, исключая поздний запуск таймера.
	if (std::exchange(lifecycle_, Lifecycle::Stopped) != Lifecycle::Ready)
		return;
	statsTimer_.stop();
	frontend_.releaseStatistics();
}

void OBSController::publishStatistics(bool active, std::optional<FrontendEvent> event)
{
	const auto sample = frontend_.readStatistics();
	stateStore_.setObsStatistics(statsCollector_.sample(sample, active, OBSStatsCollector::Clock::now(), event));
}

} // namespace gost
