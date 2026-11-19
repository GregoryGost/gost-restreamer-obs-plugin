#include "api/api-client.hpp"
#include "api/json-decoder.hpp"
#include "api/json-encoder.hpp"
#include "fixtures/api-json.hpp"
#include "fixtures/http-server.hpp"
#include "fixtures/upload-server.hpp"
#include "fixtures/workspace-temp.hpp"

#include <QFile>
#include <QPointer>
#include <QTest>
#include <QTimer>
#include <QUrlQuery>
#include <limits>

using namespace gost::api;

// Проверяет HTTP-подготовку независимо от команд Session и от фактического генератора медиа.
class FallbackApiTest final : public QObject {
	Q_OBJECT
private slots:
	// Создание и удаление сохраняют точное имя,201/204 и query U64.
	void assetCrudUsesExactRevision();
	// Полный policy содержит все четыре точных интервала; очистка передаёт явный null.
	void policyPreservesWholeDocumentAndNull();
	// Задаёт допустимый 202 и несовместимые Location/IDs/статусы.
	void preparationContract_data();
	// Непроверенный успешный ответ остаётся отправленной ошибкой, а не Receipt или готовностью.
	void preparationContract();
	// Передача бинарного тела имеет точные Content-Length/kind/revision и отдельное принятие 202.
	void uploadPreservesBytesAndProgress();
	// Задаёт локальные ошибки до отправки файла.
	void invalidUpload_data();
	// Неверные параметры, размер и файл не создают сетевого запроса.
	void invalidUpload();
	// Рост файла в очереди не расширяет захваченный Content-Length и передаваемый префикс.
	void queuedUploadUsesCapturedSize();
	// Адресная отмена очереди не прерывает чужой запрос и не отправляет upload позднее.
	void queuedCancellationKeepsOtherReceiver();
	// Отмена активного неполного upload подавляет callback и освобождает файловый источник.
	void activeCancellationAndReceiverDestruction_data();
	// Удаление receiver и явная отмена дают одинаковую границу владения транспортом.
	void activeCancellationAndReceiverDestruction();
	// Срок upload считается с постановки в очередь, без отправки после истечения.
	void uploadDeadlineIncludesQueue();
	// Upload не наследует короткий срок JSON; собственный общий срок ограничивает ожидание ответа.
	void uploadUsesSeparateActiveDeadline();
	// Смена контекста из progress callback не доставляет старое принятие и не ломает новые GET.
	void progressCallbackCanReconfigure();
	// Реально остановленное тело исчерпывает idle; клиент не повторяет PUT после ошибки.
	void partialUploadIdleNeverReplays();
	// Отмена и удаление получателя между страницами отзывают отложенный GET, сохраняя чужой scope.
	void deferredPageCancellation_data();
	// Чужой GET завершается после отзыва обхода, а вторая страница и результат обхода не доставляются.
	void deferredPageCancellation();
	// Задаёт разные пути доставки результата после диагностики.
	void diagnosticCancellationSuppressesResult_data();
	// Отмена из диагностики видит уже отправленное задание и подавляет его callback.
	void diagnosticCancellationSuppressesResult();
	// Задаёт удаление клиента из прогресса либо окончательного результата upload.
	void callbackCanDeleteClient_data();
	// Удаление клиента отзывает очередь и освобождает файл без поздних callback.
	void callbackCanDeleteClient();
	// Ранний EOF после захвата размера даёт отправленную ошибку без повторной передачи.
	void truncatedQueuedUploadFailsWithoutReplay();
};

void FallbackApiTest::assetCrudUsesExactRevision()
{
	// server — единственный endpoint; ответ создания собран отдельно от исходящего writer.
	HttpServer server;
	QVERIFY(server.listen());
	server.handler = [](const auto &request) {
		if (request.method == "POST")
			HttpServer::reply(request, fixture::bytes(fixture::asset()), 201);
		else
			HttpServer::reply(request, {}, 204, {});
	};
	// api использует узнаваемый контекст; result хранит только проверенный DTO.
	APIClient api;
	api.configure(server.origin(), {7, 9});
	std::optional<Result<Asset>> result;
	api.createAsset({QStringLiteral("Обложка / + % ключ")}, this, [&](auto context, auto value) {
		QCOMPARE(context, (RequestContext{7, 9}));
		result = value;
	});
	QTRY_VERIFY(result.has_value());
	QVERIFY(*result);
	QCOMPARE(result->value->id, QStringLiteral("asset"));
	QCOMPARE(server.requests.size(), 1);
	QCOMPARE(server.requests[0].method, QByteArray("POST"));
	QCOMPARE(server.requests[0].target, QByteArray("/api/v1/fallback-assets"));
	QCOMPARE(fixture::object(server.requests[0].body),
		 (QJsonObject{{QStringLiteral("name"), QStringLiteral("Обложка / + % ключ")}}));
	// removed фиксирует только пустой 204, id содержит символы, требующие одного кодированного сегмента.
	std::optional<Result<Empty>> removed;
	const auto id = QStringLiteral("asset /?%#ключ");
	api.deleteAsset(id, std::numeric_limits<quint64>::max(), this, [&](auto, auto value) { removed = value; });
	QTRY_VERIFY(removed.has_value());
	QVERIFY(*removed);
	QCOMPARE(server.requests.size(), 2);
	QCOMPARE(server.requests[1].method, QByteArray("DELETE"));
	QCOMPARE(server.requests[1].target, QByteArray("/api/v1/fallback-assets/") + QUrl::toPercentEncoding(id) +
						    "?expected_revision=18446744073709551615");
}

