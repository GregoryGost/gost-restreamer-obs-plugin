#include "api/api-client.hpp"
#include "api/event-client.hpp"
#include "core/connection-controller.hpp"
#include "core/settings-store.hpp"
#include "core/state-store.hpp"
#include "fixtures/api-json.hpp"
#include "fixtures/http-server.hpp"
#include "fixtures/workspace-temp.hpp"

#include <QElapsedTimer>
#include <QFile>
#include <QSet>
#include <QTest>
#include <QTimer>
#include <QUrlQuery>

#include <algorithm>

using namespace gost;
using namespace gost::api;

namespace {
ProfileDescriptor profile(const QString &name = QStringLiteral("A"))
{
	return {name, QStringLiteral("profiles/") + name, {QStringLiteral("A"), QStringLiteral("B")}, true};
}
void respond(const HttpServer::Request &request, const QString &streamName = QStringLiteral("Поток"))
{
	// Базовый сценарий даёт пустой runtime и открытый SSE с heartbeat. Специализированный handler
	// перехватывает нужный маршрут раньше этого помощника; он не моделирует все реальные REST-детали.
	if (request.target.startsWith("/api/v1/health/ready"))
		HttpServer::reply(request, R"({"status":"ready","reason":""})");
	else if (request.target.startsWith("/api/v1/system/capabilities"))
		HttpServer::reply(request, fixture::capabilities());
	else if (request.target.startsWith("/api/v1/streams")) {
		auto stream = fixture::stream();
		stream.insert(QStringLiteral("name"), streamName);
		HttpServer::reply(request, fixture::page({stream}));
	} else if (request.target.startsWith("/api/v1/events/snapshot"))
		HttpServer::reply(request, R"({"cursor":"opaque-anchor","sessions":[],"gpu":null})");
	else if (request.target.startsWith("/api/v1/events?"))
		HttpServer::stream(request, ": heartbeat\n\n");
	else
		HttpServer::reply(request, "{}", 404);
}
int requestCount(const HttpServer &server, const QByteArray &prefix)
{
	return int(std::count_if(server.requests.cbegin(), server.requests.cend(),
				 [&](const auto &request) { return request.target.startsWith(prefix); }));
}
HttpServer::Request subscription(const HttpServer &server)
{
	for (auto index = server.requests.size(); index > 0; --index)
		if (server.requests[index - 1].target.startsWith("/api/v1/events?"))
			return server.requests[index - 1];
	return {};
}
QString subscriptionCursor(const HttpServer::Request &request)
{
	const QUrlQuery query(QUrl::fromEncoded("http://local" + request.target));
	return query.queryItemValue(QStringLiteral("cursor"), QUrl::FullyDecoded);
}
void replyWithAnchor(const HttpServer::Request &request, const QString &cursor)
{
	auto anchor = fixture::object(R"({"sessions":[],"gpu":null})");
	anchor.insert(QStringLiteral("cursor"), cursor);
	HttpServer::reply(request, fixture::bytes(anchor));
}
bool readOnly(const HttpServer &server)
{
	return std::all_of(server.requests.cbegin(), server.requests.cend(), [](const auto &request) {
		return request.method == "GET" && !request.target.contains("/start") &&
		       !request.target.contains("/stop");
	});
}

QJsonObject dockOutputMetrics(quint64 videoFrames)
{
	auto metrics = fixture::outputMetrics();
	auto media = metrics.value(QStringLiteral("media")).toObject();
	media.insert(QStringLiteral("video_frames"), QString::number(videoFrames));
	metrics.insert(QStringLiteral("media"), media);
	return metrics;
}

QJsonObject dockRuntimeSession(const QString &id, const QString &sessionInstance, const QString &outputInstance)
{
	auto session = fixture::object(R"({"input":null,"groups":[],"outputs":[],"ffmpeg":[]})");
	session.insert(QStringLiteral("session_id"), id);
	session.insert(QStringLiteral("observation"), fixture::observation(sessionInstance, 20));
	auto output = fixture::observation(outputInstance, 50, QStringLiteral("PUBLISHING"));
	output.insert(QStringLiteral("output_id"), QStringLiteral("output"));
	output.insert(QStringLiteral("metrics"), dockOutputMetrics(200));
	session.insert(QStringLiteral("outputs"), QJsonArray{output});
	return session;
}

QByteArray dockAnchor()
{
	// Две Session имеют одинаковый outputId, но разные instanceId. Это позволяет выявить чтение
	// чужой телеметрии по одному конфигурационному ID без проверки выбранной Session.
	auto anchor = fixture::object(R"({"cursor":"dock-anchor","sessions":[],"gpu":null})");
	anchor.insert(QStringLiteral("sessions"),
		      QJsonArray{dockRuntimeSession(QStringLiteral("session"), QStringLiteral("epoch:1"),
						    QStringLiteral("epoch:2")),
				 dockRuntimeSession(QStringLiteral("other-session"), QStringLiteral("epoch:3"),
						    QStringLiteral("epoch:4"))});
	return fixture::bytes(anchor);
}

QByteArray dockMetricsFrame(const QString &cursor, const QString &sessionId, const QString &instanceId,
			    quint64 revision, quint64 videoFrames)
{
	auto event = fixture::event(QStringLiteral("metrics"), cursor);
	event.insert(QStringLiteral("owner"), QStringLiteral("output"));
	event.insert(QStringLiteral("session_id"), sessionId);
	event.insert(QStringLiteral("output_id"), QStringLiteral("output"));
	event.insert(QStringLiteral("observation"), fixture::observation(instanceId, revision));
	event.insert(QStringLiteral("output"), dockOutputMetrics(videoFrames));
	return fixture::frame(event);
}

void respondWithDockMetadata(const HttpServer::Request &request, bool validExactSession)
{
	// Активный список всегда даёт кандидата, а детальный GET может вернуть другой Stream.
	// current и pending одновременно существуют: запрос pending вместо точного current обнаруживается тестом.
	const QUrl url = QUrl::fromEncoded("http://local" + request.target);
	const auto path = url.path();
	if (path == QStringLiteral("/api/v1/streams")) {
		HttpServer::reply(request,
				  fixture::page({fixture::stream(), fixture::stream(QStringLiteral("other"))}));
	} else if (path == QStringLiteral("/api/v1/events/snapshot")) {
		HttpServer::reply(request, dockAnchor());
	} else if (path == QStringLiteral("/api/v1/streams/stream/sessions")) {
		const auto filter = QUrlQuery(url).queryItemValue(QStringLiteral("state"));
		HttpServer::reply(request, filter == QStringLiteral("RUNNING") ? fixture::page({fixture::session()})
									       : fixture::page({}));
	} else if (path == QStringLiteral("/api/v1/sessions/session")) {
		auto session = fixture::session();
		if (!validExactSession) {
			session.insert(QStringLiteral("stream_id"), QStringLiteral("other"));
			session.insert(QStringLiteral("stream"), fixture::stream(QStringLiteral("other")));
		}
		HttpServer::reply(request, fixture::bytes(session));
	} else if (path == QStringLiteral("/api/v1/sessions/session/outputs")) {
		HttpServer::reply(request, fixture::page({fixture::output()}));
	} else if (path == QStringLiteral("/api/v1/streams/stream/fallback-policy")) {
		HttpServer::reply(request, R"({"revision":"1","fallback":{"asset_id":"asset","watchdog":{
"video_timeout_ns":"1000000000","audio_timeout_ns":"1000000000","recovery_window_ns":"1000000000",
"keyframe_timeout_ns":"1000000000"}}})");
	} else if (path == QStringLiteral("/api/v1/fallback-assets/asset")) {
		auto asset = fixture::asset();
		asset.insert(QStringLiteral("current_generation_id"), QStringLiteral("current"));
		asset.insert(QStringLiteral("pending_generation_id"), QStringLiteral("pending"));
		HttpServer::reply(request, fixture::bytes(asset));
	} else if (path == QStringLiteral("/api/v1/fallback-assets/asset/generations/current")) {
		auto generation = fixture::record(QStringLiteral("current"));
		generation.insert(QStringLiteral("asset_id"), QStringLiteral("asset"));
		generation.insert(QStringLiteral("kind"), QStringLiteral("image"));
		generation.insert(QStringLiteral("status"), QStringLiteral("ready"));
		generation.insert(QStringLiteral("size_bytes"), QStringLiteral("1024"));
		generation.insert(QStringLiteral("failure"), QJsonValue::Null);
		HttpServer::reply(request, fixture::bytes(generation));
	} else {
		respond(request);
	}
}
} // namespace

