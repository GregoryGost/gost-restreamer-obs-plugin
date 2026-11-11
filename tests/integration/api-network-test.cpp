#include "api/api-client.hpp"
#include "api/event-client.hpp"
#include "fixtures/api-json.hpp"
#include "fixtures/http-server.hpp"

#include <QElapsedTimer>
#include <QNetworkAccessManager>
#include <QNetworkInterface>
#include <QNetworkProxyFactory>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QScopeGuard>
#include <QTest>
#include <QTimer>
#include <QUrlQuery>

#include <type_traits>
#include <atomic>

using namespace gost::api;

namespace {
// Фабрика считает реальное обращение к выбору proxy, затем разрешает прямое соединение.
// Это позволяет отделить обход фабрики собственным менеджером от случайно успешной сети.
class CountingProxyFactory final : public QNetworkProxyFactory {
public:
	explicit CountingProxyFactory(std::atomic_int &queries) : queries_(queries) {}
	QList<QNetworkProxy> queryProxy(const QNetworkProxyQuery &) override
	{
		++queries_;
		return {QNetworkProxy(QNetworkProxy::NoProxy)};
	}

private:
	std::atomic_int &queries_;
};
} // namespace

// Интеграция настоящего Qt Network с одноразовой HTTP-фикстурой, без сервера рестримера и OBS.
// QTRY_* обрабатывают цикл событий Qt до наблюдаемого результата/запроса; их timeout — предел теста,
// а не обещанная задержка продукта. Все обработчики и сервер работают в том же потоке приложения теста.
class ApiNetworkTest final : public QObject {
	Q_OBJECT
private slots:
	void everyListPreservesOpaqueCursorAndFilters_data();
	void everyListPreservesOpaqueCursorAndFilters();
	void incompleteWalkCannotReportSuccess_data();
	void incompleteWalkCannotReportSuccess();
	void listBodyLimitsPreserveOnlyWholePages_data();
	void listBodyLimitsPreserveOnlyWholePages();
	void httpProblemsAndDecodeFailures_data();
	void httpProblemsAndDecodeFailures();
	void cancellationAndOldContextAreObservable();
	void receiverDestructionSuppressesCallback();
	void deadlinesAndBodyBudgets_data();
	void deadlinesAndBodyBudgets();
	void activeAndQueuedRequestsAreBounded();
	void sseValidatesStatusMediaAndIdentity_data();
	void sseValidatesStatusMediaAndIdentity();
	void sseAcknowledgesOnlyAppliedEvents();
	void sseControlForcesSnapshotWithoutFollowingUrl();
	void sseHasIndependentLifetimeAndShutdown();
	void maximumDocumentsMeasureEventLoopGap();
	void detailRoutesKeepValuesAndEscapeOpaqueIdentifiers_data();
	void detailRoutesKeepValuesAndEscapeOpaqueIdentifiers();
	void sseYieldsBetweenBoundedBatches();
	void heartbeatKeepsTransportAliveWithoutInventingEvents();
	void replacingSubscriptionDuringDeliveryDiscardsOldContext();
	void explicitServerConnectionBypassesApplicationProxyFactory();
};

void ApiNetworkTest::everyListPreservesOpaqueCursorAndFilters_data()
{
	QTest::addColumn<int>("family");
	QTest::addColumn<QString>("path");
	const QStringList paths{QStringLiteral("/streams"),
				QStringLiteral("/destinations"),
				QStringLiteral("/transcode-profiles"),
				QStringLiteral("/transcode-groups"),
				QStringLiteral("/sessions"),
				QStringLiteral("/fallback-assets"),
				QStringLiteral("/streams/entity/destinations"),
				QStringLiteral("/streams/entity/sessions"),
				QStringLiteral("/sessions/entity/outputs"),
				QStringLiteral("/fallback-assets/entity/variants")};
	for (int family = 0; family < paths.size(); ++family)
		QTest::newRow(qPrintable(paths[family])) << family << QStringLiteral("/api/v1") + paths[family];
}

