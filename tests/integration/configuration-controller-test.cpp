#include "fixtures/configuration-rig.hpp"
#include <QApplication>
#include <QSignalSpy>
#include <QTest>

using namespace gost;
using namespace gost::api;

namespace {
// Создаёт независимые документы трёх назначений одного потока и чужого назначения в rig.
QMap<QString, QJsonObject> destinationDocuments(ConfigurationRig &rig)
{
	// documents содержит серверные JSON, config — отдельные клиентские DTO для точного исходного выбора.
	QMap<QString, QJsonObject> documents;
	auto config = rig.state.configuration();
	// id задаёт безопасный непрозрачный адрес, не используемый как отображаемое имя.
	for (const auto &id :
	     {QStringLiteral("a"), QStringLiteral("b"), QStringLiteral("c"), QStringLiteral("foreign")}) {
		// document/decoded создают независимое серверное и клиентское представление одной записи.
		auto document = fixture::destination();
		document.insert(QStringLiteral("id"), id);
		document.insert(QStringLiteral("name"), QStringLiteral("Назначение ") + id);
		if (id == QStringLiteral("foreign"))
			document.insert(QStringLiteral("stream_id"), QStringLiteral("other-stream"));
		const auto decoded = decode<Destination>(fixture::bytes(document));
		if (!decoded)
			return {};
		documents.insert(id, document);
		config.destinations.insert(id, *decoded.value);
	}
	config.destinationsStatus = {RuntimeAvailability::Fresh, true, {}};
	rig.state.setConfiguration(config);
	return documents;
}
// Возвращает адрес назначения для request, исключая коллекцию и маршруты других сущностей.
QString destinationRequestId(const HttpServer::Request &request)
{
	// path содержит только путь синтетического HTTP-запроса, prefix обозначает карточку Destination.
	const auto path = QUrl::fromEncoded(QByteArray("http://local") + request.target).path();
	const auto prefix = QStringLiteral("/api/v1/destinations/");
	return path.startsWith(prefix) ? path.mid(prefix.size()) : QString{};
}
// Обслуживает request независимыми documents, отражая enabled с новой ревизией только при успешном PATCH.
bool serveDestinations(const HttpServer::Request &request, QMap<QString, QJsonObject> &documents)
{
	// id адресует карточку, document содержит её серверную копию до отправки ответа.
	const auto id = destinationRequestId(request);
	if (documents.contains(id)) {
		auto document = documents.value(id);
		if (request.method == "PATCH") {
			// body содержит только намерение, реально поступившее через HTTP, а не RAM-форму.
			const auto body = QJsonDocument::fromJson(request.body).object();
			document.insert(QStringLiteral("enabled"), body.value(QStringLiteral("enabled")));
			document.insert(QStringLiteral("revision"),
					QJsonValue(document.value(QStringLiteral("revision")).toInteger() + 1));
			documents[id] = document;
		}
		HttpServer::reply(request, fixture::bytes(document));
		return true;
	}
	if (request.method == "GET" && (request.target.startsWith("/api/v1/destinations?") ||
					request.target.startsWith("/api/v1/streams/stream/destinations"))) {
		// items содержит доступный серверный список, включая чужую запись для проверки клиентского фильтра.
		QJsonArray items;
		// document перечисляет независимые серверные записи, не ссылки на состояние клиента.
		for (const auto &document : documents)
			items.append(document);
		HttpServer::reply(request, fixture::page(items));
		return true;
	}
	return false;
}
} // namespace

// Проверяет редактор через публичные результаты, реальные HTTP-запросы и отдельные серверные DTO.
class ConfigurationControllerTest final : public QObject {
	Q_OBJECT
private slots:
	// UUID и origin достаточны для создания первого потока без рабочего выбора.
	void firstStreamCanBeCreatedWithoutWorkingSelection();
	// Refresh меняет серверный кеш, сохраняя dirty форму, её базу и рабочий выбор.
	void rereadCannotOverwriteDirtyDraftOrWorkingStream();
	// Два редактора одной ревизии требуют явного сравнения и подтверждения нового PATCH.
	void conflictPreservesBaseDraftAndServerUntilExplicitConfirmation();
	// Предметный 409 не разрешает CAS-сравнение независимо от ревизии последующего чтения.
	void sameRevisionConflictRemainsRejected();
	// Задаёт неоднозначные исходы уже отправленного PATCH и подтверждённый отказ.
	void failedMutationsKeepIntentAndRequireReconciliation_data();
	// Никакой неоднозначный исход не повторяется автоматически; сверка не превращает его в успех.
	void failedMutationsKeepIntentAndRequireReconciliation();
	// Задаёт позднее изменение, удаление и создание после неопределённого результата CRUD.
	void unknownReconciliationRejectsLateDockCatalog_data();
	// Настоящая сверка отзывает старый каталог; новое чтение не ждёт медленный fallback.
	void unknownReconciliationRejectsLateDockCatalog();
	// Задаёт потерю ответа и непроверенный успешный ответ создания без известного ID новой записи.
	void unknownCreateRequiresCompleteCollectionReconciliation_data();
	// Полный список разрешает лишь новое явное намерение; неполный список и сверка не доказывают прежний успех.
	void unknownCreateRequiresCompleteCollectionReconciliation();
	// Новый контекст не принимает сохранение и поздний ответ старого draft.
	void changingContextRevokesOldDraftAndInFlightWrite();
	// GET, отправленный до записи, не заменяет более новый сохранённый DTO.
	void staleCollectionReadCannotUndoSuccessfulWrite();
	// Параллельные чтения разных коллекций обновляют только собственные части aggregate.
	void independentCollectionsKeepEachOthersResults();
	// Серверные пути и причины передаются уведомлению операции без повторения в форме.
	void fieldValidationMapsKnownBodyLocations();
	// Локальный CBR-отказ сохраняет точную причину, вложенный путь и исходный максимум до явного исправления.
	void cbrBitrateMismatchPreservesReasonAndDraftBeforeHttp();
	// Сбои наблюдателя и завершение command-канала не отменяют принятый CRUD.
	void configurationWriteSurvivesOtherChannelCancellation();
	// Полная замена после конфликта не удаляет свежие explicit audio/encoding server-поля.
	void conflictCannotEraseExplicitProfileContract();
	// Создание назначения не включает его в следующий эфир без выбора в карточке потока.
	void newDestinationStartsDisabled();
	// Пакет пытается сохранить все назначения и сохраняет результаты отказавших элементов.
	void streamSelectionContinuesAfterPartialFailure();
	// Потеря ответа и 409 требуют адресной сверки перед новым явным сохранением.
	void streamSelectionReconcilesUncertainAndConflictingItems_data();
	// Сверка уже применённой записи исключает повторный PATCH, отказ не уничтожает намерение.
	void streamSelectionReconcilesUncertainAndConflictingItems();
	// Неполный список и чужой поток не разрешают изменение участия назначения.
	void streamSelectionRequiresCompleteOwnDestinationList();
	// Смена контекста отменяет оставшиеся шаги пакета без переноса намерения в новый профиль.
	void streamSelectionStopsAtContextChange();
	// Задаёт окончательное удаление, перенос в чужой поток и временный отказ сверки.
	void streamSelectionReconciliationDistinguishesUnavailableFromTransientFailure_data();
	// Окончательно недоступное назначение не блокирует оставшиеся изменения, временный отказ сохраняет Unknown.
	void streamSelectionReconciliationDistinguishesUnavailableFromTransientFailure();
	// Задаёт окончательный PATCH404 и полное чтение, подтвердившее удаление либо перенос до отправки шага.
	void streamSelectionUnavailableItemsDoNotBlockOtherChanges_data();
	// Недоступный шаг получает окончательный отказ без повтора и освобождает сохранение остальных полей.
	void streamSelectionUnavailableItemsDoNotBlockOtherChanges();
};

