#include "api/api-client.hpp"
#include "api/event-client.hpp"
#include "core/connection-controller.hpp"
#include "core/settings-store.hpp"
#include "core/state-store.hpp"
#include "fixtures/api-json.hpp"
#include "fixtures/http-server.hpp"
#include "fixtures/workspace-temp.hpp"

#include <QElapsedTimer>
#include <QCoreApplication>
#include <QEvent>
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

// Возвращает независимые метрики выхода с числовым videoFrames для проверки обновления Dock.
QJsonObject dockOutputMetrics(quint64 videoFrames)
{
	// metrics содержит метрики выхода; media задаёт изменяемые счётчики его медиапотока.
	auto metrics = fixture::outputMetrics();
	auto media = metrics.value(QStringLiteral("media")).toObject();
	media.insert(QStringLiteral("video_frames"), QJsonValue(qint64(videoFrames)));
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

// Отвечает на request метаданными двух Stream, активной Session, её выходов и заглушки для проверок Dock.
// При validExactSession=false детальный GET относит найденную Session к другому Stream, хотя список
// возвращает кандидата. Одновременные current/pending поколения позволяют выявить чтение pending
// вместо current; остальные маршруты обслуживает respond, включая готовность сервера и SSE.
void respondWithDockMetadata(const HttpServer::Request &request, bool validExactSession)
{
	// url отделяет параметры фильтрации от пути полученного запроса.
	const QUrl url = QUrl::fromEncoded("http://local" + request.target);
	// path выбирает точный ресурс без совпадений по префиксам соседних маршрутов.
	const auto path = url.path();
	if (path == QStringLiteral("/api/v1/streams")) {
		HttpServer::reply(request,
				  fixture::page({fixture::stream(), fixture::stream(QStringLiteral("other"))}));
	} else if (path == QStringLiteral("/api/v1/events/snapshot")) {
		HttpServer::reply(request, dockAnchor());
	} else if (path == QStringLiteral("/api/v1/streams/stream/sessions")) {
		// filter ограничивает непустой список состоянием RUNNING; другие активные состояния дают пустой список.
		const auto filter = QUrlQuery(url).queryItemValue(QStringLiteral("state"));
		HttpServer::reply(request, filter == QStringLiteral("RUNNING") ? fixture::page({fixture::session()})
									       : fixture::page({}));
	} else if (path == QStringLiteral("/api/v1/sessions/session")) {
		// session подтверждает принадлежность кандидата либо намеренно указывает другой Stream.
		auto session = fixture::session();
		if (!validExactSession) {
			session.insert(QStringLiteral("stream_id"), QStringLiteral("other"));
			session.insert(QStringLiteral("stream"), fixture::stream(QStringLiteral("other")));
		}
		HttpServer::reply(request, fixture::bytes(session));
	} else if (path == QStringLiteral("/api/v1/sessions/session/outputs")) {
		HttpServer::reply(request, fixture::page({fixture::output()}));
	} else if (path == QStringLiteral("/api/v1/streams/stream/fallback-policy")) {
		HttpServer::reply(request, R"({"revision":1,"fallback":{"asset_id":"asset","watchdog":{
"video_timeout_ns":1000000000,"audio_timeout_ns":1000000000,"recovery_window_ns":1000000000,
"keyframe_timeout_ns":1000000000}}})");
	} else if (path == QStringLiteral("/api/v1/fallback-assets/asset")) {
		// asset одновременно ссылается на текущее и ожидающее поколения; готовность читается только у current.
		auto asset = fixture::asset();
		asset.insert(QStringLiteral("current_generation_id"), QStringLiteral("current"));
		asset.insert(QStringLiteral("pending_generation_id"), QStringLiteral("pending"));
		HttpServer::reply(request, fixture::bytes(asset));
	} else if (path == QStringLiteral("/api/v1/fallback-assets/asset/generations/current")) {
		// generation содержит готовое текущее поколение точного asset, не подменяя его ожидающим pending.
		auto generation = fixture::record(QStringLiteral("current"));
		generation.insert(QStringLiteral("asset_id"), QStringLiteral("asset"));
		generation.insert(QStringLiteral("kind"), QStringLiteral("image"));
		generation.insert(QStringLiteral("status"), QStringLiteral("ready"));
		generation.insert(QStringLiteral("size_bytes"), QJsonValue(qint64(1024)));
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
	// Прежние false-поля не отключают штатный запуск и восстановление; до FinishedLoading сеть не начинается.
	void startupAndRecoveryIgnoreLegacyDisabledFlags();
	// Каталог назначений публикуется до завершения независимых чтений Session и заглушки.
	void destinationsAppearWithoutSessionOrManagement();
	// Задаёт полное, частичное, ошибочное и противоречивое повторное чтение каталога.
	void destinationReadPreservesValidatedKnowledge_data();
	// Только полный согласованный каталог удаляет прежние записи; ошибка сохраняет их устаревшими.
	void destinationReadPreservesValidatedKnowledge();
	// Поздний GET до записи не заменяет каталог после смены конфигурационного поколения.
	void configurationInvalidationRejectsLateCatalogWhileFallbackIsPending();
	// Поздний ответ прежнего Stream не заполняет каталог нового выбора.
	void streamSwitchRejectsLateDestinationRead();

	// Различает полный пустой поиск, неполную страницу и ошибку чтения Session.
	void emptySessionDiscovery_data();
	// Только полное успешное отсутствие подтверждает Fresh+complete без активной Session.
	void emptySessionDiscovery();
	// Новая Session после пустого snapshot подхватывается без команд и без изменения паузы/выходов.
	void lateSessionPreservesIntentions_data();
	// Проверяет подхват STARTING/RUNNING с исходными сохранёнными намерениями.
	void lateSessionPreservesIntentions();
	// Начальное подключение подтверждается заголовками SSE и не выбирает Stream или медиаоперацию автоматически.
	void connectsWithoutImplicitStreamOrMedia();
	// Поздний ответ прежнего сервера не продолжает его цепочку и не заменяет выбранный origin.
	void serverSwitchDiscardsOldResponses();
	// Смена UUID профиля восстанавливает только собственную сохранённую привязку Stream.
	void profileSwitchRestoresOnlyItsBinding();
	// Задаёт точную Session выбранного Stream и несовместимый детальный ответ другого Stream.
	void dockMetadataRequiresExactSessionForSelectedStream_data();
	// Метаданные Dock требуют подтверждённой принадлежности Session и точного current поколения заглушки.
	void dockMetadataRequiresExactSessionForSelectedStream();
	// Перечисляет отсоединение, полный список и смены идентичности, влияющие на сохранение состава Output.
	void partialOutputReadPreservesOnlyTheSameSession_data();
	// Неполное чтение сохраняет известный состав только в исходной Session и контексте.
	void partialOutputReadPreservesOnlyTheSameSession();
	// После восстановления только новая принятая метрика освежает соответствующую ось выбранного Output.
	void resumedSubscriptionRefreshesOnlyAcceptedMetricLane();
	// Переподключение продолжает последний применённый курсор, исключая незавершённый хвост кадра.
	void reconnectUsesConfirmedCursor();
	// Таймаут заголовков повторяет подписку без преждевременного состояния Connected.
	void handshakeTimeoutRetriesWithoutPrematureConnection();
	// Перечисляет терминальные HTTP и MIME причины отказа подписки.
	void terminalSubscriptionDoesNotRetry_data();
	// Терминальный отказ сохраняет безопасную причину и не оставляет автоматический повтор.
	void terminalSubscriptionDoesNotRetry();
	// Задаёт конечные и неограниченные бюджеты новых снимков после отказа SSE.
	void rejectedSubscriptionUsesFreshSnapshots_data();
	// Отказы 400/409 требуют свежие снимки и расходуют общий конечный бюджет циклов восстановления.
	void rejectedSubscriptionUsesFreshSnapshots();
	// Задаёт оба отказа подписки, запускающие восстановительное чтение снимка.
	void explicitDisconnectCancelsRecoverySnapshot_data();
	// Явное отключение отменяет текущий снимок и будущие повторы, исключая позднее применение.
	void explicitDisconnectCancelsRecoverySnapshot();
	// Задаёт успешное и повторно отклонённое возобновление от восстановительного снимка.
	void transientSubscriptionFailureResumesRecoveryAnchor_data();
	// Временная ошибка возобновляет SSE от текущего снимка без сброса бюджета отказов.
	void transientSubscriptionFailureResumesRecoveryAnchor();
	// Подтверждённая подписка восстанавливает право на новый цикл снимка после следующего отказа.
	void confirmedSubscriptionResetsRejectionBudget();
	// resync_required читает закреплённый маршрут снимка и продолжает его курсор без произвольных переходов.
	void resyncObtainsNewAnchor();
	// Завершение контроллера отменяет ожидающий повтор и запрещает поздние запросы.
	void shutdownStopsPendingRetry();
};