// Проверяет всю последовательность чтения через настоящие APIClient/EventClient и управляемый HTTP-сервер.
// Настройки изолированы в WorkspaceTemp. Барьерами служат фактические запросы/переходы StateStore;
// отрицательные проверки повторов ждут дольше минимальной паузы повтора с обработкой цикла событий.
class ConnectionControllerTest final : public QObject {
	Q_OBJECT
private slots:
	void connectsWithoutImplicitStreamOrMedia();
	void serverSwitchDiscardsOldResponses();
	void profileSwitchRestoresOnlyItsBinding();
	void dockMetadataRequiresExactSessionForSelectedStream_data();
	void dockMetadataRequiresExactSessionForSelectedStream();
	void resumedSubscriptionRefreshesOnlyAcceptedMetricLane();
	void reconnectUsesConfirmedCursor();
	void handshakeTimeoutRetriesWithoutPrematureConnection();
	void terminalSubscriptionDoesNotRetry_data();
	void terminalSubscriptionDoesNotRetry();
	void rejectedSubscriptionUsesFreshSnapshots_data();
	void rejectedSubscriptionUsesFreshSnapshots();
	void disablingReconnectCancelsRecoverySnapshot_data();
	void disablingReconnectCancelsRecoverySnapshot();
	void transientSubscriptionFailureResumesRecoveryAnchor_data();
	void transientSubscriptionFailureResumesRecoveryAnchor();
	void confirmedSubscriptionResetsRejectionBudget();
	void resyncObtainsNewAnchor();
	void shutdownStopsPendingRetry();
};

void ConnectionControllerTest::connectsWithoutImplicitStreamOrMedia()
{
	// Ответ SSE удерживается после удачного ready, capabilities, списка и снимка. Только выпуск заголовков
	// разрешает Connected. Наличие единственного Stream не даёт права выбрать его или начать эфир.
	WorkspaceTemp temp(QStringLiteral("connection"));
	QVERIFY(temp.isValid());
	HttpServer server;
	QVERIFY(server.listen());
	server.handler = [](const auto &request) {
		if (!request.target.startsWith("/api/v1/events?"))
			respond(request);
	};
	SettingsStore settings(temp.filePath(QStringLiteral("settings.json")));
	settings.setAutoConnect(false);
	StateStore state;
	APIClient api;
	EventClient events;
	ConnectionController controller(state, settings, api, events);
	controller.observeProfile(FrontendEvent::FinishedLoading, profile());
	controller.connectToServer(server.origin().toString());
	QTRY_COMPARE_WITH_TIMEOUT(requestCount(server, "/api/v1/events?"), 1, 3000);
	// Наличие запроса ещё не означает подключение: до заголовков SSE состояние остаётся Connecting.
	QCOMPARE(state.connectionStatus(), ConnectionStatus::Connecting);
	QVERIFY(subscription(server).socket);
	HttpServer::stream(subscription(server));
	QTRY_COMPARE_WITH_TIMEOUT(state.connectionStatus(), ConnectionStatus::Connected, 2000);
	QVERIFY(state.streamsComplete());
	QCOMPARE(state.streams().size(), 1);
	QVERIFY(state.selectedStreamId().isEmpty());
	QVERIFY(!state.selectionAvailable());
	QCOMPARE(state.runtime().cursor, QStringLiteral("opaque-anchor"));
	QVERIFY(state.capabilities().has_value());
	QCOMPARE(state.capabilities()->nvidia.status, QStringLiteral("unverified"));
	QVERIFY(readOnly(server));
	QFile stored(temp.filePath(QStringLiteral("settings.json")));
	QVERIFY(stored.open(QIODevice::ReadOnly));
	const auto serialized = stored.readAll();
	QVERIFY(!serialized.contains(QStringLiteral("publish+%2F/ключ").toUtf8()));
	QVERIFY(!serialized.contains("publication_key"));
	QVERIFY(!serialized.contains("opaque-anchor"));
	controller.shutdown();
}