void ConfigurationControllerTest::firstStreamCanBeCreatedWithoutWorkingSelection()
{
	// rig моделирует подключённый профиль без единого выбранного Stream.
	ConfigurationRig rig;
	QVERIFY(rig.initialize(false));
	QVERIFY(rig.configuration.beginCreate(ConfigurationKind::Stream));
	// draft получает обязательное имя и явно введённый ключ без изменения рабочего выбора.
	auto draft = std::get<Stream>(*rig.state.configurationEditor().draft);
	draft.name = QStringLiteral("Первый поток");
	draft.publicationKey = {FieldState::Value, QStringLiteral("explicit-publication-key")};
	QVERIFY(rig.configuration.setDraft(draft, {QStringLiteral("name"), QStringLiteral("publication_key")}));
	QVERIFY(rig.configuration.save());
	QTRY_COMPARE_WITH_TIMEOUT(rig.state.configurationEditor().outcome, ConfigurationOutcome::Saved, 2000);
	QCOMPARE(rig.mutations().size(), 1);
	QCOMPARE(rig.mutations().first().method, QByteArray("POST"));
	QCOMPARE(rig.state.configuration().streams.value(QStringLiteral("stream")).name,
		 QStringLiteral("Первый поток"));
	QVERIFY(rig.state.selectedStreamId().isEmpty());
}
void ConfigurationControllerTest::rereadCannotOverwriteDirtyDraftOrWorkingStream()
{
	// rig открывает карточку потока, отличного от рабочего.
	ConfigurationRig rig;
	QVERIFY(rig.initialize());
	rig.stream.insert(QStringLiteral("id"), QStringLiteral("other"));
	QVERIFY(rig.configuration.open(ConfigurationKind::Stream, QStringLiteral("other")));
	QTRY_VERIFY_WITH_TIMEOUT(rig.state.configurationEditor().base.has_value(), 2000);
	// draft содержит несохранённое имя и заведомо неверную локальную revision, которой нельзя доверять.
	auto draft = std::get<Stream>(*rig.state.configurationEditor().draft);
	draft.name = QStringLiteral("Мой ввод");
	draft.revision = 999;
	QVERIFY(rig.configuration.setDraft(draft, {QStringLiteral("name")}));
	QVERIFY(!rig.configuration.open(ConfigurationKind::Group, QStringLiteral("group")));
	rig.stream.insert(QStringLiteral("revision"), QJsonValue(qint64(2)));
	rig.stream.insert(QStringLiteral("name"), QStringLiteral("Серверное имя"));
	QVERIFY(rig.configuration.reload());
	QTRY_VERIFY_WITH_TIMEOUT(!rig.state.configurationEditor().loading, 2000);
	QCOMPARE(std::get<Stream>(*rig.state.configurationEditor().draft).name, QStringLiteral("Мой ввод"));
	QCOMPARE(std::get<Stream>(*rig.state.configurationEditor().base).revision, quint64(1));
	QCOMPARE(rig.state.configuration().streams.value(QStringLiteral("other")).name,
		 QStringLiteral("Серверное имя"));
	QCOMPARE(rig.state.selectedStreamId(), QStringLiteral("stream"));
	QVERIFY(rig.mutations().isEmpty());
}
void ConfigurationControllerTest::conflictPreservesBaseDraftAndServerUntilExplicitConfirmation()
{
	// rig/patches моделируют второй редактор: первый PATCH устарел, последующий разрешён только явно.
	ConfigurationRig rig;
	QVERIFY(rig.initialize());
	int patches = 0;
	rig.intercept = [&](const HttpServer::Request &request) {
		if (request.method != "PATCH")
			return false;
		++patches;
		if (patches != 1)
			return false;
		rig.destination.insert(QStringLiteral("revision"), QJsonValue(qint64(2)));
		rig.destination.insert(QStringLiteral("name"), QStringLiteral("Чужая запись"));
		HttpServer::reply(request, R"({"status":409,"code":"revision_conflict","title":"Conflict"})", 409,
				  "application/problem+json");
		return true;
	};
	QVERIFY(rig.configuration.open(ConfigurationKind::Destination, QStringLiteral("destination")));
	QTRY_VERIFY_WITH_TIMEOUT(rig.state.configurationEditor().base.has_value(), 2000);
	// draft хранит мой ввод; intent задаёт ровно изменённое имя без секретных полей.
	auto draft = std::get<Destination>(*rig.state.configurationEditor().draft);
	draft.name = QStringLiteral("Моя запись");
	draft.revision = 999;
	QVERIFY(rig.configuration.setDraft(draft, {QStringLiteral("name")}));
	QVERIFY(rig.configuration.save());
	QTRY_COMPARE_WITH_TIMEOUT(rig.state.configurationEditor().outcome, ConfigurationOutcome::Conflict, 2000);
	QCOMPARE(std::get<Destination>(*rig.state.configurationEditor().base).revision, quint64(1));
	QCOMPARE(std::get<Destination>(*rig.state.configurationEditor().draft).name, QStringLiteral("Моя запись"));
	QCOMPARE(std::get<Destination>(*rig.state.configurationEditor().serverConflict).name,
		 QStringLiteral("Чужая запись"));
	QCOMPARE(patches, 1);
	QVERIFY(!rig.configuration.save());
	QCOMPARE(QJsonDocument::fromJson(rig.mutations().first().body)
			 .object()
			 .value(QStringLiteral("expected_revision"))
			 .toInteger(),
		 qint64(1));
	QVERIFY(rig.configuration.confirmConflict(draft, {QStringLiteral("name")}));
	QCOMPARE(patches, 1);
	QVERIFY(rig.configuration.save());
	QTRY_COMPARE_WITH_TIMEOUT(rig.state.configurationEditor().outcome, ConfigurationOutcome::Saved, 2000);
	QCOMPARE(patches, 2);
	QCOMPARE(QJsonDocument::fromJson(rig.mutations().last().body).object(),
		 QJsonObject({{QStringLiteral("name"), QStringLiteral("Моя запись")},
			      {QStringLiteral("expected_revision"), QJsonValue(qint64(2))}}));
}
void ConfigurationControllerTest::sameRevisionConflictRemainsRejected()
{
	// rig публикует более новую revision после предметного409; это не разрешает CAS-сравнение.
	ConfigurationRig rig;
	QVERIFY(rig.initialize());
	rig.intercept = [&rig](const HttpServer::Request &request) {
		if (request.method != "DELETE")
			return false;
		rig.group.insert(QStringLiteral("revision"), QJsonValue(qint64(2)));
		HttpServer::reply(request, R"({"status":409,"code":"resource_in_use","title":"In use"})", 409,
				  "application/problem+json");
		return true;
	};
	QVERIFY(rig.configuration.open(ConfigurationKind::Group, QStringLiteral("group")));
	QTRY_VERIFY_WITH_TIMEOUT(rig.state.configurationEditor().base.has_value(), 2000);
	QVERIFY(rig.configuration.remove());
	QTRY_VERIFY_WITH_TIMEOUT(rig.mutations().size() == 1 && !rig.state.configurationEditor().loading &&
					 !rig.state.configurationEditor().writing,
				 2000);
	QCOMPARE(rig.state.configurationEditor().outcome, ConfigurationOutcome::Rejected);
	QVERIFY(!rig.state.configurationEditor().serverConflict.has_value());
	QVERIFY(!rig.configuration.confirmConflict(*rig.state.configurationEditor().draft, {}));
}
void ConfigurationControllerTest::failedMutationsKeepIntentAndRequireReconciliation_data()
{
	QTest::addColumn<int>("status");
	QTest::addColumn<bool>("unknown");
	QTest::newRow("lost-response") << 0 << true;
	QTest::newRow("timeout") << -1 << true;
	QTest::newRow("server-error") << 503 << false;
	QTest::newRow("invalid-success") << 200 << true;
	QTest::newRow("validation-rejected") << 422 << false;
}
void ConfigurationControllerTest::failedMutationsKeepIntentAndRequireReconciliation()
{
	// status задаёт отказ транспорта/HTTP; unknown указывает независимый ожидаемый результат намерения.
	QFETCH(int, status);
	QFETCH(bool, unknown);
	// rig удерживает или обрывает только mutation; дальнейший GET возвращает наблюдаемое текущее состояние.
	ConfigurationRig rig;
	QVERIFY(rig.initialize());
	rig.intercept = [status](const HttpServer::Request &request) {
		if (request.method != "PATCH")
			return false;
		if (status == 0 && request.socket)
			request.socket->abort();
		if (status > 0)
			HttpServer::reply(request, status == 200 ? QByteArray("{broken") : QByteArray("{}"), status);
		return true;
	};
	QVERIFY(rig.configuration.open(ConfigurationKind::Destination, QStringLiteral("destination")));
	QTRY_VERIFY_WITH_TIMEOUT(rig.state.configurationEditor().base.has_value(), 2000);
	// draft сохраняет пользовательское имя после каждого отказа.
	auto draft = std::get<Destination>(*rig.state.configurationEditor().draft);
	draft.name = QStringLiteral("Сохранить мой ввод");
	QVERIFY(rig.configuration.setDraft(draft, {QStringLiteral("name")}));
	QVERIFY(rig.configuration.save());
	QTRY_VERIFY_WITH_TIMEOUT(!rig.state.configurationEditor().writing, 2000);
	QCOMPARE(rig.state.configurationEditor().outcome,
		 unknown ? ConfigurationOutcome::Unknown : ConfigurationOutcome::Rejected);
	QCOMPARE(rig.state.configurationEditor().reconciliationRequired, unknown);
	QCOMPARE(std::get<Destination>(*rig.state.configurationEditor().draft).name, draft.name);
	QCOMPARE(rig.mutations().size(), 1);
	if (unknown) {
		QVERIFY(!rig.configuration.save());
		QVERIFY(rig.configuration.reload());
		QTRY_VERIFY_WITH_TIMEOUT(!rig.state.configurationEditor().loading, 2000);
		QVERIFY(!rig.state.configurationEditor().reconciliationRequired);
		QCOMPARE(rig.state.configurationEditor().outcome, ConfigurationOutcome::Unknown);
		QCOMPARE(rig.mutations().size(), 1);
	}
}
// Различает адресные GET 200/404 и полный список после создания с неизвестным новым ID.
void ConfigurationControllerTest::unknownReconciliationRejectsLateDockCatalog_data()
{
	QTest::addColumn<bool>("removed");
	QTest::addColumn<bool>("creating");
	QTest::newRow("patch-observed-after-unknown") << false << false;
	QTest::newRow("delete-confirmed-by-404") << true << false;
	QTest::newRow("create-confirmed-by-complete-collection") << false << true;
}

