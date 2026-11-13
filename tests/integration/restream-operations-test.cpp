#include "api/api-client.hpp"
#include "api/event-client.hpp"
#include "api/json-decoder.hpp"
#include "core/connection-controller.hpp"
#include "core/restream-controller.hpp"
#include "core/settings-store.hpp"
#include "core/state-store.hpp"
#include "fixtures/fake-frontend.hpp"
#include "fixtures/http-server.hpp"
#include "fixtures/operation-json.hpp"
#include "fixtures/workspace-temp.hpp"
#include "obs/obs-controller.hpp"
#include "ui/management-window.hpp"
#include <QApplication>
#include <QComboBox>
#include <QMessageBox>
#include <QPushButton>

#include <QJsonDocument>
#include <QSignalSpy>
#include <QTest>
#include <QTimer>
#include <QUrlQuery>
#include <algorithm>

using namespace gost;
using namespace gost::api;

namespace {
// Предоставляет настоящему координатору локальный сервер и отдельно управляемые долговечные состояния.
// Начальный StateStore задаётся как подтверждённый результат наблюдателя; сам наблюдатель не подключается.
class OperationRig final {
public:
	// limits задают короткий ограниченный срок выполнения последовательности; все файлы и сокеты принадлежат этой фикстуре.
	explicit OperationRig(RestreamController::Limits limits = {300, 5, 30})
		: settings(temp.filePath(QStringLiteral("settings.json"))),
		  connection(state, settings, observer, events),
		  obs(frontend, state),
		  operations(state, command, connection, obs, limits)
	{
		// Обработчик полного request сначала вызывает сценарный intercept; необслуженный запрос передаёт respond.
		server.handler = [this](const auto &request) {
			if (!intercept || !intercept(request))
				respond(request);
		};
	}
	// Закрывает сервисы до уничтожения их зависимостей и локальных сокетов.
	~OperationRig()
	{
		operations.shutdown();
		connection.shutdown();
		obs.shutdown();
	}
	// Устанавливает свежий подтверждённый выбор с двумя Output; withSession=false задаёт полное отсутствие Session.
	bool initialize(bool withSession = true)
	{
		if (!temp.isValid() || !server.listen())
			return false;
		settings.setAutoConnect(false);
		state.setConnection(server.origin().toString(), ConnectionStatus::Connected);
		// stream/error являются значением REST Stream и возможной ошибкой разбора синтетического документа.
		Stream stream;
		Error error;
		if (!fromJson(fixture::stream(), stream, error))
			return false;
		state.setStreams({stream}, true);
		state.setSelection(QStringLiteral("stream"), true, {}, {}, QStringLiteral("profile-uuid"));
		// metadata подтверждает Session/Outputs отдельно от runtime SSE.
		DockServerState metadata;
		metadata.sessionStatus = {RuntimeAvailability::Fresh, true, {}};
		if (withSession)
			metadata.session = DockSession{QStringLiteral("session"), QStringLiteral("RUNNING"), false};
		metadata.outputsStatus = {RuntimeAvailability::Fresh, true, {}};
		metadata.outputs.insert(QStringLiteral("one"), {QStringLiteral("Первый"), true, true});
		metadata.outputs.insert(QStringLiteral("two"), {QStringLiteral("Второй"), true, true});
		state.setDockServer(metadata);
		obs.handleEvent(FrontendEvent::FinishedLoading);
		return state.dockContext().valid();
	}
	// Возвращает независимую копию актуального контекста для фиксации пользовательского намерения.
	DockContext context() const { return state.dockContext(); }
	// Возвращает копии реальных POST, исключая чтения, которые могут выполняться между шагами.
	QList<HttpServer::Request> posts() const
	{
		// result содержит только команды в порядке их поступления.
		QList<HttpServer::Request> result;
		// request — очередной полный запрос, сохранённый сетевой фикстурой.
		for (const auto &request : server.requests)
			if (request.method == "POST")
				result.append(request);
		return result;
	}
	// Проверяет сохранённый публичный результат без чтения внутренних фаз реализации.
	bool finished() const
	{
		return state.operations().current && state.operations().current->phase == OperationPhase::Finished;
	}
	// Формирует принятую квитанцию из полученного POST; sendReply=false моделирует потерю ответа после принятия.
	void accept(const HttpServer::Request &request, bool sendReply = true)
	{
		// path/action задают реальный маршрут, payload сохраняет переданный клиентом request_id.
		const auto path = QUrl::fromEncoded("http://local" + request.target).path();
		const auto action = path.section('/', -1);
		const auto payload = QJsonDocument::fromJson(request.body).object();
		// outputId пуст для Session-команд; kind и target определяются серверным маршрутом.
		const auto outputId = path.contains(QStringLiteral("/outputs/")) ? path.section('/', -2, -2)
										 : QString{};
		const auto kind =
			action + (outputId.isEmpty() ? QStringLiteral("_session") : QStringLiteral("_output"));
		const auto target = outputId.isEmpty()
					    ? (path.contains(QStringLiteral("/streams/")) ? QStringLiteral("stream")
											  : QStringLiteral("session"))
					    : outputId;
		// id соответствует отдельной серверной команде; ручной повтор request_id возвращает прежнюю запись.
		QString id;
		// item — уже принятая квитанция с потенциально совпадающим request_id.
		for (const auto &item : receipts)
			if (item.value(QStringLiteral("request_id")) == payload.value(QStringLiteral("request_id")))
				id = item.value(QStringLiteral("id")).toString();
		if (id.isEmpty()) {
			id = QStringLiteral("command-%1").arg(receipts.size() + 1);
			receipts.insert(id, fixture::receipt(payload.value(QStringLiteral("request_id")).toString(),
							     kind, target, QStringLiteral("session"), outputId, id));
		}
		if (action == QStringLiteral("pause"))
			paused = true;
		if (action == QStringLiteral("resume"))
			paused = false;
		if (action == QStringLiteral("stop") && outputId.isEmpty())
			sessionState = QStringLiteral("STOPPED");
		if (!outputId.isEmpty()) {
			desired[outputId] = action == QStringLiteral("start");
			attached[outputId] = action != QStringLiteral("detach");
		}
		if (sendReply)
			HttpServer::reply(request, fixture::bytes(receipts.value(id)), 202);
	}
	// Отвечает на стандартные маршруты; intercept позволяет сценариям независимо внедрять отказ/задержку.
	void respond(const HttpServer::Request &request)
	{
		// url/path отделяют непрозрачный query от адреса ресурса.
		const auto url = QUrl::fromEncoded("http://local" + request.target);
		const auto path = url.path();
		if (request.method == "POST") {
			accept(request);
		} else if (path.startsWith(QStringLiteral("/api/v1/commands/"))) {
			// receipt копируется перед сменой status, не меняя идентичность принятого POST.
			auto receipt = receipts.value(path.section('/', -1));
			receipt.insert(QStringLiteral("status"),
				       statuses.isEmpty() ? QStringLiteral("SUCCEEDED") : statuses.front());
			if (statuses.size() > 1)
				statuses.removeFirst();
			HttpServer::reply(request, fixture::bytes(receipt));
		} else if (path == QStringLiteral("/api/v1/sessions/session")) {
			// session отражает долговечное состояние независимо от runtime и receipt.status.
			auto session = fixture::session();
			session.insert(QStringLiteral("state"), sessionState);
			session.insert(QStringLiteral("desired_paused"), paused);
			HttpServer::reply(request, fixture::bytes(session));
		} else if (path.contains(QStringLiteral("/outputs/"))) {
			// id/output задают точный Output; удалённое runtime=null допустимо для завершённой очистки.
			const auto id = path.section('/', -1);
			auto output = fixture::output();
			output.insert(QStringLiteral("id"), id);
			output.insert(QStringLiteral("desired_running"), desired.value(id, true));
			output.insert(QStringLiteral("attached"), attached.value(id, true));
			output.insert(QStringLiteral("runtime"), QJsonValue::Null);
			output.insert(QStringLiteral("runtime_availability"), QStringLiteral("unavailable"));
			output.insert(QStringLiteral("runtime_reason"), QStringLiteral("execution_removed"));
			HttpServer::reply(request, fixture::bytes(output));
		} else if (path == QStringLiteral("/api/v1/streams/stream/sessions")) {
			// Полный фильтрованный поиск перед первым предварительным запуском подтверждает отсутствие активной Session.
			HttpServer::reply(request, fixture::page({}));
		} else if (path == QStringLiteral("/api/v1/streams/stream")) {
			HttpServer::reply(request, fixture::bytes(fixture::stream()));
		} else if (path == QStringLiteral("/api/v1/streams/stream/destinations")) {
			// destination — заранее настроенное назначение; disabled не должен попасть в payload.
			auto destination = fixture::destination();
			destination.insert(QStringLiteral("enabled"), enabled);
			HttpServer::reply(request, fixture::page({destination}));
		} else if (path == QStringLiteral("/api/v1/streams/stream/fallback-policy")) {
			HttpServer::reply(
				request,
				R"({"revision":"1","fallback":{"asset_id":"asset","watchdog":{"video_timeout_ns":"1","audio_timeout_ns":"1","recovery_window_ns":"1","keyframe_timeout_ns":"1"}}})");
		} else if (path == QStringLiteral("/api/v1/fallback-assets/asset")) {
			// asset публикует current и pending одновременно: выбирать pending запрещено.
			auto asset = fixture::asset();
			asset.insert(QStringLiteral("current_generation_id"), QStringLiteral("current"));
			asset.insert(QStringLiteral("pending_generation_id"), QStringLiteral("pending"));
			HttpServer::reply(request, fixture::bytes(asset));
		} else if (path == QStringLiteral("/api/v1/fallback-assets/asset/generations/current")) {
			// generation — только точное опубликованное поколение.
			auto generation = fixture::record(QStringLiteral("current"));
			generation.insert(QStringLiteral("asset_id"), QStringLiteral("asset"));
			generation.insert(QStringLiteral("kind"), QStringLiteral("image"));
			generation.insert(QStringLiteral("status"), QStringLiteral("ready"));
			generation.insert(QStringLiteral("size_bytes"), QStringLiteral("1024"));
			generation.insert(QStringLiteral("failure"), QJsonValue::Null);
			HttpServer::reply(request, fixture::bytes(generation));
		} else if (path == QStringLiteral("/api/v1/fallback-assets/asset/variants")) {
			// variant содержит полный наследуемый профиль и точный generation_id.
			auto variant = fixture::variant();
			variant.insert(QStringLiteral("generation_id"), QStringLiteral("current"));
			if (!variantProfile.isEmpty())
				variant.insert(QStringLiteral("profile"), variantProfile);
			if (paginateVariants && QUrlQuery(url).queryItemValue(QStringLiteral("cursor")).isEmpty())
				HttpServer::reply(request, fixture::page({}, QStringLiteral("opaque/+%2F")));
			else
				HttpServer::reply(request, fixture::page({variant}));
		} else if (path == QStringLiteral("/api/v1/transcode-groups/group")) {
			HttpServer::reply(request, fixture::bytes(fixture::group()));
		} else if (path == QStringLiteral("/api/v1/transcode-profiles/profile")) {
			HttpServer::reply(request, fixture::bytes(fixture::profile()));
		} else {
			HttpServer::reply(request, "{}", 404);
		}
	}
	// Каталог настроек изолирован внутри workspace и удаляется после сервисов.
	WorkspaceTemp temp{QStringLiteral("operations")};
	// HTTP-сокеты не обращаются к настоящему backend.
	HttpServer server;
	// Предпочтения нужны реальному ConnectionController, а команды в них не записываются.
	SettingsStore settings;
	// Общая публичная модель результата и наблюдений.
	StateStore state;
	// observer читает наблюдения и отменяется независимо от командной последовательности.
	APIClient observer;
	// command отправляет команды и читает их квитанции независимо от SSE.
	APIClient command;
	// Отдельный SSE-клиент остаётся неактивен, пока сценарий не подключит наблюдатель явно.
	EventClient events;
	// Настоящий наблюдатель предоставляет API refresh/disconnect.
	ConnectionController connection;
	// Управляемая граница OBS не создаёт второго output.
	FakeFrontend frontend;
	// Подтверждает остановку штатного выхода через Frontend.
	OBSController obs;
	// Единственный исполнитель настоящих command POST/GET.
	RestreamController operations;
	// Необязательный перехватчик возвращает true, если запрос полностью обслужен сценарием.
	std::function<bool(const HttpServer::Request &)> intercept;
	// Принятые квитанции по серверному command_id для проверки повторов.
	QMap<QString, QJsonObject> receipts;
	// Очередь ответов GET receipt; последнее значение повторяется.
	QStringList statuses;
	// desired содержит долговечное desired_running каждого Output, отдельно от runtime.
	QMap<QString, bool> desired;
	// attached содержит долговечную прикреплённость каждого Output.
	QMap<QString, bool> attached;
	// paused содержит долговечное desired_paused исходной Session.
	bool paused = false;
	// sessionState содержит долговечный статус исходной Session, независимо от квитанции.
	QString sessionState = QStringLiteral("RUNNING");
	// Флаг назначения позволяет проверить запрет пустого start payload.
	bool enabled = true;
	// true размещает подходящий вариант только на второй странице.
	bool paginateVariants = false;
	// Непустое значение заменяет профиль варианта для отрицательных проверок сравнения.
	QJsonObject variantProfile;
};
} // namespace

