#include "api/api-client.hpp"
#include "api/event-client.hpp"
#include "core/connection-controller.hpp"
#include "core/settings-store.hpp"
#include "core/state-store.hpp"
#include "fixtures/api-json.hpp"
#include "fixtures/http-server.hpp"
#include "fixtures/workspace-temp.hpp"

#include <QTest>
#include <QFile>
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
	QCOMPARE(state.connectionStatus(), ConnectionStatus::Connected);
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
	QTRY_COMPARE_WITH_TIMEOUT(requestCount(server, "/api/v1/events?"), 1, 3000);
	auto first = subscription(server);
	first.socket->write(fixture::frame(fixture::event(QStringLiteral("state"), QStringLiteral("opaque/%2B+last"))));
	QTRY_COMPARE_WITH_TIMEOUT(state.runtime().cursor, QStringLiteral("opaque/%2B+last"), 2000);
	first.socket->disconnectFromHost();
	QTRY_COMPARE_WITH_TIMEOUT(requestCount(server, "/api/v1/events?"), 2, 4000);
	QCOMPARE(requestCount(server, "/api/v1/events/snapshot"), 1);
	const QUrlQuery query(QUrl::fromEncoded("http://local" + subscription(server).target));
	QCOMPARE(query.queryItemValue(QStringLiteral("cursor"), QUrl::FullyDecoded), QStringLiteral("opaque/%2B+last"));
	QVERIFY(readOnly(server));
	controller.shutdown();
}

void ConnectionControllerTest::resyncObtainsNewAnchor()
{
	WorkspaceTemp temp(QStringLiteral("resync"));
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
	QTRY_COMPARE_WITH_TIMEOUT(requestCount(server, "/api/v1/events?"), 1, 3000);
	subscription(server).socket->write("event: resync_required\ndata: {\"snapshot_url\":\"/arbitrary\"}\n\n");
	QTRY_COMPARE_WITH_TIMEOUT(requestCount(server, "/api/v1/events/snapshot"), 2, 4000);
	QTRY_COMPARE_WITH_TIMEOUT(requestCount(server, "/api/v1/events?"), 2, 3000);
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
		HttpServer::reply(request, "{}", 503);
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
	bool passedRetryWindow = false;
	QTimer::singleShot(1300, this, [&] { passedRetryWindow = true; });
	QTRY_VERIFY_WITH_TIMEOUT(passedRetryWindow, 2500);
	QCOMPARE(server.requests.size(), requests);
	QCOMPARE(state.connectionStatus(), ConnectionStatus::Disconnected);
	controller.connectToServer(server.origin().toString());
	controller.selectStream(QStringLiteral("stream"));
	QCOMPARE(server.requests.size(), requests);
}

QTEST_GUILESS_MAIN(ConnectionControllerTest)
#include "connection-controller-test.moc"