// GET каталога, начатый после Unknown, но до завершения сверки, не подтверждает прежний состав.
// Синхронное наблюдение обнаруживает даже краткую публикацию старой версии перед новым GET.
void ConfigurationControllerTest::unknownReconciliationRejectsLateDockCatalog()
{
	// removed выбирает подтверждённое удаление, creating — создание с неизвестным клиенту новым ID.
	QFETCH(bool, removed);
	QFETCH(bool, creating);
	// rig предоставляет реальные независимые контроллеры и локальный HTTP без внешних систем.
	ConfigurationRig rig;
	QVERIFY(rig.initialize());
	// catalogReads и detailReads фиксируют реальное получение GET, а lateCatalog удерживает старый каталог.
	int catalogReads = 0;
	int detailReads = 0;
	// collectionReads отличает полную сверку неизвестного создания от адресных запросов.
	int collectionReads = 0;
	HttpServer::Request lateCatalog;
	// lateCatalogBody сохраняет серверный снимок до задержанной записи, slowFallback оставляет Dock batch открытым.
	QByteArray lateCatalogBody;
	HttpServer::Request slowFallback;
	// holdCatalog включает удержание после исходного чтения; committed моделирует позднее применение CRUD на сервере.
	bool holdCatalog = false;
	bool committed = false;
	// createdDestination существует на сервере лишь после задержанного POST и получает новый непрозрачный ID.
	auto createdDestination = fixture::destination();
	createdDestination.insert(QStringLiteral("id"), QStringLiteral("new-destination"));
	createdDestination.insert(QStringLiteral("name"), QStringLiteral("После сверки"));
	createdDestination.insert(QStringLiteral("enabled"), false);
	// Перехватчик request обслуживает handshake, потерянный результат CRUD и независимые границы двух GET.
	rig.intercept = [&](const HttpServer::Request &request) {
		if (request.method == "PATCH" || request.method == "DELETE" || request.method == "POST") {
			// Разрыв после получения полного намерения оставляет применение записи неопределённым.
			if (request.socket)
				request.socket->abort();
			return true;
		}
		if (request.target.startsWith("/api/v1/health/ready"))
			HttpServer::reply(request, R"({"status":"ready","reason":""})");
		else if (request.target.startsWith("/api/v1/system/capabilities"))
			HttpServer::reply(request, fixture::capabilities());
		else if (request.target.startsWith("/api/v1/events/snapshot"))
			HttpServer::reply(request, R"({"cursor":"catalog-anchor","sessions":[],"gpu":null})");
		else if (request.target.startsWith("/api/v1/events?"))
			HttpServer::stream(request, ": heartbeat\n\n");
		else if (request.target.startsWith("/api/v1/streams/stream/destinations")) {
			++catalogReads;
			if (holdCatalog && !lateCatalog.socket) {
				lateCatalog = request;
				lateCatalogBody = fixture::page({rig.destination});
			} else {
				HttpServer::reply(
					request, fixture::page(committed && removed ? QJsonArray{}
							       : committed && creating
								       ? QJsonArray{rig.destination, createdDestination}
								       : QJsonArray{rig.destination}));
			}
		} else if (request.target.startsWith("/api/v1/destinations?")) {
			++collectionReads;
			HttpServer::reply(request, fixture::page(committed && creating ? QJsonArray{rig.destination,
												    createdDestination}
										       : QJsonArray{rig.destination}));
		} else if (request.target == "/api/v1/destinations/destination") {
			++detailReads;
			if (committed && removed)
				HttpServer::reply(request, "{}", 404);
			else
				HttpServer::reply(request, fixture::bytes(rig.destination));
		} else if (request.target.startsWith("/api/v1/streams/stream/fallback-policy"))
			slowFallback = request;
		else
			return false;
		return true;
	};
	// descriptor подтверждает отдельный профиль OBS, затем settings сохраняет Stream для его настоящего UUID.
	const ProfileDescriptor descriptor{QStringLiteral("Catalog"),
					   QStringLiteral("profiles/Catalog"),
					   {QStringLiteral("Catalog")},
					   true};
	rig.connection.observeProfile(FrontendEvent::FinishedLoading, descriptor);
	QVERIFY(rig.settings.setSelectedStream(rig.server.origin().toString(), rig.settings.profileUuid(),
					       QStringLiteral("stream")));
	rig.connection.connectToServer(rig.server.origin().toString());
	QTRY_VERIFY_WITH_TIMEOUT(rig.state.connectionStatus() == ConnectionStatus::Connected &&
					 rig.state.dockServer().destinationsStatus.complete &&
					 rig.state.dockServer().sessionStatus.complete && slowFallback.socket,
				 3000);
	QCOMPARE(catalogReads, 1);
	QCOMPARE(rig.state.dockServer().destinations.value(QStringLiteral("destination")).name,
		 QStringLiteral("Назначение"));
	if (creating) {
		QVERIFY(rig.configuration.beginCreate(ConfigurationKind::Destination));
	} else {
		QVERIFY(rig.configuration.open(ConfigurationKind::Destination, QStringLiteral("destination")));
		QTRY_VERIFY_WITH_TIMEOUT(rig.state.configurationEditor().base.has_value(), 2000);
	}
	QCOMPARE(detailReads, creating ? 0 : 1);
	// draft хранит явное новое имя; для создания пользователь заполняет также обязательные поля назначения.
	auto draft = std::get<Destination>(*rig.state.configurationEditor().draft);
	draft.name = QStringLiteral("После сверки");
	if (creating) {
		draft.groupId = QStringLiteral("group");
		draft.platform = QStringLiteral("custom");
		draft.endpoint = {FieldState::Value, fixture::endpoint()};
		draft.streamKey = {FieldState::Value, QStringLiteral("explicit-test-key")};
	}
	holdCatalog = true;
	if (removed) {
		QVERIFY(rig.configuration.remove());
	} else {
		QVERIFY(rig.configuration.setDraft(draft, {QStringLiteral("name")}));
		QVERIFY(rig.configuration.save());
	}
	QTRY_COMPARE_WITH_TIMEOUT(rig.state.configurationEditor().outcome, ConfigurationOutcome::Unknown, 2000);
	QVERIFY(rig.state.configurationEditor().reconciliationRequired);
	QTRY_VERIFY_WITH_TIMEOUT(lateCatalog.socket, 2000);
	QCOMPARE(catalogReads, 2);
	QVERIFY(!rig.configuration.save());
	committed = true;
	if (!removed && !creating) {
		rig.destination.insert(QStringLiteral("revision"), QJsonValue(qint64(2)));
		rig.destination.insert(QStringLiteral("name"), draft.name);
	}
	// publishedOld отмечает запрещённое подтверждение прежнего состава после начала сверки.
	bool publishedOld = false;
	// catalogObserver отключает callback до уничтожения захваченных локальных значений и зависимостей rig.
	QObject catalogObserver;
	// Обработчик проверяет содержимое каждой свежей публикации, включая промежуточные результаты старого GET.
	connect(&rig.state, &StateStore::serverStateChanged, &catalogObserver, [&] {
		publishedOld |=
			rig.state.dockServer().destinationsStatus.availability == RuntimeAvailability::Fresh &&
			(creating ? !rig.state.dockServer().destinations.contains(QStringLiteral("new-destination"))
				  : rig.state.dockServer().destinations.value(QStringLiteral("destination")).name ==
					    QStringLiteral("Назначение"));
	});
	QVERIFY(rig.configuration.reload());
	QTRY_VERIFY_WITH_TIMEOUT(!rig.state.configurationEditor().loading &&
					 !rig.state.configurationEditor().reconciliationRequired,
				 2000);
	QCOMPARE(detailReads, creating ? 0 : 2);
	QCOMPARE(collectionReads, creating ? 1 : 0);
	if (creating) {
		QVERIFY(rig.state.configurationEditor().id.isEmpty());
		QVERIFY(rig.state.configuration().destinationsStatus.complete);
		QCOMPARE(rig.state.configuration().destinations.value(QStringLiteral("new-destination")).name,
			 draft.name);
	}
	QCOMPARE(rig.state.configurationEditor().outcome, ConfigurationOutcome::Unknown);
	QVERIFY(rig.state.dockServer().destinationsStatus.availability != RuntimeAvailability::Fresh);
	holdCatalog = false;
	HttpServer::reply(lateCatalog, lateCatalogBody);
	QTRY_VERIFY_WITH_TIMEOUT(catalogReads >= 3 &&
					 rig.state.dockServer().destinationsStatus.availability ==
						 RuntimeAvailability::Fresh &&
					 rig.state.dockServer().destinationsStatus.complete,
				 3000);
	QVERIFY(!publishedOld);
	QCOMPARE(catalogReads, 3);
	QCOMPARE(rig.state.dockServer().destinations.size(), removed ? 0 : creating ? 2 : 1);
	if (!removed)
		QCOMPARE(rig.state.dockServer()
				 .destinations
				 .value(creating ? QStringLiteral("new-destination") : QStringLiteral("destination"))
				 .name,
			 draft.name);
	if (creating)
		QCOMPARE(rig.state.dockServer().destinations.value(QStringLiteral("destination")).name,
			 QStringLiteral("Назначение"));
	QVERIFY(slowFallback.socket);
	QVERIFY(rig.state.dockServer().fallback.availability != RuntimeAvailability::Fresh);
	QCOMPARE(rig.mutations().size(), 1);
	QCOMPARE(rig.mutations().front().method, removed    ? QByteArray("DELETE")
						 : creating ? QByteArray("POST")
							    : QByteArray("PATCH"));
	QCOMPARE(rig.mutations().front().target,
		 removed    ? QByteArray("/api/v1/destinations/destination?expected_revision=1")
		 : creating ? QByteArray("/api/v1/destinations")
			    : QByteArray("/api/v1/destinations/destination"));
}

