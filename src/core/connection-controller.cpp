#include "connection-controller.hpp"

#include "api/api-client.hpp"
#include "api/event-client.hpp"
#include "core/settings-store.hpp"
#include "core/state-store.hpp"

#include <QElapsedTimer>
#include <QPointer>
#include <QRandomGenerator>
#include <QThread>
#include <QTimer>
#include <QUrl>

#include <algorithm>
#include <deque>
#include <limits>
#include <utility>

namespace gost {

class ConnectionController::Impl {
public:
	Impl(ConnectionController &owner, StateStore &state, SettingsStore &settings, api::APIClient &api,
	     api::EventClient &events)
		: owner(owner),
		  state(state),
		  settings(settings),
		  api(api),
		  events(events)
	{
		clock.start();
		retry.setSingleShot(true);
		QObject::connect(&retry, &QTimer::timeout, &owner, [this] {
			if (active && !stopped && scheduledContext == context)
				start(nextPhase);
		});
		QObject::connect(&settings, &SettingsStore::changed, &owner, [this] { syncPreferences(); });
		syncPreferences();
		state.setConnection(settings.serverOrigin(), ConnectionStatus::Disconnected);
	}

	enum class Phase { Handshake, Snapshot, Events };
	ConnectionController &owner;
	StateStore &state;
	SettingsStore &settings;
	api::APIClient &api;
	api::EventClient &events;
	api::RequestContext context;
	api::RequestContext scheduledContext;
	QTimer retry;
	QElapsedTimer clock;
	std::deque<qint64> starts;
	Phase nextPhase = Phase::Handshake;
	int failures = 0;
	int sseRejectionStatus = 0;
	int sseRejectionAttempts = 0;
	bool stopped = false;
	bool active = false;
	bool profileResolved = false;
	bool autoStarted = false;

	bool current(api::RequestContext received) const { return !stopped && active && received == context; }

	void syncPreferences()
	{
		state.setPreferences(settings.autoConnect(), settings.reconnect(), settings.sseRejectionRetries(),
				     settings.unsaved(), settings.error(), settings.errorKey());
	}

	void clearSseRejection()
	{
		sseRejectionStatus = 0;
		sseRejectionAttempts = 0;
	}

	void invalidate(bool connection)
	{
		if (connection)
			++context.connection;
		++context.selection;
		retry.stop();
		clearSseRejection();
		api.cancelAll();
		events.close();
		if (!connection && active)
			state.setConnection(state.serverOrigin(), ConnectionStatus::Connecting);
	}

	void configure() { api.configure(QUrl(state.serverOrigin()), context); }

	void resolveSelection(QString notice = {}, QString noticeKey = {})
	{
		state.setSelection(profileResolved
					   ? settings.selectedStream(state.serverOrigin(), settings.profileUuid())
					   : QString{},
				   profileResolved, std::move(notice), std::move(noticeKey));
	}

	void stopWithReason(ConnectionStatus status, QString message, QString key)
	{
		const auto expected = context;
		const QPointer<ConnectionController> guard = &owner;
		active = false;
		retry.stop();
		state.markRuntimeStale();
		if (guard && !stopped && !active && context == expected)
			state.setConnection(state.serverOrigin(), status, std::move(message), std::move(key));
	}

	bool rejectionLimitReached() const
	{
		const int limit = settings.sseRejectionRetries();
		return limit != SettingsStore::UnlimitedSseRejectionRetries && sseRejectionAttempts >= limit;
	}

	void stopAfterRejection()
	{
		const bool badRequest = sseRejectionStatus == 400;
		stopWithReason(
			ConnectionStatus::Error,
			badRequest ? QStringLiteral("Сервер отклонил запрос подписки на события (HTTP 400).")
				   : QStringLiteral("Конфликт состояния сервера при подписке на события (HTTP 409)."),
			badRequest ? QStringLiteral("Error.SseBadRequest") : QStringLiteral("Error.SseConflict"));
	}

	void schedule(Phase phase)
	{
		if (!active || stopped)
			return;
		if (phase == Phase::Snapshot && sseRejectionStatus &&
		    (!settings.reconnect() || rejectionLimitReached())) {
			stopAfterRejection();
			return;
		}
		if (!settings.reconnect()) {
			stopWithReason(ConnectionStatus::Error,
				       QStringLiteral("Нет связи с сервером. Повторите подключение."),
				       QStringLiteral("Error.ConnectionLost"));
			return;
		}
		nextPhase = phase;
		scheduledContext = context;
		const int exponent = failures;
		failures = std::min(failures + 1, 5);
		const int base = std::min(30000, 1000 * (1 << exponent));
		const int jitter = QRandomGenerator::global()->bounded(std::max(1, base / 5));
		const int delay = std::clamp(base - base / 10 + jitter, 1000, 30000);
		const auto expected = context;
		const QPointer<ConnectionController> guard = &owner;
		retry.start(delay);
		state.markRuntimeStale();
		if (guard && current(expected))
			state.setConnection(state.serverOrigin(), ConnectionStatus::Reconnecting,
					    QStringLiteral("Нет связи с сервером. Ожидается повторное подключение."),
					    QStringLiteral("Error.Reconnecting"));
	}