void ApiNetworkTest::everyListPreservesOpaqueCursorAndFilters()
{
	// Одинаковая проверка проходит все верхние и вложенные семейства. Курсор включает разделители,
	// Unicode и уже закодированный процент: повторное чтение проверяет точное значение после одного
	// URL-декодирования, а не просто наличие query. Вторая страница с пустым cursor завершает обход.
	QFETCH(int, family);
	QFETCH(QString, path);
	HttpServer server;
	QVERIFY(server.listen());
	QJsonObject item;
	switch (family) {
	case 0:
		item = fixture::stream();
		break;
	case 1:
	case 6:
		item = fixture::destination();
		break;
	case 2:
		item = fixture::profile();
		break;
	case 3:
		item = fixture::group();
		break;
	case 4:
	case 7:
		item = fixture::session();
		break;
	case 5:
		item = fixture::asset();
		break;
	case 8:
		item = fixture::output();
		break;
	case 9:
		item = fixture::variant();
		break;
	}
	const QString opaque = QStringLiteral("cursor/+?=%2F&next=значение");
	server.handler = [&](const HttpServer::Request &request) {
		HttpServer::reply(request, fixture::page({item}, server.requests.size() == 1 ? opaque : QString{}));
	};
	APIClient api;
	const RequestContext expected{11, 22};
	api.configure(server.origin(), expected);
	bool done = false;
	bool success = false;
	bool complete = false;
	int count = 0;
	RequestContext context;
	auto receive = [&](auto actualContext, auto result) {
		context = actualContext;
		success = bool(result);
		if (result.value) {
			complete = result.value->complete;
			count = int(result.value->items.size());
		}
		done = true;
	};
	ListQuery query;
	query.limit = 1;
	query.from = QStringLiteral("2026-11-01T00:00:00Z");
	query.to = QStringLiteral("2026-11-10T00:00:00Z");
	query.streamId = QStringLiteral("stream/filter");
	query.groupId = QStringLiteral("group/filter");
	query.profileId = QStringLiteral("profile/filter");
	query.assetId = QStringLiteral("asset/filter");
	query.generationId = QStringLiteral("generation/filter");
	query.state = QStringLiteral("READY");
	switch (family) {
	case 0:
		api.getAllStreams(this, receive, query);
		break;
	case 1:
		api.getAllDestinations(this, receive, query);
		break;
	case 2:
		api.getAllProfiles(this, receive, query);
		break;
	case 3:
		api.getAllGroups(this, receive, query);
		break;
	case 4:
		api.getAllSessions(this, receive, query);
		break;
	case 5:
		api.getAllAssets(this, receive, query);
		break;
	case 6:
		api.getAllStreamDestinations(QStringLiteral("entity"), this, receive, query);
		break;
	case 7:
		api.getAllStreamSessions(QStringLiteral("entity"), this, receive, query);
		break;
	case 8:
		api.getAllOutputs(QStringLiteral("entity"), this, receive, query);
		break;
	case 9:
		api.getAllVariants(QStringLiteral("entity"), this, receive, query);
		break;
	}
	QTRY_VERIFY_WITH_TIMEOUT(done, 3000);
	QVERIFY(success);
	QVERIFY(complete);
	QCOMPARE(count, 2);
	QVERIFY(context == expected);
	QCOMPARE(server.requests.size(), 2);
	for (const auto &request : server.requests) {
		QCOMPARE(request.method, QByteArray("GET"));
		const QUrl url = QUrl::fromEncoded("http://local" + request.target);
		QCOMPARE(url.path(), path);
		const QUrlQuery actual(url);
		QCOMPARE(actual.queryItemValue(QStringLiteral("limit")), QStringLiteral("1"));
		for (const auto &key :
		     {QStringLiteral("from"), QStringLiteral("to"), QStringLiteral("stream_id"),
		      QStringLiteral("group_id"), QStringLiteral("profile_id"), QStringLiteral("asset_id"),
		      QStringLiteral("generation_id"), QStringLiteral("state")})
			QVERIFY2(actual.hasQueryItem(key), qPrintable(key));
	}
	const QUrlQuery second(QUrl::fromEncoded("http://local" + server.requests[1].target));
	QCOMPARE(second.queryItemValue(QStringLiteral("cursor"), QUrl::FullyDecoded), opaque);
	QCOMPARE(second.queryItemValue(QStringLiteral("stream_id"), QUrl::FullyDecoded), query.streamId);
}

void ApiNetworkTest::incompleteWalkCannotReportSuccess_data()
{
	QTest::addColumn<int>("failure");
	QTest::newRow("repeated-cursor") << 0;
	QTest::newRow("page-limit") << 1;
	QTest::newRow("item-limit") << 2;
	QTest::newRow("http-page-two") << 3;
}
void ApiNetworkTest::incompleteWalkCannotReportSuccess()
{
	// «Успех» здесь означает полноту списка. Предел общего обхода может вернуть успешную часть,
	// но complete обязан остаться false; цикл cursor/ошибка страницы не должны выглядеть полным списком.
	QFETCH(int, failure);
	HttpServer server;
	QVERIFY(server.listen());
	server.handler = [&](const HttpServer::Request &request) {
		if (failure == 3 && server.requests.size() > 1)
			HttpServer::reply(request, R"({"title":"Unavailable","status":503})", 503,
					  "application/problem+json");
		else
			HttpServer::reply(request, fixture::page({fixture::stream()}, QStringLiteral("same-cursor")));
	};
	Limits limits;
	if (failure == 1)
		limits.maxPages = 1;
	if (failure == 2)
		limits.maxItems = 1;
	APIClient api(limits);
	api.configure(server.origin(), {1, 1});
	std::optional<Result<Collection<Stream>>> result;
	api.getAllStreams(this, [&](auto, auto value) { result = std::move(value); });
	QTRY_VERIFY_WITH_TIMEOUT(result.has_value(), 3000);
	QVERIFY(!result->value || !result->value->complete);
	QVERIFY(result->value.has_value() || result->error.has_value());
	QVERIFY(server.requests.size() <= 3);
}

void ApiNetworkTest::listBodyLimitsPreserveOnlyWholePages_data()
{
	QTest::addColumn<bool>("individualOverflow");
	QTest::newRow("aggregate-overflow") << false;
	QTest::newRow("individual-overflow") << true;
}