// Проверяет команды по независимым серверным квитанциям, состояниям и записанным POST.
class RestreamOperationsTest final : public QObject {
	Q_OBJECT
private slots:
	// Задаёт два явных способа прервать живую предварительную проверку.
	void interruptedPrecheckDoesNotRemainChecking_data();
	// Stop All и отключение снимают Checking; поздний ответ не возвращает отменённой проверке Ready.
	void interruptedPrecheckDoesNotRemainChecking();
	// Явный повтор неизвестного шага пакета не возобновляет ни start текущего Output, ни следующие элементы.
	void manualRetryDoesNotResumeBatch();
	// Каждая ручная попытка получает отдельный ограниченный срок, включая неизвестную повторную квитанцию.
	void manualRetryHasFreshBoundedBudget();
	// OBS Stop и потеря наблюдения без явного намерения не посылают серверные команды.
	void passiveEventsNeverControlServer();
	// Состояние RUNNING точной Session обязательно даже при SUCCEEDED и активном OBS.
	void fallbackReceiptDoesNotReplaceRunningSession();
	// Статус чтения отсутствия Session контролирует пригодность первого предварительного запуска.
	void fallbackRequiresConfirmedAbsence_data();
	// Неизвестное/неполное отсутствие не разрешает start, известная завершённая Session его не блокирует.
	void fallbackRequiresConfirmedAbsence();
	// Принятый предварительный запуск с потерянным ответом повторяется тем же ID даже после появления Session в состоянии RUNNING.
	void lostStartRetryAfterSessionAppears();
	// Противоречащая автопубликации команда 409 не порождает новый start или ложный собственный успех.
	void conflictingStartDoesNotCreateAnotherIntent();
	// Подтверждённая строка адресуется её исходным ID, а смена Session во время диалога отвергает действие.
	void outputConfirmationKeepsIdentity_data();
	// Выполняет смену строки или Session внутри модального подтверждения.
	void outputConfirmationKeepsIdentity();
	// Закрытие ManagementWindow не уничтожает общее ожидание принятой команды.
	void closingManagementDoesNotCancelCommand();
	// RETRYING с desired=true не считается остановленным; фиксированный пакет не расширяется новыми строками.
	void startStoppedKeepsFixedEligibleIds();
	// Pause ждёт промежуточные квитанции и отдельное desired_paused, Resume создаёт новый request_id.
	void pauseResumeRequireReceiptAndState();
	// Терминальные отказы и несовместимая квитанция не становятся успехом.
	void invalidReceipts_data();
	// Выполняет отрицательный сценарий status/identity из строки данных.
	void invalidReceipts();
	// SUCCEEDED при противоречащем долговечном состоянии завершается Unknown по бюджету.
	void receiptWithoutStateIsUnknown();
	// Повтор потерянного ответа сохраняет исходные байты POST и получает новый бюджет.
	void manualRetryKeepsRequestAndPayload();
	// Stop/detach допускают удалённое runtime; restart ждёт stop и создаёт отдельный start ID.
	void outputActions_data();
	// Проверяет точные маршруты и итог выбранного Output.
	void outputActions();
	// При отказе второго элемента пакет хранит Completed/Failed без общего успеха.
	void batchReportsPerElementFailure();
	// Обрыв связи прекращает новые шаги, сохраняя чтение принятой квитанции.
	void lossFreezesBatchButReceiptRemainsReadable();
	// Смена Session не позволяет направить остаток пакета следующему эфиру.
	void sessionChangeFreezesBatch();
	// Локальный отказ OBS не подменяется успехом даже при успешной очистке Session.
	void stopAllKeepsObsAndServerResultsSeparate_data();
	// Сопоставляет подтверждение OBS, server stop и появление следующей Session.
	void stopAllKeepsObsAndServerResultsSeparate();
	// Полная проверка использует current и все страницы вариантов; start передаёт фиксированный непустой список.
	void fallbackPrecheckAndStart();
	// Пустые назначения и различия полного профиля запрещают предварительный запуск.
	void fallbackRejectsMissingCompatibility_data();
	// Проверяет независимый отрицательный сценарий проверки готовности.
	void fallbackRejectsMissingCompatibility();
	// Старый контекст и новый исполнитель последовательности не отправляют сохранённые команды автоматически.
	void contextChangeAndRestartDoNotReplay();
};

