#include "connection-controller.hpp"

#include "api/api-client.hpp"
#include "api/event-client.hpp"
#include "core/settings-store.hpp"
#include "core/state-store.hpp"

#include <QElapsedTimer>
#include <QRandomGenerator>
#include <QThread>
#include <QTimer>
#include <QUrl>

#include <algorithm>
#include <deque>
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
	bool stopped = false;
	bool active = false;
	bool profileResolved = false;
	bool autoStarted = false;

	bool current(api::RequestContext received) const { return !stopped && active && received == context; }

	void syncPreferences()
	{
		state.setPreferences(settings.autoConnect(), settings.reconnect(), settings.unsaved(), settings.error(),
				     settings.errorKey());
	}

	void invalidate(bool connection)
	{
		if (connection)
			++context.connection;
		++context.selection;
		retry.stop();
		api.cancelAll();
		events.close();
	}

	void configure() { api.configure(QUrl(state.serverOrigin()), context); }

	void resolveSelection(QString notice = {}, QString noticeKey = {})
	{
		state.setSelection(profileResolved
					   ? settings.selectedStream(state.serverOrigin(), settings.profileUuid())
					   : QString{},
				   profileResolved, std::move(notice), std::move(noticeKey));
	}

	void schedule(Phase phase, const api::Error &error)
	{
		Q_UNUSED(error);
		if (!active || stopped)
			return;
		state.markRuntimeStale();
		if (!settings.reconnect()) {
			active = false;
			state.setConnection(state.serverOrigin(), ConnectionStatus::Error,
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
		state.setConnection(state.serverOrigin(), ConnectionStatus::Reconnecting,
				    QStringLiteral("Нет связи с сервером. Ожидается повторное подключение."),
				    QStringLiteral("Error.Reconnecting"));
		retry.start(delay);
	}

	void start(Phase phase)
	{
		if (!active || stopped)
			return;
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
			snapshot();
			return;
		}
		api.getReady(&owner, [this](auto ctx, api::Result<api::Health> result) {
			if (!current(ctx))
				return;
			if (!result) {
				schedule(Phase::Handshake, result.error.value_or(api::Error{}));
				return;
			}
			state.setReady(std::move(*result.value));
			if (!current(ctx))
				return;
			api.getCapabilities(&owner, [this](auto capsContext, api::Result<api::Capabilities> caps) {
				if (!current(capsContext))
					return;
				if (!caps) {
					schedule(Phase::Handshake, caps.error.value_or(api::Error{}));
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
						schedule(Phase::Handshake, streams.error.value_or(api::Error{}));
						return;
					}
					const bool complete = streams.value->complete;
					state.setStreams(std::move(streams.value->items), complete);
					if (!current(listContext))
						return;
					resolveSelection(state.profileNotice(), state.profileNoticeKey());
					if (!current(listContext))
						return;
					if (!complete) {
						schedule(Phase::Handshake, api::Error{});
						return;
					}
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
				schedule(Phase::Snapshot, result.error.value_or(api::Error{}));
				return;
			}
			if (!current(ctx))
				return;
			subscribe();
		});
	}

	void subscribe()
	{
		const auto expected = context;
		state.setConnection(state.serverOrigin(), ConnectionStatus::Connected);
		if (!current(expected))
			return;
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
				state.setConnection(state.serverOrigin(), ConnectionStatus::Connected);
				return true;
			},
			[this](auto ctx, api::Error error, bool requiresSnapshot) {
				if (!current(ctx))
					return;
				schedule(requiresSnapshot ? Phase::Snapshot : Phase::Events, error);
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
	if (!enabled && impl_->retry.isActive())
		disconnectFromServer();
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