void ConfigurationControllerTest::unknownCreateRequiresCompleteCollectionReconciliation_data()
{
	QTest::addColumn<bool>("responseLost");
	QTest::newRow("lost-response") << true;
	QTest::newRow("malformed-created-entity") << false;
}
void ConfigurationControllerTest::unknownCreateRequiresCompleteCollectionReconciliation()
{
	// responseLost различает разрыв соединения после POST и некорректный ответ создания 201.
	QFETCH(bool, responseLost);
	// rig предоставляет независимые сервисы, а limitedApi ограничивает обход одним листом для проверки неполноты.
	ConfigurationRig rig;
	QVERIFY(rig.initialize(false));
	Limits limits;
	limits.maxPages = 1;
	APIClient limitedApi(limits);
	ConfigurationController controller(rig.state, limitedApi, rig.connection, rig.operations);
	// partial задаёт наличие ещё одной страницы; listReads доказывает завершение каждого запрошенного перечитывания.
	bool partial = true;
	int listReads = 0;
	rig.intercept = [&](const HttpServer::Request &request) {
		if (request.method == "POST" && rig.mutations().size() == 1) {
			if (responseLost) {
				if (request.socket)
					request.socket->abort();
			} else
				HttpServer::reply(request, "{broken", 201);
			return true;
		}
		if (request.method == "GET" && request.target.startsWith("/api/v1/streams?")) {
			++listReads;
			HttpServer::reply(request, fixture::page({fixture::stream()},
								 partial ? QStringLiteral("next-page") : QString{}));
			return true;
		}
		return false;
	};
	QVERIFY(controller.beginCreate(ConfigurationKind::Stream));
	// draft содержит явное исходное создание, которое нельзя автоматически повторять после потери результата.
	auto draft = std::get<Stream>(*rig.state.configurationEditor().draft);
	draft.name = QStringLiteral("Первое намерение");
	draft.publicationKey = {FieldState::Value, QStringLiteral("explicit-key")};
	QVERIFY(controller.setDraft(draft, {QStringLiteral("name"), QStringLiteral("publication_key")}));
	QVERIFY(controller.save());
	QTRY_COMPARE_WITH_TIMEOUT(rig.state.configurationEditor().outcome, ConfigurationOutcome::Unknown, 1000);
	QVERIFY(rig.state.configurationEditor().reconciliationRequired);
	QVERIFY(rig.state.configurationEditor().id.isEmpty());
	QVERIFY(!controller.save());
	QCOMPARE(rig.mutations().size(), 1);
	QVERIFY(controller.reload());
	QTRY_VERIFY_WITH_TIMEOUT(listReads == 1 && rig.state.configuration().streamsStatus.availability ==
							   RuntimeAvailability::Fresh,
				 1000);
	QVERIFY(!rig.state.configuration().streamsStatus.complete);
	QVERIFY(rig.state.configurationEditor().reconciliationRequired);
	QVERIFY(!controller.save());
	partial = false;
	QVERIFY(controller.reload());
	QTRY_VERIFY_WITH_TIMEOUT(listReads == 2 && !rig.state.configurationEditor().reconciliationRequired, 1000);
	QVERIFY(rig.state.configuration().streamsStatus.complete);
	QCOMPARE(rig.state.configurationEditor().outcome, ConfigurationOutcome::Unknown);
	QCOMPARE(std::get<Stream>(*rig.state.configurationEditor().draft).name, QStringLiteral("Первое намерение"));
	QCOMPARE(rig.mutations().size(), 1);
	draft.name = QStringLiteral("Новое явное намерение");
	QVERIFY(controller.setDraft(draft, {QStringLiteral("name"), QStringLiteral("publication_key")}));
	QVERIFY(controller.save());
	QTRY_COMPARE_WITH_TIMEOUT(rig.state.configurationEditor().outcome, ConfigurationOutcome::Saved, 1000);
	QCOMPARE(rig.mutations().size(), 2);
	QCOMPARE(QJsonDocument::fromJson(rig.mutations().last().body).object().value(QStringLiteral("name")).toString(),
		 QStringLiteral("Новое явное намерение"));
}