void ApiNetworkTest::listBodyLimitsPreserveOnlyWholePages()
{
	QFETCH(bool, individualOverflow);
	const QString cursor = QStringLiteral("page/+%2F");
	const auto first = fixture::page({fixture::stream(QStringLiteral("first"))}, cursor);
	const auto second =
		fixture::page({fixture::stream(QStringLiteral("second")), fixture::stream(QStringLiteral("third"))},
			      QStringLiteral("later"));
	QVERIFY(first.size() < second.size());
	HttpServer server;
	QVERIFY(server.listen());
	server.handler = [&](const auto &request) {
		HttpServer::reply(request, server.requests.size() == 1 ? first : second);
	};
	Limits limits;
	limits.pageWalkBytes = first.size() + second.size() - 1;
	// Разница в один байт отделяет ошибку отдельного тела от допустимого частичного результата всего обхода.
	limits.restBodyBytes = second.size() - int(individualOverflow);
	APIClient api(limits);
	api.configure(server.origin(), {1, 1});
	std::optional<Result<Collection<Stream>>> result;
	api.getAllStreams(this, [&](auto, auto value) { result = std::move(value); });
	QTRY_VERIFY_WITH_TIMEOUT(result.has_value(), 3000);
	QCOMPARE(server.requests.size(), 2);
	const QUrlQuery query(QUrl::fromEncoded("http://local" + server.requests.back().target));
	QCOMPARE(query.queryItemValue(QStringLiteral("cursor"), QUrl::FullyDecoded), cursor);
	if (individualOverflow) {
		QVERIFY(!result->value);
		QVERIFY(result->error);
		QCOMPARE(result->error->kind, ErrorKind::Limit);
	} else {
		QVERIFY(*result);
		QVERIFY(!result->error);
		QVERIFY(!result->value->complete);
		QCOMPARE(result->value->items.size(), 1);
		QCOMPARE(result->value->items.front().id, QStringLiteral("first"));
		QCOMPARE(result->value->nextCursor, cursor);
		QCOMPARE(result->value->bytes, first.size());
	}
}

void ApiNetworkTest::httpProblemsAndDecodeFailures_data()
{
	QTest::addColumn<int>("status");
	QTest::addColumn<QByteArray>("body");
	QTest::addColumn<int>("kind");
	QTest::newRow("problem") << 409 << QByteArray(R"({"type":"about:blank","title":"Conflict","status":409,
"detail":"synthetic-secret-in-detail","code":"revision_conflict","instance":"/private?key=synthetic"})")
				 << int(ErrorKind::Http);
	QTest::newRow("malformed") << 200 << QByteArray("{") << int(ErrorKind::Decode);
	QTest::newRow("wrong-shape") << 200 << QByteArray("[]") << int(ErrorKind::Decode);
}
void ApiNetworkTest::httpProblemsAndDecodeFailures()
{
	// Ошибка HTTP отличается от неверного JSON при 200. Синтетические секреты в Problem могут
	// сохраниться в структурированных данных, но не должны попадать в общее пользовательское message.
	QFETCH(int, status);
	QFETCH(QByteArray, body);
	QFETCH(int, kind);
	HttpServer server;
	QVERIFY(server.listen());
	server.handler = [&](const auto &request) {
		HttpServer::reply(request, body, status,
				  status == 200 ? "application/json" : "application/problem+json");
	};
	APIClient api;
	api.configure(server.origin(), {4, 5});
	std::optional<Result<Health>> result;
	api.getReady(this, [&](auto, auto value) { result = std::move(value); });
	QTRY_VERIFY_WITH_TIMEOUT(result.has_value(), 2000);
	QVERIFY(!*result);
	QVERIFY(result->error);
	QCOMPARE(int(result->error->kind), kind);
	QVERIFY(!result->error->message.contains(QStringLiteral("synthetic")));
	if (status == 409) {
		QCOMPARE(result->error->httpStatus, 409);
		QVERIFY(result->error->problem);
		QCOMPARE(result->error->problem->code, QStringLiteral("revision_conflict"));
	}
}

void ApiNetworkTest::cancellationAndOldContextAreObservable()
{
	// Сервер удерживает два ответа и выпускает старый после configure. Только новый контекст может
	// получить успешный DTO. Для cancelAll оракул — закрытие сокета; обязательный callback Cancelled
	// не требуется контрактом, поэтому отсутствие уведомления также допустимо.
	HttpServer server;
	QVERIFY(server.listen());
	APIClient api;
	const RequestContext old{1, 2};
	const RequestContext current{2, 1};
	api.configure(server.origin(), old);
	QVector<RequestContext> contexts;
	QVector<Result<Health>> results;
	auto receive = [&](auto context, auto value) {
		contexts.append(context);
		results.append(std::move(value));
	};
	api.getReady(this, receive);
	QTRY_COMPARE_WITH_TIMEOUT(server.requests.size(), 1, 2000);
	api.configure(server.origin(), current);
	api.getReady(this, receive);
	QTRY_COMPARE_WITH_TIMEOUT(server.requests.size(), 2, 2000);
	HttpServer::reply(server.requests[0], R"({"status":"stale","reason":""})");
	HttpServer::reply(server.requests[1], R"({"status":"ready","reason":""})");
	QTRY_VERIFY_WITH_TIMEOUT(!results.isEmpty() && results.back().value.has_value(), 2000);
	QVERIFY(contexts.back() == current);
	QCOMPARE(results.back().value->status, QStringLiteral("ready"));
	for (qsizetype i = 0; i < results.size(); ++i)
		if (contexts[i] == old) {
			QVERIFY(results[i].error);
			QCOMPARE(results[i].error->kind, ErrorKind::Cancelled);
		}
	std::optional<Result<Health>> cancelled;
	api.getReady(this, [&](auto context, auto result) {
		QVERIFY(context == current);
		cancelled = std::move(result);
	});
	QTRY_COMPARE_WITH_TIMEOUT(server.requests.size(), 3, 2000);
	QPointer<QTcpSocket> pending = server.requests.back().socket;
	api.cancelAll();
	QTRY_VERIFY_WITH_TIMEOUT(!pending || pending->state() == QAbstractSocket::UnconnectedState, 2000);
	if (cancelled) {
		QVERIFY(cancelled->error);
		QCOMPARE(cancelled->error->kind, ErrorKind::Cancelled);
	}
}