void RestreamOperationsTest::pauseResumeRequireReceiptAndState()
{
	// rig задаёт RUNNING Session; 1000 мс допускают проверку цели, POST, промежуточные квитанции и два чтения состояния.
	OperationRig rig({1000, 5, 30});
	QVERIFY(rig.initialize());
	rig.statuses = {QStringLiteral("PENDING"), QStringLiteral("CLAIMED"), QStringLiteral("ISSUED"),
			QStringLiteral("SUCCEEDED")};
	// reads считает чтения точной Session после успеха квитанции; преждевременный успех недопустим.
	int reads = 0;
	// earlyCompletion обнаруживает успех, объявленный до отдельного подтверждения сохранённого состояния.
	bool earlyCompletion = false;
	// rig.intercept получает HTTP-запрос request и подменяет первый GET Session после POST ещё не обновлённым
	// состоянием, проверяя отсутствие раннего успеха. false передаёт остальные запросы обычному серверу фикстуры.
	rig.intercept = [&](const auto &request) {
		if (request.target == "/api/v1/sessions/session" && !rig.posts().isEmpty() && ++reads == 1) {
			earlyCompletion = !rig.state.operations().current ||
					  rig.state.operations().current->outcome == OperationOutcome::Completed;
			// session намеренно ещё не подтверждает паузу, несмотря на SUCCEEDED.
			auto session = fixture::session();
			HttpServer::reply(request, fixture::bytes(session));
			return true;
		}
		return false;
	};
	QVERIFY(rig.operations.pause(rig.context(), QStringLiteral("session")));
	QTRY_VERIFY_WITH_TIMEOUT(rig.finished(), 2000);
	QCOMPARE(rig.state.operations().current->outcome, OperationOutcome::Completed);
	QVERIFY(rig.state.operations().current->stateConfirmed);
	QVERIFY(reads >= 2);
	QVERIFY(!earlyCompletion);
	QCOMPARE(rig.posts().size(), 1);
	// previousRequest/previousCommand фиксируют идентичности завершённого намерения, не генератор ID реализации.
	const auto previousRequest = rig.state.operations().current->requestId;
	const auto previousCommand = rig.state.operations().current->commandId;
	QVERIFY(!previousRequest.isEmpty());
	rig.intercept = {};
	QVERIFY(rig.operations.resume(rig.context(), QStringLiteral("session")));
	QTRY_VERIFY_WITH_TIMEOUT(rig.finished(), 2000);
	QCOMPARE(rig.state.operations().current->outcome, OperationOutcome::Completed);
	QVERIFY(rig.state.operations().current->requestId != previousRequest);
	QVERIFY(rig.state.operations().current->commandId != previousCommand);
	QCOMPARE(rig.posts().size(), 2);
	QVERIFY(!rig.frontend.streaming);
	QCOMPARE(rig.frontend.stopRequests, 0);
}

void RestreamOperationsTest::invalidReceipts_data()
{
	QTest::addColumn<QString>("field");
	QTest::addColumn<QString>("value");
	QTest::addColumn<bool>("terminalFailure");
	QTest::newRow("failed") << QStringLiteral("status") << QStringLiteral("FAILED") << true;
	QTest::newRow("interrupted") << QStringLiteral("status") << QStringLiteral("INTERRUPTED") << true;
	QTest::newRow("unknown-status") << QStringLiteral("status") << QStringLiteral("FUTURE_STATUS") << false;
	QTest::newRow("request-mismatch") << QStringLiteral("request_id") << QStringLiteral("other-request") << false;
	QTest::newRow("command-mismatch") << QStringLiteral("id") << QStringLiteral("other-command") << false;
	QTest::newRow("kind-mismatch") << QStringLiteral("kind") << QStringLiteral("resume_session") << false;
	QTest::newRow("target-mismatch") << QStringLiteral("target_id") << QStringLiteral("other-session") << false;
	QTest::newRow("session-mismatch") << QStringLiteral("session_id") << QStringLiteral("other-session") << false;
	QTest::newRow("output-mismatch") << QStringLiteral("output_id") << QStringLiteral("unexpected-output") << false;
}

void RestreamOperationsTest::invalidReceipts()
{
	// field называет изменяемое поле отрицательной квитанции.
	QFETCH(QString, field);
	// value задаёт несовместимое или терминальное значение выбранного поля.
	QFETCH(QString, value);
	// terminalFailure отличает подтверждённый отказ от несовместимой неизвестной квитанции.
	QFETCH(bool, terminalFailure);
	// rig принимает POST корректно, но GET квитанции возвращает независимо повреждённый контракт.
	OperationRig rig;
	QVERIFY(rig.initialize());
	rig.intercept = [&](const auto &request) {
		if (!request.target.startsWith("/api/v1/commands/"))
			return false;
		// receipt сохраняет все поля принятой команды кроме выбранного отрицательного отличия.
		auto receipt =
			rig.receipts.value(QUrl::fromEncoded("http://local" + request.target).path().section('/', -1));
		receipt.insert(QStringLiteral("status"), QStringLiteral("SUCCEEDED"));
		receipt.insert(field, value);
		HttpServer::reply(request, fixture::bytes(receipt));
		return true;
	};
	QVERIFY(rig.operations.pause(rig.context(), QStringLiteral("session")));
	QTRY_VERIFY_WITH_TIMEOUT(rig.finished(), 2000);
	QCOMPARE(rig.state.operations().current->outcome,
		 terminalFailure ? OperationOutcome::Failed : OperationOutcome::Unknown);
	QVERIFY(!rig.state.operations().current->stateConfirmed);
	QCOMPARE(rig.posts().size(), 1);
}