void FallbackApiTest::policyPreservesWholeDocumentAndNull()
{
	// server возвращает revision Stream, намеренно отличную от expected_revision.
	HttpServer server;
	QVERIFY(server.listen());
	server.handler = [](const auto &request) {
		HttpServer::reply(request, R"({"revision":9007199254740997,"fallback":null})");
	};
	// api передаёт полный документ, watchdog содержит числа выше точности double.
	APIClient api;
	api.configure(server.origin(), {1, 2});
	const Watchdog watchdog{9007199254740993ULL, 9007199254740995ULL, 1, 5000000001ULL};
	// result сохраняет полученную revision Stream независимо от исходного CAS-намерения.
	std::optional<Result<PolicyResult>> result;
	api.setFallbackPolicy(QStringLiteral("stream"),
			      {18446744073709551615ULL, Policy{QStringLiteral("asset"), watchdog}}, this,
			      [&](auto, auto value) { result = value; });
	QTRY_VERIFY(result.has_value());
	QVERIFY(*result);
	QCOMPARE(result->value->revision, quint64(9007199254740997ULL));
	QCOMPARE(server.requests.size(), 1);
	QCOMPARE(server.requests[0].method, QByteArray("PUT"));
	QCOMPARE(server.requests[0].target, QByteArray("/api/v1/streams/stream/fallback-policy"));
	// wire проверяет точные литералы, которые нельзя сравнивать через double дерева Qt.
	const auto wire = server.requests[0].body;
	QVERIFY(wire.contains("\"expected_revision\":18446744073709551615"));
	QVERIFY(wire.contains("\"video_timeout_ns\":9007199254740993"));
	QVERIFY(wire.contains("\"audio_timeout_ns\":9007199254740995"));
	QVERIFY(wire.contains("\"recovery_window_ns\":1"));
	QVERIFY(wire.contains("\"keyframe_timeout_ns\":5000000001"));
	QCOMPARE(fixture::object(wire).value(QStringLiteral("fallback")).toObject().size(), 2);
	result.reset();
	api.setFallbackPolicy(QStringLiteral("stream"), {9007199254740997ULL, std::nullopt}, this,
			      [&](auto, auto value) { result = value; });
	QTRY_VERIFY(result.has_value());
	QVERIFY(*result);
	QVERIFY(server.requests[1].body.contains("\"expected_revision\":9007199254740997"));
	QCOMPARE(fixture::object(server.requests[1].body).value(QStringLiteral("fallback")),
		 QJsonValue(QJsonValue::Null));
	QCOMPARE(fixture::object(server.requests[1].body).size(), 2);
}

void FallbackApiTest::preparationContract_data()
{
	QTest::addColumn<QByteArray>("body");
	QTest::addColumn<QByteArray>("location");
	QTest::addColumn<int>("status");
	QTest::addColumn<bool>("accepted");
	QTest::newRow("accepted") << QByteArray(R"({"asset_id":"asset","generation_id":"generation"})")
				  << QByteArray("/api/v1/fallback-assets/asset/generations/generation") << 202 << true;
	QTest::newRow("missing-location")
		<< QByteArray(R"({"asset_id":"asset","generation_id":"generation"})") << QByteArray{} << 202 << false;
	QTest::newRow("foreign-origin")
		<< QByteArray(R"({"asset_id":"asset","generation_id":"generation"})")
		<< QByteArray("http://127.0.0.1:1/api/v1/fallback-assets/asset/generations/generation") << 202 << false;
	QTest::newRow("wrong-generation-location")
		<< QByteArray(R"({"asset_id":"asset","generation_id":"generation"})")
		<< QByteArray("/api/v1/fallback-assets/asset/generations/other") << 202 << false;
	QTest::newRow("query-location")
		<< QByteArray(R"({"asset_id":"asset","generation_id":"generation"})")
		<< QByteArray("/api/v1/fallback-assets/asset/generations/generation?x=1") << 202 << false;
	QTest::newRow("fragment-location")
		<< QByteArray(R"({"asset_id":"asset","generation_id":"generation"})")
		<< QByteArray("/api/v1/fallback-assets/asset/generations/generation#fragment") << 202 << false;
	QTest::newRow("wrong-parent")
		<< QByteArray(R"({"asset_id":"other","generation_id":"generation"})")
		<< QByteArray("/api/v1/fallback-assets/other/generations/generation") << 202 << false;
	QTest::newRow("empty-generation") << QByteArray(R"({"asset_id":"asset","generation_id":""})")
					  << QByteArray("/api/v1/fallback-assets/asset/generations/") << 202 << false;
	QTest::newRow("receipt-is-not-preparation")
		<< QByteArray(R"({"id":"command","request_id":"request","status":"PENDING"})")
		<< QByteArray("/api/v1/commands/command") << 202 << false;
	QTest::newRow("wrong-success-status")
		<< QByteArray(R"({"asset_id":"asset","generation_id":"generation"})")
		<< QByteArray("/api/v1/fallback-assets/asset/generations/generation") << 200 << false;
}