void ConfigurationControllerTest::changingContextRevokesOldDraftAndInFlightWrite()
{
	// rig удерживает PATCH старого origin до смены подтверждённого UUID профиля.
	ConfigurationRig rig;
	QVERIFY(rig.initialize());
	// held сохраняет уже отправленный PATCH прежнего UUID до его позднего ответа.
	std::optional<HttpServer::Request> held;
	rig.intercept = [&](const HttpServer::Request &request) {
		if (request.method != "PATCH")
			return false;
		held = request;
		return true;
	};
	QVERIFY(rig.configuration.open(ConfigurationKind::Stream, QStringLiteral("stream")));
	QTRY_VERIFY_WITH_TIMEOUT(rig.state.configurationEditor().base.has_value(), 2000);
	// draft сохраняет ввод первого контекста, который нельзя перенести в новый UUID.
	auto draft = std::get<Stream>(*rig.state.configurationEditor().draft);
	draft.name = QStringLiteral("Старый контекст");
	QVERIFY(rig.configuration.setDraft(draft, {QStringLiteral("name")}));
	QVERIFY(rig.configuration.save());
	QTRY_VERIFY_WITH_TIMEOUT(held.has_value(), 1000);
	rig.state.setSelection(QStringLiteral("stream"), true, {}, {}, QStringLiteral("other-profile"));
	QVERIFY(!rig.configuration.save());
	QVERIFY(!rig.state.configurationEditor().writing);
	QCOMPARE(rig.state.configurationEditor().outcome, ConfigurationOutcome::Unknown);
	HttpServer::reply(*held, fixture::bytes(rig.stream));
	QCOMPARE(rig.state.configurationEditor().context.profileUuid, QStringLiteral("profile-uuid"));
	QCOMPARE(rig.mutations().size(), 1);
}
void ConfigurationControllerTest::staleCollectionReadCannotUndoSuccessfulWrite()
{
	// rig удерживает старый GET списка, позволяя более новому PATCH завершиться первым.
	ConfigurationRig rig;
	QVERIFY(rig.initialize());
	rig.configApi.setHttpTimeoutMs(2000);
	// held сохраняет прежний список, который должен завершиться после успешной новой записи.
	std::optional<HttpServer::Request> held;
	rig.intercept = [&](const HttpServer::Request &request) {
		if (request.method == "GET" && request.target.startsWith("/api/v1/streams?") && !held) {
			held = request;
			return true;
		}
		return false;
	};
	rig.configuration.refresh(ConfigurationKind::Stream);
	QTRY_VERIFY_WITH_TIMEOUT(held.has_value(), 1000);
	QVERIFY(rig.configuration.open(ConfigurationKind::Stream, QStringLiteral("stream")));
	// Адресный GET и старый список используют независимые активные запросы этой транспортной фикстуры.
	QTRY_VERIFY_WITH_TIMEOUT(rig.state.configurationEditor().base.has_value(), 1000);
	// draft/newName задают подтверждённое более новое имя после записи.
	auto draft = std::get<Stream>(*rig.state.configurationEditor().draft);
	draft.name = QStringLiteral("Новое подтверждённое имя");
	QVERIFY(rig.configuration.setDraft(draft, {QStringLiteral("name")}));
	QVERIFY(rig.configuration.save());
	QTRY_COMPARE_WITH_TIMEOUT(rig.state.configurationEditor().outcome, ConfigurationOutcome::Saved, 1000);
	HttpServer::reply(*held, fixture::page({fixture::stream()}));
	// barrier — независимый живой GET: его завершение означает обработку предшествующего сетевого ответа.
	bool barrier = false;
	rig.configApi.getLive(this, [&](RequestContext, Result<Health>) { barrier = true; });
	QTRY_VERIFY_WITH_TIMEOUT(barrier, 1000);
	QCOMPARE(rig.state.configuration().streams.value(QStringLiteral("stream")).name, draft.name);
}
void ConfigurationControllerTest::independentCollectionsKeepEachOthersResults()
{
	// rig/held позволяют поменять порядок завершения двух разных коллекций.
	ConfigurationRig rig;
	QVERIFY(rig.initialize());
	rig.configApi.setHttpTimeoutMs(2000);
	// held удерживает одну коллекцию, чтобы другая успела обновить свежий общий aggregate.
	std::optional<HttpServer::Request> held;
	rig.intercept = [&](const HttpServer::Request &request) {
		if (!request.target.startsWith("/api/v1/transcode-groups?"))
			return false;
		held = request;
		return true;
	};
	rig.configuration.refresh(ConfigurationKind::Group);
	rig.configuration.refresh(ConfigurationKind::Profile);
	QTRY_VERIFY_WITH_TIMEOUT(
		held.has_value() && rig.state.configuration().profiles.contains(QStringLiteral("profile")), 1000);
	HttpServer::reply(*held, fixture::page({rig.group}));
	QTRY_VERIFY_WITH_TIMEOUT(rig.state.configuration().groups.contains(QStringLiteral("group")), 1000);
	QVERIFY(rig.state.configuration().profiles.contains(QStringLiteral("profile")));
}
// Проверяет раздельное хранение серверной причины и недоверенного value без подмены причины ключом перевода.
void ConfigurationControllerTest::fieldValidationMapsKnownBodyLocations()
{
	// rig возвращает стандартный Huma location body.name и потенциально секретное value.
	ConfigurationRig rig;
	QVERIFY(rig.initialize());
	// failures получает серверную причину только для popup, без копирования в локальную валидацию формы.
	QSignalSpy failures(&rig.configuration, &ConfigurationController::operationFailed);
	QVERIFY(failures.isValid());
	// Перехватчик request отклоняет PATCH, сохраняя чтение карточки в обычной петлевой фикстуре.
	rig.intercept = [](const HttpServer::Request &request) {
		if (request.method != "PATCH")
			return false;
		HttpServer::reply(
			request,
			R"({"status":422,"title":"Validation","errors":[{"location":"body.name","message":"Required",)"
			R"("value":"synthetic-secret","messageKey":"Configuration.Saved"}]})",
			422, "application/problem+json");
		return true;
	};
	QVERIFY(rig.configuration.open(ConfigurationKind::Stream, QStringLiteral("stream")));
	QTRY_VERIFY_WITH_TIMEOUT(rig.state.configurationEditor().base.has_value(), 1000);
	// draft содержит допустимый локально ввод, отклонённый серверной проверкой.
	auto draft = std::get<Stream>(*rig.state.configurationEditor().draft);
	draft.name = QStringLiteral("Имя");
	QVERIFY(rig.configuration.setDraft(draft, {QStringLiteral("name")}));
	QVERIFY(rig.configuration.save());
	QTRY_VERIFY_WITH_TIMEOUT(!rig.state.configurationEditor().writing, 1000);
	QCOMPARE(rig.state.configurationEditor().outcome, ConfigurationOutcome::Rejected);
	QVERIFY(rig.state.configurationEditor().fieldIssues.isEmpty());
	QCOMPARE(failures.size(), 1);
	// notification сохраняет полный исходный путь и обычную причину в событии владельца операции.
	const auto notification = qvariant_cast<OperationFailure>(failures.at(0).at(0));
	QVERIFY(notification.reason.contains(QStringLiteral("body.name")));
	QVERIFY(notification.reason.contains(QStringLiteral("Required")));
	QVERIFY(notification.diagnosticBody.contains("synthetic-secret"));
}

// Локальная CBR ошибка сохраняет точный путь и значения до явного исправления.
void ConfigurationControllerTest::cbrBitrateMismatchPreservesReasonAndDraftBeforeHttp()
{
	// rig предоставляет независимый серверный профиль с согласованными CBR-битрейтами 12 Мбит/с.
	ConfigurationRig rig;
	QVERIFY(rig.initialize());
	// profile и encoding задают полный современный рецепт, video содержит серверные битрейты до редактирования.
	auto profile = fixture::groupProfile();
	auto encoding = fixture::encoding();
	encoding.insert(QStringLiteral("rate_control"), QStringLiteral("cbr"));
	profile.insert(QStringLiteral("encoding"), encoding);
	profile.insert(QStringLiteral("live_audio_mode"), QStringLiteral("copy"));
	auto video = profile.value(QStringLiteral("video_encoding")).toObject();
	video.insert(QStringLiteral("target_bitrate_bps"), QJsonValue(qint64(12000000)));
	video.insert(QStringLiteral("max_bitrate_bps"), QJsonValue(qint64(12000000)));
	profile.insert(QStringLiteral("video_encoding"), video);
	rig.profile.insert(QStringLiteral("profile"), profile);
	QVERIFY(rig.configuration.open(ConfigurationKind::Profile, QStringLiteral("profile")));
	QTRY_VERIFY_WITH_TIMEOUT(rig.state.configurationEditor().base.has_value(), 1000);
	// draft отличается только основным битрейтом: скрытый максимум остаётся пользовательским значением.
	auto draft = std::get<Profile>(*rig.state.configurationEditor().draft);
	draft.profile.videoEncoding.targetBitrateBps = 10000000;
	QVERIFY(rig.configuration.setDraft(draft, {QStringLiteral("profile")}));
	QVERIFY(!rig.configuration.save());
	QCOMPARE(rig.state.configurationEditor().outcome, ConfigurationOutcome::Rejected);
	QVERIFY(!rig.state.configurationEditor().reconciliationRequired);
	QVERIFY(!rig.state.configurationEditor().writing);
	QCOMPARE(rig.state.configurationEditor().fieldIssues.size(), 1);
	QVERIFY(rig.state.configurationEditor().fieldIssues.contains(
		QStringLiteral("profile.video_encoding.max_bitrate_bps")));
	// issue фиксирует точную локальную причину вместо общего сообщения о profile.
	const auto issue = rig.state.configurationEditor().fieldIssues.value(
		QStringLiteral("profile.video_encoding.max_bitrate_bps"));
	QCOMPARE(issue.messageKey, QStringLiteral("Validation.CbrBitrateMismatch"));
	QVERIFY(issue.detail.isEmpty());
	QCOMPARE(std::get<Profile>(*rig.state.configurationEditor().draft).profile.videoEncoding.targetBitrateBps,
		 quint64(10000000));
	QCOMPARE(std::get<Profile>(*rig.state.configurationEditor().draft).profile.videoEncoding.maxBitrateBps,
		 quint64(12000000));
	QCOMPARE(std::get<Profile>(*rig.state.configurationEditor().base).profile.videoEncoding.targetBitrateBps,
		 quint64(12000000));
	QVERIFY(rig.mutations().isEmpty());
	draft.profile.videoEncoding.targetBitrateBps = 12000000;
	QVERIFY(rig.configuration.setDraft(draft, {QStringLiteral("profile")}));
	QVERIFY(rig.state.configurationEditor().fieldIssues.isEmpty());
	QVERIFY(rig.configuration.save());
	QTRY_COMPARE_WITH_TIMEOUT(rig.state.configurationEditor().outcome, ConfigurationOutcome::Saved, 1000);
	QCOMPARE(rig.mutations().size(), 1);
	QVERIFY(rig.state.configurationEditor().fieldIssues.isEmpty());
	QCOMPARE(std::get<Profile>(*rig.state.configurationEditor().base).profile.videoEncoding.maxBitrateBps,
		 quint64(12000000));
}
void ConfigurationControllerTest::configurationWriteSurvivesOtherChannelCancellation()
{
	// rig удерживает запись отдельного configApi; отмены двух остальных клиентов не должны её затронуть.
	ConfigurationRig rig;
	QVERIFY(rig.initialize());
	// held содержит принятый config API запрос, который должен пережить отмену остальных каналов.
	std::optional<HttpServer::Request> held;
	rig.intercept = [&](const HttpServer::Request &request) {
		if (request.method != "PATCH")
			return false;
		held = request;
		return true;
	};
	QVERIFY(rig.configuration.open(ConfigurationKind::Stream, QStringLiteral("stream")));
	QTRY_VERIFY_WITH_TIMEOUT(rig.state.configurationEditor().base.has_value(), 1000);
	// draft задаёт единственное новое имя; серверная версия после ответа имеет revision2.
	auto draft = std::get<Stream>(*rig.state.configurationEditor().draft);
	draft.name = QStringLiteral("Независимая запись");
	QVERIFY(rig.configuration.setDraft(draft, {QStringLiteral("name")}));
	QVERIFY(rig.configuration.save());
	QTRY_VERIFY_WITH_TIMEOUT(held.has_value(), 1000);
	rig.observer.cancelAll();
	rig.command.cancelAll();
	rig.events.close();
	rig.stream.insert(QStringLiteral("name"), draft.name);
	rig.stream.insert(QStringLiteral("revision"), QJsonValue(qint64(2)));
	HttpServer::reply(*held, fixture::bytes(rig.stream));
	QTRY_COMPARE_WITH_TIMEOUT(rig.state.configurationEditor().outcome, ConfigurationOutcome::Saved, 1000);
	QCOMPARE(rig.state.configuration().streams.value(QStringLiteral("stream")).name, draft.name);
}
void ConfigurationControllerTest::conflictCannotEraseExplicitProfileContract()
{
	// rig обновляет полный серверный профиль между GET и PATCH.
	ConfigurationRig rig;
	QVERIFY(rig.initialize());
	rig.intercept = [&](const HttpServer::Request &request) {
		if (request.method != "PATCH")
			return false;
		// modern — свежий полный серверный рецепт, который нельзя удалить stale заменой.
		auto modern = fixture::groupProfile();
		modern.insert(QStringLiteral("live_audio_mode"), QStringLiteral("copy"));
		modern.insert(QStringLiteral("encoding"), fixture::encoding());
		rig.profile.insert(QStringLiteral("profile"), modern);
		rig.profile.insert(QStringLiteral("revision"), QJsonValue(qint64(2)));
		HttpServer::reply(request, R"({"status":409,"code":"revision_conflict","title":"Conflict"})", 409,
				  "application/problem+json");
		return true;
	};
	QVERIFY(rig.configuration.open(ConfigurationKind::Profile, QStringLiteral("profile")));
	QTRY_VERIFY_WITH_TIMEOUT(rig.state.configurationEditor().base.has_value(), 1000);
	// draft сохраняет полное намерение; конфликт не разрешает удалить обязательный encoding.
	auto draft = std::get<Profile>(*rig.state.configurationEditor().draft);
	draft.profile.audioEncoding.bitrateBps = 128000;
	QVERIFY(rig.configuration.setDraft(draft, {QStringLiteral("profile")}));
	QVERIFY(rig.configuration.save());
	QTRY_COMPARE_WITH_TIMEOUT(rig.state.configurationEditor().outcome, ConfigurationOutcome::Conflict, 1000);
	draft.profile.encoding.state = FieldState::Missing;
	QVERIFY(!rig.configuration.confirmConflict(draft, {QStringLiteral("profile")}));
	QCOMPARE(rig.mutations().size(), 1);
}

