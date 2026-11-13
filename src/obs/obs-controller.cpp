#include "obs-controller.hpp"

#include "core/state-store.hpp"

#include <QThread>

#include <utility>
#include <algorithm>

namespace gost {

// frontend и stateStore заимствуются; parent управляет QObject. Все три таймера работают в GUI-потоке.
OBSController::OBSController(Frontend &frontend, StateStore &stateStore, QObject *parent)
	: QObject(parent),
	  frontend_(frontend),
	  stateStore_(stateStore),
	  statsTimer_(this),
	  stopPollTimer_(this),
	  stopDeadlineTimer_(this)
{
	statsTimer_.setInterval(std::chrono::seconds(1));
	// Периодический обработчик читает статистику только после FinishedLoading и до shutdown.
	connect(&statsTimer_, &QTimer::timeout, this, [this] {
		if (lifecycle_ == Lifecycle::Ready)
			publishStatistics(frontend_.streamingActive());
	});
	stopPollTimer_.setInterval(50);
	stopDeadlineTimer_.setSingleShot(true);
	connect(&stopPollTimer_, &QTimer::timeout, this, &OBSController::checkStreamingStop);
	// Истечение абсолютного срока означает неподтверждённую остановку, без нового запроса stop.
	connect(&stopDeadlineTimer_, &QTimer::timeout, this, [this] { finishStreamingStop(false); });
}

// Снимает ожидания и подписки до разрушения таймеров; после предыдущего Exit OBS больше не читается.
OBSController::~OBSController()
{
	shutdown();
}

// event обновляет фазу/поколение трансляции; reliableProfile разрешает подтверждать идентичность профиля.
void OBSController::handleEvent(FrontendEvent event, bool reliableProfile)
{
	Q_ASSERT(QThread::currentThread() == thread());
	if (event == FrontendEvent::Exit) {
		shutdown();
		return;
	}
	if (lifecycle_ == Lifecycle::Stopped)
		return;
	// Фазу события сохраняем и до FinishedLoading без чтения OBS: ранний Starting нельзя потерять.
	switch (event) {
	case FrontendEvent::StreamingStarting:
		streamingEventObserved_ = true;
		++streamingGeneration_;
		streamingPhase_ = StreamingPhase::Starting;
		break;
	case FrontendEvent::StreamingStarted:
		streamingEventObserved_ = true;
		// Если Starting пропущен, Started после Stopped/Stopping всё равно обозначает новый запуск.
		if (streamingPhase_ == StreamingPhase::Stopped || streamingPhase_ == StreamingPhase::Stopping)
			++streamingGeneration_;
		streamingPhase_ = StreamingPhase::Started;
		break;
	case FrontendEvent::StreamingStopping:
		streamingEventObserved_ = true;
		streamingPhase_ = StreamingPhase::Stopping;
		break;
	case FrontendEvent::StreamingStopped:
		streamingEventObserved_ = true;
		streamingPhase_ = StreamingPhase::Stopped;
		break;
	default:
		break;
	}
	if (event == FrontendEvent::FinishedLoading && lifecycle_ == Lifecycle::AwaitingFrontend) {
		// До готовности интерфейса OBS допустима регистрация плагина, но чтение профиля и выхода отложено.
		lifecycle_ = Lifecycle::Ready;
		if (!streamingEventObserved_) {
			// streaming — первая выборка, когда фазовых событий ещё нет; эфир мог начаться до загрузки плагина.
			const auto streaming = frontend_.streamingState();
			if (!streaming.available) {
				streamingPhase_ = StreamingPhase::Starting;
			} else if (streaming.active || streaming.reconnecting) {
				streamingPhase_ = StreamingPhase::Started;
				++streamingGeneration_;
			}
		}
		statsTimer_.start();
	}
	if (lifecycle_ != Lifecycle::Ready)
		return;
	// guard защищает продолжение после синхронных подписчиков StateStore/profileObserved.
	const QPointer<OBSController> guard(this);
	// profile — отображаемое имя; active — штатный Frontend-флаг для независимой локальной статистики.
	const auto profile = frontend_.currentProfile();
	const bool active = frontend_.streamingActive();
	stateStore_.setObsState(profile, active);
	if (!guard || lifecycle_ != Lifecycle::Ready)
		return;
	publishStatistics(active, event);
	if (!guard || lifecycle_ != Lifecycle::Ready)
		return;
	if (event == FrontendEvent::FinishedLoading || event == FrontendEvent::ProfileChanging ||
	    event == FrontendEvent::ProfileChanged || event == FrontendEvent::ProfileRenamed ||
	    event == FrontendEvent::ProfileListChanged) {
		// descriptor содержит имя и, только при надёжном событии, данные проверки идентичности.
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
	if (guard)
		checkStreamingStop();
}

// Запрещает новые чтения до очистки таймеров; frontend освобождается только при первом выходе из Ready.
void OBSController::shutdown() noexcept
{
	// До releaseStatistics переводим lifecycle_ в Stopped. Повторный shutdown, в том числе из
	// деструктора после EXIT, завершится без вызова адаптера: Frontend API к этому моменту уже может быть недоступен.
	Q_ASSERT(QThread::currentThread() == thread());
	cancelStreamingStop();
	// Состояние Stopped устанавливается и до первого FinishedLoading, исключая поздний запуск таймера.
	if (std::exchange(lifecycle_, Lifecycle::Stopped) != Lifecycle::Ready)
		return;
	statsTimer_.stop();
	frontend_.releaseStatistics();
}

// active задаёт состояние OBS для счётчиков; event при наличии сбрасывает соответствующие интервалы.
void OBSController::publishStatistics(bool active, std::optional<FrontendEvent> event)
{
	// sample — собственная выборка значений адаптера без сохранения указателей libobs.
	const auto sample = frontend_.readStatistics();
	stateStore_.setObsStatistics(statsCollector_.sample(sample, active, OBSStatsCollector::Clock::now(), event));
}

// receiver ограничивает callback; timeoutMs — общий срок в миллисекундах, без продления при активности.
void OBSController::stopStreaming(QObject *receiver, std::function<void(bool confirmed)> callback, int timeoutMs)
{
	Q_ASSERT(QThread::currentThread() == thread());
	if (!receiver || !callback || lifecycle_ == Lifecycle::Stopped)
		return;
	if (lifecycle_ != Lifecycle::Ready || stopCallback_) {
		// guardedReceiver не удерживает чужой объект. Отложенный отказ также подавляется при shutdown.
		const QPointer<QObject> guardedReceiver(receiver);
		QTimer::singleShot(0, this, [this, guardedReceiver, callback = std::move(callback)] {
			if (guardedReceiver && lifecycle_ != Lifecycle::Stopped)
				callback(false);
		});
		return;
	}
	stopGeneration_ = streamingGeneration_;
	stopReceiver_ = receiver;
	stopCallback_ = std::move(callback);
	stopDispatched_ = false;
	// Исчезновение receiver прекращает не только доставку, но и дальнейшую попытку остановить выход.
	stopReceiverConnection_ = connect(receiver, &QObject::destroyed, this, [this] { cancelStreamingStop(); });
	stopTimeoutMs_ = std::clamp(timeoutMs, 1, 5000);
	stopElapsed_.start();
	stopDeadlineTimer_.start(stopTimeoutMs_);
	stopPollTimer_.start();
	// Даже уже остановленный выход завершается асинхронно, после возврата stopStreaming.
	QTimer::singleShot(0, this, &OBSController::checkStreamingStop);
}

// Проверяет только stopGeneration_; Starting с false-флагами ждёт активности либо достоверного Stopped.
void OBSController::checkStreamingStop()
{
	if (!stopCallback_ || lifecycle_ != Lifecycle::Ready)
		return;
	if (!stopReceiver_) {
		cancelStreamingStop();
		return;
	}
	if (stopElapsed_.elapsed() >= stopTimeoutMs_) {
		finishStreamingStop(false);
		return;
	}
	if (streamingGeneration_ != stopGeneration_) {
		finishStreamingStop(false);
		return;
	}
	// streaming не раскрывает output и читается только в Ready до возврата из Exit.
	const auto streaming = frontend_.streamingState();
	if (!streaming.available)
		return;
	if (streamingPhase_ == StreamingPhase::Stopped && !streaming.active && !streaming.reconnecting) {
		finishStreamingStop(true);
		return;
	}
	if (!stopDispatched_ && (streaming.active || streaming.reconnecting)) {
		// Устанавливается до void-вызова: синхронное событие адаптера не должно послать второй stop.
		stopDispatched_ = true;
		frontend_.requestStreamingStop();
	}
}

// confirmed — наблюдённый исход; снимает таймеры до callback, допускающего удаление или новый запрос.
void OBSController::finishStreamingStop(bool confirmed)
{
	if (!stopCallback_)
		return;
	// receiver и callback сохраняются локально, поскольку очистка освобождает поля текущего ожидания.
	const QPointer<QObject> receiver = stopReceiver_;
	auto callback = std::move(stopCallback_);
	cancelStreamingStop();
	if (receiver && lifecycle_ == Lifecycle::Ready)
		callback(confirmed);
}

// Отмена не подтверждает остановку и не вызывает callback; после неё не остаётся активного опроса.
void OBSController::cancelStreamingStop() noexcept
{
	stopPollTimer_.stop();
	stopDeadlineTimer_.stop();
	QObject::disconnect(stopReceiverConnection_);
	stopReceiverConnection_ = {};
	stopReceiver_.clear();
	stopCallback_ = {};
	stopDispatched_ = false;
}

} // namespace gost