void RestreamOperationsTest::receiptWithoutStateIsUnknown()
{
	// rig выдаёт SUCCEEDED, но сохранённое desired_paused остаётся false до исчерпания бюджета.
	OperationRig rig({500, 5, 20});
	QVERIFY(rig.initialize());
	rig.intercept = [&](const auto &request) {
		if (request.target != "/api/v1/sessions/session")
			return false;
		HttpServer::reply(request, fixture::bytes(fixture::session()));
		return true;
	};
	QVERIFY(rig.operations.pause(rig.context(), QStringLiteral("session")));
	QTRY_COMPARE_WITH_TIMEOUT(rig.posts().size(), 1, 2000);
	QTRY_VERIFY_WITH_TIMEOUT(rig.state.operations().current->receipt &&
					 rig.state.operations().current->receipt->status == QStringLiteral("SUCCEEDED"),
				 2000);
	QTRY_VERIFY_WITH_TIMEOUT(rig.finished(), 2000);
	QCOMPARE(rig.state.operations().current->outcome, OperationOutcome::Unknown);
	QVERIFY(!rig.state.operations().current->stateConfirmed);
	QCOMPARE(rig.posts().size(), 1);
}

void RestreamOperationsTest::manualRetryKeepsRequestAndPayload()
{
	// rig принимает первую команду на сервере, затем обрывает сокет до HTTP-ответа.
	OperationRig rig({500, 5, 20});
	QVERIFY(rig.initialize());
	rig.intercept = [&](const auto &request) {
		if (request.method != "POST")
			return false;
		rig.accept(request, false);
		request.socket->abort();
		return true;
	};
	QVERIFY(rig.operations.pause(rig.context(), QStringLiteral("session")));
	QTRY_VERIFY_WITH_TIMEOUT(rig.finished(), 2000);
	QCOMPARE(rig.state.operations().current->outcome, OperationOutcome::Unknown);
	QVERIFY(rig.state.operations().current->retryAvailable);
	QCOMPARE(rig.posts().size(), 1);
	// original содержит фактические неизменяемые байты потерянного POST.
	const auto original = rig.posts().first();
	rig.intercept = {};
	QVERIFY(rig.operations.retryUnknown(rig.context()));
	QTRY_VERIFY_WITH_TIMEOUT(rig.finished(), 2000);
	QCOMPARE(rig.state.operations().current->outcome, OperationOutcome::Completed);
	QCOMPARE(rig.posts().size(), 2);
	QCOMPARE(rig.posts()[1].target, original.target);
	QCOMPARE(rig.posts()[1].body, original.body);
	QCOMPARE(rig.receipts.size(), 1);
}

void RestreamOperationsTest::outputActions_data()
{
	QTest::addColumn<int>("action");
	QTest::addColumn<QByteArray>("suffix");
	QTest::newRow("stop-runtime-removed") << int(OperationAction::StopOutput) << QByteArray("/stop");
	QTest::newRow("detach-runtime-removed") << int(OperationAction::DetachOutput) << QByteArray("/detach");
	QTest::newRow("restart") << int(OperationAction::RestartOutput) << QByteArray("/stop");
}

void RestreamOperationsTest::outputActions()
{
	// action задаёт выбранное явное действие из строки данных.
	QFETCH(int, action);
	// suffix задаёт ожидаемое окончание маршрута первого шага Output.
	QFETCH(QByteArray, suffix);
	// rig возвращает runtime=null даже после очистки; оракулом служат attached/desired_running.
	OperationRig rig;
	QVERIFY(rig.initialize());
	QVERIFY(rig.operations.outputActions(rig.context(), QStringLiteral("session"), QStringLiteral("one"),
					     OperationAction(action)));
	QTRY_VERIFY_WITH_TIMEOUT(rig.finished(), 2000);
	QCOMPARE(rig.state.operations().current->outcome, OperationOutcome::Completed);
	QVERIFY(rig.state.operations().current->stateConfirmed);
	QCOMPARE(rig.posts().size(), OperationAction(action) == OperationAction::RestartOutput ? 2 : 1);
	QCOMPARE(rig.posts().first().target, QByteArray("/api/v1/sessions/session/outputs/one") + suffix);
	if (OperationAction(action) == OperationAction::RestartOutput) {
		QCOMPARE(rig.posts().size(), 2);
		QCOMPARE(rig.posts()[1].target, QByteArray("/api/v1/sessions/session/outputs/one/start"));
		QVERIFY(rig.posts()[0].body != rig.posts()[1].body);
		// stopReceipt — первая самостоятельная квитанция, которую нельзя переиспользовать как start.
		const auto stopReceipt = rig.receipts.value(QStringLiteral("command-1"));
		QCOMPARE(stopReceipt.value(QStringLiteral("kind")).toString(), QStringLiteral("stop_output"));
		QCOMPARE(rig.receipts.value(QStringLiteral("command-2")).value(QStringLiteral("kind")).toString(),
			 QStringLiteral("start_output"));
	} else {
		QCOMPARE(rig.posts().size(), 1);
	}
}

void RestreamOperationsTest::batchReportsPerElementFailure()
{
	// Второй Output отвергается самостоятельной терминальной квитанцией после успешного первого restart.
	OperationRig rig;
	QVERIFY(rig.initialize());
	rig.intercept = [&](const auto &request) {
		if (!request.target.startsWith("/api/v1/commands/"))
			return false;
		// id/receipt выбирают точную принятую команду; отказ не меняет её Output или request_id.
		const auto id = QUrl::fromEncoded("http://local" + request.target).path().section('/', -1);
		auto receipt = rig.receipts.value(id);
		receipt.insert(QStringLiteral("status"),
			       receipt.value(QStringLiteral("output_id")) == QStringLiteral("two")
				       ? QStringLiteral("FAILED")
				       : QStringLiteral("SUCCEEDED"));
		HttpServer::reply(request, fixture::bytes(receipt));
		return true;
	};
	QVERIFY(rig.operations.restartAll(rig.context(), QStringLiteral("session"),
					  {QStringLiteral("one"), QStringLiteral("two")}));
	QTRY_VERIFY_WITH_TIMEOUT(rig.finished(), 2000);
	QVERIFY(rig.state.operations().current->outcome != OperationOutcome::Completed);
	QCOMPARE(rig.state.operations().current->outputs.size(), 2);
	QCOMPARE(rig.state.operations().current->outputs[0].outputId, QStringLiteral("one"));
	QCOMPARE(rig.state.operations().current->outputs[0].outcome, OperationOutcome::Completed);
	QCOMPARE(rig.state.operations().current->outputs[1].outputId, QStringLiteral("two"));
	QCOMPARE(rig.state.operations().current->outputs[1].outcome, OperationOutcome::Failed);
	QCOMPARE(rig.posts().size(), 3);
}