// Проверяет безопасный default до первого серверного POST и после его подтверждения.
void ConfigurationControllerTest::newDestinationStartsDisabled()
{
	// rig предоставляет только синтетическую конфигурацию связей.
	ConfigurationRig rig;
	QVERIFY(rig.initialize());
	QVERIFY(rig.configuration.beginCreate(ConfigurationKind::Destination));
	// draft заполняет обязательные поля без установки enabled.
	auto draft = std::get<Destination>(*rig.state.configurationEditor().draft);
	QVERIFY(!draft.enabled);
	draft.name = QStringLiteral("Новое назначение");
	draft.streamId = QStringLiteral("stream");
	draft.groupId = QStringLiteral("group");
	draft.platform = QStringLiteral("youtube");
	draft.endpoint = {FieldState::Value, fixture::endpoint()};
	draft.streamKey = {FieldState::Value, QString{}};
	QVERIFY(rig.configuration.setDraft(draft, {QStringLiteral("name"), QStringLiteral("stream_id"),
						   QStringLiteral("group_id"), QStringLiteral("platform"),
						   QStringLiteral("endpoint"), QStringLiteral("stream_key")}));
	QVERIFY(rig.configuration.save());
	QTRY_VERIFY_WITH_TIMEOUT(!rig.state.configurationEditor().writing, 1000);
	QCOMPARE(rig.mutations().size(), 1);
	QCOMPARE(rig.mutations().first().method, QByteArray("POST"));
	// body проверяет именно явный false в API, а не отсутствие, которое могло бы включить default backend.
	const auto body = QJsonDocument::fromJson(rig.mutations().first().body).object();
	QVERIFY(body.contains(QStringLiteral("enabled")));
	QCOMPARE(body.value(QStringLiteral("enabled")), QJsonValue(false));
}

// Частичный отказ одного назначения не блокирует попытки остальных и не меняет текущий эфир.
void ConfigurationControllerTest::streamSelectionContinuesAfterPartialFailure()
{
	// rig/documents моделируют три включённых назначения с одинаковой исходной ревизией.
	ConfigurationRig rig;
	QVERIFY(rig.initialize());
	auto documents = destinationDocuments(rig);
	QCOMPARE(documents.size(), 4);
	// Перехватчик отклоняет только b; остальные PATCH/GET обслуживаются независимыми серверными документами.
	rig.intercept = [&](const HttpServer::Request &request) {
		if (request.method == "PATCH" && destinationRequestId(request) == QStringLiteral("b")) {
			HttpServer::reply(request, "{}", 422, "application/problem+json");
			return true;
		}
		return serveDestinations(request, documents);
	};
	QVERIFY(rig.configuration.open(ConfigurationKind::Stream, QStringLiteral("stream")));
	QTRY_VERIFY_WITH_TIMEOUT(rig.state.configurationEditor().base.has_value(), 1000);
	QVERIFY(rig.configuration.setDestinationEnabled(QStringLiteral("a"), false));
	QVERIFY(rig.configuration.setDestinationEnabled(QStringLiteral("b"), false));
	QVERIFY(rig.configuration.setDestinationEnabled(QStringLiteral("c"), false));
	QVERIFY(rig.configuration.save());
	QTRY_VERIFY_WITH_TIMEOUT(!rig.state.configurationEditor().writing, 2000);
	QCOMPARE(rig.mutations().size(), 3);
	QVERIFY(!documents.value(QStringLiteral("a")).value(QStringLiteral("enabled")).toBool());
	QVERIFY(documents.value(QStringLiteral("b")).value(QStringLiteral("enabled")).toBool());
	QVERIFY(!documents.value(QStringLiteral("c")).value(QStringLiteral("enabled")).toBool());
	QCOMPARE(rig.state.configurationEditor().destinationChanges,
		 (QMap<QString, bool>{{QStringLiteral("b"), false}}));
	QCOMPARE(rig.state.configurationEditor().destinationOutcomes.value(QStringLiteral("a")),
		 ConfigurationOutcome::Saved);
	QCOMPARE(rig.state.configurationEditor().destinationOutcomes.value(QStringLiteral("b")),
		 ConfigurationOutcome::Rejected);
	QCOMPARE(rig.state.configurationEditor().destinationOutcomes.value(QStringLiteral("c")),
		 ConfigurationOutcome::Saved);
	// request должен быть только PATCH enabled; серверных команд текущей Session здесь быть не может.
	for (const auto &request : rig.mutations()) {
		QCOMPARE(request.method, QByteArray("PATCH"));
		QVERIFY(request.target.startsWith("/api/v1/destinations/"));
		QCOMPARE(QJsonDocument::fromJson(request.body).object(),
			 (QJsonObject{{QStringLiteral("enabled"), false},
				      {QStringLiteral("expected_revision"), QJsonValue(qint64(1))}}));
	}
	QCOMPARE(rig.state.selectedStreamId(), QStringLiteral("stream"));
}

// Задаёт независимые виды отказа уже отправленного элемента пакета.
void ConfigurationControllerTest::streamSelectionReconcilesUncertainAndConflictingItems_data()
{
	QTest::addColumn<int>("status");
	QTest::addColumn<bool>("committed");
	QTest::newRow("lost-after-commit") << 0 << true;
	QTest::newRow("lost-before-commit") << 0 << false;
	QTest::newRow("revision-conflict") << 409 << false;
}