void FallbackApiTest::preparationContract()
{
	// body/location/status задают независимый ответ; accepted — результат внешнего контракта.
	QFETCH(QByteArray, body);
	QFETCH(QByteArray, location);
	QFETCH(int, status);
	QFETCH(bool, accepted);
	// server не делает переход по Location и регистрирует единственную исходную запись.
	HttpServer server;
	QVERIFY(server.listen());
	server.handler = [&](const auto &request) {
		HttpServer::reply(request, body, status, "application/json",
				  location.isEmpty() ? QByteArray{} : "Location: " + location + "\r\n");
	};
	// api и result наблюдают именно Preparation, а не Receipt.
	APIClient api;
	api.configure(server.origin(), {1, 1});
	// result заполняется callback один раз и отделяет проверенный 202 от ошибок транспорта/контракта.
	std::optional<Result<Preparation>> result;
	api.rebuildAsset(QStringLiteral("asset"), 18446744073709551615ULL, this,
			 [&](auto, auto value) { result = value; });
	QTRY_VERIFY(result.has_value());
	QCOMPARE(bool(*result), accepted);
	QCOMPARE(server.requests.size(), 1);
	QCOMPARE(server.requests[0].method, QByteArray("POST"));
	QCOMPARE(server.requests[0].target, QByteArray("/api/v1/fallback-assets/asset/rebuild"));
	QCOMPARE(server.requests[0].body, QByteArray(R"({"expected_revision":18446744073709551615})"));
	if (accepted) {
		QCOMPARE(result->value->assetId, QStringLiteral("asset"));
		QCOMPARE(result->value->generationId, QStringLiteral("generation"));
	} else {
		QVERIFY(result->error.has_value());
		QCOMPARE(result->error->kind, ErrorKind::Contract);
		QVERIFY(result->error->requestSent);
	}
}

void FallbackApiTest::uploadPreservesBytesAndProgress()
{
	// temp/file содержат синтетические бинарные байты в workspace, включая нуль и 0xff.
	WorkspaceTemp temp(QStringLiteral("fallback-upload"));
	QVERIFY(temp.isValid());
	QFile file(temp.filePath(QStringLiteral("source.bin")));
	QVERIFY(file.open(QIODevice::WriteOnly));
	// bytes содержит независимый бинарный оригинал, который должен пройти без multipart и перекодирования.
	const QByteArray bytes = QByteArray::fromHex("000102ff0d0a7f80") + QByteArray(65536, 'x');
	QCOMPARE(file.write(bytes), qint64(bytes.size()));
	file.close();
	// server принимает ограниченный файл и отвечает только после полного тела.
	HttpServer server;
	QVERIFY(server.listen());
	server.handler = [](const auto &request) {
		HttpServer::reply(request, R"({"asset_id":"asset","generation_id":"generation"})", 202,
				  "application/json",
				  "Location: /api/v1/fallback-assets/asset/generations/generation\r\n");
	};
	// api/result отделяют HTTP-принятие от прогресса; progress хранит пары sent/total.
	APIClient api;
	api.configure(server.origin(), {3, 4});
	// result заполняется callback один раз и отделяет проверенный 202 от ошибок транспорта/контракта.
	std::optional<Result<Preparation>> result;
	QList<QPair<qint64, qint64>> progress;
	api.uploadAsset(
		QStringLiteral("asset"), 9007199254740993ULL, QStringLiteral("video"), file.fileName(), this,
		[&](auto, auto value) { result = value; },
		[&](auto context, qint64 sent, qint64 total) {
			QCOMPARE(context, (RequestContext{3, 4}));
			progress.append({sent, total});
		});
	QTRY_VERIFY(result.has_value());
	QVERIFY(*result);
	QCOMPARE(result->value->generationId, QStringLiteral("generation"));
	QCOMPARE(server.requests.size(), 1);
	QCOMPARE(server.requests[0].body, bytes);
	QCOMPARE(server.requests[0].method, QByteArray("PUT"));
	// query проверяет обе независимые query-величины без зависимости от порядка параметров.
	const QUrlQuery query(QUrl::fromEncoded("http://local" + server.requests[0].target));
	QCOMPARE(query.queryItemValue(QStringLiteral("expected_revision")), QStringLiteral("9007199254740993"));
	QCOMPARE(query.queryItemValue(QStringLiteral("kind")), QStringLiteral("video"));
	QVERIFY(server.requests[0].headers.toLower().contains("content-type: application/octet-stream"));
	QVERIFY(server.requests[0].headers.toLower().contains("content-length: " + QByteArray::number(bytes.size())));
	QVERIFY(!progress.isEmpty());
	// item содержит реальный сигнал передачи; total всегда равен захваченному размеру файла.
	for (const auto &item : progress) {
		QVERIFY(item.first >= 0 && item.first <= bytes.size());
		QCOMPARE(item.second, qint64(bytes.size()));
	}
}

void FallbackApiTest::invalidUpload_data()
{
	QTest::addColumn<QString>("violation");
	// violation задаёт отдельный локальный запрет, а не ожидаемый текст реализации.
	for (const auto *violation : {"missing", "empty", "directory", "oversized", "revision", "kind"})
		QTest::newRow(violation) << QString::fromLatin1(violation);
}