void RestreamOperationsTest::lossFreezesBatchButReceiptRemainsReadable()
{
	// Пакет теряет SSE после принятия stop первого Output; command API остаётся самостоятельным.
	OperationRig rig;
	QVERIFY(rig.initialize());
	// commandReads/stateReads подтверждают возможность дочитать принятый шаг без наблюдателя.
	int commandReads = 0;
	int stateReads = 0;
	rig.intercept = [&](const auto &request) {
		if (request.target.startsWith("/api/v1/commands/")) {
			++commandReads;
			rig.observer.cancelAll();
			rig.state.setConnection(rig.server.origin().toString(), ConnectionStatus::Reconnecting);
		}
		if (commandReads > 0 && request.target == "/api/v1/sessions/session/outputs/one")
			++stateReads;
		return false;
	};
	QVERIFY(rig.operations.restartAll(rig.context(), QStringLiteral("session"),
					  {QStringLiteral("one"), QStringLiteral("two")}));
	QTRY_VERIFY_WITH_TIMEOUT(rig.finished(), 2000);
	QVERIFY(commandReads > 0);
	QVERIFY(stateReads > 0);
	QCOMPARE(rig.posts().size(), 1);
	QVERIFY(rig.state.operations().current->remainingStopped);
	QVERIFY(rig.state.operations().current->outcome != OperationOutcome::Completed);
	QCOMPARE(rig.state.operations().current->outputs[1].outcome, OperationOutcome::NotSent);
}

void RestreamOperationsTest::sessionChangeFreezesBatch()
{
	// Следующая Session появляется после принятия первого шага; старый пакет сохраняет первоначальную цель.
	OperationRig rig;
	QVERIFY(rig.initialize());
	rig.intercept = [&](const auto &request) {
		if (!request.target.startsWith("/api/v1/commands/"))
			return false;
		// metadata представляет подтверждённый следующий эфир с другим Session ID.
		auto metadata = rig.state.dockServer();
		metadata.session = DockSession{QStringLiteral("next-session"), QStringLiteral("RUNNING"), false};
		rig.state.setDockServer(metadata);
		return false;
	};
	QVERIFY(rig.operations.restartAll(rig.context(), QStringLiteral("session"),
					  {QStringLiteral("one"), QStringLiteral("two")}));
	QTRY_VERIFY_WITH_TIMEOUT(rig.finished(), 2000);
	QCOMPARE(rig.posts().size(), 1);
	QCOMPARE(rig.state.dockServer().session->id, QStringLiteral("next-session"));
	QVERIFY(rig.state.operations().current->outcome != OperationOutcome::Completed);
	QCOMPARE(rig.state.operations().current->outputs[1].outcome, OperationOutcome::NotSent);
	QCOMPARE(rig.state.operations().current->sessionId, QStringLiteral("session"));
}

void RestreamOperationsTest::stopAllKeepsObsAndServerResultsSeparate_data()
{
	QTest::addColumn<bool>("obsConfirmed");
	QTest::addColumn<bool>("nextSession");
	QTest::newRow("both-confirmed") << true << false;
	QTest::newRow("obs-refused") << false << false;
	QTest::newRow("new-session") << true << true;
}

void RestreamOperationsTest::stopAllKeepsObsAndServerResultsSeparate()
{
	// obsConfirmed задаёт возможность отдельно подтвердить локальную остановку OBS.
	QFETCH(bool, obsConfirmed);
	// nextSession задаёт обнаружение следующего эфира во время завершения исходного.
	QFETCH(bool, nextSession);
	// Локальная и серверная части подтверждаются независимо; следующая Session не является целью старого stop.
	OperationRig rig;
	QVERIFY(rig.initialize());
	rig.frontend.streaming = true;
	rig.obs.handleEvent(FrontendEvent::StreamingStarting);
	rig.obs.handleEvent(FrontendEvent::StreamingStarted);
	// Callback адаптера отдельно доставляет Stopped, когда остановка действительно разрешена сценарием.
	if (obsConfirmed)
		rig.frontend.onStopRequest = [&] {
			rig.frontend.streaming = false;
			rig.obs.handleEvent(FrontendEvent::StreamingStopped);
		};
	// Наблюдатель может обнаружить следующий эфир во время чтения принятой старой квитанции.
	if (nextSession)
		rig.intercept = [&](const auto &request) {
			if (request.target.startsWith("/api/v1/commands/")) {
				// metadata содержит новую Session с теми же Output IDs, не меняя адрес старой команды.
				auto metadata = rig.state.dockServer();
				metadata.session =
					DockSession{QStringLiteral("next-session"), QStringLiteral("RUNNING"), false};
				rig.state.setDockServer(metadata);
			}
			return false;
		};
	QVERIFY(rig.operations.stopAll(rig.context(), QStringLiteral("session")));
	QTRY_VERIFY_WITH_TIMEOUT(rig.finished(), 2000);
	QCOMPARE(rig.frontend.stopRequests, 1);
	QCOMPARE(rig.posts().size(), 1);
	QCOMPARE(rig.posts().first().target, QByteArray("/api/v1/sessions/session/stop"));
	QCOMPARE(rig.state.operations().current->serverOutcome, OperationOutcome::Completed);
	QCOMPARE(rig.state.operations().current->obsOutcome == OperationOutcome::Completed, obsConfirmed);
	QCOMPARE(rig.state.operations().current->outcome == OperationOutcome::Completed, obsConfirmed && !nextSession);
	QCOMPARE(rig.frontend.streaming, !obsConfirmed);
	if (nextSession)
		QCOMPARE(rig.state.dockServer().session->id, QStringLiteral("next-session"));
}

void RestreamOperationsTest::fallbackPrecheckAndStart()
{
	// Начальная Fresh+complete пустота позволяет только явный предварительный запуск; подходящий вариант находится на второй странице.
	OperationRig rig;
	QVERIFY(rig.initialize(false));
	rig.paginateVariants = true;
	QVERIFY(rig.operations.checkFallback(rig.context()));
	QTRY_VERIFY_WITH_TIMEOUT(rig.state.operations().fallback.readiness != FallbackReadiness::Checking, 2000);
	QCOMPARE(rig.state.operations().fallback.readiness, FallbackReadiness::Ready);
	QCOMPARE(rig.state.operations().fallback.generationId, QStringLiteral("current"));
	QVERIFY(rig.state.operations().fallback.rebuildWarning);
	QCOMPARE(rig.state.operations().fallback.destinations.size(), 1);
	QCOMPARE(rig.posts().size(), 0);
	// precheckRevision фиксирует одобренный набор; последующие изменения не должны его перенаправить.
	const auto precheckRevision = rig.state.operations().fallback.revision;
	QVERIFY(rig.operations.startFallback(rig.context(), precheckRevision));
	QTRY_VERIFY_WITH_TIMEOUT(rig.finished(), 2000);
	QCOMPARE(rig.state.operations().current->outcome, OperationOutcome::Completed);
	QCOMPARE(rig.posts().size(), 1);
	QCOMPARE(rig.posts().first().target, QByteArray("/api/v1/streams/stream/start"));
	QCOMPARE(QJsonDocument::fromJson(rig.posts().first().body)
			 .object()
			 .value(QStringLiteral("destination_ids"))
			 .toArray(),
		 QJsonArray{QStringLiteral("destination")});
	QVERIFY(!rig.frontend.streaming);
	QCOMPARE(rig.frontend.stopRequests, 0);
	// request — реальный запрос проверки готовности; pending не заменяет current ни в пути, ни в query вариантов.
	for (const auto &request : rig.server.requests) {
		QVERIFY(!request.target.contains("/generations/pending"));
		if (request.target.startsWith("/api/v1/fallback-assets/asset/variants"))
			QCOMPARE(QUrlQuery(QUrl::fromEncoded("http://local" + request.target))
					 .queryItemValue(QStringLiteral("generation_id")),
				 QStringLiteral("current"));
	}
}

void RestreamOperationsTest::fallbackRejectsMissingCompatibility_data()
{
	QTest::addColumn<int>("difference");
	QTest::newRow("no-enabled-destinations") << 0;
	QTest::newRow("live-audio-is-not-legacy") << 1;
	QTest::newRow("encoding-v2-is-not-null") << 2;
	QTest::newRow("wrong-generation") << 3;
}