void ConnectionControllerTest::serverSwitchDiscardsOldResponses()
{
	// Старый ready возвращается уже после начала подключения нового сервера. Проверяется отсутствие
	// продолжения старой цепочки и сохранение данных нового origin, а не просто отмена одного reply.
	WorkspaceTemp temp(QStringLiteral("server-switch"));
	QVERIFY(temp.isValid());
	HttpServer oldServer;
	HttpServer currentServer;
	QVERIFY(oldServer.listen());
	QVERIFY(currentServer.listen());
	currentServer.handler = [](const auto &request) {
		respond(request, QStringLiteral("Новый сервер"));
	};
	SettingsStore settings(temp.filePath(QStringLiteral("settings.json")));
	settings.setAutoConnect(false);
	StateStore state;
	APIClient api;
	EventClient events;
	ConnectionController controller(state, settings, api, events);
	controller.observeProfile(FrontendEvent::FinishedLoading, profile());
	controller.connectToServer(oldServer.origin().toString());
	QTRY_COMPARE_WITH_TIMEOUT(oldServer.requests.size(), 1, 2000);
	controller.connectToServer(currentServer.origin().toString());
	QTRY_COMPARE_WITH_TIMEOUT(requestCount(currentServer, "/api/v1/events?"), 1, 3000);
	HttpServer::reply(oldServer.requests.front(), R"({"status":"old","reason":""})");
	QCOMPARE(state.serverOrigin(), currentServer.origin().toString());
	QCOMPARE(state.ready()->status, QStringLiteral("ready"));
	QCOMPARE(state.streams().front().name, QStringLiteral("Новый сервер"));
	QCOMPARE(requestCount(oldServer, "/api/v1/events?"), 0);
	QVERIFY(state.selectedStreamId().isEmpty());
	QVERIFY(readOnly(oldServer) && readOnly(currentServer));
	controller.shutdown();
}

void ConnectionControllerTest::profileSwitchRestoresOnlyItsBinding()
{
	// Профили A/B различаются UUID. После явного Changing/Changed у B нет выбора A; возврат к A
	// восстанавливает его Stream без медиаопераций, несмотря на тот же origin и серверный список.
	WorkspaceTemp temp(QStringLiteral("profile-switch"));
	QVERIFY(temp.isValid());
	HttpServer server;
	QVERIFY(server.listen());
	server.handler = [](const auto &request) {
		respond(request);
	};
	SettingsStore settings(temp.filePath(QStringLiteral("settings.json")));
	settings.setAutoConnect(false);
	StateStore state;
	APIClient api;
	EventClient events;
	ConnectionController controller(state, settings, api, events);
	controller.observeProfile(FrontendEvent::FinishedLoading, profile());
	controller.connectToServer(server.origin().toString());
	QTRY_VERIFY_WITH_TIMEOUT(state.streamsComplete(), 3000);
	controller.selectStream(QStringLiteral("stream"));
	QCOMPARE(state.selectedStreamId(), QStringLiteral("stream"));
	controller.observeProfile(FrontendEvent::ProfileChanging, profile());
	controller.observeProfile(FrontendEvent::ProfileChanged, profile(QStringLiteral("B")));
	QVERIFY(state.selectedStreamId().isEmpty());
	QVERIFY(!state.profileNotice().isEmpty());
	controller.observeProfile(FrontendEvent::ProfileChanging, profile(QStringLiteral("B")));
	controller.observeProfile(FrontendEvent::ProfileChanged, profile());
	QCOMPARE(state.selectedStreamId(), QStringLiteral("stream"));
	QVERIFY(readOnly(server));
	controller.shutdown();
}

void ConnectionControllerTest::dockMetadataRequiresExactSessionForSelectedStream_data()
{
	QTest::addColumn<bool>("validExactSession");
	QTest::newRow("matching-session") << true;
	QTest::newRow("session-from-another-stream") << false;
}

void ConnectionControllerTest::dockMetadataRequiresExactSessionForSelectedStream()
{
	// Заголовки SSE сначала удерживаются, затем отдельно задерживается current Generation.
	// Session/Outputs обязаны публиковаться раньше медленного fallback, но только при подтверждении
	// принадлежности детального GET. Anchor другого Stream не является кандидатом Dock.
	QFETCH(bool, validExactSession);
	WorkspaceTemp temp(QStringLiteral("dock-context"));
	QVERIFY(temp.isValid());
	HttpServer server;
	QVERIFY(server.listen());
	HttpServer::Request pendingGeneration;
	server.handler = [&](const auto &request) {
		if (request.target == "/api/v1/fallback-assets/asset/generations/current")
			pendingGeneration = request;
		else if (!request.target.startsWith("/api/v1/events?"))
			respondWithDockMetadata(request, validExactSession);
	};
	SettingsStore settings(temp.filePath(QStringLiteral("settings.json")));
	settings.setAutoConnect(false);
	StateStore state;
	APIClient api;
	EventClient events;
	ConnectionController controller(state, settings, api, events);
	const auto origin = server.origin().toString();
	controller.observeProfile(FrontendEvent::FinishedLoading, profile());
	QVERIFY(settings.setSelectedStream(origin, settings.profileUuid(), QStringLiteral("stream")));
	controller.connectToServer(origin);
	QTRY_COMPARE_WITH_TIMEOUT(requestCount(server, "/api/v1/events?"), 1, 3000);
	QVERIFY(state.selectionAvailable());
	QCOMPARE(state.dockContext().streamId, QStringLiteral("stream"));
	QCOMPARE(state.dockContext().profileUuid, settings.profileUuid());
	QVERIFY(state.runtimeEvidence(QStringLiteral("epoch:2")).metrics != RuntimeAvailability::Fresh);
	QVERIFY(!state.dockServer().session);
	QVERIFY(subscription(server).socket);
	HttpServer::stream(subscription(server));
	QTRY_VERIFY_WITH_TIMEOUT(pendingGeneration.socket, 3000);
	if (validExactSession) {
		QTRY_VERIFY_WITH_TIMEOUT(state.dockServer().outputsStatus.complete, 3000);
		QVERIFY(state.dockServer().session);
		QVERIFY(pendingGeneration.socket);
		QVERIFY(state.dockServer().fallback.currentStatus.isEmpty());
	}
	respondWithDockMetadata(pendingGeneration, validExactSession);
	QTRY_COMPARE_WITH_TIMEOUT(state.dockServer().fallback.currentStatus, QStringLiteral("ready"), 3000);
	QCOMPARE(state.dockServer().fallback.assigned, std::optional{true});
	QCOMPARE(state.dockServer().fallback.currentGenerationId, QStringLiteral("current"));
	QCOMPARE(state.dockServer().fallback.pendingGenerationId, QStringLiteral("pending"));
	QCOMPARE(state.runtimeEvidence(QStringLiteral("epoch:2")).metrics, RuntimeAvailability::Fresh);
	QSet<QString> stateFilters;
	for (const auto &request : server.requests) {
		if (!request.target.startsWith("/api/v1/streams/stream/sessions?"))
			continue;
		const QUrlQuery query(QUrl::fromEncoded("http://local" + request.target));
		stateFilters.insert(query.queryItemValue(QStringLiteral("state")));
	}
	const QSet<QString> activeStates{QStringLiteral("STARTING"), QStringLiteral("RUNNING"),
					 QStringLiteral("STOPPING")};
	QCOMPARE(stateFilters, activeStates);
	QCOMPARE(requestCount(server, "/api/v1/streams/other/"), 0);
	QCOMPARE(requestCount(server, "/api/v1/sessions/other-session"), 0);
	QCOMPARE(requestCount(server, "/api/v1/fallback-assets/asset/generations/pending"), 0);
	QVERIFY(std::any_of(server.requests.cbegin(), server.requests.cend(), [](const auto &request) {
		return QUrl::fromEncoded("http://local" + request.target).path() ==
		       QStringLiteral("/api/v1/sessions/session");
	}));
	QCOMPARE(requestCount(server, "/api/v1/sessions/session/outputs"), validExactSession ? 1 : 0);
	if (validExactSession) {
		QVERIFY(state.dockServer().session);
		QCOMPARE(state.dockServer().session->id, QStringLiteral("session"));
		QCOMPARE(state.dockServer().session->state, QStringLiteral("RUNNING"));
		QVERIFY(state.dockServer().outputsStatus.complete);
		QCOMPARE(state.dockServer().outputs.size(), 1);
		QCOMPARE(state.dockServer().outputs.value(QStringLiteral("output")).name, QStringLiteral("Назначение"));
	} else {
		QVERIFY(!state.dockServer().session);
		QVERIFY(state.dockServer().outputs.isEmpty());
	}
	QVERIFY(readOnly(server));
	controller.shutdown();
}