void ApiNetworkTest::receiverDestructionSuppressesCallback()
{
	HttpServer server;
	QVERIFY(server.listen());
	APIClient api;
	api.configure(server.origin(), {1, 1});
	auto receiver = std::make_unique<QObject>();
	int calls = 0;
	api.getReady(receiver.get(), [&](auto, auto) { ++calls; });
	QTRY_COMPARE_WITH_TIMEOUT(server.requests.size(), 1, 2000);
	receiver.reset();
	HttpServer::reply(server.requests.front(), R"({"status":"ready","reason":""})");
	// Новый успешный запрос служит барьером обработки событий после уничтожения прежнего получателя.
	bool barrier = false;
	api.getReady(this, [&](auto, auto) { barrier = true; });
	QTRY_COMPARE_WITH_TIMEOUT(server.requests.size(), 2, 2000);
	HttpServer::reply(server.requests.back(), R"({"status":"ready","reason":""})");
	QTRY_VERIFY_WITH_TIMEOUT(barrier, 2000);
	QCOMPARE(calls, 0);
}

void ApiNetworkTest::deadlinesAndBodyBudgets_data()
{
	QTest::addColumn<int>("failure");
	QTest::newRow("absolute-deadline") << 0;
	QTest::newRow("idle-timeout") << 1;
	QTest::newRow("body-limit") << 2;
}
void ApiNetworkTest::deadlinesAndBodyBudgets()
{
	// В двух строках сервер намеренно вообще не отвечает, а независимые таймеры имеют разные сроки.
	// Третья строка отвечает сразу и пересекает только размер тела; таймаут не должен маскировать Limit.
	QFETCH(int, failure);
	HttpServer server;
	QVERIFY(server.listen());
	Limits limits;
	limits.httpDeadlineMs = failure == 0 ? 80 : 2000;
	limits.httpIdleMs = failure == 1 ? 80 : 2000;
	limits.restBodyBytes = failure == 2 ? 32 : 1024;
	if (failure == 2)
		server.handler = [](const auto &request) {
			HttpServer::reply(request, QByteArray(100, 'x'));
		};
	APIClient api(limits);
	api.configure(server.origin(), {1, 1});
	std::optional<Result<Health>> result;
	api.getReady(this, [&](auto, auto value) { result = std::move(value); });
	QTRY_VERIFY_WITH_TIMEOUT(result.has_value(), 3000);
	QVERIFY(result->error);
	QCOMPARE(result->error->kind, failure == 2 ? ErrorKind::Limit : ErrorKind::Timeout);
}

void ApiNetworkTest::activeAndQueuedRequestsAreBounded()
{
	// Три GET при одном активном и одном ожидающем дают ровно один отказ допуска. Последовательная
	// выдача ответов проверяет освобождение слота и факт, что третий запрос не отправлялся на сервер.
	HttpServer server;
	QVERIFY(server.listen());
	Limits limits;
	limits.maxActiveRequests = 1;
	limits.maxQueuedRequests = 1;
	APIClient api(limits);
	api.configure(server.origin(), {1, 1});
	int done = 0;
	int rejected = 0;
	int succeeded = 0;
	auto receive = [&](auto, auto result) {
		++done;
		if (result.error && result.error->kind == ErrorKind::Limit)
			++rejected;
		if (result.value)
			++succeeded;
	};
	for (int index = 0; index < 3; ++index)
		api.getReady(this, receive);
	QTRY_COMPARE_WITH_TIMEOUT(rejected, 1, 2000);
	QTRY_COMPARE_WITH_TIMEOUT(server.requests.size(), 1, 2000);
	HttpServer::reply(server.requests[0], R"({"status":"ready","reason":""})");
	QTRY_COMPARE_WITH_TIMEOUT(server.requests.size(), 2, 2000);
	HttpServer::reply(server.requests[1], R"({"status":"ready","reason":""})");
	QTRY_COMPARE_WITH_TIMEOUT(done, 3, 2000);
	QCOMPARE(succeeded, 2);
	QCOMPARE(server.requests.size(), 2);
}

void ApiNetworkTest::sseValidatesStatusMediaAndIdentity_data()
{
	QTest::addColumn<int>("failure");
	QTest::newRow("http-status") << 0;
	QTest::newRow("media-type") << 1;
	QTest::newRow("event-kind") << 2;
	QTest::newRow("id-cursor") << 3;
	QTest::newRow("invalid-json") << 4;
}
void ApiNetworkTest::sseValidatesStatusMediaAndIdentity()
{
	// Каждая строка ломает один слой: статус, MIME, SSE event, SSE id или JSON. Ни один плохой кадр
	// не должен дойти до потребителя/подтвердить cursor; несовместимость события требует повторной синхронизации.
	QFETCH(int, failure);
	HttpServer server;
	QVERIFY(server.listen());
	server.handler = [&](const auto &request) {
		const auto frame = failure == 4 ? QByteArray("event: state\nid: x\ndata: {\n\n")
						: fixture::frame(fixture::event(),
								 failure == 2 ? QByteArray("metrics") : QByteArray{},
								 failure == 3 ? QByteArray("different") : QByteArray{});
		HttpServer::stream(request, frame, failure == 0 ? 503 : 200,
				   failure == 1 ? QByteArray("application/json")
						: QByteArray("text/event-stream; charset=utf-8"));
	};
	EventClient client;
	int deliveries = 0;
	std::optional<Error> error;
	bool resync = false;
	client.subscribe(
		server.origin(), QStringLiteral("anchor"), {1, 2}, this,
		[&](auto, const auto &) {
			++deliveries;
			return true;
		},
		[&](auto, auto value, bool required) {
			error = value;
			resync = required;
		});
	QTRY_VERIFY_WITH_TIMEOUT(error.has_value(), 2000);
	QCOMPARE(deliveries, 0);
	QCOMPARE(client.confirmedCursor(), QStringLiteral("anchor"));
	if (failure >= 2)
		QVERIFY(resync);
	client.shutdown();
}