void FallbackApiTest::invalidUpload()
{
	// violation выбирает один дефект входа; temp и file никогда не выходят из workspace.
	QFETCH(QString, violation);
	WorkspaceTemp temp(QStringLiteral("fallback-invalid"));
	QVERIFY(temp.isValid());
	QFile file(temp.filePath(QStringLiteral("source.bin")));
	if (violation != QStringLiteral("missing")) {
		QVERIFY(file.open(QIODevice::WriteOnly));
		if (violation != QStringLiteral("empty"))
			QCOMPARE(file.write(QByteArray(32, 'a')), qint64(32));
		file.close();
	}
	// limits снижает размер до 16 байт только для границы oversized; остальные пределы штатные.
	// limits задаёт только внутренние бюджеты этой изолированной последовательности запросов.
	Limits limits;
	if (violation == QStringLiteral("oversized"))
		limits.uploadMaxBytes = 16;
	// server сохраняет реальные байты HTTP-запросов и обслуживает только loopback.
	HttpServer server;
	QVERIFY(server.listen());
	// api выполняет настоящие HTTP-запросы только к принадлежащей сценарию фикстуре.
	APIClient api(limits);
	api.configure(server.origin(), {1, 1});
	// result заполняется callback один раз и отделяет проверенный 202 от ошибок транспорта/контракта.
	std::optional<Result<Preparation>> result;
	api.uploadAsset(QStringLiteral("asset"), violation == QStringLiteral("revision") ? 0 : 1,
			violation == QStringLiteral("kind") ? QStringLiteral("audio") : QStringLiteral("image"),
			violation == QStringLiteral("directory") ? temp.path() : file.fileName(), this,
			[&](auto, auto value) { result = value; });
	QTRY_VERIFY(result.has_value());
	QVERIFY(!*result);
	QVERIFY(!result->error->requestSent);
	QVERIFY(server.requests.isEmpty());
}

void FallbackApiTest::queuedUploadUsesCapturedSize()
{
	// server удерживает GET, чтобы изменить внешний файл после принятия upload в очередь.
	HttpServer server;
	QVERIFY(server.listen());
	server.handler = [](const auto &request) {
		if (request.method == "PUT")
			HttpServer::reply(request, R"({"asset_id":"asset","generation_id":"generation"})", 202,
					  "application/json",
					  "Location: /api/v1/fallback-assets/asset/generations/generation\r\n");
	};
	// limits оставляет один транспортный слот; api хранит ожидающий upload отдельно от GET.
	Limits limits;
	limits.maxActiveRequests = 1;
	APIClient api(limits);
	api.configure(server.origin(), {1, 1});
	// other удерживает транспортный слот, пока источник upload меняется в очереди.
	QObject other;
	api.getAsset(QStringLiteral("asset"), &other, [](auto, auto) {});
	QTRY_COMPARE(server.requests.size(), 1);
	// temp/file содержат исходные шесть байт до постановки upload в очередь.
	WorkspaceTemp temp(QStringLiteral("fallback-captured-size"));
	QVERIFY(temp.isValid());
	QFile file(temp.filePath(QStringLiteral("source.bin")));
	QVERIFY(file.open(QIODevice::WriteOnly));
	QCOMPARE(file.write("source"), qint64(6));
	file.close();
	// result заполняется только после передачи первоначального префикса и проверенного 202.
	std::optional<Result<Preparation>> result;
	api.uploadAsset(QStringLiteral("asset"), 1, QStringLiteral("image"), file.fileName(), this,
			[&](auto, auto value) { result = value; });
	QVERIFY(file.open(QIODevice::Append));
	QCOMPARE(file.write("grown"), qint64(5));
	file.close();
	HttpServer::reply(server.requests[0], fixture::bytes(fixture::asset()));
	QTRY_VERIFY(result.has_value());
	QVERIFY(*result);
	QCOMPARE(server.requests.size(), 2);
	QCOMPARE(server.requests[1].body, QByteArray("source"));
	QVERIFY(server.requests[1].headers.toLower().contains("content-length: 6"));
}

void FallbackApiTest::queuedCancellationKeepsOtherReceiver()
{
	// server удерживает чужой GET, чтобы upload гарантированно остался в очереди.
	HttpServer server;
	QVERIFY(server.listen());
	// limits задаёт только внутренние бюджеты этой изолированной последовательности запросов.
	Limits limits;
	limits.maxActiveRequests = 1;
	// api выполняет настоящие HTTP-запросы только к принадлежащей сценарию фикстуре.
	APIClient api(limits);
	api.configure(server.origin(), {1, 1});
	// other адресует чужой GET: отмена владельца upload не должна затрагивать его запрос.
	QObject other;
	// uploadOwner ограничивает жизнь единственного ожидающего upload.
	QObject uploadOwner;
	// otherResult сохраняет исход чужого GET после отмены очереди upload.
	std::optional<Result<Asset>> otherResult;
	// uploadCallback обнаруживает запрещённую доставку после адресной отмены.
	bool uploadCallback = false;
	api.getAsset(QStringLiteral("asset"), &other, [&](auto, auto value) { otherResult = value; });
	QTRY_COMPARE(server.requests.size(), 1);
	// temp/file задают читаемый источник, открываемый очередью без его полной копии.
	WorkspaceTemp temp(QStringLiteral("fallback-queued"));
	QVERIFY(temp.isValid());
	QFile file(temp.filePath(QStringLiteral("source.bin")));
	QVERIFY(file.open(QIODevice::WriteOnly));
	QCOMPARE(file.write("source"), qint64(6));
	file.close();
	api.uploadAsset(QStringLiteral("asset"), 1, QStringLiteral("image"), file.fileName(), &uploadOwner,
			[&](auto, auto) { uploadCallback = true; });
	QVERIFY(!api.cancelRequests(&uploadOwner));
	HttpServer::reply(server.requests[0], fixture::bytes(fixture::asset()));
	QTRY_VERIFY(otherResult.has_value());
	QVERIFY(*otherResult);
	QCoreApplication::processEvents();
	QVERIFY(!uploadCallback);
	QCOMPARE(server.requests.size(), 1);
	QVERIFY(QFile::remove(file.fileName()));
}