void RestreamOperationsTest::fallbackRejectsMissingCompatibility()
{
	// difference выбирает независимое отличие назначений, профиля или поколения заглушки.
	QFETCH(int, difference);
	// rig содержит подходящий наследуемый профиль, которому сценарий противопоставляет одно реальное отличие.
	OperationRig rig;
	QVERIFY(rig.initialize(false));
	if (difference == 0)
		rig.enabled = false;
	if (difference == 1) {
		rig.variantProfile = fixture::groupProfile();
		rig.variantProfile.insert(QStringLiteral("live_audio_mode"), QStringLiteral("copy"));
	}
	if (difference == 2) {
		rig.variantProfile = fixture::groupProfile();
		rig.variantProfile.insert(QStringLiteral("encoding"), fixture::encoding());
	}
	if (difference == 3) {
		rig.intercept = [&](const auto &request) {
			if (!request.target.startsWith("/api/v1/fallback-assets/asset/variants"))
				return false;
			// variant несовместим по поколению, хотя его профиль и READY подходят.
			auto variant = fixture::variant();
			variant.insert(QStringLiteral("generation_id"), QStringLiteral("pending"));
			HttpServer::reply(request, fixture::page({variant}));
			return true;
		};
	}
	QVERIFY(rig.operations.checkFallback(rig.context()));
	QTRY_VERIFY_WITH_TIMEOUT(rig.state.operations().fallback.readiness != FallbackReadiness::Checking, 2000);
	QVERIFY(rig.state.operations().fallback.readiness != FallbackReadiness::Ready);
	QVERIFY(!rig.operations.startFallback(rig.context(), rig.state.operations().fallback.revision));
	QCOMPARE(rig.posts().size(), 0);
}

void RestreamOperationsTest::contextChangeAndRestartDoNotReplay()
{
	// Принятая команда теряет ответ; изменение логического контекста не переносит её в новый Stream.
	OperationRig rig;
	QVERIFY(rig.initialize());
	rig.intercept = [&](const auto &request) {
		if (request.method != "POST")
			return false;
		rig.accept(request, false);
		return true;
	};
	QVERIFY(rig.operations.pause(rig.context(), QStringLiteral("session")));
	QTRY_COMPARE_WITH_TIMEOUT(rig.posts().size(), 1, 2000);
	// originalContext фиксирует прежний выбор, к которому относится потерянный POST.
	const auto originalContext = rig.context();
	rig.state.setSelection(QStringLiteral("other-stream"), true, {}, {}, QStringLiteral("profile-uuid"));
	QVERIFY(!rig.operations.retryUnknown(originalContext));
	rig.operations.shutdown();
	// freshState/freshCommand/newRuntime моделируют новый процесс без параметров прежнего намерения в RAM.
	StateStore freshState;
	APIClient freshCommand;
	RestreamController newRuntime(freshState, freshCommand, rig.connection, rig.obs);
	QVERIFY(!freshState.operations().current);
	QVERIFY(!freshState.operations().lastInterrupted);
	QCOMPARE(rig.posts().size(), 1);
	QVERIFY(!newRuntime.retryUnknown(originalContext));
	newRuntime.shutdown();
}

QTEST_MAIN(RestreamOperationsTest)
#include "restream-operations-test.moc"

void RestreamOperationsTest::lostStartRetryAfterSessionAppears()
{
	// rig принимает предварительный запуск, но теряет ответ; новое наблюдение Session не является новым намерением.
	OperationRig rig;
	QVERIFY(rig.initialize(false));
	QVERIFY(rig.operations.checkFallback(rig.context()));
	QTRY_COMPARE_WITH_TIMEOUT(rig.state.operations().fallback.readiness, FallbackReadiness::Ready, 2000);
	rig.intercept = [&](const auto &request) {
		if (request.method != "POST")
			return false;
		rig.accept(request, false);
		request.socket->abort();
		return true;
	};
	QVERIFY(rig.operations.startFallback(rig.context(), rig.state.operations().fallback.revision));
	QTRY_COMPARE_WITH_TIMEOUT(rig.posts().size(), 1, 2000);
	QTRY_VERIFY_WITH_TIMEOUT(rig.finished(), 2000);
	QCOMPARE(rig.state.operations().current->outcome, OperationOutcome::Unknown);
	// original фиксирует принятый start, metadata имитирует появившуюся Session в состоянии RUNNING.
	const auto original = rig.posts().first();
	auto metadata = rig.state.dockServer();
	metadata.session = DockSession{QStringLiteral("session"), QStringLiteral("RUNNING"), false};
	rig.state.setDockServer(metadata);
	rig.intercept = {};
	QVERIFY(rig.operations.retryUnknown(rig.context()));
	QTRY_VERIFY_WITH_TIMEOUT(rig.finished(), 2000);
	QCOMPARE(rig.state.operations().current->outcome, OperationOutcome::Completed);
	QCOMPARE(rig.posts().size(), 2);
	QCOMPARE(rig.posts()[1].body, original.body);
	QCOMPARE(rig.posts()[1].target, original.target);
	QCOMPARE(rig.receipts.size(), 1);
	QVERIFY(!rig.frontend.streaming);
}

void RestreamOperationsTest::conflictingStartDoesNotCreateAnotherIntent()
{
	// rig сообщает конфликт с публикацией OBS; принятой собственной команды у клиента нет.
	OperationRig rig;
	QVERIFY(rig.initialize(false));
	QVERIFY(rig.operations.checkFallback(rig.context()));
	QTRY_COMPARE_WITH_TIMEOUT(rig.state.operations().fallback.readiness, FallbackReadiness::Ready, 2000);
	rig.intercept = [&](const auto &request) {
		if (request.method != "POST")
			return false;
		HttpServer::reply(
			request,
			R"({"type":"about:blank","title":"Conflict","status":409,"detail":"Session already exists"})",
			409, "application/problem+json");
		return true;
	};
	QVERIFY(rig.operations.startFallback(rig.context(), rig.state.operations().fallback.revision));
	QTRY_VERIFY_WITH_TIMEOUT(rig.finished(), 2000);
	QCOMPARE(rig.state.operations().current->outcome, OperationOutcome::Failed);
	QVERIFY(!rig.state.operations().current->stateConfirmed);
	QVERIFY(!rig.state.operations().current->retryAvailable);
	// metadata показывает чужую Session в состоянии RUNNING после конфликта, не превращая её в успех собственного start.
	auto metadata = rig.state.dockServer();
	metadata.session = DockSession{QStringLiteral("obs-session"), QStringLiteral("RUNNING"), false};
	rig.state.setDockServer(metadata);
	QVERIFY(!rig.operations.startFallback(rig.context(), rig.state.operations().fallback.revision));
	QCOMPARE(rig.posts().size(), 1);
}

void RestreamOperationsTest::outputConfirmationKeepsIdentity_data()
{
	QTest::addColumn<bool>("changeSession");
	QTest::newRow("selection-reused") << false;
	QTest::newRow("session-changed") << true;
}