void ConnectionControllerTest::resumedSubscriptionRefreshesOnlyAcceptedMetricLane()
{
	// После обрыва последовательно приходят заголовки, метрика чужой Session, старая и новая
	// ревизия выбранного Output. Только последнее событие освежает его metrics; state остаётся stale.
	// Проверки cursor отличают потреблённое старое событие от нового принятого измерения.
	WorkspaceTemp temp(QStringLiteral("dock-resume"));
	QVERIFY(temp.isValid());
	HttpServer server;
	QVERIFY(server.listen());
	server.handler = [](const auto &request) {
		respondWithDockMetadata(request, true);
	};
	SettingsStore settings(temp.filePath(QStringLiteral("settings.json")));
	settings.setAutoConnect(false);
	StateStore state;
	APIClient api;
	EventClient events;
	ConnectionController controller(state, settings, api, events);
	const auto origin = server.origin().toString();
	controller.observeProfile(FrontendEvent::FinishedLoading, profile());
	QVERIFY(settings.setSelectedStream(origin, settings.profileUuid(), QStringLiteral("stream")));
	controller.connectToServer(origin);
	QTRY_VERIFY_WITH_TIMEOUT(state.dockServer().session, 3000);
	const QString sessionId = state.dockServer().session->id;
	const QString instance = QStringLiteral("epoch:2");
	const auto frames = [&] {
		return state.runtime().sessions.value(sessionId).outputs.value(0).metrics.value.media.videoFrames;
	};
	const auto beforeBreak = subscription(server);
	QVERIFY(beforeBreak.socket);
	const QString currentCursor = QStringLiteral("selected-current");
	beforeBreak.socket->write(dockMetricsFrame(currentCursor, sessionId, instance, 10, 320));
	QTRY_COMPARE_WITH_TIMEOUT(state.runtime().cursor, currentCursor, 2000);
	QCOMPARE(frames(), quint64{320});
	QCOMPARE(state.runtimeEvidence(instance).metrics, RuntimeAvailability::Fresh);
	beforeBreak.socket->disconnectFromHost();
	QTRY_COMPARE_WITH_TIMEOUT(state.connectionStatus(), ConnectionStatus::Reconnecting, 2000);
	QCOMPARE(state.runtimeEvidence(instance).metrics, RuntimeAvailability::Stale);
	QTRY_COMPARE_WITH_TIMEOUT(requestCount(server, "/api/v1/events?"), 2, 4000);
	QTRY_COMPARE_WITH_TIMEOUT(state.connectionStatus(), ConnectionStatus::Connected, 2000);
	const auto afterBreak = subscription(server);
	QVERIFY(afterBreak.socket);
	QCOMPARE(subscriptionCursor(afterBreak), currentCursor);
	// Принятая подписка восстанавливает транспорт; прежнее измерение остаётся устаревшим до новой ревизии.
	QCOMPARE(state.runtimeEvidence(instance).metrics, RuntimeAvailability::Stale);

	const QString otherCursor = QStringLiteral("another-session-metric");
	afterBreak.socket->write(
		dockMetricsFrame(otherCursor, QStringLiteral("other-session"), QStringLiteral("epoch:4"), 100, 500));
	QTRY_COMPARE_WITH_TIMEOUT(state.runtime().cursor, otherCursor, 2000);
	QCOMPARE(state.runtimeEvidence(instance).metrics, RuntimeAvailability::Stale);
	QCOMPARE(state.runtimeEvidence(QStringLiteral("epoch:4")).metrics, RuntimeAvailability::Fresh);

	const QString oldCursor = QStringLiteral("selected-old");
	afterBreak.socket->write(dockMetricsFrame(oldCursor, sessionId, instance, 9, 15));
	QTRY_COMPARE_WITH_TIMEOUT(state.runtime().cursor, oldCursor, 2000);
	QCOMPARE(state.runtimeEvidence(instance).metrics, RuntimeAvailability::Stale);
	QCOMPARE(frames(), quint64{320});

	const QString nextCursor = QStringLiteral("selected-next");
	afterBreak.socket->write(dockMetricsFrame(nextCursor, sessionId, instance, 11, 365));
	QTRY_COMPARE_WITH_TIMEOUT(state.runtime().cursor, nextCursor, 2000);
	QCOMPARE(state.runtimeEvidence(instance).metrics, RuntimeAvailability::Fresh);
	QCOMPARE(state.runtimeEvidence(instance).state, RuntimeAvailability::Stale);
	QCOMPARE(frames(), quint64{365});
	QVERIFY(readOnly(server));
	controller.shutdown();
}