void FallbackApiTest::activeCancellationAndReceiverDestruction_data()
{
	QTest::addColumn<bool>("destroyReceiver");
	QTest::newRow("cancel-owner") << false;
	QTest::newRow("destroy-owner") << true;
}

void FallbackApiTest::activeCancellationAndReceiverDestruction()
{
	// destroyReceiver выбирает один из двух контрактных способов адресной отмены.
	QFETCH(bool, destroyReceiver);
	// temp/file дают большой файл без полной копии в памяти теста.
	WorkspaceTemp temp(QStringLiteral("fallback-active"));
	QVERIFY(temp.isValid());
	QFile file(temp.filePath(QStringLiteral("large.bin")));
	QVERIFY(file.open(QIODevice::WriteOnly));
	QVERIFY(file.resize(64LL * 1024 * 1024));
	file.close();
	// server останавливает чтение после заголовков, делая отмену частичной наблюдаемой.
	// server ограничивает сетевой буфер и не копирует полное тело оригинала.
	UploadServer server;
	QVERIFY(server.listen());
	server.onHeaders = [](const auto &request) {
		request->drain = false;
	};
	// api выполняет настоящие HTTP-запросы только к принадлежащей сценарию фикстуре.
	APIClient api;
	api.configure(server.origin(), {1, 1});
	// owner ограничивает жизнь callback; called обнаруживает ошибочную доставку после отмены.
	auto owner = std::make_unique<QObject>();
	bool called = false;
	api.uploadAsset(QStringLiteral("asset"), 1, QStringLiteral("video"), file.fileName(), owner.get(),
			[&](auto, auto) { called = true; });
	QTRY_VERIFY(!server.requests.isEmpty() && server.requests[0]->ready);
	QVERIFY(server.requests[0]->received < server.requests[0]->length);
	if (destroyReceiver)
		owner.reset();
	else
		QVERIFY(api.cancelRequests(owner.get()));
	server.resume(server.requests[0]);
	QTRY_VERIFY(server.requests[0]->disconnected);
	QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
	QVERIFY(!called);
	QVERIFY(QFile::remove(file.fileName()));
	QCOMPARE(server.requests.size(), 1);
}

void FallbackApiTest::uploadDeadlineIncludesQueue()
{
	// server удерживает GET; upload имеет собственный короткий бюджет с момента enqueue.
	HttpServer server;
	QVERIFY(server.listen());
	// limits задаёт только внутренние бюджеты этой изолированной последовательности запросов.
	Limits limits;
	limits.maxActiveRequests = 1;
	limits.uploadDeadlineMs = 80;
	// api выполняет настоящие HTTP-запросы только к принадлежащей сценарию фикстуре.
	APIClient api(limits);
	api.configure(server.origin(), {1, 1});
	// other адресует чужой GET: отмена владельца upload не должна затрагивать его запрос.
	QObject other;
	api.getAsset(QStringLiteral("asset"), &other, [](auto, auto) {});
	QTRY_COMPARE(server.requests.size(), 1);
	// temp создаёт каталог источника внутри workspace и удаляет его после завершения сценария.
	WorkspaceTemp temp(QStringLiteral("fallback-deadline"));
	QVERIFY(temp.isValid());
	// file содержит небольшой читаемый оригинал для ожидающей загрузки.
	QFile file(temp.filePath(QStringLiteral("source.bin")));
	QVERIFY(file.open(QIODevice::WriteOnly));
	QCOMPARE(file.write("source"), qint64(6));
	file.close();
	// result заполняется callback один раз и отделяет проверенный 202 от ошибок транспорта/контракта.
	std::optional<Result<Preparation>> result;
	api.uploadAsset(QStringLiteral("asset"), 1, QStringLiteral("image"), file.fileName(), this,
			[&](auto, auto value) { result = value; });
	QTRY_VERIFY_WITH_TIMEOUT(result.has_value(), 1500);
	QVERIFY(!*result);
	QCOMPARE(result->error->kind, ErrorKind::Timeout);
	QVERIFY(!result->error->requestSent);
	QVERIFY(api.cancelRequests(&other));
	QCoreApplication::processEvents();
	QCOMPARE(server.requests.size(), 1);
}