void ApiNetworkTest::sseAcknowledgesOnlyAppliedEvents()
{
	// Первый обработчик принимает событие, второй возвращает false. Курсор проверяется после
	// завершения callback, чтобы не принять полученный из сети id за уже применённое наблюдение.
	HttpServer server;
	QVERIFY(server.listen());
	server.handler = [](const auto &request) {
		HttpServer::stream(request);
	};
	EventClient client;
	const RequestContext expected{9, 8};
	int received = 0;
	bool accept = true;
	bool validEvent = true;
	client.subscribe(
		server.origin(), QStringLiteral("opaque-anchor"), expected, this,
		[&](auto context, const auto &event) {
			validEvent = validEvent && context == expected && event.kind == QStringLiteral("state");
			++received;
			return accept;
		},
		[](auto, auto, bool) {});
	QTRY_COMPARE_WITH_TIMEOUT(server.requests.size(), 1, 2000);
	server.requests.front().socket->write(
		fixture::frame(fixture::event(QStringLiteral("state"), QStringLiteral("opaque-z"))));
	QTRY_COMPARE_WITH_TIMEOUT(received, 1, 2000);
	QCOMPARE(client.confirmedCursor(), QStringLiteral("opaque-z"));
	accept = false;
	// Порядок курсоров намеренно не лексический: отвергнутое событие не меняет подтверждённую позицию.
	server.requests.front().socket->write(
		fixture::frame(fixture::event(QStringLiteral("state"), QStringLiteral("opaque-a"))));
	QTRY_COMPARE_WITH_TIMEOUT(received, 2, 2000);
	QVERIFY(validEvent);
	QCOMPARE(client.confirmedCursor(), QStringLiteral("opaque-z"));
	client.shutdown();
}

void ApiNetworkTest::sseControlForcesSnapshotWithoutFollowingUrl()
{
	// snapshot_url намеренно указывает на произвольный путь: управляющий кадр должен лишь сообщить
	// requiresSnapshot, а выбор закреплённого маршрута снимка принадлежит контроллеру.
	HttpServer server;
	QVERIFY(server.listen());
	server.handler = [](const auto &request) {
		HttpServer::stream(
			request,
			"event: resync_required\ndata: {\"reason\":\"cursor_expired\",\"snapshot_url\":\"/arbitrary\"}\n\n");
	};
	EventClient client;
	bool error = false;
	bool resync = false;
	int received = 0;
	client.subscribe(
		server.origin(), QStringLiteral("anchor"), {1, 1}, this,
		[&](auto, const auto &) {
			++received;
			return true;
		},
		[&](auto, auto, bool required) {
			error = true;
			resync = required;
		});
	QTRY_VERIFY_WITH_TIMEOUT(error, 2000);
	QVERIFY(resync);
	QCOMPARE(received, 0);
	QCOMPARE(server.requests.size(), 1);
}

void ApiNetworkTest::sseHasIndependentLifetimeAndShutdown()
{
	// Событие приходит позже REST deadline=1 мс, но внутри собственного SSE idle. Успех доказывает
	// независимость бюджетов. После shutdown следующий оборот Qt не должен доставить записанный хвост.
	HttpServer server;
	QVERIFY(server.listen());
	Limits limits;
	limits.httpDeadlineMs = 1;
	limits.sseHandshakeMs = 1000;
	limits.sseIdleMs = 2000;
	server.handler = [this](const auto &request) {
		HttpServer::stream(request, ": heartbeat\n\n");
		QTimer::singleShot(100, this, [request] {
			if (request.socket)
				request.socket->write(fixture::frame(fixture::event()));
		});
	};
	EventClient client(limits);
	int received = 0;
	int errors = 0;
	client.subscribe(
		server.origin(), QStringLiteral("anchor"), {1, 1}, this,
		[&](auto, const auto &) {
			++received;
			return true;
		},
		[&](auto, auto, bool) { ++errors; });
	QTRY_COMPARE_WITH_TIMEOUT(received, 1, 2000);
	QCOMPARE(errors, 0);
	client.shutdown();
	if (server.requests.front().socket)
		server.requests.front().socket->write(fixture::frame(fixture::event()));
	bool barrier = false;
	QTimer::singleShot(0, this, [&] { barrier = true; });
	QTRY_VERIFY_WITH_TIMEOUT(barrier, 1000);
	QCOMPARE(received, 1);
	QCOMPARE(server.requests.size(), 1);
}