void ConnectionControllerTest::reconnectUsesConfirmedCursor()
{
	// После полного применённого кадра записывается незавершённый UTF-8 хвост с другим id и EOF.
	// Возобновление должно использовать только первый cursor, сохранив единственный исходный snapshot.
	WorkspaceTemp temp(QStringLiteral("reconnect"));
	QVERIFY(temp.isValid());
	HttpServer server;
	QVERIFY(server.listen());
	server.handler = [&](const auto &request) {
		if (!request.target.startsWith("/api/v1/events?") || requestCount(server, "/api/v1/events?") == 1)
			respond(request);
	};
	SettingsStore settings(temp.filePath(QStringLiteral("settings.json")));
	settings.setAutoConnect(false);
	StateStore state;
	APIClient api;
	EventClient events;
	ConnectionController controller(state, settings, api, events);
	controller.observeProfile(FrontendEvent::FinishedLoading, profile());
	controller.connectToServer(server.origin().toString());
	QTRY_COMPARE_WITH_TIMEOUT(requestCount(server, "/api/v1/events?"), 1, 3000);
	QTRY_COMPARE_WITH_TIMEOUT(state.connectionStatus(), ConnectionStatus::Connected, 2000);
	auto first = subscription(server);
	QVERIFY(first.socket);
	first.socket->write(fixture::frame(fixture::event(QStringLiteral("state"), QStringLiteral("opaque/%2B+last"))));
	QTRY_COMPARE_WITH_TIMEOUT(state.runtime().cursor, QStringLiteral("opaque/%2B+last"), 2000);
	const auto unfinished = "event: state\nid: unfinished\ndata: \"" + QByteArray::fromHex("e2");
	QCOMPARE(first.socket->write(unfinished), qint64(unfinished.size()));
	first.socket->disconnectFromHost();
	QTRY_COMPARE_WITH_TIMEOUT(requestCount(server, "/api/v1/events?"), 2, 4000);
	QCOMPARE(state.connectionStatus(), ConnectionStatus::Reconnecting);
	QCOMPARE(requestCount(server, "/api/v1/events/snapshot"), 1);
	QCOMPARE(subscriptionCursor(subscription(server)), QStringLiteral("opaque/%2B+last"));
	HttpServer::stream(subscription(server));
	QTRY_COMPARE_WITH_TIMEOUT(state.connectionStatus(), ConnectionStatus::Connected, 2000);
	QVERIFY(readOnly(server));
	controller.shutdown();
}

void ConnectionControllerTest::handshakeTimeoutRetriesWithoutPrematureConnection()
{
	// Сервер получает subscribe, но не отдаёт заголовки. Таймаут касается только ожидания заголовков;
	// повтор использует прежний anchor и не успевает объявить Connected до реального второго 200 SSE.
	WorkspaceTemp temp(QStringLiteral("handshake-timeout"));
	QVERIFY(temp.isValid());
	HttpServer server;
	QVERIFY(server.listen());
	server.handler = [](const auto &request) {
		if (!request.target.startsWith("/api/v1/events?"))
			respond(request);
	};
	SettingsStore settings(temp.filePath(QStringLiteral("settings.json")));
	settings.setAutoConnect(false);
	StateStore state;
	APIClient api;
	Limits limits;
	limits.sseHandshakeMs = 600;
	EventClient events(limits);
	bool connected = false;
	ConnectionController controller(state, settings, api, events);
	connect(&state, &StateStore::serverStateChanged, &controller,
		[&] { connected |= state.connectionStatus() == ConnectionStatus::Connected; });
	controller.observeProfile(FrontendEvent::FinishedLoading, profile());
	controller.connectToServer(server.origin().toString());
	QTRY_COMPARE_WITH_TIMEOUT(requestCount(server, "/api/v1/events?"), 1, 3000);
	QTRY_COMPARE_WITH_TIMEOUT(state.connectionStatus(), ConnectionStatus::Reconnecting, 2500);
	QTRY_COMPARE_WITH_TIMEOUT(requestCount(server, "/api/v1/events?"), 2, 3000);
	QVERIFY(!connected);
	QCOMPARE(state.connectionStatus(), ConnectionStatus::Reconnecting);
	QCOMPARE(requestCount(server, "/api/v1/events/snapshot"), 1);
	QCOMPARE(subscriptionCursor(subscription(server)), QStringLiteral("opaque-anchor"));
	HttpServer::stream(subscription(server), {}, 200, "text/event-stream; charset=utf-8");
	QTRY_COMPARE_WITH_TIMEOUT(state.connectionStatus(), ConnectionStatus::Connected, 2000);
	controller.shutdown();
}

void ConnectionControllerTest::terminalSubscriptionDoesNotRetry_data()
{
	QTest::addColumn<int>("httpStatus");
	QTest::addColumn<QByteArray>("contentType");
	QTest::addColumn<QString>("reasonKey");
	QTest::newRow("204") << 204 << QByteArray("text/event-stream") << QStringLiteral("Error.SseEnded");
	QTest::newRow("404") << 404 << QByteArray("application/json") << QStringLiteral("Error.SseNotFound");
	QTest::newRow("wrong-media")
		<< 200 << QByteArray("application/json") << QStringLiteral("Error.SseInvalidContentType");
}