void FallbackApiTest::uploadUsesSeparateActiveDeadline()
{
	// temp/file содержат небольшой оригинал; обычный REST имеет меньший срок, чем управляемый ответ upload.
	WorkspaceTemp temp(QStringLiteral("fallback-separate-deadline"));
	QVERIFY(temp.isValid());
	QFile file(temp.filePath(QStringLiteral("source.bin")));
	QVERIFY(file.open(QIODevice::WriteOnly));
	QCOMPARE(file.write("source"), qint64(6));
	file.close();
	// server сохраняет реальные байты HTTP-запросов и обслуживает только loopback.
	HttpServer server;
	QVERIFY(server.listen());
	// limits задаёт только внутренние бюджеты этой изолированной последовательности запросов.
	Limits limits;
	limits.httpDeadlineMs = 10;
	limits.httpIdleMs = 10;
	limits.uploadDeadlineMs = 300;
	limits.uploadIdleMs = 1000;
	// api выполняет настоящие HTTP-запросы только к принадлежащей сценарию фикстуре.
	APIClient api(limits);
	api.configure(server.origin(), {1, 1});
	// first задерживает лишь первый ответ за границу JSON-срока; второй ответ удерживается до upload deadline.
	bool first = true;
	server.handler = [&](const auto &request) {
		if (!first)
			return;
		first = false;
		QTimer::singleShot(50, &server, [request] {
			HttpServer::reply(request, R"({"asset_id":"asset","generation_id":"generation"})", 202,
					  "application/json",
					  "Location: /api/v1/fallback-assets/asset/generations/generation\r\n");
		});
	};
	// result хранит ответ каждого последовательного upload независимо от обычного REST timeout.
	// result заполняется callback один раз и отделяет проверенный 202 от ошибок транспорта/контракта.
	std::optional<Result<Preparation>> result;
	api.uploadAsset(QStringLiteral("asset"), 1, QStringLiteral("image"), file.fileName(), this,
			[&](auto, auto value) { result = value; });
	QTRY_VERIFY(result.has_value());
	QVERIFY(*result);
	result.reset();
	api.uploadAsset(QStringLiteral("asset"), 1, QStringLiteral("image"), file.fileName(), this,
			[&](auto, auto value) { result = value; });
	QTRY_VERIFY(result.has_value());
	QVERIFY(!*result);
	QCOMPARE(result->error->kind, ErrorKind::Timeout);
	QVERIFY(result->error->requestSent);
	QCOMPARE(server.requests.size(), 2);
}

void FallbackApiTest::progressCallbackCanReconfigure()
{
	// temp/file содержат оригинал для реального Qt uploadProgress.
	WorkspaceTemp temp(QStringLiteral("fallback-progress-context"));
	QVERIFY(temp.isValid());
	QFile file(temp.filePath(QStringLiteral("source.bin")));
	QVERIFY(file.open(QIODevice::WriteOnly));
	QCOMPARE(file.write(QByteArray(65536, 'p')), qint64(65536));
	file.close();
	// server сохраняет реальные байты HTTP-запросов и обслуживает только loopback.
	HttpServer server;
	QVERIFY(server.listen());
	server.handler = [](const auto &request) {
		if (request.method == "GET")
			HttpServer::reply(request, fixture::bytes(fixture::asset()));
	};
	// api выполняет настоящие HTTP-запросы только к принадлежащей сценарию фикстуре.
	APIClient api;
	api.configure(server.origin(), {1, 1});
	// changed/called отделяют смену контекста из callback от запрещённой доставки старогорезультата.
	bool changed = false;
	bool called = false;
	api.uploadAsset(
		QStringLiteral("asset"), 1, QStringLiteral("image"), file.fileName(), this,
		[&](auto, auto) { called = true; },
		[&](auto context, qint64 sent, qint64) {
			QCOMPARE(context, (RequestContext{1, 1}));
			if (!changed && sent > 0) {
				changed = true;
				api.configure(server.origin(), {2, 2});
			}
		});
	QTRY_VERIFY(changed);
	// current доказывает работоспособность клиента после повторной configure.
	std::optional<Result<Asset>> current;
	api.getAsset(QStringLiteral("asset"), this, [&](auto context, auto value) {
		QCOMPARE(context, (RequestContext{2, 2}));
		current = value;
	});
	QTRY_VERIFY(current.has_value());
	QVERIFY(*current);
	QVERIFY(!called);
	QVERIFY(QFile::remove(file.fileName()));
}

void FallbackApiTest::partialUploadIdleNeverReplays()
{
	// temp/file создают источник больше сетевого окна, server прекращает его чтение при заголовках.
	WorkspaceTemp temp(QStringLiteral("fallback-idle"));
	QVERIFY(temp.isValid());
	QFile file(temp.filePath(QStringLiteral("large.bin")));
	QVERIFY(file.open(QIODevice::WriteOnly));
	QVERIFY(file.resize(64LL * 1024 * 1024));
	file.close();
	// server ограничивает сетевой буфер и не копирует полное тело оригинала.
	UploadServer server;
	QVERIFY(server.listen());
	server.onHeaders = [](const auto &request) {
		request->drain = false;
	};
	// limits отличает короткий idle от более длинного общего бюджета.
	// limits задаёт только внутренние бюджеты этой изолированной последовательности запросов.
	Limits limits;
	limits.uploadIdleMs = 80;
	limits.uploadDeadlineMs = 2000;
	// api выполняет настоящие HTTP-запросы только к принадлежащей сценарию фикстуре.
	APIClient api(limits);
	api.configure(server.origin(), {1, 1});
	// result заполняется callback один раз и отделяет проверенный 202 от ошибок транспорта/контракта.
	std::optional<Result<Preparation>> result;
	api.uploadAsset(QStringLiteral("asset"), 1, QStringLiteral("video"), file.fileName(), this,
			[&](auto, auto value) { result = value; });
	QTRY_VERIFY_WITH_TIMEOUT(result.has_value(), 3000);
	QVERIFY(!*result);
	QCOMPARE(result->error->kind, ErrorKind::Timeout);
	QVERIFY(result->error->requestSent);
	QCOMPARE(server.requests.size(), 1);
	QVERIFY(server.requests[0]->received < server.requests[0]->length);
	// После проверки частичного тела разрешаем чтение буфера: иначе backpressure скрывает входящий FIN.
	server.resume(server.requests[0]);
	QTRY_VERIFY(server.requests[0]->disconnected);
	QVERIFY(QFile::remove(file.fileName()));
}