void ApiNetworkTest::maximumDocumentsMeasureEventLoopGap()
{
	// Измеряется наблюдаемый разрыв цикла Qt на предельных документах; жёсткая гарантия задержки не утверждается.
	HttpServer server;
	QVERIFY(server.listen());
	Limits limits;
	const auto restPrefix = QByteArray("{\"status\":\"ready\",\"reason\":\"");
	const auto snapshotPrefix = QByteArray("{\"sessions\":[],\"gpu\":null,\"cursor\":\"");
	server.handler = [&](const auto &request) {
		const bool snapshot = request.target.startsWith("/api/v1/events/snapshot");
		const auto prefix = snapshot ? snapshotPrefix : restPrefix;
		const auto maximum = snapshot ? limits.snapshotBodyBytes : limits.restBodyBytes;
		HttpServer::reply(request, prefix + QByteArray(maximum - prefix.size() - 2, 'a') + "\"}");
	};
	APIClient api(limits);
	api.configure(server.origin(), {1, 1});
	QElapsedTimer clock;
	clock.start();
	qint64 last = 0;
	qint64 gap = 0;
	QTimer ticker;
	connect(&ticker, &QTimer::timeout, this, [&] {
		const auto now = clock.nsecsElapsed();
		gap = qMax(gap, now - last);
		last = now;
	});
	ticker.start(0);
	bool restDone = false;
	bool snapshotDone = false;
	bool restOk = false;
	bool snapshotOk = false;
	api.getReady(this, [&](auto, auto result) {
		restDone = true;
		restOk = bool(result);
	});
	QTRY_VERIFY_WITH_TIMEOUT(restDone, 10000);
	QVERIFY(restOk);
	api.getSnapshot(this, [&](auto, auto result) {
		snapshotDone = true;
		snapshotOk = bool(result);
	});
	QTRY_VERIFY_WITH_TIMEOUT(snapshotDone, 10000);
	QVERIFY(snapshotOk);
	ticker.stop();
	qInfo("Максимальные документы: REST %lld байт, anchor %lld байт; наибольший наблюдаемый разрыв Qt event loop %.3f мс; Qt %s.",
	      qint64(limits.restBodyBytes), qint64(limits.snapshotBodyBytes), double(gap) / 1000000.0, qVersion());
}

void ApiNetworkTest::detailRoutesKeepValuesAndEscapeOpaqueIdentifiers_data()
{
	QTest::addColumn<int>("family");
	QTest::addColumn<QByteArray>("path");
	const QList<QByteArray> paths{"/streams/",          "/destinations/",    "/transcode-profiles/",
				      "/transcode-groups/", "/sessions/",        "/commands/",
				      "/fallback-assets/",  "/fallback-assets/", "/streams/",
				      "/sessions/",         "/sessions/"};
	for (int index = 0; index < paths.size(); ++index)
		QTest::newRow(qPrintable(QString::number(index))) << index << paths[index];
}

void ApiNetworkTest::detailRoutesKeepValuesAndEscapeOpaqueIdentifiers()
{
	// Один ID содержит пробел, косую черту, разделители параметров и фрагмента URL, '%' и кириллицу. Сравнивается
	// исходный адрес в строке запроса, включая оба сегмента вложенных маршрутов; иначе QUrl мог бы скрыть
	// ошибку нормализации при чтении уже разобранного пути. Значения DTO проверяются независимо.
	QFETCH(int, family);
	QFETCH(QByteArray, path);
	HttpServer server;
	QVERIFY(server.listen());
	auto body = fixture::stream();
	switch (family) {
	case 1:
		body = fixture::destination();
		break;
	case 2:
		body = fixture::profile();
		break;
	case 3:
		body = fixture::group();
		break;
	case 4:
		body = fixture::session();
		break;
	case 5:
		body = fixture::record(QStringLiteral("receipt"));
		for (const auto *key : {"request_id", "kind", "target_id", "session_id", "output_id"})
			body.insert(QLatin1String(key), QStringLiteral("identity"));
		body.insert(QStringLiteral("status"), QStringLiteral("PENDING"));
		body.insert(QStringLiteral("failure"), QJsonValue::Null);
		break;
	case 6:
		body = fixture::asset();
		break;
	case 7:
		body = fixture::record(QStringLiteral("generation"));
		body.insert(QStringLiteral("asset_id"), QStringLiteral("asset"));
		body.insert(QStringLiteral("kind"), QStringLiteral("image"));
		body.insert(QStringLiteral("status"), QStringLiteral("READY"));
		body.insert(QStringLiteral("size_bytes"), QStringLiteral("9007199254740993"));
		body.insert(QStringLiteral("failure"), QJsonValue::Null);
		break;
	case 8:
		body = fixture::object(R"({"revision":"9007199254740993","fallback":null})");
		break;
	case 9:
		body = fixture::object(R"({"availability":"unavailable","reason":"not_observed","snapshot":null})");
		break;
	case 10:
		body = fixture::output();
		body.insert(QStringLiteral("runtime"), QJsonValue::Null);
		body.insert(QStringLiteral("runtime_availability"), QStringLiteral("unavailable"));
		body.insert(QStringLiteral("runtime_reason"), QStringLiteral("not_observed"));
		break;
	}
	server.handler = [&](const auto &request) {
		HttpServer::reply(request, fixture::bytes(body));
	};
	APIClient api;
	api.configure(server.origin(), {1, 1});
	bool done = false;
	bool success = false;
	bool accurate = true;
	auto callback = [&](auto, auto result) {
		success = bool(result);
		if (result.value) {
			using Value = typename decltype(result.value)::value_type;
			if constexpr (std::is_same_v<Value, Stream>)
				accurate = result.value->publicationKey.value == QStringLiteral("publish+%2F/ключ");
			if constexpr (std::is_same_v<Value, Destination>)
				accurate = result.value->endpoint.value == fixture::endpoint();
			if constexpr (std::is_same_v<Value, Receipt>)
				accurate = result.value->status == QStringLiteral("PENDING");
			if constexpr (std::is_same_v<Value, Generation>)
				accurate = result.value->sizeBytes == quint64(9007199254740993ULL);
			if constexpr (std::is_same_v<Value, PolicyResult>)
				accurate = result.value->revision == quint64(9007199254740993ULL) &&
					   result.value->fallback.isNull();
			if constexpr (std::is_same_v<Value, OutputDetail>)
				accurate = result.value->runtime.isNull() &&
					   result.value->destination.endpoint.value == fixture::endpoint();
			if constexpr (std::is_same_v<Value, SessionMetrics>)
				accurate = result.value->snapshot.isNull();
		}
		done = true;
	};
	const QString id = QStringLiteral("entity /?%#ключ");
	switch (family) {
	case 0:
		api.getStream(id, this, callback);
		break;
	case 1:
		api.getDestination(id, this, callback);
		break;
	case 2:
		api.getProfile(id, this, callback);
		break;
	case 3:
		api.getGroup(id, this, callback);
		break;
	case 4:
		api.getSession(id, this, callback);
		break;
	case 5:
		api.getCommand(id, this, callback);
		break;
	case 6:
		api.getAsset(id, this, callback);
		break;
	case 7:
		api.getGeneration(id, id, this, callback);
		break;
	case 8:
		api.getFallbackPolicy(id, this, callback);
		break;
	case 9:
		api.getSessionMetrics(id, this, callback);
		break;
	case 10:
		api.getOutput(id, id, this, callback);
		break;
	}
	QTRY_VERIFY_WITH_TIMEOUT(done, 2000);
	QVERIFY(success);
	QVERIFY(accurate);
	QCOMPARE(server.requests.size(), 1);
	const auto segment = QByteArray("entity%20%2F%3F%25%23%D0%BA%D0%BB%D1%8E%D1%87");
	auto expected = QByteArray("/api/v1") + path + segment;
	if (family == 7)
		expected += "/generations/" + segment;
	if (family == 8)
		expected += "/fallback-policy";
	if (family == 9)
		expected += "/metrics";
	if (family == 10)
		expected += "/outputs/" + segment;
	QCOMPARE(server.requests.front().target, expected);
}