void RestreamOperationsTest::outputConfirmationKeepsIdentity()
{
	// changeSession выбирает смену Session вместо простого перевыбора строки внутри подтверждения.
	QFETCH(bool, changeSession);
	// rig предоставляет два настоящих Output с разными идентификаторами; window получает общий фасад.
	OperationRig rig;
	QVERIFY(rig.initialize());
	// window отображает общий результат; переводчик возвращает ключ, позволяя проверять маршрутизацию без локализации.
	ManagementWindow window(
		rig.state, [](const char *key) { return QString::fromUtf8(key); }, nullptr, &rig.connection,
		&rig.operations);
	window.selectOutput(rig.context(), QStringLiteral("session"), QStringLiteral("two"));
	window.show();
	// button — явный restart выбранной второй строки, который открывает модальное подтверждение.
	auto *button = window.findChild<QPushButton *>(QStringLiteral("outputRestartButton"));
	QVERIFY(button);
	QVERIFY(button->isEnabled());
	// affirmativeClicked подтверждает реальное нажатие стандартной кнопки Yes, а не простое закрытие диалога.
	bool affirmativeClicked = false;
	// Диалог запускает вложенный цикл событий; callback меняет цель UI и подтверждает исходный вопрос.
	QTimer::singleShot(0, &window, [&] {
		if (changeSession) {
			// metadata относится уже к следующему эфиру, хотя Output IDs могут совпадать.
			auto metadata = rig.state.dockServer();
			metadata.session =
				DockSession{QStringLiteral("next-session"), QStringLiteral("RUNNING"), false};
			rig.state.setDockServer(metadata);
		} else {
			window.selectOutput(rig.context(), QStringLiteral("session"), QStringLiteral("one"));
		}
		// widget проходит только окна отдельного тестового QApplication; box — открытое подтверждение.
		for (auto *widget : QApplication::topLevelWidgets()) {
			if (auto *box = qobject_cast<QMessageBox *>(widget)) {
				// yes адресует настоящую стандартную кнопку подтверждения, которую использует question().
				auto *yes = box->button(QMessageBox::Yes);
				if (yes && yes->isEnabled()) {
					affirmativeClicked = true;
					yes->click();
				} else {
					box->reject();
				}
			}
		}
	});
	button->click();
	QVERIFY(affirmativeClicked);
	if (changeSession) {
		QCOMPARE(rig.posts().size(), 0);
	} else {
		QTRY_VERIFY_WITH_TIMEOUT(rig.finished(), 2000);
		QCOMPARE(rig.posts().size(), 2);
		QCOMPARE(rig.posts()[0].target, QByteArray("/api/v1/sessions/session/outputs/two/stop"));
		QCOMPARE(rig.posts()[1].target, QByteArray("/api/v1/sessions/session/outputs/two/start"));
	}
}

void RestreamOperationsTest::closingManagementDoesNotCancelCommand()
{
	// rig удерживает GET принятой квитанции до закрытия окна.
	OperationRig rig;
	QVERIFY(rig.initialize());
	// held содержит заимствованный сокет отложенного GET квитанции.
	HttpServer::Request held;
	rig.intercept = [&](const auto &request) {
		if (!request.target.startsWith("/api/v1/commands/"))
			return false;
		held = request;
		return true;
	};
	// window отображает общий результат; переводчик возвращает ключ, позволяя проверять маршрутизацию без локализации.
	ManagementWindow window(
		rig.state, [](const char *key) { return QString::fromUtf8(key); }, nullptr, &rig.connection,
		&rig.operations);
	window.show();
	QVERIFY(rig.operations.pause(rig.context(), QStringLiteral("session")));
	QTRY_VERIFY_WITH_TIMEOUT(held.socket, 2000);
	window.close();
	QVERIFY(!window.isVisible());
	rig.intercept = {};
	rig.respond(held);
	QTRY_VERIFY_WITH_TIMEOUT(rig.finished(), 2000);
	QCOMPARE(rig.state.operations().current->outcome, OperationOutcome::Completed);
	QCOMPARE(rig.posts().size(), 1);
}

void RestreamOperationsTest::startStoppedKeepsFixedEligibleIds()
{
	// rig имеет один действительно остановленный Output и второй с желанием работы, даже если он RETRYING.
	OperationRig rig;
	QVERIFY(rig.initialize());
	// metadata фиксирует сохранённые намерения; имя второго выхода не влияет на его пригодность для start.
	auto metadata = rig.state.dockServer();
	metadata.outputs[QStringLiteral("one")].desiredRunning = false;
	rig.desired[QStringLiteral("one")] = false;
	rig.state.setDockServer(metadata);
	QVERIFY(!rig.operations.outputActions(rig.context(), QStringLiteral("session"), QStringLiteral("two"),
					      OperationAction::StartOutput));
	QCOMPARE(rig.posts().size(), 0);
	QVERIFY(rig.operations.startStopped(rig.context(), QStringLiteral("session"), {QStringLiteral("one")}));
	// Новая строка появляется после фиксации состава и не должна попасть в текущий пакет.
	metadata.outputs.insert(QStringLiteral("new-output"), {QStringLiteral("Новый"), true, false});
	rig.state.setDockServer(metadata);
	QTRY_VERIFY_WITH_TIMEOUT(rig.finished(), 2000);
	QCOMPARE(rig.state.operations().current->outcome, OperationOutcome::Completed);
	QCOMPARE(rig.posts().size(), 1);
	QCOMPARE(rig.posts()[0].target, QByteArray("/api/v1/sessions/session/outputs/one/start"));
	QCOMPARE(rig.state.operations().current->outputs.size(), 1);
}

void RestreamOperationsTest::manualRetryDoesNotResumeBatch()
{
	// rig принял stop первого Output, но потерял ответ до получения command_id клиентом.
	OperationRig rig;
	QVERIFY(rig.initialize());
	rig.intercept = [&](const auto &request) {
		if (request.method != "POST")
			return false;
		rig.accept(request, false);
		request.socket->abort();
		return true;
	};
	QVERIFY(rig.operations.restartAll(rig.context(), QStringLiteral("session"),
					  {QStringLiteral("one"), QStringLiteral("two")}));
	QTRY_COMPARE_WITH_TIMEOUT(rig.posts().size(), 1, 2000);
	QTRY_VERIFY_WITH_TIMEOUT(rig.finished(), 2000);
	QCOMPARE(rig.state.operations().current->outcome, OperationOutcome::Unknown);
	// original фиксирует единственный неизвестный шаг, который разрешено повторять.
	const auto original = rig.posts().first();
	rig.intercept = {};
	QVERIFY(rig.operations.retryUnknown(rig.context()));
	QTRY_VERIFY_WITH_TIMEOUT(rig.finished(), 2000);
	QCOMPARE(rig.posts().size(), 2);
	QCOMPARE(rig.posts()[1].body, original.body);
	QCOMPARE(rig.posts()[1].target, original.target);
	QVERIFY(rig.state.operations().current->remainingStopped);
	QVERIFY(rig.state.operations().current->outcome != OperationOutcome::Completed);
	QCOMPARE(rig.state.operations().current->outputs[1].outcome, OperationOutcome::NotSent);
}

void RestreamOperationsTest::manualRetryHasFreshBoundedBudget()
{
	// rig удерживает HTTP-ответ до истечения первого срока, затем выдаёт только промежуточные квитанции.
	OperationRig rig({500, 5, 20});
	QVERIFY(rig.initialize());
	rig.intercept = [&](const auto &request) {
		if (request.method != "POST")
			return false;
		rig.accept(request, false);
		return true;
	};
	QVERIFY(rig.operations.pause(rig.context(), QStringLiteral("session")));
	QTRY_COMPARE_WITH_TIMEOUT(rig.posts().size(), 1, 2000);
	QTRY_VERIFY_WITH_TIMEOUT(rig.finished(), 2000);
	QCOMPARE(rig.state.operations().current->outcome, OperationOutcome::Unknown);
	// firstRequest фиксирует исходный request_id; secondPolls подтверждает реальную работу новой попытки.
	const auto firstRequest = rig.posts().first().body;
	int secondPolls = 0;
	rig.statuses = {QStringLiteral("ISSUED")};
	rig.intercept = [&](const auto &request) {
		if (request.target.startsWith("/api/v1/commands/"))
			++secondPolls;
		return false;
	};
	QVERIFY(rig.operations.retryUnknown(rig.context()));
	QVERIFY(!rig.finished());
	QTRY_COMPARE_WITH_TIMEOUT(rig.posts().size(), 2, 2000);
	QTRY_VERIFY_WITH_TIMEOUT(secondPolls > 0, 2000);
	QTRY_VERIFY_WITH_TIMEOUT(rig.finished(), 2000);
	QVERIFY(secondPolls > 0);
	QCOMPARE(rig.state.operations().current->outcome, OperationOutcome::Unknown);
	QCOMPARE(rig.posts().size(), 2);
	QCOMPARE(rig.posts()[1].body, firstRequest);
}