void FallbackApiTest::deferredPageCancellation_data()
{
	QTest::addColumn<bool>("destroyReceiver");
	QTest::newRow("cancel-between-pages") << false;
	QTest::newRow("destroy-between-pages") << true;
}

void FallbackApiTest::deferredPageCancellation()
{
	// destroyReceiver выбирает способ отзыва уже принятой первой страницы.
	QFETCH(bool, destroyReceiver);
	// server удерживает чужой GET; первая страница всегда требует продолжения.
	HttpServer server;
	QVERIFY(server.listen());
	server.handler = [](const auto &request) {
		if (request.target.startsWith("/api/v1/fallback-assets?"))
			HttpServer::reply(request, fixture::page({fixture::asset()}, QStringLiteral("page-two")));
	};
	// limits позволяет чужому GET оставаться активным во время обработки страницы.
	Limits limits;
	limits.maxActiveRequests = 2;
	// api разделяет адресуемые scopes, сохраняя один origin/context.
	APIClient api(limits);
	api.configure(server.origin(), {1, 1});
	// other удерживает независимый запрос; otherResult подтверждает сохранение его callback.
	QObject other;
	std::optional<Result<Asset>> otherResult;
	api.getAsset(QStringLiteral("asset"), &other, [&](auto, auto value) { otherResult = value; });
	QTRY_COMPARE(server.requests.size(), 1);
	// owner ограничивает обход; called обнаруживает запрещённую доставку коллекции после отмены.
	auto owner = std::make_unique<QObject>();
	bool called = false;
	// scheduled ставится при диагностике первой страницы, cancelled — на следующем обороте событий.
	bool scheduled = false;
	bool cancelled = false;
	api.setDiagnosticCallback([&](const auto &) {
		if (scheduled)
			return;
		scheduled = true;
		// Отмена попадает в очередь раньше walkNext, поставленного после обработки первой страницы.
		QTimer::singleShot(0, this, [&] {
			if (destroyReceiver)
				owner.reset();
			else
				api.cancelRequests(owner.get());
			cancelled = true;
			HttpServer::reply(server.requests[0], fixture::bytes(fixture::asset()));
		});
	});
	api.getAllAssets(owner.get(), [&](auto, auto) { called = true; });
	QTRY_VERIFY(cancelled && otherResult.has_value());
	QVERIFY(*otherResult);
	QCoreApplication::processEvents();
	QVERIFY(!called);
	QCOMPARE(server.requests.size(), 2);
	QVERIFY(!server.requests[1].target.contains("cursor="));
}

void FallbackApiTest::diagnosticCancellationSuppressesResult_data()
{
	QTest::addColumn<QString>("operation");
	// operation покрывает typed decode, пустой204, Preparation202 и обход коллекции.
	for (const auto *operation : {"get", "delete", "rebuild", "walk"})
		QTest::newRow(operation) << QString::fromLatin1(operation);
}

void FallbackApiTest::diagnosticCancellationSuppressesResult()
{
	// operation выбирает путь окончательной доставки, не меняя семантику адресной отмены.
	QFETCH(QString, operation);
	// server возвращает независимые корректные документы, чтобы отмена была единственной причиной подавления.
	HttpServer server;
	QVERIFY(server.listen());
	server.handler = [](const auto &request) {
		if (request.method == "DELETE")
			HttpServer::reply(request, {}, 204, {});
		else if (request.method == "POST")
			HttpServer::reply(request, R"({"asset_id":"asset","generation_id":"generation"})", 202,
					  "application/json",
					  "Location: /api/v1/fallback-assets/asset/generations/generation\r\n");
		else if (request.target.startsWith("/api/v1/fallback-assets?"))
			HttpServer::reply(request, fixture::page({fixture::asset()}));
		else
			HttpServer::reply(request, fixture::bytes(fixture::asset()));
	};
	// api ограничивает очередь одним слотом: чужой запрос продолжится после отозванной доставки.
	Limits limits;
	limits.maxActiveRequests = 1;
	APIClient api(limits);
	api.configure(server.origin(), {1, 1});
	// owner адресует отменяемый результат; other имеет независимое право доставки.
	QObject owner;
	QObject other;
	// cancelled и sent сохраняют факт входа в диагностику и точный результат cancelRequests.
	bool cancelled = false;
	bool sent = false;
	// called обнаруживает запрещённый callback; otherResult подтверждает работоспособность чужого scope.
	bool called = false;
	std::optional<Result<Asset>> otherResult;
	api.setDiagnosticCallback([&](const auto &) {
		if (!cancelled) {
			cancelled = true;
			sent = api.cancelRequests(&owner);
		}
	});
	if (operation == QStringLiteral("get"))
		api.getAsset(QStringLiteral("asset"), &owner, [&](auto, auto) { called = true; });
	else if (operation == QStringLiteral("delete"))
		api.deleteAsset(QStringLiteral("asset"), 1, &owner, [&](auto, auto) { called = true; });
	else if (operation == QStringLiteral("rebuild"))
		api.rebuildAsset(QStringLiteral("asset"), 1, &owner, [&](auto, auto) { called = true; });
	else
		api.getAllAssets(&owner, [&](auto, auto) { called = true; });
	api.getAsset(QStringLiteral("asset"), &other, [&](auto, auto value) { otherResult = value; });
	QTRY_VERIFY(otherResult.has_value());
	QVERIFY(*otherResult);
	QVERIFY(cancelled);
	QVERIFY(sent);
	QVERIFY(!called);
	QCOMPARE(server.requests.size(), 2);
}