void ConnectionControllerTest::terminalSubscriptionDoesNotRetry()
{
	// Разные терминальные причины должны сохранить свой безопасный UI-ключ. Окно 1400 мс
	// превышает минимальную паузу повтора и обнаруживает ошибочно оставленный retry-таймер.
	QFETCH(int, httpStatus);
	QFETCH(QByteArray, contentType);
	QFETCH(QString, reasonKey);
	WorkspaceTemp temp(QStringLiteral("terminal-subscription"));
	QVERIFY(temp.isValid());
	HttpServer server;
	QVERIFY(server.listen());
	server.handler = [&](const auto &request) {
		if (request.target.startsWith("/api/v1/events?"))
			HttpServer::reply(request, httpStatus == 204 ? QByteArray{} : QByteArray("private-server-body"),
					  httpStatus, contentType);
		else
			respond(request);
	};
	SettingsStore settings(temp.filePath(QStringLiteral("settings.json")));
	settings.setAutoConnect(false);
	StateStore state;
	APIClient api;
	EventClient events;
	bool connected = false;
	ConnectionController controller(state, settings, api, events);
	connect(&state, &StateStore::serverStateChanged, &controller,
		[&] { connected |= state.connectionStatus() == ConnectionStatus::Connected; });
	controller.observeProfile(FrontendEvent::FinishedLoading, profile());
	controller.connectToServer(server.origin().toString());
	const auto expected = httpStatus == 204 ? ConnectionStatus::Disconnected : ConnectionStatus::Error;
	QTRY_COMPARE_WITH_TIMEOUT(state.safeErrorKey(), reasonKey, 3000);
	QCOMPARE(state.connectionStatus(), expected);
	QVERIFY(!state.safeError().isEmpty());
	QVERIFY(!state.safeError().contains(QStringLiteral("private-server-body")));
	QTimer retryWindow;
	retryWindow.setSingleShot(true);
	retryWindow.start(1400);
	QTRY_VERIFY_WITH_TIMEOUT(!retryWindow.isActive(), 2500);
	QCOMPARE(requestCount(server, "/api/v1/events?"), 1);
	QCOMPARE(requestCount(server, "/api/v1/events/snapshot"), 1);
	QCOMPARE(state.connectionStatus(), expected);
	QCOMPARE(state.safeErrorKey(), reasonKey);
	QVERIFY(!connected);
	controller.shutdown();
}

void ConnectionControllerTest::rejectedSubscriptionUsesFreshSnapshots_data()
{
	QTest::addColumn<int>("retryLimit");
	QTest::addColumn<bool>("reconnect");
	QTest::addColumn<bool>("failedRetrySnapshot");
	QTest::addColumn<int>("snapshotCount");
	QTest::addColumn<int>("eventCount");
	QTest::newRow("default-three") << -2 << true << false << 4 << 4;
	QTest::newRow("zero") << 0 << true << false << 1 << 1;
	QTest::newRow("one-rejected-retry") << 1 << true << false << 2 << 2;
	QTest::newRow("one-failed-snapshot") << 1 << true << true << 2 << 1;
	QTest::newRow("unlimited-until-cancel") << -1 << true << false << 5 << 5;
	QTest::newRow("reconnect-disabled") << -1 << false << false << 1 << 1;
}

void ConnectionControllerTest::rejectedSubscriptionUsesFreshSnapshots()
{
	// Каждый 400/409 обязан сменить anchor. Таблица различает начальную попытку и число новых
	// циклов: стандартный лимит 3 даёт 4 снимка/подписки; неудачный повторный snapshot тоже расходует цикл.
	// Неограниченный режим останавливается самим тестом после заданного числа попыток, сохраняя паузы и ограничение частоты.
	QFETCH(int, retryLimit);
	QFETCH(bool, reconnect);
	QFETCH(bool, failedRetrySnapshot);
	QFETCH(int, snapshotCount);
	QFETCH(int, eventCount);
	WorkspaceTemp temp(QStringLiteral("rejected-subscription"));
	QVERIFY(temp.isValid());
	HttpServer server;
	QVERIFY(server.listen());
	QElapsedTimer clock;
	QList<qint64> snapshotTimes;
	QList<qint64> rejectionTimes;
	QStringList cursors;
	server.handler = [&](const auto &request) {
		if (request.target.startsWith("/api/v1/events/snapshot")) {
			snapshotTimes.append(clock.elapsed());
			if (failedRetrySnapshot && snapshotTimes.size() == 2)
				HttpServer::reply(request, "{}", 503);
			else
				replyWithAnchor(request, QStringLiteral("anchor-%1").arg(snapshotTimes.size()));
		} else if (request.target.startsWith("/api/v1/events?")) {
			cursors.append(subscriptionCursor(request));
			rejectionTimes.append(clock.elapsed());
			HttpServer::reply(request, "{}", cursors.size() % 2 == 0 ? 409 : 400);
		} else
			respond(request);
	};
	SettingsStore settings(temp.filePath(QStringLiteral("settings.json")));
	settings.setAutoConnect(false);
	settings.setReconnect(reconnect);
	if (retryLimit >= -1)
		settings.setSseRejectionRetries(retryLimit);
	StateStore state;
	APIClient api;
	EventClient events;
	bool connected = false;
	ConnectionController controller(state, settings, api, events);
	connect(&state, &StateStore::serverStateChanged, &controller,
		[&] { connected |= state.connectionStatus() == ConnectionStatus::Connected; });
	controller.observeProfile(FrontendEvent::FinishedLoading, profile());
	clock.start();
	controller.connectToServer(server.origin().toString());
	if (retryLimit == -1 && reconnect) {
		QTRY_COMPARE_WITH_TIMEOUT(cursors.size(), eventCount, 26000);
		QCOMPARE(state.connectionStatus(), ConnectionStatus::Reconnecting);
		controller.shutdown();
	} else {
		QTRY_COMPARE_WITH_TIMEOUT(state.connectionStatus(), ConnectionStatus::Error, 20000);
		QCOMPARE(state.safeErrorKey(), eventCount % 2 == 0 ? QStringLiteral("Error.SseConflict")
								   : QStringLiteral("Error.SseBadRequest"));
	}
	QCOMPARE(snapshotTimes.size(), snapshotCount);
	QCOMPARE(cursors.size(), eventCount);
	QCOMPARE(requestCount(server, "/api/v1/health/ready"), 1);
	QVERIFY(!connected);
	for (int index = 0; index < eventCount; ++index)
		QCOMPARE(cursors[index], QStringLiteral("anchor-%1").arg(index + 1));
	for (qsizetype index = 1; index < snapshotTimes.size(); ++index)
		QVERIFY(snapshotTimes[index] - rejectionTimes[index - 1] >= 850);
	if (snapshotTimes.size() >= 3)
		QVERIFY(snapshotTimes[2] - rejectionTimes[1] >= 1650);
	// Нижние границы допускают неточность таймеров, но должны обнаруживать отсутствие пауз и ограничения частоты.
	if (snapshotTimes.size() >= 4)
		QVERIFY(snapshotTimes[3] - snapshotTimes[0] >= 8500);
	QVERIFY(readOnly(server));
	controller.shutdown();
}