// Сохранённый origin начинает ровно один handshake после готовности OBS и восстанавливает SSE после обрыва.
// Прежние false-поля остаются совместимыми данными файла, но не меняют текущую политику клиента.
void ConnectionControllerTest::startupAndRecoveryIgnoreLegacyDisabledFlags()
{
	// temp/path ограничивают настройки workspace; server обслуживает только синтетические GET.
	WorkspaceTemp temp(QStringLiteral("automatic-connection"));
	QVERIFY(temp.isValid());
	const auto path = temp.filePath(QStringLiteral("settings.json"));
	HttpServer server;
	QVERIFY(server.listen());
	// Обработчик request возвращает полный handshake и открытую SSE-подписку без медиакоманд.
	server.handler = [](const auto &request) {
		respond(request);
	};
	{
		// persisted записывает выключенные поля старого формата до создания рабочего контроллера.
		SettingsStore persisted(path);
		QVERIFY(persisted.setServerOrigin(server.origin().toString()));
		persisted.setAutoConnect(false);
		persisted.setReconnect(false);
		QVERIFY(persisted.save());
	}
	// settings перечитывает файл; state хранит наблюдения, api/events обеспечивают настоящую асинхронную сеть.
	SettingsStore settings(path);
	StateStore state;
	APIClient api;
	EventClient events;
	ConnectionController controller(state, settings, api, events);
	QVERIFY(!settings.autoConnect() && !settings.reconnect());
	QVERIFY(state.autoConnect() && state.reconnect());
	controller.observeProfile(FrontendEvent::ProfileChanged, profile());
	QCoreApplication::sendPostedEvents(nullptr, QEvent::MetaCall);
	QCOMPARE(server.requests.size(), 0);
	QCOMPARE(state.connectionStatus(), ConnectionStatus::Disconnected);
	controller.observeProfile(FrontendEvent::FinishedLoading, profile());
	QTRY_COMPARE_WITH_TIMEOUT(state.connectionStatus(), ConnectionStatus::Connected, 3000);
	QCOMPARE(requestCount(server, "/api/v1/health/ready"), 1);
	QCOMPARE(requestCount(server, "/api/v1/events?"), 1);
	QVERIFY(state.selectedStreamId().isEmpty());
	controller.observeProfile(FrontendEvent::FinishedLoading, profile());
	QCOMPARE(state.connectionStatus(), ConnectionStatus::Connected);
	QCOMPARE(requestCount(server, "/api/v1/health/ready"), 1);
	// first сохраняет сокет подтверждённой подписки, чтобы воспроизвести временный сетевой обрыв.
	const auto first = subscription(server);
	QVERIFY(first.socket);
	first.socket->disconnectFromHost();
	QTRY_COMPARE_WITH_TIMEOUT(requestCount(server, "/api/v1/events?"), 2, 5000);
	QTRY_COMPARE_WITH_TIMEOUT(state.connectionStatus(), ConnectionStatus::Connected, 2000);
	QCOMPARE(requestCount(server, "/api/v1/health/ready"), 1);
	QCOMPARE(requestCount(server, "/api/v1/events/snapshot"), 1);
	QCOMPARE(subscriptionCursor(subscription(server)), QStringLiteral("opaque-anchor"));
	QVERIFY(!settings.autoConnect() && !settings.reconnect());
	QVERIFY(state.autoConnect() && state.reconnect());
	QVERIFY(readOnly(server));
	controller.shutdown();
}

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

void ConnectionControllerTest::partialOutputReadPreservesOnlyTheSameSession_data()
{
	QTest::addColumn<QString>("boundary");
	for (const auto *boundary : {"detach", "complete-list", "session", "stream", "profile"})
		QTest::newRow(boundary) << QString::fromLatin1(boundary);
}

