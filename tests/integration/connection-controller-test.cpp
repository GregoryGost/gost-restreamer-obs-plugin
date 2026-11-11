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
} // namespace

class ConnectionControllerTest final : public QObject {
	Q_OBJECT
private slots:
	void connectsWithoutImplicitStreamOrMedia();
	void serverSwitchDiscardsOldResponses();
	void profileSwitchRestoresOnlyItsBinding();
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

void ConnectionControllerTest::reconnectUsesConfirmedCursor()
{
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