	void start(Phase phase)
	{
		if (!active || stopped)
			return;
		if (phase == Phase::Snapshot && sseRejectionStatus && rejectionLimitReached()) {
			stopAfterRejection();
			return;
		}
		const auto now = clock.elapsed();
		while (!starts.empty() && now - starts.front() >= 10000)
			starts.pop_front();
		if (starts.size() >= 3) {
			nextPhase = phase;
			scheduledContext = context;
			retry.start(static_cast<int>(std::max<qint64>(1, 10000 - (now - starts.front()))));
			return;
		}
		starts.push_back(now);
		configure();
		if (phase == Phase::Events && !state.runtime().cursor.isEmpty()) {
			subscribe();
			return;
		}
		if (phase == Phase::Snapshot) {
			if (sseRejectionStatus && sseRejectionAttempts < std::numeric_limits<int>::max())
				++sseRejectionAttempts;
			snapshot();
			return;
		}
		api.getReady(&owner, [this](auto ctx, api::Result<api::Health> result) {
			if (!current(ctx))
				return;
			if (!result) {
				schedule(Phase::Handshake);
				return;
			}
			state.setReady(std::move(*result.value));
			if (!current(ctx))
				return;
			api.getCapabilities(&owner, [this](auto capsContext, api::Result<api::Capabilities> caps) {
				if (!current(capsContext))
					return;
				if (!caps) {
					schedule(Phase::Handshake);
					return;
				}
				state.setCapabilities(std::move(*caps.value));
				if (!current(capsContext))
					return;
				api.getAllStreams(&owner, [this](auto listContext,
								 api::Result<api::Collection<api::Stream>> streams) {
					if (!current(listContext))
						return;
					if (!streams) {
						schedule(Phase::Handshake);
						return;
					}
					const bool complete = streams.value->complete;
					state.setStreams(std::move(streams.value->items), complete);
					if (!current(listContext))
						return;
					resolveSelection(state.profileNotice(), state.profileNoticeKey());
					if (!current(listContext))
						return;
					snapshot();
				});
			});
		});
	}

	void snapshot()
	{
		api.getSnapshot(&owner, [this](auto ctx, api::Result<api::Anchor> result) {
			if (!current(ctx))
				return;
			if (!result || !state.installAnchor(*result.value)) {
				schedule(Phase::Snapshot);
				return;
			}
			if (!current(ctx))
				return;
			subscribe();
		});
	}

	void handleSseFailure(const api::Error &error, bool requiresSnapshot)
	{
		const int status = error.httpStatus;
		if (error.kind == api::ErrorKind::Http) {
			if (status == 400 || status == 409) {
				sseRejectionStatus = status;
				schedule(Phase::Snapshot);
				return;
			}
			if (status == 204) {
				stopWithReason(ConnectionStatus::Disconnected,
					       QStringLiteral("Сервер завершил подписку на события."),
					       QStringLiteral("Error.SseEnded"));
				return;
			}
			if (status == 404) {
				stopWithReason(ConnectionStatus::Error,
					       QStringLiteral("На сервере не найден маршрут событий (HTTP 404)."),
					       QStringLiteral("Error.SseNotFound"));
				return;
			}
			if (status != 408 && status != 429 && (status < 500 || status > 599)) {
				stopWithReason(ConnectionStatus::Error,
					       QStringLiteral("Сервер отклонил подписку на события."),
					       QStringLiteral("Error.SseRejected"));
				return;
			}
		}
		if (error.kind == api::ErrorKind::Tls) {
			stopWithReason(ConnectionStatus::Error, error.message, QStringLiteral("Error.SseTls"));
			return;
		}
		if (error.kind == api::ErrorKind::Contract && status == 200) {
			stopWithReason(ConnectionStatus::Error,
				       QStringLiteral("Ответ сервера не содержит поток событий SSE."),
				       QStringLiteral("Error.SseInvalidContentType"));
			return;
		}
		schedule(requiresSnapshot ? Phase::Snapshot : Phase::Events);
	}

	void subscribe()
	{
		events.subscribe(
			QUrl(state.serverOrigin()), state.runtime().cursor, context, &owner,
			[this](auto ctx, const api::RuntimeEvent &event) {
				if (!current(ctx))
					return false;
				if (!state.applyEvent(event))
					return false;
				if (!current(ctx))
					return false;
				failures = 0;
				return true;
			},
			[this](auto ctx, api::Error error, bool requiresSnapshot) {
				if (!current(ctx))
					return;
				handleSseFailure(error, requiresSnapshot);
			},
			[this](auto ctx) {
				if (!current(ctx))
					return;
				clearSseRejection();
				state.setConnection(state.serverOrigin(), ConnectionStatus::Connected);
			});
	}