void FallbackApiTest::callbackCanDeleteClient_data()
{
	QTest::addColumn<bool>("deleteFromProgress");
	QTest::newRow("delete-in-progress") << true;
	QTest::newRow("delete-in-result") << false;
}

void FallbackApiTest::callbackCanDeleteClient()
{
	// deleteFromProgress выбирает момент уничтожения клиента прямо из внешнего callback.
	QFETCH(bool, deleteFromProgress);
	// temp/file содержат оригинал, дескриптор которого должен освободиться при удалении клиента.
	WorkspaceTemp temp(QStringLiteral("fallback-delete-client"));
	QVERIFY(temp.isValid());
	QFile file(temp.filePath(QStringLiteral("source.bin")));
	QVERIFY(file.open(QIODevice::WriteOnly));
	QCOMPARE(file.write(QByteArray(65536, 'x')), qint64(65536));
	file.close();
	// server принимает upload, но отвечает только в сценарии удаления из результата.
	HttpServer server;
	QVERIFY(server.listen());
	server.handler = [&](const auto &request) {
		if (!deleteFromProgress)
			HttpServer::reply(request, R"({"asset_id":"asset","generation_id":"generation"})", 202,
					  "application/json",
					  "Location: /api/v1/fallback-assets/asset/generations/generation\r\n");
	};
	// limits оставляет чужой GET в очереди; api удаляется синхронно из одного из двух обработчиков.
	Limits limits;
	limits.maxActiveRequests = 1;
	auto api = std::make_unique<APIClient>(limits);
	api->configure(server.origin(), {1, 1});
	// guard подтверждает реальное уничтожение QObject независимо от указателя владельца.
	const QPointer<APIClient> guard = api.get();
	// results считает доставленные принятия; queuedCalled обнаруживает callback после удаления клиента.
	int results = 0;
	bool queuedCalled = false;
	api->uploadAsset(
		QStringLiteral("asset"), 1, QStringLiteral("image"), file.fileName(), this,
		[&](auto, auto value) {
			QVERIFY(value);
			++results;
			api.reset();
		},
		[&](auto, qint64 sent, qint64) {
			if (deleteFromProgress && sent > 0)
				api.reset();
		});
	api->getAsset(QStringLiteral("asset"), this, [&](auto, auto) { queuedCalled = true; });
	QTRY_VERIFY(guard.isNull());
	QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
	QCoreApplication::processEvents();
	QCOMPARE(results, deleteFromProgress ? 0 : 1);
	QVERIFY(!queuedCalled);
	QVERIFY(QFile::remove(file.fileName()));
	QVERIFY(server.requests.size() <= 1);
}

void FallbackApiTest::truncatedQueuedUploadFailsWithoutReplay()
{
	// temp/file содержат источник с захваченными шестью байтами; блокирующий upload использует тот же путь раньше.
	WorkspaceTemp temp(QStringLiteral("fallback-truncated"));
	QVERIFY(temp.isValid());
	QFile file(temp.filePath(QStringLiteral("source.bin")));
	QVERIFY(file.open(QIODevice::WriteOnly));
	QCOMPARE(file.write("source"), qint64(6));
	file.close();
	// server считает соединения без накопления тела; первый полный upload удерживает единственный слот.
	UploadServer server;
	QVERIFY(server.listen());
	// limits ограничивает неответивший транспорт, но не повторяет его при раннем EOF.
	Limits limits;
	limits.maxActiveRequests = 1;
	limits.uploadDeadlineMs = 1000;
	limits.uploadIdleMs = 200;
	// api открывает второй источник до усечения file, сохраняя исходный Content-Length.
	APIClient api(limits);
	api.configure(server.origin(), {1, 1});
	// blockerResult подтверждает освобождение слота; result содержит только исход усечённого источника.
	std::optional<Result<Preparation>> blockerResult;
	std::optional<Result<Preparation>> result;
	api.uploadAsset(QStringLiteral("asset"), 1, QStringLiteral("image"), file.fileName(), this,
			[&](auto, auto value) { blockerResult = value; });
	QTRY_VERIFY(server.requests.size() == 1 && server.requests[0]->completed);
	api.uploadAsset(QStringLiteral("asset"), 1, QStringLiteral("image"), file.fileName(), this,
			[&](auto, auto value) { result = value; });
	QVERIFY(file.open(QIODevice::WriteOnly));
	QCOMPARE(file.write("x"), qint64(1));
	file.close();
	UploadServer::reply(server.requests[0], R"({"asset_id":"asset","generation_id":"generation"})", 202,
			    "Location: /api/v1/fallback-assets/asset/generations/generation\r\n");
	QTRY_VERIFY_WITH_TIMEOUT(result.has_value(), 2000);
	QVERIFY(blockerResult.has_value() && *blockerResult);
	QVERIFY(!*result);
	QVERIFY(result->error->requestSent);
	QVERIFY(result->error->kind == ErrorKind::Transport || result->error->kind == ErrorKind::Timeout);
	QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
	QCoreApplication::processEvents();
	QVERIFY(server.requests.size() <= 2);
	if (server.requests.size() == 2) {
		QVERIFY(server.requests[1]->received < 6);
		QVERIFY(!server.requests[1]->completed);
	}
	QVERIFY(QFile::remove(file.fileName()));
}

QTEST_GUILESS_MAIN(FallbackApiTest)
#include "fallback-api-test.moc"