// Повтор использует наблюдаемую новую ревизию и не отправляет уже достигнутое enabled повторно.
void ConfigurationControllerTest::streamSelectionReconcilesUncertainAndConflictingItems()
{
	// status задаёт транспортный/ревизионный отказ; committed означает реальное применение до потери ответа.
	QFETCH(int, status);
	QFETCH(bool, committed);
	// rig/documents независимы от формы, firstFailure разрешает только один управляемый отказ.
	ConfigurationRig rig;
	QVERIFY(rig.initialize());
	auto documents = destinationDocuments(rig);
	bool firstFailure = true;
	// Перехватчик меняет серверную ревизию до отказа и затем обслуживает только адресные сверки/новые намерения.
	rig.intercept = [&](const HttpServer::Request &request) {
		if (request.method == "PATCH" && destinationRequestId(request) == QStringLiteral("a") && firstFailure) {
			firstFailure = false;
			documents[QStringLiteral("a")].insert(QStringLiteral("revision"), QJsonValue(qint64(2)));
			if (committed)
				documents[QStringLiteral("a")].insert(QStringLiteral("enabled"), false);
			if (status)
				HttpServer::reply(request,
						  status == 409 ? R"({"status":409,"code":"revision_conflict"})" : "{}",
						  status, "application/problem+json");
			else if (request.socket)
				request.socket->abort();
			return true;
		}
		return serveDestinations(request, documents);
	};
	QVERIFY(rig.configuration.open(ConfigurationKind::Stream, QStringLiteral("stream")));
	QTRY_VERIFY_WITH_TIMEOUT(rig.state.configurationEditor().base.has_value(), 1000);
	QVERIFY(rig.configuration.setDestinationEnabled(QStringLiteral("a"), false));
	QVERIFY(rig.configuration.setDestinationEnabled(QStringLiteral("c"), false));
	QVERIFY(rig.configuration.save());
	QTRY_VERIFY_WITH_TIMEOUT(!rig.state.configurationEditor().writing, 2000);
	QCOMPARE(rig.mutations().size(), 2);
	QVERIFY(rig.state.configurationEditor().destinationChanges.contains(QStringLiteral("a")));
	QVERIFY(!rig.state.configurationEditor().destinationChanges.contains(QStringLiteral("c")));
	QVERIFY(!rig.configuration.save());
	QVERIFY(rig.configuration.reload());
	QTRY_VERIFY_WITH_TIMEOUT(!rig.state.configurationEditor().loading, 2000);
	QCOMPARE(rig.mutations().size(), 2);
	if (committed) {
		QVERIFY(!rig.state.configurationEditor().destinationChanges.contains(QStringLiteral("a")));
	} else {
		QVERIFY(rig.state.configurationEditor().destinationChanges.contains(QStringLiteral("a")));
		QVERIFY(rig.configuration.save());
		QTRY_VERIFY_WITH_TIMEOUT(!rig.state.configurationEditor().writing, 2000);
		QCOMPARE(rig.mutations().size(), 3);
		QCOMPARE(QJsonDocument::fromJson(rig.mutations().last().body)
				 .object()
				 .value(QStringLiteral("expected_revision"))
				 .toInteger(),
			 qint64(2));
		QVERIFY(rig.state.configurationEditor().destinationChanges.isEmpty());
	}
}

// Проверяет границу владения назначениями и запрет вывода об отсутствующих элементах частичного списка.
void ConfigurationControllerTest::streamSelectionRequiresCompleteOwnDestinationList()
{
	// rig/documents/config задают известные записи, но заведомо неполную коллекцию.
	ConfigurationRig rig;
	QVERIFY(rig.initialize());
	auto documents = destinationDocuments(rig);
	QCOMPARE(documents.size(), 4);
	QVERIFY(rig.configuration.open(ConfigurationKind::Stream, QStringLiteral("stream")));
	QTRY_VERIFY_WITH_TIMEOUT(rig.state.configurationEditor().base.has_value(), 1000);
	auto config = rig.state.configuration();
	config.destinationsStatus.complete = false;
	rig.state.setConfiguration(config);
	QVERIFY(!rig.configuration.setDestinationEnabled(QStringLiteral("a"), false));
	config.destinationsStatus.complete = true;
	rig.state.setConfiguration(config);
	QVERIFY(!rig.configuration.setDestinationEnabled(QStringLiteral("foreign"), false));
	QVERIFY(!rig.configuration.setDestinationEnabled(QStringLiteral("missing"), false));
	QVERIFY(rig.state.configurationEditor().destinationChanges.isEmpty());
	QVERIFY(rig.mutations().isEmpty());
}

// Контекстная отмена не продолжает пакет после уже отправленного PATCH и не принимает его поздний ответ.
void ConfigurationControllerTest::streamSelectionStopsAtContextChange()
{
	// rig/documents/held сохраняют точную первую запись до смены профиля OBS.
	ConfigurationRig rig;
	QVERIFY(rig.initialize());
	auto documents = destinationDocuments(rig);
	std::optional<HttpServer::Request> held;
	// Перехватчик удерживает только PATCH, оставляя чтения доступными в тестовом origin.
	rig.intercept = [&](const HttpServer::Request &request) {
		if (request.method == "PATCH") {
			held = request;
			return true;
		}
		return serveDestinations(request, documents);
	};
	QVERIFY(rig.configuration.open(ConfigurationKind::Stream, QStringLiteral("stream")));
	QTRY_VERIFY_WITH_TIMEOUT(rig.state.configurationEditor().base.has_value(), 1000);
	QVERIFY(rig.configuration.setDestinationEnabled(QStringLiteral("a"), false));
	QVERIFY(rig.configuration.setDestinationEnabled(QStringLiteral("c"), false));
	QVERIFY(rig.configuration.save());
	QTRY_VERIFY_WITH_TIMEOUT(held.has_value(), 1000);
	rig.state.setSelection(QStringLiteral("stream"), true, {}, {}, QStringLiteral("other-profile"));
	QVERIFY(!rig.state.configurationEditor().writing);
	QVERIFY(!rig.configuration.save());
	HttpServer::reply(*held, fixture::bytes(documents.value(QStringLiteral("a"))));
	// barrier подтверждает обработку позднего ответа без произвольной задержки.
	bool barrier = false;
	rig.configApi.getLive(this, [&](RequestContext, Result<Health>) { barrier = true; });
	QTRY_VERIFY_WITH_TIMEOUT(barrier, 1000);
	QCOMPARE(rig.mutations().size(), 1);
	QCOMPARE(rig.state.configurationEditor().context.profileUuid, QStringLiteral("profile-uuid"));
}

// Разделяет подтверждённое отсутствие/смену владельца и отсутствие достоверного чтения после Unknown PATCH.
void ConfigurationControllerTest::streamSelectionReconciliationDistinguishesUnavailableFromTransientFailure_data()
{
	QTest::addColumn<int>("readStatus");
	QTest::newRow("deleted-destination") << 404;
	QTest::newRow("moved-to-other-stream") << 200;
	QTest::newRow("transient-read-failure") << 503;
}