void ApiNetworkTest::sseYieldsBetweenBoundedBatches()
{
	// Восемь кадров уже доступны одним ответом, бюджет равен двум. singleShot из первого callback
	// обязан выполниться до третьей доставки: это барьер возврата управления Qt, без измерения скорости CPU.
	HttpServer server;
	QVERIFY(server.listen());
	server.handler = [](const auto &request) {
		QByteArray wire;
		for (int index = 0; index < 8; ++index)
			wire += fixture::frame(
				fixture::event(QStringLiteral("state"), QStringLiteral("event-%1").arg(index)));
		HttpServer::stream(request, wire);
	};
	Limits limits;
	limits.eventsPerTurn = 2;
	EventClient client(limits);
	int received = 0;
	bool yielded = false;
	bool respectedBudget = true;
	client.subscribe(
		server.origin(), QStringLiteral("anchor"), {1, 1}, this,
		[&](auto, const auto &) {
			++received;
			if (received == 1)
				QTimer::singleShot(0, this, [&] { yielded = true; });
			if (received > 2 && !yielded)
				respectedBudget = false;
			return true;
		},
		[](auto, auto, bool) {});
	QTRY_COMPARE_WITH_TIMEOUT(received, 8, 2000);
	QVERIFY(respectedBudget);
	QCOMPARE(client.confirmedCursor(), QStringLiteral("event-7"));
}

void ApiNetworkTest::heartbeatKeepsTransportAliveWithoutInventingEvents()
{
	// Не менее восьми heartbeat занимают дольше одного idle=250 мс. Живой транспорт не должен
	// породить ни прикладное событие, ни ошибку, ни продвижение cursor при отсутствии data.
	HttpServer server;
	QVERIFY(server.listen());
	QTimer heartbeats;
	heartbeats.setInterval(50);
	int beats = 0;
	server.handler = [&](const auto &request) {
		HttpServer::stream(request, ": initial\n\n");
		connect(&heartbeats, &QTimer::timeout, this, [&, request] {
			if (request.socket)
				request.socket->write(": heartbeat\n\n");
			++beats;
		});
		heartbeats.start();
	};
	Limits limits;
	limits.sseIdleMs = 250;
	EventClient client(limits);
	int received = 0;
	int errors = 0;
	client.subscribe(
		server.origin(), QStringLiteral("anchor"), {1, 1}, this,
		[&](auto, const auto &) {
			++received;
			return true;
		},
		[&](auto, auto, bool) { ++errors; });
	QTRY_VERIFY_WITH_TIMEOUT(beats >= 8, 1500);
	QCOMPARE(received, 0);
	QCOMPARE(errors, 0);
	QCOMPARE(client.confirmedCursor(), QStringLiteral("anchor"));
	heartbeats.stop();
	client.shutdown();
}