void ConnectionControllerTest::disablingReconnectCancelsRecoverySnapshot_data()
{
	QTest::addColumn<int>("httpStatus");
	QTest::newRow("400") << 400;
	QTest::newRow("409") << 409;
}

void ConnectionControllerTest::disablingReconnectCancelsRecoverySnapshot()
{
	// Восстановительный GET намеренно держится активным во время выключения reconnect.
	// Позднее тело не должно заменить anchor; исчезновение сокета и полное окно следующего retry
	// проверяют отмену как текущей сети, так и будущих шагов подключения.
	QFETCH(int, httpStatus);
	WorkspaceTemp temp(QStringLiteral("cancel-recovery-snapshot"));
	QVERIFY(temp.isValid());
	HttpServer server;
	QVERIFY(server.listen());
	HttpServer::Request recovery;
	server.handler = [&](const auto &request) {
		if (request.target.startsWith("/api/v1/events?")) {
			HttpServer::reply(request, "{}", httpStatus);
		} else if (request.target.startsWith("/api/v1/events/snapshot") &&
			   requestCount(server, "/api/v1/events/snapshot") == 2) {
			recovery = request;
		} else {
			respond(request);
		}
	};
	SettingsStore settings(temp.filePath(QStringLiteral("settings.json")));
	settings.setAutoConnect(false);
	settings.setReconnect(true);
	StateStore state;
	APIClient api;
	EventClient events;
	ConnectionController controller(state, settings, api, events);
	controller.observeProfile(FrontendEvent::FinishedLoading, profile());
	controller.connectToServer(server.origin().toString());
	QTRY_VERIFY_WITH_TIMEOUT(recovery.socket, 4000);
	QCOMPARE(state.connectionStatus(), ConnectionStatus::Reconnecting);
	QCOMPARE(state.runtime().cursor, QStringLiteral("opaque-anchor"));
	QCOMPARE(requestCount(server, "/api/v1/events?"), 1);
	const auto requestsBeforeCancel = server.requests.size();

	controller.setReconnect(false);
	QCOMPARE(state.connectionStatus(), ConnectionStatus::Disconnected);
	QVERIFY(!settings.reconnect());
	const QByteArray lateBody = R"({"cursor":"late-recovery-anchor","sessions":[],"gpu":null})";
	if (recovery.socket)
		recovery.socket->write("HTTP/1.1 200 Response\r\nContent-Type: application/json\r\nContent-Length: " +
				       QByteArray::number(lateBody.size()) + "\r\n\r\n" + lateBody);
	QTRY_VERIFY_WITH_TIMEOUT(recovery.socket.isNull(), 2000);
	QTimer retryWindow;
	retryWindow.setSingleShot(true);
	retryWindow.start(2400);
	QTRY_VERIFY_WITH_TIMEOUT(!retryWindow.isActive(), 3500);
	QCOMPARE(state.connectionStatus(), ConnectionStatus::Disconnected);
	QVERIFY(state.runtime().cursor != QStringLiteral("late-recovery-anchor"));
	QCOMPARE(server.requests.size(), requestsBeforeCancel);
	QCOMPARE(requestCount(server, "/api/v1/events/snapshot"), 2);
	QCOMPARE(requestCount(server, "/api/v1/events?"), 1);
	QVERIFY(readOnly(server));
	controller.shutdown();
}

void ConnectionControllerTest::transientSubscriptionFailureResumesRecoveryAnchor_data()
{
	QTest::addColumn<int>("resumedStatus");
	QTest::newRow("accepted-resume") << 200;
	QTest::newRow("rejected-resume") << 409;
}

void ConnectionControllerTest::transientSubscriptionFailureResumesRecoveryAnchor()
{
	// Цепочка 409 -> anchor-2 -> 503 должна повторить SSE от anchor-2 без третьего snapshot.
	// Если это возобновление снова даёт 409, прежний конечный бюджет всё ещё исчерпан: успех снимка
	// и временный транспортный отказ не являются подтверждением подписки и не обнуляют счётчик.
	QFETCH(int, resumedStatus);
	WorkspaceTemp temp(QStringLiteral("transient-subscription-failure"));
	QVERIFY(temp.isValid());
	HttpServer server;
	QVERIFY(server.listen());
	int snapshots = 0;
	QStringList cursors;
	server.handler = [&](const auto &request) {
		if (request.target.startsWith("/api/v1/events/snapshot")) {
			replyWithAnchor(request, QStringLiteral("anchor-%1").arg(++snapshots));
		} else if (request.target.startsWith("/api/v1/events?")) {
			cursors.append(subscriptionCursor(request));
			if (cursors.size() == 1)
				HttpServer::reply(request, "{}", 409);
			else if (cursors.size() == 2)
				HttpServer::reply(request, "{}", 503);
		} else {
			respond(request);
		}
	};
	SettingsStore settings(temp.filePath(QStringLiteral("settings.json")));
	settings.setAutoConnect(false);
	settings.setReconnect(true);
	settings.setSseRejectionRetries(1);
	StateStore state;
	APIClient api;
	EventClient events;
	ConnectionController controller(state, settings, api, events);
	controller.observeProfile(FrontendEvent::FinishedLoading, profile());
	controller.connectToServer(server.origin().toString());
	QTRY_VERIFY_WITH_TIMEOUT(cursors.size() >= 3 || state.connectionStatus() == ConnectionStatus::Error, 8000);
	const QStringList expected{QStringLiteral("anchor-1"), QStringLiteral("anchor-2"), QStringLiteral("anchor-2")};
	QCOMPARE(cursors, expected);
	QCOMPARE(snapshots, 2);
	QCOMPARE(state.connectionStatus(), ConnectionStatus::Reconnecting);
	const auto resumed = subscription(server);
	QVERIFY(resumed.socket);
	if (resumedStatus == 200) {
		HttpServer::stream(resumed);
		QTRY_COMPARE_WITH_TIMEOUT(state.connectionStatus(), ConnectionStatus::Connected, 2000);
	} else {
		HttpServer::reply(resumed, "{}", resumedStatus);
		QTRY_COMPARE_WITH_TIMEOUT(state.connectionStatus(), ConnectionStatus::Error, 2000);
		QCOMPARE(state.safeErrorKey(), QStringLiteral("Error.SseConflict"));
	}
	QCOMPARE(cursors, expected);
	QCOMPARE(snapshots, 2);
	QVERIFY(readOnly(server));
	controller.shutdown();
}