void ConnectionControllerTest::partialOutputReadPreservesOnlyTheSameSession()
{
	// Известный attached Output намеренно отсутствует в runtime. Повторный обход получает только
	// первый элемент: следующая страница превышает общий бюджет, оставаясь допустимым HTTP-ответом.
	// Только явный detach, полный список или новая идентичность могут отменить прежний состав.
	QFETCH(QString, boundary);
	WorkspaceTemp temp(QStringLiteral("partial-output-metadata"));
	QVERIFY(temp.isValid());
	HttpServer server;
	QVERIFY(server.listen());
	int phase = 0;
	int outputReads = 0;
	QString streamId = QStringLiteral("stream");
	QString sessionId = QStringLiteral("session");
	const auto output = [&](const QString &id, bool attached = true) {
		auto value = fixture::output();
		value.insert(QStringLiteral("id"), id);
		value.insert(QStringLiteral("session_id"), sessionId);
		value.insert(QStringLiteral("attached"), attached);
		auto destination = fixture::destination();
		destination.insert(QStringLiteral("stream_id"), streamId);
		destination.insert(QStringLiteral("name"), id + QStringLiteral(" name"));
		value.insert(QStringLiteral("destination"), destination);
		return value;
	};
	const auto exactSession = [&] {
		auto value = fixture::session();
		value.insert(QStringLiteral("id"), sessionId);
		value.insert(QStringLiteral("stream_id"), streamId);
		value.insert(QStringLiteral("stream"), fixture::stream(streamId));
		return value;
	};
	const auto complete = fixture::page({output(QStringLiteral("known")), output(QStringLiteral("seen"))});
	const auto first = fixture::page({output(QStringLiteral("seen"))}, QStringLiteral("next"));
	auto large = output(QStringLiteral("omitted"));
	auto destination = large.value(QStringLiteral("destination")).toObject();
	destination.insert(QStringLiteral("name"), QString(20000, QLatin1Char('x')));
	large.insert(QStringLiteral("destination"), destination);
	const auto second = fixture::page({large});
	const auto budget = qMax(complete.size(), first.size()) + 64;
	QVERIFY(first.size() < budget && second.size() > budget);
	server.handler = [&](const auto &request) {
		const QUrl url = QUrl::fromEncoded("http://local" + request.target);
		const auto path = url.path();
		if (path == QStringLiteral("/api/v1/streams")) {
			HttpServer::reply(request,
					  fixture::page({fixture::stream(), fixture::stream(QStringLiteral("other"))}));
		} else if (path == QStringLiteral("/api/v1/events/snapshot")) {
			auto runtime = dockRuntimeSession(QStringLiteral("session"), QStringLiteral("epoch:1"),
							  QStringLiteral("epoch:2"));
			runtime.insert(QStringLiteral("outputs"), QJsonArray{});
			HttpServer::reply(request,
					  fixture::bytes({{QStringLiteral("cursor"), QStringLiteral("partial-anchor")},
							  {QStringLiteral("sessions"), QJsonArray{runtime}},
							  {QStringLiteral("gpu"), QJsonValue::Null}}));
		} else if (path == QStringLiteral("/api/v1/streams/") + streamId + QStringLiteral("/sessions")) {
			HttpServer::reply(request, QUrlQuery(url).queryItemValue(QStringLiteral("state")) ==
								   QStringLiteral("RUNNING")
							   ? fixture::page({exactSession()})
							   : fixture::page({}));
		} else if (path == QStringLiteral("/api/v1/sessions/") + sessionId) {
			HttpServer::reply(request, fixture::bytes(exactSession()));
		} else if (path == QStringLiteral("/api/v1/sessions/") + sessionId + QStringLiteral("/outputs")) {
			if (QUrlQuery(url).hasQueryItem(QStringLiteral("cursor"))) {
				HttpServer::reply(request, second);
			} else {
				++outputReads;
				if (phase == 0)
					HttpServer::reply(request, complete);
				else if (phase == 2 && boundary == QStringLiteral("complete-list"))
					HttpServer::reply(request, fixture::page({output(QStringLiteral("seen"))}));
				else if (phase == 2 && boundary == QStringLiteral("detach"))
					HttpServer::reply(request,
							  fixture::page({output(QStringLiteral("known"), false)},
									QStringLiteral("next")));
				else
					HttpServer::reply(request, fixture::page({output(QStringLiteral("seen"))},
										 QStringLiteral("next")));
			}
		} else {
			respond(request);
		}
	};
	SettingsStore settings(temp.filePath(QStringLiteral("settings.json")));
	settings.setAutoConnect(false);
	StateStore state;
	Limits limits;
	limits.pageWalkBytes = budget;
	APIClient api(limits);
	EventClient events;
	ConnectionController controller(state, settings, api, events);
	controller.observeProfile(FrontendEvent::FinishedLoading, profile());
	QVERIFY(settings.setSelectedStream(server.origin().toString(), settings.profileUuid(), streamId));
	controller.connectToServer(server.origin().toString());
	QTRY_VERIFY_WITH_TIMEOUT(state.dockServer().outputsStatus.complete, 3000);
	QCOMPARE(state.dockServer().outputs.size(), 2);
	QVERIFY(state.runtime().sessions.value(sessionId).outputs.isEmpty());
	QVERIFY(state.dockServer().outputs.value(QStringLiteral("known")).attached);
	const auto refresh = [&](quint64 revision) {
		auto event = fixture::event(QStringLiteral("state"), QStringLiteral("refresh-%1").arg(revision));
		event.insert(QStringLiteral("observation"), fixture::observation(QStringLiteral("epoch:1"), revision));
		subscription(server).socket->write(fixture::frame(event));
	};
	phase = 1;
	QVERIFY(subscription(server).socket);
	refresh(21);
	QTRY_VERIFY_WITH_TIMEOUT(outputReads == 2 &&
					 state.dockServer().outputsStatus.availability == RuntimeAvailability::Fresh &&
					 !state.dockServer().outputsStatus.complete,
				 3000);
	QCOMPARE(state.dockServer().outputs.size(), 2);
	QCOMPARE(state.dockServer().outputs.value(QStringLiteral("known")).name, QStringLiteral("known name"));
	QVERIFY(state.dockServer().outputs.value(QStringLiteral("known")).attached);
	QVERIFY(!state.dockServer().outputs.contains(QStringLiteral("omitted")));
	phase = 2;
	if (boundary == QStringLiteral("session")) {
		sessionId = QStringLiteral("replacement-session");
		refresh(22);
	} else if (boundary == QStringLiteral("stream")) {
		streamId = QStringLiteral("other");
		controller.selectStream(streamId);
	} else if (boundary == QStringLiteral("profile")) {
		controller.observeProfile(FrontendEvent::ProfileChanged, profile(QStringLiteral("B")));
		QVERIFY(state.dockServer().outputs.isEmpty());
		QVERIFY(settings.setSelectedStream(server.origin().toString(), settings.profileUuid(), streamId));
		controller.selectStream(streamId);
	} else {
		refresh(22);
	}
	QTRY_VERIFY_WITH_TIMEOUT(
		outputReads >= 3 && state.dockServer().outputsStatus.availability == RuntimeAvailability::Fresh, 3000);
	QVERIFY(state.dockServer().session);
	QCOMPARE(state.dockServer().session->id, sessionId);
	if (boundary == QStringLiteral("detach")) {
		QVERIFY(state.dockServer().outputs.contains(QStringLiteral("known")));
		QVERIFY(!state.dockServer().outputs.value(QStringLiteral("known")).attached);
		QVERIFY(state.dockServer().outputs.contains(QStringLiteral("seen")));
	} else {
		QVERIFY(!state.dockServer().outputs.contains(QStringLiteral("known")));
		QVERIFY(state.dockServer().outputs.contains(QStringLiteral("seen")));
	}
	QCOMPARE(state.dockServer().outputsStatus.complete, boundary == QStringLiteral("complete-list"));
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
	QTest::newRow("legacy-reconnect-false") << 1 << false << false << 2 << 2;
}