// Проверяет, что сверка снимает только доказанно устаревшее намерение, сохраняя остальные пользовательские изменения.
void ConfigurationControllerTest::streamSelectionReconciliationDistinguishesUnavailableFromTransientFailure()
{
	// readStatus выбирает независимый ответ адресного GET после потери ответа PATCH.
	QFETCH(int, readStatus);
	// rig/documents содержат три собственных назначения; failB отклоняет b только в первом пакете.
	ConfigurationRig rig;
	QVERIFY(rig.initialize());
	auto documents = destinationDocuments(rig);
	bool failB = true;
	// Перехватчик теряет ответ a, отклоняет первый b, затем моделирует точное серверное состояние спорной a.
	rig.intercept = [&](const HttpServer::Request &request) {
		if (request.method == "PATCH" && destinationRequestId(request) == QStringLiteral("a")) {
			if (request.socket)
				request.socket->abort();
			return true;
		}
		if (request.method == "PATCH" && destinationRequestId(request) == QStringLiteral("b") && failB) {
			failB = false;
			HttpServer::reply(request, "{}", 422, "application/problem+json");
			return true;
		}
		if (request.method == "GET" && destinationRequestId(request) == QStringLiteral("a")) {
			if (readStatus == 200) {
				// moved — полный подтверждённый DTO, чья принадлежность изменилась независимо от клиента.
				auto moved = documents.value(QStringLiteral("a"));
				moved.insert(QStringLiteral("stream_id"), QStringLiteral("other-stream"));
				moved.insert(QStringLiteral("revision"), QJsonValue(qint64(2)));
				HttpServer::reply(request, fixture::bytes(moved));
			} else
				HttpServer::reply(request, "{}", readStatus, "application/problem+json");
			return true;
		}
		return serveDestinations(request, documents);
	};
	QVERIFY(rig.configuration.open(ConfigurationKind::Stream, QStringLiteral("stream")));
	QTRY_VERIFY_WITH_TIMEOUT(rig.state.configurationEditor().base.has_value(), 1000);
	QVERIFY(rig.configuration.setDestinationEnabled(QStringLiteral("a"), false));
	QVERIFY(rig.configuration.setDestinationEnabled(QStringLiteral("b"), false));
	QVERIFY(rig.configuration.save());
	QTRY_VERIFY_WITH_TIMEOUT(!rig.state.configurationEditor().writing, 2000);
	QCOMPARE(rig.mutations().size(), 2);
	QVERIFY(rig.state.configurationEditor().destinationReconciliation.contains(QStringLiteral("a")));
	QCOMPARE(rig.state.configurationEditor().destinationOutcomes.value(QStringLiteral("a")),
		 ConfigurationOutcome::Unknown);
	QVERIFY(rig.configuration.reload());
	QTRY_VERIFY_WITH_TIMEOUT(!rig.state.configurationEditor().loading, 2000);
	QCOMPARE(rig.mutations().size(), 2);
	QVERIFY(rig.state.configurationEditor().destinationChanges.contains(QStringLiteral("b")));
	// draft задаёт обычное изменение имени Stream после сверки, независимо от результата enabled назначения a.
	auto draft = std::get<Stream>(*rig.state.configurationEditor().draft);
	draft.name = QStringLiteral("Имя после сверки назначений");
	QVERIFY(rig.configuration.setDraft(draft, {QStringLiteral("name")}));
	if (readStatus == 503) {
		QVERIFY(rig.state.configurationEditor().destinationReconciliation.contains(QStringLiteral("a")));
		QVERIFY(rig.state.configurationEditor().destinationChanges.contains(QStringLiteral("a")));
		QVERIFY(rig.state.configurationEditor().destinationRevisions.contains(QStringLiteral("a")));
		QCOMPARE(rig.state.configurationEditor().destinationOutcomes.value(QStringLiteral("a")),
			 ConfigurationOutcome::Unknown);
		QVERIFY(!rig.configuration.save());
		QCOMPARE(rig.mutations().size(), 2);
		return;
	}
	QVERIFY(!rig.state.configurationEditor().destinationReconciliation.contains(QStringLiteral("a")));
	QVERIFY(!rig.state.configurationEditor().destinationChanges.contains(QStringLiteral("a")));
	QVERIFY(!rig.state.configurationEditor().destinationRevisions.contains(QStringLiteral("a")));
	QVERIFY(rig.state.configurationEditor().destinationOutcomes.contains(QStringLiteral("a")));
	QCOMPARE(rig.state.configurationEditor().destinationOutcomes.value(QStringLiteral("a")),
		 ConfigurationOutcome::Rejected);
	QVERIFY(rig.configuration.save());
	QTRY_VERIFY_WITH_TIMEOUT(!rig.state.configurationEditor().writing, 2000);
	QCOMPARE(rig.mutations().size(), 4);
	QVERIFY(!documents.value(QStringLiteral("b")).value(QStringLiteral("enabled")).toBool());
	QCOMPARE(rig.stream.value(QStringLiteral("name")).toString(), draft.name);
	QVERIFY(rig.state.configurationEditor().destinationChanges.isEmpty());
	QCOMPARE(rig.state.configurationEditor().destinationOutcomes.value(QStringLiteral("a")),
		 ConfigurationOutcome::Rejected);
	// aWrites считает только прежний PATCH спорного назначения: после доказанной недоступности его повтор запрещён.
	int aWrites = 0;
	// request проходит реальные записи, отличая PATCH назначения от PATCH обычного имени Stream.
	for (const auto &request : rig.mutations())
		if (destinationRequestId(request) == QStringLiteral("a"))
			++aWrites;
	QCOMPARE(aWrites, 1);
}

// Разделяет окончательный отказ записи и подтверждённые полным списком изменения до очередного PATCH.
void ConfigurationControllerTest::streamSelectionUnavailableItemsDoNotBlockOtherChanges_data()
{
	QTest::addColumn<QString>("scenario");
	QTest::newRow("known-patch-not-found") << QStringLiteral("patch404");
	QTest::newRow("removed-before-queued-send") << QStringLiteral("removed");
	QTest::newRow("moved-before-queued-send") << QStringLiteral("moved");
}

// Полный свежий список может доказать недоступность, но не должен лишать пользователя сохранения соседних назначений.
void ConfigurationControllerTest::streamSelectionUnavailableItemsDoNotBlockOtherChanges()
{
	// scenario задаёт точный момент подтверждения недоступности назначения b.
	QFETCH(QString, scenario);
	// rig/documents содержат серверные записи; held удерживает первый шаг только для чтения нового полного списка.
	ConfigurationRig rig;
	QVERIFY(rig.initialize());
	rig.configApi.setHttpTimeoutMs(2000);
	auto documents = destinationDocuments(rig);
	std::optional<HttpServer::Request> held;
	// Перехватчик возвращает PATCH404 либо удерживает a до завершения настоящего GET списка через контроллер.
	rig.intercept = [&](const HttpServer::Request &request) {
		if (request.method == "PATCH" && destinationRequestId(request) == QStringLiteral("b") &&
		    scenario == QStringLiteral("patch404")) {
			HttpServer::reply(request, "{}", 404, "application/problem+json");
			return true;
		}
		if (request.method == "PATCH" && destinationRequestId(request) == QStringLiteral("a") &&
		    scenario != QStringLiteral("patch404")) {
			held = request;
			return true;
		}
		return serveDestinations(request, documents);
	};
	QVERIFY(rig.configuration.open(ConfigurationKind::Stream, QStringLiteral("stream")));
	QTRY_VERIFY_WITH_TIMEOUT(rig.state.configurationEditor().base.has_value(), 1000);
	QVERIFY(rig.configuration.setDestinationEnabled(QStringLiteral("a"), false));
	QVERIFY(rig.configuration.setDestinationEnabled(QStringLiteral("b"), false));
	QVERIFY(rig.configuration.setDestinationEnabled(QStringLiteral("c"), false));
	QVERIFY(rig.configuration.save());
	if (scenario != QStringLiteral("patch404")) {
		QTRY_VERIFY_WITH_TIMEOUT(held.has_value(), 1000);
		if (scenario == QStringLiteral("removed"))
			documents.remove(QStringLiteral("b"));
		else
			documents[QStringLiteral("b")].insert(QStringLiteral("stream_id"),
							      QStringLiteral("other-stream"));
		rig.configuration.refresh(ConfigurationKind::Destination);
		QTRY_VERIFY_WITH_TIMEOUT(
			rig.state.configuration().destinationsStatus.complete &&
				rig.state.configuration().destinationsStatus.availability ==
					RuntimeAvailability::Fresh &&
				(scenario == QStringLiteral("removed")
					 ? !rig.state.configuration().destinations.contains(QStringLiteral("b"))
					 : rig.state.configuration().destinations.value(QStringLiteral("b")).streamId ==
						   QStringLiteral("other-stream")),
			1000);
		QVERIFY(serveDestinations(*held, documents));
	}
	QTRY_VERIFY_WITH_TIMEOUT(!rig.state.configurationEditor().writing, 2000);
	// firstBatchWrites фиксирует число реально отправленных записей до сохранения обычного имени Stream.
	const int firstBatchWrites = scenario == QStringLiteral("patch404") ? 3 : 2;
	QCOMPARE(rig.mutations().size(), firstBatchWrites);
	QVERIFY(!documents.value(QStringLiteral("a")).value(QStringLiteral("enabled")).toBool());
	QVERIFY(!documents.value(QStringLiteral("c")).value(QStringLiteral("enabled")).toBool());
	QVERIFY(!rig.state.configurationEditor().destinationChanges.contains(QStringLiteral("b")));
	QVERIFY(!rig.state.configurationEditor().destinationRevisions.contains(QStringLiteral("b")));
	QVERIFY(!rig.state.configurationEditor().destinationReconciliation.contains(QStringLiteral("b")));
	QCOMPARE(rig.state.configurationEditor().destinationOutcomes.value(QStringLiteral("b")),
		 ConfigurationOutcome::Rejected);
	// draft добавляет новое обычное намерение после окончательного отказа одного элемента пакета.
	auto draft = std::get<Stream>(*rig.state.configurationEditor().draft);
	draft.name = QStringLiteral("Сохранение после недоступного назначения");
	QVERIFY(rig.configuration.setDraft(draft, {QStringLiteral("name")}));
	QVERIFY(rig.configuration.save());
	QTRY_VERIFY_WITH_TIMEOUT(!rig.state.configurationEditor().writing, 2000);
	QCOMPARE(rig.mutations().size(), firstBatchWrites + 1);
	QCOMPARE(rig.stream.value(QStringLiteral("name")).toString(), draft.name);
	QCOMPARE(rig.state.configurationEditor().destinationOutcomes.value(QStringLiteral("b")),
		 ConfigurationOutcome::Rejected);
}

QTEST_MAIN(ConfigurationControllerTest)
#include "configuration-controller-test.moc"