void RestreamOperationsTest::passiveEventsNeverControlServer()
{
	// Начальное наблюдение RUNNING не даёт права посылать start/resume/stop.
	OperationRig rig;
	QVERIFY(rig.initialize());
	rig.frontend.streaming = true;
	rig.obs.handleEvent(FrontendEvent::StreamingStarted);
	rig.frontend.streaming = false;
	rig.obs.handleEvent(FrontendEvent::StreamingStopped);
	rig.state.setConnection(rig.server.origin().toString(), ConnectionStatus::Reconnecting);
	rig.state.setConnection(rig.server.origin().toString(), ConnectionStatus::Connected);
	QCOMPARE(rig.posts().size(), 0);
	QVERIFY(!rig.state.operations().current);
	QCOMPARE(rig.frontend.stopRequests, 0);
	rig.operations.shutdown();
	QCOMPARE(rig.posts().size(), 0);
	QCOMPARE(rig.frontend.stopRequests, 0);
}

void RestreamOperationsTest::fallbackReceiptDoesNotReplaceRunningSession()
{
	// Сервер принял собственный start, но точная Session остаётся STARTING при уже работающем OBS.
	OperationRig rig({1000, 5, 20});
	QVERIFY(rig.initialize(false));
	QVERIFY(rig.operations.checkFallback(rig.context()));
	QTRY_COMPARE_WITH_TIMEOUT(rig.state.operations().fallback.readiness, FallbackReadiness::Ready, 2000);
	rig.sessionState = QStringLiteral("STARTING");
	rig.frontend.streaming = true;
	rig.obs.handleEvent(FrontendEvent::StreamingStarted);
	QVERIFY(rig.operations.startFallback(rig.context(), rig.state.operations().fallback.revision));
	QTRY_COMPARE_WITH_TIMEOUT(rig.posts().size(), 1, 2000);
	QTRY_VERIFY_WITH_TIMEOUT(rig.state.operations().current->receipt &&
					 rig.state.operations().current->receipt->status == QStringLiteral("SUCCEEDED"),
				 2000);
	QTRY_VERIFY_WITH_TIMEOUT(rig.finished(), 2000);
	QCOMPARE(rig.state.operations().current->outcome, OperationOutcome::Unknown);
	QVERIFY(rig.state.operations().current->receipt);
	QCOMPARE(rig.state.operations().current->receipt->status, QStringLiteral("SUCCEEDED"));
	QVERIFY(!rig.state.operations().current->stateConfirmed);
	QCOMPARE(rig.posts().size(), 1);
}

void RestreamOperationsTest::fallbackRequiresConfirmedAbsence_data()
{
	QTest::addColumn<int>("boundary");
	QTest::newRow("unknown") << 0;
	QTest::newRow("incomplete") << 1;
	QTest::newRow("known-terminal") << 2;
}

void RestreamOperationsTest::fallbackRequiresConfirmedAbsence()
{
	// boundary выбирает проверяемую границу достоверности результата поиска Session.
	QFETCH(int, boundary);
	// Проверка fallback завершена в пригодном контексте, затем меняется независимая достоверность поиска Session.
	OperationRig rig;
	QVERIFY(rig.initialize(false));
	QVERIFY(rig.operations.checkFallback(rig.context()));
	QTRY_COMPARE_WITH_TIMEOUT(rig.state.operations().fallback.readiness, FallbackReadiness::Ready, 2000);
	// metadata задаёт неизвестность/неполноту либо известную завершённую Session.
	auto metadata = rig.state.dockServer();
	if (boundary == 0)
		metadata.sessionStatus.availability = RuntimeAvailability::Unknown;
	if (boundary == 1)
		metadata.sessionStatus.complete = false;
	if (boundary == 2)
		metadata.session = DockSession{QStringLiteral("old-session"), QStringLiteral("STOPPED"), false};
	rig.state.setDockServer(metadata);
	QCOMPARE(rig.operations.startFallback(rig.context(), rig.state.operations().fallback.revision), boundary == 2);
	if (boundary == 2) {
		QTRY_VERIFY_WITH_TIMEOUT(rig.finished(), 2000);
		QCOMPARE(rig.state.operations().current->outcome, OperationOutcome::Completed);
		QCOMPARE(rig.posts().size(), 1);
	} else {
		QCOMPARE(rig.posts().size(), 0);
	}
}

void RestreamOperationsTest::interruptedPrecheckDoesNotRemainChecking_data()
{
	QTest::addColumn<bool>("explicitDisconnect");
	QTest::newRow("stop-all-preempts") << false;
	QTest::newRow("explicit-disconnect") << true;
}

void RestreamOperationsTest::interruptedPrecheckDoesNotRemainChecking()
{
	// explicitDisconnect выбирает отключение вместо замещения проверки явным Stop All.
	QFETCH(bool, explicitDisconnect);
	// rig даёт живой проверке достаточно времени дойти до удерживаемого GET политики.
	OperationRig rig({1000, 5, 30});
	QVERIFY(rig.initialize());
	// held сохраняет реальный незавершённый GET, который мог бы поздно продолжить цепочку проверки.
	HttpServer::Request held;
	// Перехватчик сохраняет запрос политики request в held без ответа, оставляя прочие маршруты respond.
	rig.intercept = [&](const auto &request) {
		if (request.target != "/api/v1/streams/stream/fallback-policy")
			return false;
		held = request;
		return true;
	};
	QVERIFY(rig.operations.checkFallback(rig.context()));
	QTRY_VERIFY_WITH_TIMEOUT(held.socket, 2000);
	QCOMPARE(rig.state.operations().fallback.readiness, FallbackReadiness::Checking);
	if (explicitDisconnect)
		rig.connection.disconnectFromServer();
	else
		QVERIFY(rig.operations.stopAll(rig.context(), QStringLiteral("session")));
	QCOMPARE(rig.state.operations().fallback.readiness, FallbackReadiness::Unknown);
	QCOMPARE(rig.state.operations().fallback.messageKey, QStringLiteral("Fallback.Incomplete"));
	QVERIFY(rig.state.operations().lastInterrupted);
	QCOMPARE(rig.state.operations().lastInterrupted->action, OperationAction::CheckFallback);
	rig.intercept = {};
	rig.respond(held);
	if (!explicitDisconnect)
		QTRY_VERIFY_WITH_TIMEOUT(rig.finished(), 2000);
	// barrierApi выполняет независимый живой GET после выпуска старого ответа, не возобновляя отменённую последовательность.
	APIClient barrierApi;
	barrierApi.configure(rig.server.origin(), {});
	// barrier отмечает принятие корректного Stream; callback подтверждает прохождение цикла событий сети.
	bool barrier = false;
	barrierApi.getStream(QStringLiteral("stream"), this,
			     [&](RequestContext, Result<Stream> result) { barrier = bool(result); });
	QTRY_VERIFY_WITH_TIMEOUT(barrier, 2000);
	QCOMPARE(rig.state.operations().fallback.readiness, FallbackReadiness::Unknown);
	QCOMPARE(rig.state.operations().fallback.messageKey, QStringLiteral("Fallback.Incomplete"));
	QCOMPARE(rig.posts().size(), explicitDisconnect ? 0 : 1);
}