void ConnectionControllerTest::rejectedSubscriptionUsesFreshSnapshots()
{
	// Каждый 400/409 обязан сменить anchor. Таблица различает начальную попытку и число новых
	// циклов: стандартный лимит 3 даёт 4 снимка/подписки; неудачный повторный snapshot тоже расходует цикл.
	// Неограниченный режим останавливается самим тестом после заданного числа попыток, сохраняя паузы и ограничение частоты.
	// retryLimit задаёт число разрешённых восстановительных циклов; -2 сохраняет штатный default.
	QFETCH(int, retryLimit);
	// reconnect задаёт прежнее поле файла, которое не должно менять действующее восстановление.
	QFETCH(bool, reconnect);
	// failedRetrySnapshot выбирает отказ второго снимка для проверки расходования бюджета.
	QFETCH(bool, failedRetrySnapshot);
	// snapshotCount задаёт ожидаемое число начальных и восстановительных снимков.
	QFETCH(int, snapshotCount);
	// eventCount задаёт ожидаемое число попыток подписки.
	QFETCH(int, eventCount);
	// temp изолирует настройки сценария внутри workspace.
	WorkspaceTemp temp(QStringLiteral("rejected-subscription"));
	QVERIFY(temp.isValid());
	// server обслуживает управляемые ответы только на петлевом интерфейсе.
	HttpServer server;
	QVERIFY(server.listen());
	// clock измеряет фактические паузы восстановления монотонным временем.
	QElapsedTimer clock;
	// snapshotTimes сохраняет время начала каждого снимка в миллисекундах.
	QList<qint64> snapshotTimes;
	// rejectionTimes сохраняет время отказа каждой попытки подписки в миллисекундах.
	QList<qint64> rejectionTimes;
	// cursors сохраняет курсоры фактически отправленных подписок.
	QStringList cursors;
	// Обработчик request чередует HTTP 400/409 и создаёт отдельный cursor для каждого нового снимка.
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
	// settings содержит изолированные preferences, включая совместимое поле reconnect.
	SettingsStore settings(temp.filePath(QStringLiteral("settings.json")));
	settings.setAutoConnect(false);
	settings.setReconnect(reconnect);
	if (retryLimit >= -1)
		settings.setSseRejectionRetries(retryLimit);
	// state хранит наблюдаемые переходы подключения и установленный снимок.
	StateStore state;
	// api выполняет асинхронные REST-чтения сценария.
	APIClient api;
	// events разбирает SSE и управляет временем жизни подписки.
	EventClient events;
	// connected фиксирует любое преждевременное достижение Connected; callback сигнала обновляет этот признак.
	bool connected = false;
	// controller координирует поколения подключения, восстановления и отмены.
	ConnectionController controller(state, settings, api, events);
	// Обработчик serverStateChanged фиксирует Connected даже между итоговыми проверками.
	connect(&state, &StateStore::serverStateChanged, &controller,
		[&] { connected |= state.connectionStatus() == ConnectionStatus::Connected; });
	controller.observeProfile(FrontendEvent::FinishedLoading, profile());
	clock.start();
	controller.connectToServer(server.origin().toString());
	if (retryLimit == -1) {
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
	// index адресует проверяемый курсор или временную отметку предыдущего восстановления.
	for (int index = 0; index < eventCount; ++index)
		QCOMPARE(cursors[index], QStringLiteral("anchor-%1").arg(index + 1));
	// index адресует проверяемый курсор или временную отметку предыдущего восстановления.
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

void ConnectionControllerTest::explicitDisconnectCancelsRecoverySnapshot_data()
{
	QTest::addColumn<int>("httpStatus");
	QTest::newRow("400") << 400;
	QTest::newRow("409") << 409;
}

void ConnectionControllerTest::explicitDisconnectCancelsRecoverySnapshot()
{
	// Восстановительный GET намеренно держится активным во время явного отключения.
	// Позднее тело не должно заменить anchor; исчезновение сокета и полное окно следующего retry
	// проверяют отмену как текущей сети, так и будущих шагов подключения.
	// httpStatus выбирает 400 либо 409 как причину нового снимка.
	QFETCH(int, httpStatus);
	// temp изолирует настройки сценария внутри workspace.
	WorkspaceTemp temp(QStringLiteral("cancel-recovery-snapshot"));
	QVERIFY(temp.isValid());
	// server обслуживает управляемые ответы только на петлевом интерфейсе.
	HttpServer server;
	QVERIFY(server.listen());
	// recovery удерживает восстановительный GET, чтобы проверить его отмену и поздний ответ.
	HttpServer::Request recovery;
	// Обработчик request удерживает второй snapshot и отклоняет исходную подписку.
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
	// settings содержит изолированные preferences, включая совместимое поле reconnect.
	SettingsStore settings(temp.filePath(QStringLiteral("settings.json")));
	settings.setAutoConnect(false);
	settings.setReconnect(true);
	// state хранит наблюдаемые переходы подключения и установленный снимок.
	StateStore state;
	// api выполняет асинхронные REST-чтения сценария.
	APIClient api;
	// events разбирает SSE и управляет временем жизни подписки.
	EventClient events;
	// controller координирует поколения подключения, восстановления и отмены.
	ConnectionController controller(state, settings, api, events);
	controller.observeProfile(FrontendEvent::FinishedLoading, profile());
	controller.connectToServer(server.origin().toString());
	QTRY_VERIFY_WITH_TIMEOUT(recovery.socket, 4000);
	QCOMPARE(state.connectionStatus(), ConnectionStatus::Reconnecting);
	QCOMPARE(state.runtime().cursor, QStringLiteral("opaque-anchor"));
	QCOMPARE(requestCount(server, "/api/v1/events?"), 1);
	// requestsBeforeCancel сохраняет число сетевых шагов до явного отключения.
	const auto requestsBeforeCancel = server.requests.size();

	controller.disconnectFromServer();
	QCOMPARE(state.connectionStatus(), ConnectionStatus::Disconnected);
	QVERIFY(settings.reconnect());
	// lateBody содержит отличимый поздний снимок, который не должен примениться после отключения.
	const QByteArray lateBody = R"({"cursor":"late-recovery-anchor","sessions":[],"gpu":null})";
	if (recovery.socket)
		recovery.socket->write("HTTP/1.1 200 Response\r\nContent-Type: application/json\r\nContent-Length: " +
				       QByteArray::number(lateBody.size()) + "\r\n\r\n" + lateBody);
	QTRY_VERIFY_WITH_TIMEOUT(recovery.socket.isNull(), 2000);
	// retryWindow ограничивает отрицательное наблюдение окном длиннее следующей минимальной паузы retry.
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

void ConnectionControllerTest::emptySessionDiscovery_data()
{
	QTest::addColumn<int>("boundary");
	QTest::newRow("complete-empty") << 0;
	QTest::newRow("incomplete-list") << 1;
	QTest::newRow("failed-read") << 2;
}

void ConnectionControllerTest::emptySessionDiscovery()
{
	// boundary выбирает проверяемую границу достоверности результата поиска Session.
	QFETCH(int, boundary);
	// temp предоставляет отдельный каталог; settings хранит привязку профиля; server управляет полнотой поиска.
	WorkspaceTemp temp(QStringLiteral("empty-discovery"));
	QVERIFY(temp.isValid());
	// server обслуживает изолированное подключение и управляемую подписку событий.
	HttpServer server;
	QVERIFY(server.listen());
	// searches считает три фильтра активных состояний до публикации единого результата.
	int searches = 0;
	// Обработчик server.handler считает запросы request к списку Session и задаёт границу boundary:
	// полный пустой ответ, продолжение списка или HTTP-отказ. Остальные чтения обслуживает respond.
	server.handler = [&](const auto &request) {
		if (request.target.startsWith("/api/v1/streams/stream/sessions")) {
			++searches;
			HttpServer::reply(request,
					  fixture::page({}, boundary == 1 ? QStringLiteral("more") : QString{}),
					  boundary == 2 ? 503 : 200);
		} else if (request.target.startsWith("/api/v1/streams/stream/fallback-policy")) {
			HttpServer::reply(request, R"({"revision":1,"fallback":null})");
		} else {
			respond(request);
		}
	};
	SettingsStore settings(temp.filePath(QStringLiteral("settings.json")));
	settings.setAutoConnect(false);
	// state хранит наблюдения; api читает REST; events принимает SSE; controller согласует порядок чтений.
	StateStore state;
	APIClient api;
	EventClient events;
	ConnectionController controller(state, settings, api, events);
	controller.observeProfile(FrontendEvent::FinishedLoading, profile());
	QVERIFY(settings.setSelectedStream(server.origin().toString(), settings.profileUuid(),
					   QStringLiteral("stream")));
	controller.connectToServer(server.origin().toString());
	QTRY_VERIFY_WITH_TIMEOUT(
		searches >= 3 && state.dockServer().fallback.availability == RuntimeAvailability::Fresh, 3000);
	if (boundary == 0) {
		QTRY_VERIFY_WITH_TIMEOUT(state.dockServer().sessionStatus.availability == RuntimeAvailability::Fresh,
					 3000);
		QVERIFY(state.dockServer().sessionStatus.complete);
		QVERIFY(!state.dockServer().session);
	} else {
		QVERIFY(state.dockServer().sessionStatus.availability != RuntimeAvailability::Fresh ||
			!state.dockServer().sessionStatus.complete);
	}
	QVERIFY(readOnly(server));
}

void ConnectionControllerTest::lateSessionPreservesIntentions_data()
{
	QTest::addColumn<QString>("sessionState");
	QTest::newRow("starting") << QStringLiteral("STARTING");
	QTest::newRow("running-paused") << QStringLiteral("RUNNING");
}

void ConnectionControllerTest::lateSessionPreservesIntentions()
{
	// sessionState задаёт опубликованное сервером активное состояние новой Session.
	QFETCH(QString, sessionState);
	// temp изолирует каталог; settings сохраняет пользовательский выбор, исключая историю операций.
	WorkspaceTemp temp(QStringLiteral("late-session"));
	QVERIFY(temp.isValid());
	// server обслуживает изолированное подключение и управляемую подписку событий.
	HttpServer server;
	QVERIFY(server.listen());
	// appeared переключает сервер с полного отсутствия на точную выбранную Session.
	bool appeared = false;
	// Обработчик server.handler возвращает для request новую Session только после appeared, сохраняя
	// sessionState, паузу и намерения её выходов. До появления он подтверждает полное отсутствие Session.
	server.handler = [&](const auto &request) {
		// url содержит полный адрес запроса; path отделяет адрес ресурса от параметра state в строке запроса.
		const auto url = QUrl::fromEncoded("http://local" + request.target);
		const auto path = url.path();
		if (path == QStringLiteral("/api/v1/streams/stream/sessions")) {
			// session сохраняет паузу, заданную сервером, независимо от наблюдаемого RUNNING/STARTING.
			auto session = fixture::session();
			session.insert(QStringLiteral("state"), sessionState);
			session.insert(QStringLiteral("desired_paused"), true);
			HttpServer::reply(request,
					  fixture::page(appeared && QUrlQuery(url).queryItemValue(
									    QStringLiteral("state")) == sessionState
								? QJsonArray{session}
								: QJsonArray{}));
		} else if (path == QStringLiteral("/api/v1/sessions/session")) {
			// session — независимая REST-проверка принадлежности кандидата Stream.
			auto session = fixture::session();
			session.insert(QStringLiteral("state"), sessionState);
			session.insert(QStringLiteral("desired_paused"), true);
			HttpServer::reply(request, fixture::bytes(session));
		} else if (path == QStringLiteral("/api/v1/sessions/session/outputs")) {
			// stopped остаётся прикреплённым без desired_running; detached сохраняет отсоединение.
			auto stopped = fixture::output();
			stopped.insert(QStringLiteral("id"), QStringLiteral("stopped"));
			stopped.insert(QStringLiteral("desired_running"), false);
			auto detached = stopped;
			detached.insert(QStringLiteral("id"), QStringLiteral("detached"));
			detached.insert(QStringLiteral("attached"), false);
			HttpServer::reply(request, fixture::page({stopped, detached}));
		} else if (path == QStringLiteral("/api/v1/streams/stream/fallback-policy")) {
			HttpServer::reply(request, R"({"revision":1,"fallback":null})");
		} else {
			respond(request);
		}
	};
	SettingsStore settings(temp.filePath(QStringLiteral("settings.json")));
	settings.setAutoConnect(false);
	// state применяет события; api читает REST; events разбирает SSE; controller согласует наблюдения.
	StateStore state;
	APIClient api;
	EventClient events;
	ConnectionController controller(state, settings, api, events);
	controller.observeProfile(FrontendEvent::FinishedLoading, profile());
	QVERIFY(settings.setSelectedStream(server.origin().toString(), settings.profileUuid(),
					   QStringLiteral("stream")));
	controller.connectToServer(server.origin().toString());
	QTRY_VERIFY_WITH_TIMEOUT(state.dockServer().sessionStatus.complete, 3000);
	QVERIFY(!state.dockServer().session);
	QVERIFY(state.runtime().sessions.isEmpty());
	appeared = true;
	// event сообщает новую идентичность исполнения; сохранённая пауза и состав читаются отдельным GET.
	auto event = fixture::event(QStringLiteral("state"), QStringLiteral("new-session-event"));
	event.insert(QStringLiteral("observation"),
		     fixture::observation(QStringLiteral("new-session-instance"), 1, sessionState));
	QVERIFY(subscription(server).socket);
	subscription(server).socket->write(fixture::frame(event));
	QTRY_VERIFY_WITH_TIMEOUT(state.dockServer().session && state.dockServer().outputsStatus.complete, 3000);
	QCOMPARE(state.dockServer().session->id, QStringLiteral("session"));
	QCOMPARE(state.dockServer().session->state, sessionState);
	QVERIFY(state.dockServer().session->desiredPaused);
	QVERIFY(!state.dockServer().outputs.value(QStringLiteral("stopped")).desiredRunning);
	QVERIFY(state.dockServer().outputs.value(QStringLiteral("stopped")).attached);
	QVERIFY(!state.dockServer().outputs.value(QStringLiteral("detached")).attached);
	QVERIFY(!state.dockServer().outputs.value(QStringLiteral("detached")).desiredRunning);
	controller.shutdown();
	// restoredSettings читает сохранённый выбор без прежних намерений в RAM.
	SettingsStore restoredSettings(temp.filePath(QStringLiteral("settings.json")));
	// restoredState хранит новые наблюдения, не наследуя результаты прежнего процесса.
	StateStore restoredState;
	// restoredApi выполняет чтения состояния выбранного сервера.
	APIClient restoredApi;
	// restoredEvents принимает новую подписку событий.
	EventClient restoredEvents;
	// restoredController согласует новое подключение с восстановленной привязкой профиля.
	ConnectionController restoredController(restoredState, restoredSettings, restoredApi, restoredEvents);
	restoredController.observeProfile(FrontendEvent::FinishedLoading, profile());
	QTRY_VERIFY_WITH_TIMEOUT(
		restoredState.dockServer().session && restoredState.dockServer().outputsStatus.complete, 3000);
	QVERIFY(restoredState.dockServer().session->desiredPaused);
	QVERIFY(!restoredState.dockServer().outputs.value(QStringLiteral("stopped")).desiredRunning);
	QVERIFY(!restoredState.dockServer().outputs.value(QStringLiteral("detached")).attached);
	QVERIFY(!restoredState.operations().current);
	QVERIFY(!restoredState.operations().lastInterrupted);
	QVERIFY(readOnly(server));
}

// Чтение имени назначения не требует текущей Session и не открывает конфигурационный редактор.
// Удержанные ответы независимых частей доказывают, что каталог публикуется самостоятельно.
void ConnectionControllerTest::destinationsAppearWithoutSessionOrManagement()
{
	// temp изолирует настройки, server обслуживает только синтетический сервер на свободном порту.
	WorkspaceTemp temp(QStringLiteral("dock-catalog-independent"));
	QVERIFY(temp.isValid());
	HttpServer server;
	QVERIFY(server.listen());
	// waitingSession и waitingFallback удерживают две независимые части без таймеров и случайных пауз.
	HttpServer::Request waitingSession;
	HttpServer::Request waitingFallback;
	// Обработчик request задерживает только RUNNING и fallback, остальные запросы освобождают бюджет REST.
	server.handler = [&](const auto &request) {
		if (request.target.startsWith("/api/v1/streams/stream/destinations")) {
			// disabled остаётся видимым назначением, хотя следующий эфир не должен автоматически его запускать.
			auto disabled = fixture::destination();
			disabled.insert(QStringLiteral("enabled"), false);
			HttpServer::reply(request, fixture::page({disabled}));
		} else if (request.target.startsWith("/api/v1/streams/stream/sessions")) {
			if (request.target.contains("state=RUNNING"))
				waitingSession = request;
			else
				HttpServer::reply(request, fixture::page({}));
		} else if (request.target.startsWith("/api/v1/streams/stream/fallback-policy")) {
			waitingFallback = request;
		} else {
			respond(request);
		}
	};
	// settings хранит выбранный Stream; state не получает конфигурацию из редактора.
	SettingsStore settings(temp.filePath(QStringLiteral("settings.json")));
	StateStore state;
	// api и events выполняют настоящие асинхронные GET и SSE внутри процесса теста.
	APIClient api;
	EventClient events;
	// controller подключается к локальному серверу и читает проекцию Dock.
	ConnectionController controller(state, settings, api, events);
	controller.observeProfile(FrontendEvent::FinishedLoading, profile());
	QVERIFY(settings.setSelectedStream(server.origin().toString(), settings.profileUuid(),
					   QStringLiteral("stream")));
	controller.connectToServer(server.origin().toString());
	QTRY_COMPARE_WITH_TIMEOUT(state.dockServer().destinationsStatus.availability, RuntimeAvailability::Fresh, 3000);
	QVERIFY(state.dockServer().destinationsStatus.complete);
	QCOMPARE(state.dockServer().destinations.size(), 1);
	QCOMPARE(state.dockServer().destinations.value(QStringLiteral("destination")).name,
		 QStringLiteral("Назначение"));
	QVERIFY(!state.dockServer().session);
	QVERIFY(state.configuration().destinations.isEmpty());
	// Каталог может быть опубликован до доставки остальных GET из очереди REST.
	// Оба ответа остаются удержанными, пока проверяется независимая доступность каталога.
	QTRY_VERIFY_WITH_TIMEOUT(waitingSession.socket && waitingFallback.socket, 3000);
	QVERIFY(state.dockServer().sessionStatus.availability != RuntimeAvailability::Fresh);
	HttpServer::reply(waitingSession, fixture::page({}));
	HttpServer::reply(waitingFallback, R"({"revision":1,"fallback":null})");
	QTRY_VERIFY_WITH_TIMEOUT(state.dockServer().sessionStatus.complete, 3000);
	QVERIFY(!state.dockServer().session);
	QCOMPARE(state.dockServer().destinations.size(), 1);
	QVERIFY(readOnly(server));
	controller.shutdown();
}

// Неполная страница имеет корректные элементы, а ошибки и противоречия не дают права заменить известный каталог.
void ConnectionControllerTest::destinationReadPreservesValidatedKnowledge_data()
{
	QTest::addColumn<QString>("boundary");
	QTest::newRow("full-replacement") << QStringLiteral("full");
	QTest::newRow("partial-merge") << QStringLiteral("partial");
	QTest::newRow("http-error") << QStringLiteral("error");
	QTest::newRow("duplicate-id") << QStringLiteral("duplicate");
	QTest::newRow("empty-id") << QStringLiteral("empty");
	QTest::newRow("wrong-stream") << QStringLiteral("parent");
}

// Сравнивает сохранённые имена и полноту после повторного чтения, а не число вызовов приватного редуктора.
void ConnectionControllerTest::destinationReadPreservesValidatedKnowledge()
{
	// boundary определяет независимую границу достоверности ответа сервера.
	QFETCH(QString, boundary);
	// temp и server изолируют настройки и HTTP от настоящего backend.
	WorkspaceTemp temp(QStringLiteral("dock-catalog-validation"));
	QVERIFY(temp.isValid());
	HttpServer server;
	QVERIFY(server.listen());
	// phase переключает исходный полный каталог на управляемый повтор; reads отмечает получение нового GET.
	int phase = 0;
	int reads = 0;
	// Обработчик request возвращает два исходных назначения, затем выбранный ошибочный или частичный ответ.
	server.handler = [&](const auto &request) {
		if (request.target.startsWith("/api/v1/streams/stream/destinations")) {
			++reads;
			// item содержит обновляемое назначение, omitted — запись, отсутствующую во втором ответе.
			auto item = fixture::destination();
			auto omitted = fixture::destination();
			omitted.insert(QStringLiteral("id"), QStringLiteral("omitted"));
			omitted.insert(QStringLiteral("name"), QStringLiteral("Сохранённое"));
			if (phase == 0) {
				HttpServer::reply(request, fixture::page({item, omitted}));
				return;
			}
			item.insert(QStringLiteral("name"), QStringLiteral("Новое имя"));
			if (boundary == QStringLiteral("empty"))
				item.insert(QStringLiteral("id"), QString{});
			if (boundary == QStringLiteral("parent"))
				item.insert(QStringLiteral("stream_id"), QStringLiteral("other"));
			HttpServer::reply(request,
					  fixture::page(boundary == QStringLiteral("duplicate") ? QJsonArray{item, item}
												: QJsonArray{item},
							boundary == QStringLiteral("partial")
								? QStringLiteral("next-page")
								: QString{}),
					  boundary == QStringLiteral("error") ? 503 : 200);
		} else if (request.target.startsWith("/api/v1/streams/stream/sessions")) {
			HttpServer::reply(request, fixture::page({}));
		} else if (request.target.startsWith("/api/v1/streams/stream/fallback-policy")) {
			HttpServer::reply(request, R"({"revision":1,"fallback":null})");
		} else {
			respond(request);
		}
	};
	// settings/state задают исходный выбор без редактора; limits ограничивает обход одним корректным ответом.
	SettingsStore settings(temp.filePath(QStringLiteral("settings.json")));
	StateStore state;
	Limits limits;
	limits.maxPages = 1;
	// api/events/controller образуют настоящий локальный путь наблюдения.
	APIClient api(limits);
	EventClient events;
	ConnectionController controller(state, settings, api, events);
	controller.observeProfile(FrontendEvent::FinishedLoading, profile());
	QVERIFY(settings.setSelectedStream(server.origin().toString(), settings.profileUuid(),
					   QStringLiteral("stream")));
	controller.connectToServer(server.origin().toString());
	QTRY_VERIFY_WITH_TIMEOUT(state.dockServer().destinationsStatus.complete &&
					 state.dockServer().sessionStatus.complete &&
					 state.dockServer().fallback.availability == RuntimeAvailability::Fresh,
				 3000);
	QCOMPARE(state.dockServer().destinations.size(), 2);
	phase = 1;
	controller.refreshState();
	QTRY_COMPARE_WITH_TIMEOUT(reads, 2, 3000);
	if (boundary == QStringLiteral("full") || boundary == QStringLiteral("partial")) {
		QTRY_COMPARE_WITH_TIMEOUT(state.dockServer().destinations.value(QStringLiteral("destination")).name,
					  QStringLiteral("Новое имя"), 3000);
		QCOMPARE(state.dockServer().destinationsStatus.availability, RuntimeAvailability::Fresh);
		QCOMPARE(state.dockServer().destinationsStatus.complete, boundary == QStringLiteral("full"));
		QCOMPARE(state.dockServer().destinations.contains(QStringLiteral("omitted")),
			 boundary == QStringLiteral("partial"));
	} else {
		QTRY_COMPARE_WITH_TIMEOUT(state.dockServer().destinationsStatus.availability,
					  RuntimeAvailability::Stale, 3000);
		QCOMPARE(state.dockServer().destinations.size(), 2);
		QCOMPARE(state.dockServer().destinations.value(QStringLiteral("destination")).name,
			 QStringLiteral("Назначение"));
		QCOMPARE(state.dockServer().destinations.value(QStringLiteral("omitted")).name,
			 QStringLiteral("Сохранённое"));
	}
	QVERIFY(readOnly(server));
	controller.shutdown();
}

// Имя из GET, начатого до записи, нельзя публиковать даже на короткое время; следующий GET не ждёт fallback.
void ConnectionControllerTest::configurationInvalidationRejectsLateCatalogWhileFallbackIsPending()
{
	// temp/server обеспечивают файловую и сетевую изоляцию сценария.
	WorkspaceTemp temp(QStringLiteral("dock-catalog-generation"));
	QVERIFY(temp.isValid());
	HttpServer server;
	QVERIFY(server.listen());
	// reads считает каталоги; late удерживает первый GET, slowFallback оставляет другой пакет незавершённым.
	int reads = 0;
	HttpServer::Request late;
	HttpServer::Request slowFallback;
	// Обработчик request даёт свежий каталог только со второго GET после инвалидации.
	server.handler = [&](const auto &request) {
		if (request.target.startsWith("/api/v1/streams/stream/destinations")) {
			if (++reads == 1) {
				late = request;
			} else {
				// renamed отличается от устаревшего ответа до сохранения.
				auto renamed = fixture::destination();
				renamed.insert(QStringLiteral("name"), QStringLiteral("После сохранения"));
				HttpServer::reply(request, fixture::page({renamed}));
			}
		} else if (request.target.startsWith("/api/v1/streams/stream/sessions")) {
			HttpServer::reply(request, fixture::page({}));
		} else if (request.target.startsWith("/api/v1/streams/stream/fallback-policy")) {
			slowFallback = request;
		} else {
			respond(request);
		}
	};
	// settings/state/api/events/controller создают наблюдатель без конфигурационного окна.
	SettingsStore settings(temp.filePath(QStringLiteral("settings.json")));
	StateStore state;
	APIClient api;
	EventClient events;
	ConnectionController controller(state, settings, api, events);
	// publishedOld обнаруживает даже промежуточную публикацию ответа, потерявшего право на применение.
	bool publishedOld = false;
	// Синхронный обработчик читает уже установленный каталог после каждого serverStateChanged.
	connect(&state, &StateStore::serverStateChanged, &controller, [&] {
		publishedOld |= state.dockServer().destinations.value(QStringLiteral("destination")).name ==
				QStringLiteral("Назначение");
	});
	controller.observeProfile(FrontendEvent::FinishedLoading, profile());
	QVERIFY(settings.setSelectedStream(server.origin().toString(), settings.profileUuid(),
					   QStringLiteral("stream")));
	controller.connectToServer(server.origin().toString());
	QTRY_VERIFY_WITH_TIMEOUT(late.socket && slowFallback.socket, 3000);
	state.invalidateConfiguration();
	controller.refreshState();
	controller.refreshState();
	HttpServer::reply(late, fixture::page({fixture::destination()}));
	QTRY_COMPARE_WITH_TIMEOUT(state.dockServer().destinations.value(QStringLiteral("destination")).name,
				  QStringLiteral("После сохранения"), 3000);
	QVERIFY(!publishedOld);
	QCOMPARE(reads, 2);
	QVERIFY(slowFallback.socket);
	QVERIFY(state.dockServer().fallback.availability != RuntimeAvailability::Fresh);
	QVERIFY(readOnly(server));
	controller.shutdown();
}

// Смена выбора отзывает прежний GET и не переносит назначение первого Stream во второй.
void ConnectionControllerTest::streamSwitchRejectsLateDestinationRead()
{
	// temp/server предоставляют независимые настройки и локальные ответы двух потоков.
	WorkspaceTemp temp(QStringLiteral("dock-catalog-selection"));
	QVERIFY(temp.isValid());
	HttpServer server;
	QVERIFY(server.listen());
	// late сохраняет незавершённый ответ исходного Stream до смены выбора.
	HttpServer::Request late;
	// Обработчик request читает каталоги двух Stream по точному маршруту, исключая сопоставление по имени.
	server.handler = [&](const auto &request) {
		if (request.target.startsWith("/api/v1/streams/stream/destinations")) {
			late = request;
		} else if (request.target.startsWith("/api/v1/streams/other/destinations")) {
			// other принадлежит второму Stream и имеет собственный идентификатор.
			auto other = fixture::destination();
			other.insert(QStringLiteral("stream_id"), QStringLiteral("other"));
			other.insert(QStringLiteral("id"), QStringLiteral("other-destination"));
			HttpServer::reply(request, fixture::page({other}));
		} else if (request.target.startsWith("/api/v1/streams?")) {
			HttpServer::reply(request,
					  fixture::page({fixture::stream(), fixture::stream(QStringLiteral("other"))}));
		} else if (request.target.contains("/sessions")) {
			HttpServer::reply(request, fixture::page({}));
		} else if (request.target.contains("/fallback-policy")) {
			HttpServer::reply(request, R"({"revision":1,"fallback":null})");
		} else {
			respond(request);
		}
	};
	// settings/state/api/events/controller выполняют настоящий переход контекста наблюдателя.
	SettingsStore settings(temp.filePath(QStringLiteral("settings.json")));
	StateStore state;
	APIClient api;
	EventClient events;
	ConnectionController controller(state, settings, api, events);
	controller.observeProfile(FrontendEvent::FinishedLoading, profile());
	QVERIFY(settings.setSelectedStream(server.origin().toString(), settings.profileUuid(),
					   QStringLiteral("stream")));
	controller.connectToServer(server.origin().toString());
	QTRY_VERIFY_WITH_TIMEOUT(late.socket, 3000);
	controller.selectStream(QStringLiteral("other"));
	QVERIFY(state.dockServer().destinations.isEmpty());
	HttpServer::reply(late, fixture::page({fixture::destination()}));
	QTRY_VERIFY_WITH_TIMEOUT(state.dockServer().destinationsStatus.complete, 3000);
	QCOMPARE(state.dockServer().destinations.size(), 1);
	QVERIFY(state.dockServer().destinations.contains(QStringLiteral("other-destination")));
	QVERIFY(!state.dockServer().destinations.contains(QStringLiteral("destination")));
	QCOMPARE(state.dockContext().streamId, QStringLiteral("other"));
	QVERIFY(readOnly(server));
	controller.shutdown();
}