	void connectToServer(const QString &origin)
	{
		if (stopped)
			return;
		const auto canonical = SettingsStore::canonicalOrigin(origin);
		if (!canonical) {
			state.setConnection(
				state.serverOrigin(), state.connectionStatus(),
				QStringLiteral("Укажите адрес http/https без пути, учётных данных, query и fragment."),
				QStringLiteral("Error.InvalidServerUrl"));
			return;
		}
		active = false;
		invalidate(true);
		settings.setServerOrigin(*canonical);
		state.clearServerState();
		state.setConnection(*canonical, ConnectionStatus::Connecting);
		resolveSelection();
		failures = 0;
		active = true;
		start(Phase::Handshake);
	}
};

ConnectionController::ConnectionController(StateStore &state, SettingsStore &settings, api::APIClient &api,
					   api::EventClient &events, QObject *parent)
	: QObject(parent),
	  impl_(std::make_unique<Impl>(*this, state, settings, api, events))
{
}

ConnectionController::~ConnectionController()
{
	shutdown();
}

void ConnectionController::observeProfile(FrontendEvent event, const ProfileDescriptor &descriptor)
{
	Q_ASSERT(QThread::currentThread() == thread());
	auto &self = *impl_;
	if (self.stopped)
		return;
	const bool wasResolved = self.profileResolved;
	const auto resolution = self.settings.observeProfile(event, descriptor);
	self.profileResolved = resolution.resolved;
	const bool selectionChanged = resolution.changed || wasResolved != resolution.resolved;
	if (selectionChanged && !resolution.renamed) {
		self.invalidate(false);
		self.state.clearRuntime();
		self.configure();
	}
	self.resolveSelection(
		selectionChanged && !resolution.renamed
			? QStringLiteral("Профиль OBS изменён. Проверьте соответствие профиля выбранному Stream.")
			: self.state.profileNotice(),
		selectionChanged && !resolution.renamed ? QStringLiteral("Profile.BindingNotice")
							: self.state.profileNoticeKey());
	if (self.active && selectionChanged && !resolution.renamed)
		self.start(Impl::Phase::Handshake);
	if (event == FrontendEvent::FinishedLoading && !self.autoStarted) {
		self.autoStarted = true;
		if (self.settings.autoConnect() && !self.settings.serverOrigin().isEmpty() && !self.active)
			self.connectToServer(self.settings.serverOrigin());
	}
}

void ConnectionController::connectToServer(QString origin)
{
	Q_ASSERT(QThread::currentThread() == thread());
	impl_->connectToServer(origin);
}

void ConnectionController::disconnectFromServer()
{
	auto &self = *impl_;
	if (self.stopped)
		return;
	self.active = false;
	self.invalidate(true);
	self.state.markRuntimeStale();
	self.state.setConnection(self.state.serverOrigin(), ConnectionStatus::Disconnected);
}

void ConnectionController::selectStream(QString streamId)
{
	auto &self = *impl_;
	if (self.stopped || !self.profileResolved || streamId == self.state.selectedStreamId())
		return;
	if (!streamId.isEmpty() && std::none_of(self.state.streams().cbegin(), self.state.streams().cend(),
						[&streamId](const auto &stream) { return stream.id == streamId; }))
		return;
	if (!self.settings.setSelectedStream(self.state.serverOrigin(), self.settings.profileUuid(), streamId))
		return;
	self.invalidate(false);
	self.state.clearRuntime();
	self.resolveSelection();
	if (self.active)
		self.start(Impl::Phase::Handshake);
}

void ConnectionController::setAutoConnect(bool enabled)
{
	if (!impl_->stopped)
		impl_->settings.setAutoConnect(enabled);
}

void ConnectionController::setReconnect(bool enabled)
{
	if (impl_->stopped)
		return;
	impl_->settings.setReconnect(enabled);
	if (!enabled && (impl_->retry.isActive() || impl_->state.connectionStatus() == ConnectionStatus::Reconnecting))
		disconnectFromServer();
}

void ConnectionController::setSseRejectionRetries(int retries)
{
	if (!impl_->stopped)
		impl_->settings.setSseRejectionRetries(retries);
}

void ConnectionController::shutdown()
{
	if (!impl_ || impl_->stopped)
		return;
	impl_->stopped = true;
	impl_->active = false;
	impl_->invalidate(true);
	impl_->api.shutdown();
	impl_->events.shutdown();
	impl_->state.markRuntimeStale();
	impl_->state.setConnection(impl_->state.serverOrigin(), ConnectionStatus::Disconnected);
}

} // namespace gost