void ConnectionControllerTest::confirmedSubscriptionResetsRejectionBudget()
{
	// Подтверждённая подписка между двумя 409 восстанавливает право на один новый цикл snapshot.
	// Ожидаемая последовательность cursor фиксирует и возобновление от старого снимка, и переход к anchor-3.
	WorkspaceTemp temp(QStringLiteral("rejection-budget-reset"));
	QVERIFY(temp.isValid());
	HttpServer server;
	QVERIFY(server.listen());
	int snapshots = 0;
	QStringList cursors;
	server.handler = [&](const auto &request) {
		if (request.target.startsWith("/api/v1/events/snapshot")) {
			replyWithAnchor(request, QStringLiteral("anchor-%1").arg(++snapshots));
		} else if (request.target.startsWith("/api/v1/events?")) {
			cursors.append(subscriptionCursor(request));
			if (cursors.size() % 2 == 0)
				HttpServer::stream(request);
			else
				HttpServer::reply(request, "{}", 409);
		} else {
			respond(request);
		}
	};
	SettingsStore settings(temp.filePath(QStringLiteral("settings.json")));
	settings.setAutoConnect(false);
	settings.setSseRejectionRetries(1);
	StateStore state;
	APIClient api;
	EventClient events;
	ConnectionController controller(state, settings, api, events);
	controller.observeProfile(FrontendEvent::FinishedLoading, profile());
	controller.connectToServer(server.origin().toString());
	QTRY_COMPARE_WITH_TIMEOUT(state.connectionStatus(), ConnectionStatus::Connected, 4000);
	QCOMPARE(snapshots, 2);
	const auto accepted = subscription(server);
	QVERIFY(accepted.socket);
	accepted.socket->disconnectFromHost();
	QTRY_COMPARE_WITH_TIMEOUT(state.connectionStatus(), ConnectionStatus::Reconnecting, 2500);
	QTRY_COMPARE_WITH_TIMEOUT(cursors.size(), 4, 18000);
	QTRY_COMPARE_WITH_TIMEOUT(state.connectionStatus(), ConnectionStatus::Connected, 2000);
	const QStringList expected{QStringLiteral("anchor-1"), QStringLiteral("anchor-2"), QStringLiteral("anchor-2"),
				   QStringLiteral("anchor-3")};
	QCOMPARE(cursors, expected);
	QCOMPARE(snapshots, 3);
	controller.shutdown();
}

void ConnectionControllerTest::resyncObtainsNewAnchor()
{
	// Управляющий кадр должен запустить закреплённый /events/snapshot и новую подписку с его cursor.
	// Произвольный snapshot_url в данных события не является разрешением следовать другому маршруту.
	WorkspaceTemp temp(QStringLiteral("resync"));
	QVERIFY(temp.isValid());
	HttpServer server;
	QVERIFY(server.listen());
	int snapshots = 0;
	server.handler = [&](const auto &request) {
		if (request.target.startsWith("/api/v1/events/snapshot"))
			replyWithAnchor(request, ++snapshots == 1 ? QStringLiteral("opaque-anchor")
								  : QStringLiteral("fresh-anchor"));
		else
			respond(request);
	};
	SettingsStore settings(temp.filePath(QStringLiteral("settings.json")));
	settings.setAutoConnect(false);
	StateStore state;
	APIClient api;
	EventClient events;
	ConnectionController controller(state, settings, api, events);
	controller.observeProfile(FrontendEvent::FinishedLoading, profile());
	controller.connectToServer(server.origin().toString());
	QTRY_COMPARE_WITH_TIMEOUT(requestCount(server, "/api/v1/events?"), 1, 3000);
	QVERIFY(subscription(server).socket);
	subscription(server).socket->write(
		"event: resync_required\ndata: {\"reason\":\"cursor_expired\",\"snapshot_url\":\"/arbitrary\"}\n\n");
	QTRY_COMPARE_WITH_TIMEOUT(requestCount(server, "/api/v1/events/snapshot"), 2, 4000);
	QTRY_COMPARE_WITH_TIMEOUT(requestCount(server, "/api/v1/events?"), 2, 3000);
	QCOMPARE(subscriptionCursor(subscription(server)), QStringLiteral("fresh-anchor"));
	QCOMPARE(requestCount(server, "/arbitrary"), 0);
	QVERIFY(readOnly(server));
	controller.shutdown();
}

void ConnectionControllerTest::shutdownStopsPendingRetry()
{
	WorkspaceTemp temp(QStringLiteral("shutdown"));
	QVERIFY(temp.isValid());
	HttpServer server;
	QVERIFY(server.listen());
	server.handler = [](const auto &request) {
		if (request.target.startsWith("/api/v1/events?"))
			HttpServer::reply(request, "{}", 409);
		else
			respond(request);
	};
	SettingsStore settings(temp.filePath(QStringLiteral("settings.json")));
	settings.setAutoConnect(false);
	StateStore state;
	APIClient api;
	EventClient events;
	ConnectionController controller(state, settings, api, events);
	controller.observeProfile(FrontendEvent::FinishedLoading, profile());
	controller.connectToServer(server.origin().toString());
	QTRY_COMPARE_WITH_TIMEOUT(state.connectionStatus(), ConnectionStatus::Reconnecting, 2000);
	controller.shutdown();
	const auto requests = server.requests.size();
	QTimer retryWindow;
	retryWindow.setSingleShot(true);
	// Окно наблюдения длиннее минимальной паузы повтора; одного оборота Qt для отрицательной проверки недостаточно.
	retryWindow.start(1300);
	QTRY_VERIFY_WITH_TIMEOUT(!retryWindow.isActive(), 2500);
	QCOMPARE(server.requests.size(), requests);
	QCOMPARE(state.connectionStatus(), ConnectionStatus::Disconnected);
	controller.connectToServer(server.origin().toString());
	controller.selectStream(QStringLiteral("stream"));
	QCOMPARE(server.requests.size(), requests);
}

QTEST_GUILESS_MAIN(ConnectionControllerTest)
#include "connection-controller-test.moc"