void ApiNetworkTest::replacingSubscriptionDuringDeliveryDiscardsOldContext()
{
	// Новый subscribe выполняется синхронно из старого EventHandler. В старом ответе ещё семь кадров:
	// они и возвращённое true старого обработчика не вправе изменить новую подписку/confirmed cursor.
	HttpServer server;
	QVERIFY(server.listen());
	server.handler = [&](const auto &request) {
		if (server.requests.size() == 1) {
			QByteArray wire;
			for (int index = 0; index < 8; ++index)
				wire += fixture::frame(
					fixture::event(QStringLiteral("state"), QStringLiteral("old-%1").arg(index)));
			HttpServer::stream(request, wire);
		} else
			HttpServer::stream(request, fixture::frame(fixture::event(QStringLiteral("state"),
										  QStringLiteral("new-current"))));
	};
	Limits limits;
	limits.eventsPerTurn = 1;
	EventClient client(limits);
	int oldDeliveries = 0;
	int currentDeliveries = 0;
	bool contextsValid = true;
	client.subscribe(
		server.origin(), QStringLiteral("old-anchor"), {1, 1}, this,
		[&](auto context, const auto &) {
			contextsValid = contextsValid && context == RequestContext{1, 1};
			++oldDeliveries;
			// Новая подписка создаётся внутри доставки: остаток старого пакета и его подтверждение должны исчезнуть.
			client.subscribe(
				server.origin(), QStringLiteral("new-anchor"), {2, 1}, this,
				[&](auto newContext, const auto &) {
					contextsValid = contextsValid && newContext == RequestContext{2, 1};
					++currentDeliveries;
					return true;
				},
				[](auto, auto, bool) {});
			return true;
		},
		[](auto, auto, bool) {});
	QTRY_COMPARE_WITH_TIMEOUT(currentDeliveries, 1, 3000);
	QCOMPARE(oldDeliveries, 1);
	QVERIFY(contextsValid);
	QCOMPARE(client.confirmedCursor(), QStringLiteral("new-current"));
	QCOMPARE(server.requests.size(), 2);
}

void ApiNetworkTest::explicitServerConnectionBypassesApplicationProxyFactory()
{
	// Петлевой интерфейс может обходить прокси сам по правилам Qt, поэтому используется собственный частный IPv4
	// адрес и контрольный менеджер. Глобальная фабрика меняется только внутри отдельного тестового
	// процесса и восстанавливается qScopeGuard при любом выходе. Нет подходящего адреса — QSKIP.
	QHostAddress localAddress;
	for (const auto &address : QNetworkInterface::allAddresses())
		if (address.protocol() == QAbstractSocket::IPv4Protocol && address.isPrivateUse() &&
		    !address.isLoopback()) {
			localAddress = address;
			break;
		}
	if (localAddress.isNull())
		QSKIP("Для проверки прямого соединения требуется собственный частный IPv4-интерфейс хоста.");
	const bool usedSystemConfiguration = QNetworkProxyFactory::usesSystemConfiguration();
	const auto previousProxy = usedSystemConfiguration ? QNetworkProxy{} : QNetworkProxy::applicationProxy();
	std::atomic_int queries{0};
	const auto restoreProxy = qScopeGuard([&] {
		QNetworkProxyFactory::setApplicationProxyFactory(nullptr);
		QNetworkProxy::setApplicationProxy(previousProxy);
		if (usedSystemConfiguration)
			QNetworkProxyFactory::setUseSystemConfiguration(true);
	});
	QNetworkProxyFactory::setApplicationProxyFactory(new CountingProxyFactory(queries));
	HttpServer server;
	QVERIFY(server.listen(localAddress));
	auto origin = server.origin();
	origin.setHost(localAddress.toString());
	const QByteArray health = R"({"status":"ready","reason":""})";
	server.handler = [&](const auto &request) {
		if (request.target.startsWith("/api/v1/events?"))
			HttpServer::stream(request, fixture::frame(fixture::event()));
		else
			HttpServer::reply(request, health);
	};
	queries = 0;
	{
		// Контрольный клиент доказывает, что фабрика действительно активна перед проверкой собственных клиентов.
		QNetworkAccessManager control;
		auto *reply =
			control.get(QNetworkRequest(origin.resolved(QUrl(QStringLiteral("/api/v1/health/ready")))));
		QTRY_VERIFY_WITH_TIMEOUT(reply->isFinished(), 2000);
		QCOMPARE(reply->error(), QNetworkReply::NoError);
		QCOMPARE(reply->readAll(), health);
		QVERIFY2(queries.load() > 0, "Контрольный запрос обязан вызывать тестовую фабрику прокси.");
	}
	queries = 0;
	APIClient api;
	const RequestContext expected{10, 20};
	api.configure(origin, expected);
	std::optional<Result<Health>> ready;
	RequestContext readyContext;
	api.getReady(this, [&](auto context, auto result) {
		readyContext = context;
		ready = std::move(result);
	});
	QTRY_VERIFY_WITH_TIMEOUT(ready.has_value(), 2000);
	QVERIFY(*ready);
	QCOMPARE(ready->value->status, QStringLiteral("ready"));
	QVERIFY(readyContext == expected);
	EventClient events;
	std::optional<RuntimeEvent> received;
	bool failed = false;
	RequestContext eventContext;
	events.subscribe(
		origin, QStringLiteral("anchor"), expected, this,
		[&](auto context, const auto &event) {
			eventContext = context;
			received = event;
			return true;
		},
		[&](auto, auto, bool) { failed = true; });
	QTRY_VERIFY_WITH_TIMEOUT(received.has_value() || failed, 2000);
	QVERIFY(!failed);
	QVERIFY(eventContext == expected);
	QCOMPARE(received->cursor, QStringLiteral("opaque/next"));
	QCOMPARE(events.confirmedCursor(), received->cursor);
	events.shutdown();
	api.shutdown();
	QCOMPARE(queries.load(), 0);
	QVERIFY(!QNetworkProxyFactory::usesSystemConfiguration());
	QNetworkProxyFactory::proxyForQuery(QNetworkProxyQuery(origin));
	QCOMPARE(queries.load(), 1);
	QCOMPARE(server.requests.size(), 3);
	for (const auto &request : server.requests)
		QCOMPARE(request.method, QByteArray("GET"));
}

QTEST_GUILESS_MAIN(ApiNetworkTest)
#include "api-network-test.moc"
