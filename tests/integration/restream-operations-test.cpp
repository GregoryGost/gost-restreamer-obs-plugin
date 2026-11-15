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
#include <QSet>
#include <QEvent>
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

// Отвечает только на новые публичные маршруты прикрепления; остальные GET обслуживает OperationRig.
// Начальная квитанция уже содержит Output ID, а runtime намеренно недоступен.
class AddOutputScenario final {
public:
	// serverRig задаёт петлевую границу сервера, которая должна переживать объект сценария.
	explicit AddOutputScenario(OperationRig &serverRig) : rig(serverRig)
	{
		// Перехватчик независимо моделирует принятие и долговечный результат команды.
		rig.intercept = [this](const HttpServer::Request &request) {
			if (request.method == "GET" && request.target == "/api/v1/destinations/destination") {
				// destination выключен для автозапуска, но доступен для явного AddOutput.
				auto destination = fixture::destination();
				destination.insert(QStringLiteral("enabled"), false);
				HttpServer::reply(request, fixture::bytes(destination));
				return true;
			}
			if (request.method == "GET" && request.target.startsWith("/api/v1/sessions/session/outputs?")) {
				HttpServer::reply(request,
						  fixture::page(duplicateAttached ? QJsonArray{fixture::output()}
										  : QJsonArray{}));
				return true;
			}
			if (request.method == "POST" && request.target == "/api/v1/sessions/session/outputs") {
				// payload сохраняет исходное намерение; receipt остаётся одной записью при повторе того же request_id.
				const auto payload = QJsonDocument::fromJson(request.body).object();
				if (receipt.isEmpty())
					receipt = fixture::receipt(
						payload.value(QStringLiteral("request_id")).toString(),
						QStringLiteral("add_output"), QStringLiteral("session"),
						QStringLiteral("session"), QStringLiteral("created-output"),
						QStringLiteral("add-command"), QStringLiteral("PENDING"));
				if (loseFirstResponse && rig.posts().size() == 1) {
					if (request.socket)
						request.socket->abort();
					return true;
				}
				// initial отдельно изменяет начальную идентичность, не меняя сохранённую серверную квитанцию.
				auto initial = receipt;
				if (violation == QStringLiteral("initial-empty-output"))
					initial.insert(QStringLiteral("output_id"), QString{});
				if (violation == QStringLiteral("initial-wrong-target"))
					initial.insert(QStringLiteral("target_id"), QStringLiteral("created-output"));
				HttpServer::reply(request, fixture::bytes(initial), 202);
				return true;
			}
			if (request.target == "/api/v1/commands/add-command") {
				// terminal подтверждает именно эту команду, сохраняя либо намеренно нарушая её точные IDs.
				auto terminal = receipt;
				terminal.insert(QStringLiteral("status"), QStringLiteral("SUCCEEDED"));
				if (violation == QStringLiteral("changed-receipt-output"))
					terminal.insert(QStringLiteral("output_id"), QStringLiteral("other-output"));
				HttpServer::reply(request, fixture::bytes(terminal));
				return true;
			}
			if (request.target == "/api/v1/sessions/session/outputs/created-output") {
				// output содержит публичные снимки и прикреплённость; unavailable runtime не означает публикацию.
				auto output = fixture::output();
				output.insert(QStringLiteral("id"), QStringLiteral("created-output"));
				output.insert(QStringLiteral("runtime"), QJsonValue::Null);
				output.insert(QStringLiteral("runtime_availability"), QStringLiteral("unavailable"));
				output.insert(QStringLiteral("runtime_reason"), QStringLiteral("not_observed"));
				if (violation == QStringLiteral("durable-wrong-session"))
					output.insert(QStringLiteral("session_id"), QStringLiteral("other-session"));
				if (violation == QStringLiteral("durable-wrong-destination")) {
					// destination моделирует чужой сохранённый снимок при совпадающем ID самого Output.
					auto destination = fixture::destination();
					destination.insert(QStringLiteral("id"), QStringLiteral("other-destination"));
					output.insert(QStringLiteral("destination"), destination);
				}
				if (violation == QStringLiteral("not-attached"))
					output.insert(QStringLiteral("attached"), false);
				if (violation == QStringLiteral("not-desired"))
					output.insert(QStringLiteral("desired_running"), false);
				HttpServer::reply(request, fixture::bytes(output));
				return true;
			}
			return false;
		};
	}
	// Снимает перехватчик до уничтожения захваченного this; запросы принадлежат rig.
	~AddOutputScenario() { rig.intercept = {}; }
	// Заимствованный сервер и настоящий исполнитель операции.
	OperationRig &rig;
	// Один независимо выбранный дефект начальной квитанции или долговечного DTO.
	QString violation;
	// Истинное значение моделирует уже прикреплённое назначение до POST.
	bool duplicateAttached = false;
	// Первый POST принят, но его HTTP-ответ потерян.
	bool loseFirstResponse = false;
	// Единственная серверная квитанция сохраняет идентичность при ручном повторе.
	QJsonObject receipt;
};

// Моделирует ручной запуск одного Destination на петлевом сервере независимо от клиента.
// Долговечный Output не имеет runtime: успешный запуск не означает публикацию площадки.
class StartDestinationScenario final {
public:
	// serverRig переживает сценарий; перехватчик обслуживает только выбранное назначение и результат start.
	explicit StartDestinationScenario(OperationRig &serverRig) : rig(serverRig)
	{
		// metadata подтверждает свежий полный каталог выбранного назначения отдельно от приватного GET.
		auto metadata = rig.state.dockServer();
		metadata.destinationsStatus = {RuntimeAvailability::Fresh, true, {}};
		metadata.destinations.insert(QStringLiteral("destination"), {QStringLiteral("Назначение")});
		rig.state.setDockServer(metadata);
		// request сначала позволяет сценарию удержать ответ, затем получает независимый серверный DTO.
		rig.intercept = [this](const HttpServer::Request &request) {
			if (before && before(request))
				return true;
			return respond(request);
		};
	}
	// Удаляет callback с захваченным this до уничтожения сценарных значений.
	~StartDestinationScenario() { rig.intercept = {}; }
	// Создаёт сохранённый Output id; имена отличаются от текущей конфигурации и не участвуют в адресации.
	QJsonObject output(const QString &id = QStringLiteral("created-output")) const
	{
		// result сохраняет исторические связи, destination и group меняют только отображаемые имена.
		auto result = fixture::output();
		auto destination = fixture::destination();
		auto group = fixture::group();
		destination.insert(QStringLiteral("enabled"), false);
		destination.insert(QStringLiteral("name"), QStringLiteral("Сохранённое назначение"));
		group.insert(QStringLiteral("name"), QStringLiteral("Сохранённая группа"));
		result.insert(QStringLiteral("id"), id);
		result.insert(QStringLiteral("destination"), destination);
		result.insert(QStringLiteral("group"), group);
		result.insert(QStringLiteral("runtime"), QJsonValue::Null);
		result.insert(QStringLiteral("runtime_availability"), QStringLiteral("unavailable"));
		result.insert(QStringLiteral("runtime_reason"), QStringLiteral("not_observed"));
		return result;
	}
	// Обслуживает request по опубликованному API; false оставляет общие пригодные GET фикстуре OperationRig.
	bool respond(const HttpServer::Request &request)
	{
		// url и path выделяют адрес сущности, сохраняя query для проверки полноты обхода страниц.
		const auto url = QUrl::fromEncoded("http://local" + request.target);
		const auto path = url.path();
		if (request.method == "GET" && path == QStringLiteral("/api/v1/destinations/destination")) {
			// destination выключен для автозапуска, но явно выбран пользователем.
			auto destination = fixture::destination();
			destination.insert(QStringLiteral("enabled"), false);
			if (violation == QStringLiteral("destination-stream"))
				destination.insert(QStringLiteral("stream_id"), QStringLiteral("other-stream"));
			HttpServer::reply(request, fixture::bytes(destination));
			return true;
		}
		if (path == QStringLiteral("/api/v1/streams/stream/destinations")) {
			// selected и neighbor разделяют явный выбор и автоматический набор Enabled.
			auto selected = fixture::destination();
			auto neighbor = fixture::destination();
			selected.insert(QStringLiteral("enabled"), false);
			neighbor.insert(QStringLiteral("id"), QStringLiteral("neighbor"));
			neighbor.insert(QStringLiteral("group_id"), QStringLiteral("unprepared-group"));
			HttpServer::reply(request, fixture::page({selected, neighbor}));
			return true;
		}
		if (violation == QStringLiteral("no-policy") && path.endsWith(QStringLiteral("/fallback-policy"))) {
			HttpServer::reply(request, R"({"revision":"1","fallback":null})");
			return true;
		}
		if (violation == QStringLiteral("no-current") &&
		    path == QStringLiteral("/api/v1/fallback-assets/asset")) {
			// asset имеет только pending: это не опубликованное поколение.
			auto asset = fixture::asset();
			asset.insert(QStringLiteral("current_generation_id"), QString{});
			asset.insert(QStringLiteral("pending_generation_id"), QStringLiteral("pending"));
			HttpServer::reply(request, fixture::bytes(asset));
			return true;
		}
		if (path == QStringLiteral("/api/v1/fallback-assets/asset/variants") &&
		    (violation == QStringLiteral("pending-variant") ||
		     violation == QStringLiteral("partial-variants"))) {
			// variant пригоден по профилю, но принадлежит pending либо находится в неполном списке.
			auto variant = fixture::variant();
			variant.insert(QStringLiteral("generation_id"), violation == QStringLiteral("pending-variant")
										? QStringLiteral("pending")
										: QStringLiteral("current"));
			if (violation == QStringLiteral("partial-variants") &&
			    !QUrlQuery(url).queryItemValue(QStringLiteral("cursor")).isEmpty())
				HttpServer::reply(request, "{}", 503);
			else
				HttpServer::reply(request, fixture::page({variant},
									 violation == QStringLiteral("partial-variants")
										 ? QStringLiteral("next")
										 : QString{}));
			return true;
		}
		if (request.method == "POST" && path == QStringLiteral("/api/v1/streams/stream/start")) {
			if (conflict) {
				HttpServer::reply(request, R"({"type":"about:blank","title":"Conflict","status":409})",
						  409, "application/problem+json");
				return true;
			}
			rig.accept(request, !loseFirstResponse || rig.posts().size() != 1);
			if (loseFirstResponse && rig.posts().size() == 1 && request.socket)
				request.socket->abort();
			return true;
		}
		if (path.startsWith(QStringLiteral("/api/v1/commands/"))) {
			// receipt сохраняет исходные IDs кроме одного независимо проверяемого нарушения.
			auto receipt = rig.receipts.value(path.section('/', -1));
			receipt.insert(QStringLiteral("status"), QStringLiteral("SUCCEEDED"));
			if (violation == QStringLiteral("receipt-session"))
				receipt.insert(QStringLiteral("session_id"), QStringLiteral("other-session"));
			if (violation == QStringLiteral("receipt-request"))
				receipt.insert(QStringLiteral("request_id"), QStringLiteral("other-request"));
			if (violation == QStringLiteral("receipt-command"))
				receipt.insert(QStringLiteral("id"), QStringLiteral("other-command"));
			if (violation == QStringLiteral("receipt-kind"))
				receipt.insert(QStringLiteral("kind"), QStringLiteral("add_output"));
			if (violation == QStringLiteral("receipt-target"))
				receipt.insert(QStringLiteral("target_id"), QStringLiteral("other-stream"));
			if (violation == QStringLiteral("receipt-failed"))
				receipt.insert(QStringLiteral("status"), QStringLiteral("FAILED"));
			HttpServer::reply(request, fixture::bytes(receipt));
			return true;
		}
		if (path == QStringLiteral("/api/v1/sessions/session") &&
		    violation.startsWith(QStringLiteral("session-"))) {
			// session проверяет верхний и вложенный Stream отдельно от статуса квитанции.
			auto session = fixture::session();
			if (violation == QStringLiteral("session-starting"))
				session.insert(QStringLiteral("state"), QStringLiteral("STARTING"));
			if (violation == QStringLiteral("session-stream"))
				session.insert(QStringLiteral("stream_id"), QStringLiteral("other-stream"));
			if (violation == QStringLiteral("session-snapshot-stream"))
				session.insert(QStringLiteral("stream"),
					       fixture::stream(QStringLiteral("other-stream")));
			HttpServer::reply(request, fixture::bytes(session));
			return true;
		}
		if (path == QStringLiteral("/api/v1/sessions/session/outputs")) {
			// cursor позволяет требовать все страницы, даже если подходящий Output уже найден на первой.
			const auto cursor = QUrlQuery(url).queryItemValue(QStringLiteral("cursor"));
			if (violation == QStringLiteral("partial-outputs") && !cursor.isEmpty()) {
				HttpServer::reply(request, "{}", 503);
				return true;
			}
			// outputs содержит выбранные записи; постороннее назначение ниже не создаёт неоднозначности выбора.
			QJsonArray outputs;
			if (violation != QStringLiteral("missing-output"))
				outputs.append(output());
			if (violation == QStringLiteral("duplicate-attached") ||
			    violation == QStringLiteral("duplicate-detached")) {
				// duplicate представляет отдельную историю того же Destination, включая detach перед re-add.
				auto duplicate = output(QStringLiteral("history-output"));
				duplicate.insert(QStringLiteral("attached"),
						 violation == QStringLiteral("duplicate-attached"));
				outputs.append(duplicate);
			}
			// unrelated и unrelatedDestination сохраняют другую допустимую серверную запись того же эфира.
			auto unrelated = output(QStringLiteral("unrelated-output"));
			auto unrelatedDestination = fixture::destination();
			unrelatedDestination.insert(QStringLiteral("id"), QStringLiteral("neighbor"));
			unrelated.insert(QStringLiteral("destination"), unrelatedDestination);
			outputs.append(unrelated);
			HttpServer::reply(request, fixture::page(outputs, violation == QStringLiteral("partial-outputs")
										  ? QStringLiteral("next")
										  : QString{}));
			return true;
		}
		if (path == QStringLiteral("/api/v1/sessions/session/outputs/created-output")) {
			// result — адресный снимок уже закреплённого ID; destination и group позволяют нарушать связи отдельно.
			auto result = output();
			auto destination = result.value(QStringLiteral("destination")).toObject();
			auto group = result.value(QStringLiteral("group")).toObject();
			if (violation == QStringLiteral("output-id"))
				result.insert(QStringLiteral("id"), QStringLiteral("replacement-output"));
			if (violation == QStringLiteral("output-session"))
				result.insert(QStringLiteral("session_id"), QStringLiteral("other-session"));
			if (violation == QStringLiteral("output-destination"))
				destination.insert(QStringLiteral("id"), QStringLiteral("other-destination"));
			if (violation == QStringLiteral("output-stream"))
				destination.insert(QStringLiteral("stream_id"), QStringLiteral("other-stream"));
			if (violation == QStringLiteral("output-group"))
				destination.insert(QStringLiteral("group_id"), QStringLiteral("other-group"));
			if (violation == QStringLiteral("group-stream"))
				group.insert(QStringLiteral("stream_id"), QStringLiteral("other-stream"));
			if (violation == QStringLiteral("not-attached"))
				result.insert(QStringLiteral("attached"), false);
			if (violation == QStringLiteral("not-desired"))
				result.insert(QStringLiteral("desired_running"), false);
			result.insert(QStringLiteral("destination"), destination);
			result.insert(QStringLiteral("group"), group);
			HttpServer::reply(request, fixture::bytes(result));
			return true;
		}
		return false;
	}
	// Заимствованный настоящий исполнитель и сервер; не обращается к реальному OBS или backend.
	OperationRig &rig;
	// Одно нарушение контракта на попытку сохраняет независимость отрицательных сценариев.
	QString violation;
	// before удерживает либо подменяет request до стандартного сценарного ответа.
	std::function<bool(const HttpServer::Request &)> before;
	// Первый принятый POST теряет HTTP-ответ для проверки явной идемпотентной попытки.
	bool loseFirstResponse = false;
	// Сервер отклоняет start из-за параллельной Session без принятия клиентской команды.
	bool conflict = false;
};

// Моделирует набор независимых площадок с общим эфиром; все DTO и квитанции создаёт серверная сторона.
// Сценарий не публикует частные GET в StateStore, поэтому проверяет продолжение при отставшем наблюдателе.
class VisibleBatchScenario final {
public:
	// serverRig предоставляет петлевой сервер; withSession задаёт исходный режим до фиксации списка.
	VisibleBatchScenario(OperationRig &serverRig, bool withSession = true) : rig(serverRig), fallback(serverRig)
	{
		// metadata содержит полный показанный каталог; сосед доступен серверу, но не включён в запрос пакета.
		auto metadata = rig.state.dockServer();
		metadata.outputs.clear();
		metadata.destinations.clear();
		metadata.destinationsStatus = {RuntimeAvailability::Fresh, true, {}};
		metadata.destinations.insert(QStringLiteral("destination"), {QStringLiteral("Первая площадка")});
		metadata.destinations.insert(QStringLiteral("second"), {QStringLiteral("Вторая площадка")});
		metadata.destinations.insert(QStringLiteral("third"), {QStringLiteral("Третья площадка")});
		metadata.destinations.insert(QStringLiteral("neighbor"), {QStringLiteral("Непоказанный сосед")});
		if (withSession) {
			outputs.insert(QStringLiteral("one"),
				       makeOutput(QStringLiteral("one"), QStringLiteral("destination"), true));
			outputs.insert(QStringLiteral("two"),
				       makeOutput(QStringLiteral("two"), QStringLiteral("second"), false));
			metadata.outputs.insert(QStringLiteral("one"), {QStringLiteral("Первое исполнение"), true, true,
									QStringLiteral("destination")});
			metadata.outputs.insert(QStringLiteral("two"), {QStringLiteral("Второе исполнение"), true,
									false, QStringLiteral("second")});
		}
		rig.state.setDockServer(metadata);
		// request проходит управляемое before, затем независимые DTO; общую проверку fallback обслуживает прежняя фикстура.
		rig.intercept = [this](const HttpServer::Request &request) {
			if (before && before(request))
				return true;
			return respond(request) || fallback.respond(request);
		};
	}
	// Удаляет callback с this до уничтожения значений сценария; rig переживает этот объект.
	~VisibleBatchScenario() { rig.intercept = {}; }
	// Возвращает исторический DTO Output id назначения destinationId с намерением desired, без runtime.
	QJsonObject makeOutput(const QString &id, const QString &destinationId, bool desired) const
	{
		// output и destination — независимые копии серверных снимков, их имена не участвуют в адресации.
		auto output = fixture::output();
		auto destination = fixture::destination();
		destination.insert(QStringLiteral("id"), destinationId);
		destination.insert(QStringLiteral("enabled"), false);
		output.insert(QStringLiteral("id"), id);
		output.insert(QStringLiteral("destination"), destination);
		output.insert(QStringLiteral("desired_running"), desired);
		output.insert(QStringLiteral("runtime"), QJsonValue::Null);
		output.insert(QStringLiteral("runtime_availability"), QStringLiteral("unavailable"));
		output.insert(QStringLiteral("runtime_reason"), QStringLiteral("not_observed"));
		return output;
	}
	// Копирует исходные targets и общий режим, не добавляя серверных соседей или последующие строки.
	OutputBatchRequest request(const QVector<OutputBatchTarget> &targets) const
	{
		return {rig.context(), rig.state.dockServer().session ? rig.state.dockServer().session->id : QString{},
			!rig.state.dockServer().session.has_value(), rig.state.configurationGeneration(), targets};
	}
	// Обслуживает request по публичным маршрутам; false передаёт общие чтения fallback в базовый сценарий.
	bool respond(const HttpServer::Request &request)
	{
		// path отделяет query полного списка от адреса сущности.
		const auto path = QUrl::fromEncoded("http://local" + request.target).path();
		if (request.method == "GET" && path.startsWith(QStringLiteral("/api/v1/destinations/"))) {
			// id задаёт точный Destination; неподготовленность вводится только явным сценарием отказа.
			const auto id = path.section('/', -1);
			if (unprepared.contains(id)) {
				HttpServer::reply(request, "{}", 404);
				return true;
			}
			// destination остаётся disabled: явная команда не должна изменять enabled.
			auto destination = fixture::destination();
			destination.insert(QStringLiteral("id"), id);
			destination.insert(QStringLiteral("enabled"), false);
			HttpServer::reply(request, fixture::bytes(destination));
			return true;
		}
		if (request.method == "GET" && path == QStringLiteral("/api/v1/sessions/session")) {
			// session подтверждает общий эфир отдельно от доступности первого Output.
			auto session = fixture::session();
			if (foreignSession)
				session.insert(QStringLiteral("stream"),
					       fixture::stream(QStringLiteral("foreign-stream")));
			HttpServer::reply(request, fixture::bytes(session));
			return true;
		}
		if (request.method == "GET" && path == QStringLiteral("/api/v1/sessions/session/outputs")) {
			// items содержит полную историю; omitBootstrapOutput скрывает только первый Output.
			QJsonArray items;
			// output перебирает серверные исполнения, не изменяя наблюдатель StateStore.
			for (const auto &output : outputs)
				if (!omitBootstrapOutput ||
				    output.value(QStringLiteral("id")) != QStringLiteral("bootstrap-output"))
					items.append(output);
			HttpServer::reply(request, fixture::page(items));
			return true;
		}
		if (request.method == "GET" && path.startsWith(QStringLiteral("/api/v1/sessions/session/outputs/"))) {
			HttpServer::reply(request, fixture::bytes(outputs.value(path.section('/', -1))),
					  outputs.contains(path.section('/', -1)) ? 200 : 404);
			return true;
		}
		if (request.method == "POST") {
			// payload содержит полученные байты намерения; id/kind/target описывают независимо выбранный сервером результат.
			const auto payload = QJsonDocument::fromJson(request.body).object();
			QString id;
			QString kind;
			QString target;
			if (path == QStringLiteral("/api/v1/streams/stream/start")) {
				if (bootstrapConflict) {
					HttpServer::reply(request,
							  R"({"type":"about:blank","title":"Conflict","status":409})",
							  409, "application/problem+json");
					return true;
				}
				if (payload.value(QStringLiteral("destination_ids")).toArray().isEmpty()) {
					HttpServer::reply(request, "{}", 400);
					return true;
				}
				id = QStringLiteral("bootstrap-output");
				kind = QStringLiteral("start_session");
				target = QStringLiteral("stream");
				outputs.insert(id, makeOutput(id,
							      payload.value(QStringLiteral("destination_ids"))
								      .toArray()
								      .first()
								      .toString(),
							      true));
			} else if (path == QStringLiteral("/api/v1/sessions/session/outputs")) {
				id = QStringLiteral("added-") +
				     payload.value(QStringLiteral("destination_id")).toString();
				kind = QStringLiteral("add_output");
				target = QStringLiteral("session");
				outputs.insert(
					id, makeOutput(id, payload.value(QStringLiteral("destination_id")).toString(),
						       true));
			} else if (path.startsWith(QStringLiteral("/api/v1/sessions/session/outputs/"))) {
				id = path.section('/', -2, -2);
				kind = path.section('/', -1) + QStringLiteral("_output");
				target = id;
				outputs[id].insert(QStringLiteral("desired_running"),
						   path.endsWith(QStringLiteral("/start")));
			} else {
				HttpServer::reply(request, "{}", 404);
				return true;
			}
			// commandId возвращается повторно при том же request_id, а не создаётся новая серверная команда.
			const auto commandId = payload.value(QStringLiteral("request_id")).toString();
			// receipt хранит ожидаемую API-идентичность, независимо от внутренних типов контроллера.
			auto receipt = fixture::receipt(commandId, kind, target, QStringLiteral("session"),
							kind == QStringLiteral("start_session") ? QString{} : id,
							commandId);
			receipts.insert(commandId, receipt);
			if (failedOutputs.contains(id))
				failedCommands.insert(commandId);
			if (lostOutputs.contains(id)) {
				if (request.socket)
					request.socket->abort();
			} else {
				HttpServer::reply(request, fixture::bytes(receipt), 202);
			}
			return true;
		}
		if (request.method == "GET" && path.startsWith(QStringLiteral("/api/v1/commands/"))) {
			// receipt связывает GET с принятым POST; failure содержит небезопасный серверный текст для проверки безопасного сообщения.
			auto receipt = receipts.value(path.section('/', -1));
			receipt.insert(QStringLiteral("status"), failedCommands.contains(path.section('/', -1))
									 ? QStringLiteral("FAILED")
									 : QStringLiteral("SUCCEEDED"));
			if (failedCommands.contains(path.section('/', -1)))
				receipt.insert(QStringLiteral("failure"),
					       QJsonObject{{QStringLiteral("code"), QStringLiteral("delivery_failed")},
							   {QStringLiteral("message"),
							    QStringLiteral("SECRET_SERVER_MESSAGE")}});
			HttpServer::reply(request, fixture::bytes(receipt));
			return true;
		}
		return false;
	}
	// Заимствованный сервер с настоящим RestreamController.
	OperationRig &rig;
	// DTO прежней предварительной проверки выбранной заглушки без нового транспорта.
	StartDestinationScenario fallback;
	// Полная серверная история Outputs, включая созданные и добавленные исполнения.
	QMap<QString, QJsonObject> outputs;
	// Собственные квитанции по command ID, равному синтетическому request ID.
	QMap<QString, QJsonObject> receipts;
	// ID назначений, чья приватная предварительная проверка определённо не проходит.
	QSet<QString> unprepared;
	// ID Outputs с терминально ошибочной квитанцией.
	QSet<QString> failedOutputs;
	// ID команд, чей терминальный статус является FAILED.
	QSet<QString> failedCommands;
	// ID Outputs с принятым POST и потерянным ответом.
	QSet<QString> lostOutputs;
	// true отклоняет создание из-за параллельного эфира, не принимая собственную команду.
	bool bootstrapConflict = false;
	// true нарушает вложенный Stream общей Session.
	bool foreignSession = false;
	// true оставляет первый Output неизвестным при доказанной общей RUNNING Session.
	bool omitBootstrapOutput = false;
	// before получает полный request до серверного ответа, позволяя детерминированно внедрять межшаговые события.
	std::function<bool(const HttpServer::Request &)> before;
};
} // namespace

// Проверяет команды по независимым серверным квитанциям, состояниям и записанным POST.
class RestreamOperationsTest final : public QObject {
	Q_OBJECT
private slots:
	// Отдельно останавливает два исторических Output одного Destination без схлопывания ID.
	void visibleBatchDoesNotCollapseExecutionsOfSameDestination();
	// Поздняя квитанция старой строки не меняет новую; явный Retry известного commandId использует GET.
	void visibleLateReceiptCannotCrossStepAndKnownRetryUsesGet();
	// Подтверждение Session, адресация и верхняя граница проверяются до первого сетевого побочного эффекта.
	void visibleBatchRejectsUnsafeAdmission_data();
	// Нельзя трактовать неизвестность или переходную Session как отсутствие и нельзя скрытно усекать состав.
	void visibleBatchRejectsUnsafeAdmission();
	// Новые конфигурационные снимки требуют исходной эпохи; исторический Output не зависит от каталога.
	void visibleBatchConfigurationEpochStopsNewSnapshotsOnly();
	// Потеря общей связи или замена Session останавливает новые строки; возврат связи не воспроизводит остаток.
	void visibleBatchInterruptionsDoNotResume_data();
	// Принятый stop дочитывается отдельно, но start того же restart и соседняя строка не отправляются.
	void visibleBatchInterruptionsDoNotResume();
	// Все три общие команды различают уже запрошенный запуск, остановленный Output и новое назначение.
	void visibleBatchCommandsRespectFrozenTargets_data();
	// Проверяет точные POST вместо одного общего флага успеха; сосед сервера в намерение не входит.
	void visibleBatchCommandsRespectFrozenTargets();
	// Полное свежее отсутствие Session превращает Stop Outputs в подтверждённый no-op без создания эфира.
	void visibleStopWithoutSessionDoesNotCreateOrStopAnything();
	// Определённый отказ предварительной проверки первой площадки не блокирует создание Session с первым пригодным назначением.
	void visibleBootstrapSkipsUnpreparedAndUsesOwnSessionWithLaggingCache();
	// Неизвестность первого Output не равна неизвестности общей собственной Session.
	void visibleBootstrapOutputUnknownAllowsIndependentAdd();
	// Общая неизвестность первого создания Session и чужой Stream общей Session закрывают независимый остаток.
	void visibleBootstrapUncertaintyStopsRemainder_data();
	// Retry принятого создания Session восстанавливает только прежний шаг и не прикрепляет неотправленный остаток.
	void visibleBootstrapUncertaintyStopsRemainder();
	// Отказ и потерянный ответ первой stop-команды не отменяют независимую вторую строку.
	void visibleRestartContinuesRowsWithoutStartingAfterUnconfirmedStop_data();
	// Долговечный desired=false сам по себе не заменяет успешную stop-квитанцию перед зависимым start.
	void visibleRestartContinuesRowsWithoutStartingAfterUnconfirmedStop();
	// Два неизвестных stop сохраняются независимо, а последнее удачное выполнение не подменяет общий итог.
	void visibleRetryChoosesFirstUnknownAndOnlyOneSavedStep();
	// Неадресуемая runtime-строка остаётся в исходном отчёте; успешный сосед не превращает весь пакет в Completed.
	void visibleUnaddressableRowRemainsInAggregate();
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
	// Явное прикрепление выключенного Destination подтверждается квитанцией и публичным снимком без runtime.
	void explicitAddOutputAcceptsDisabledDestinationWithoutClaimingPublication();
	// Задаёт нарушения точной идентичности и неполное долговечное состояние AddOutput.
	void addOutputRequiresExactReceiptAndDurableSnapshot_data();
	// Ни чужая квитанция, ни другой Output, ни отсутствие attached/desired не объявляются успехом.
	void addOutputRequiresExactReceiptAndDurableSnapshot();
	// Дублирующее attached назначение отклоняется до отправки команды.
	void addOutputRejectsAlreadyAttachedDestination();
	// Ручной повтор потерянного AddOutput сохраняет Session, Destination, request_id и payload.
	void lostAddOutputRetryKeepsExactIntent();
	// Смена Session не переносит явный дедуплицированный повтор старого прикрепления на новый эфир.
	void addOutputDoesNotMoveToReplacementSession();
	// Изменение конфигурации отзывает Ready и не допускает запоздалое восстановление старой проверкой.
	void configurationGenerationInvalidatesCompletedAndInFlightPrechecks();
	// Обновление пригодности ждёт живую команду и объединяет несколько запросов в одну проверку.
	void configurationRefreshDefersPrecheckUntilCommandFinishes();
	// Проверка, начатая во время записи или неизвестного её исхода, не разрешает предварительный запуск.
	void precheckCannotBecomeReadyWhileConfigurationNeedsConfirmation();
	// Запись, начатая во время повторного поиска активной Session, отзывает право первого start POST.
	void configurationWriteDuringSessionCheckPreventsFallbackPost();
	// Явный выбор disabled назначения запускается без OBS и без проверки неподготовленного соседа.
	void selectedDestinationStartsAloneAndPreservesLegacyPrecheck();
	// Задаёт отдельные нарушения пригодности выбранного назначения до первого POST.
	void selectedDestinationRequiresPreparedCurrentFallback_data();
	// Policy, current и полный совместимый вариант обязательны даже для явного одиночного запуска.
	void selectedDestinationRequiresPreparedCurrentFallback();
	// Задаёт повреждения собственной квитанции, Session и выбранного Output.
	void selectedDestinationRequiresOwnReceiptAndUniqueDurableOutput_data();
	// Успех не допускается при чужой идентичности, неоднозначной истории или неполном чтении.
	void selectedDestinationRequiresOwnReceiptAndUniqueDurableOutput();
	// После закрепления Output ID отсоединение и новый Output не перенаправляют проверку собственной команды.
	void selectedDestinationKeepsPinnedOutputAfterReplacement();
	// Задаёт границы подтверждения отсутствия Session и текущей конфигурации.
	void selectedDestinationRejectsUnsafeAdmission_data();
	// Неизвестность, переходная Session и незавершённая конфигурация запрещают новую команду.
	void selectedDestinationRejectsUnsafeAdmission();
	// Задаёт изменения после допуска, но до первого POST выбранного запуска.
	void selectedDestinationRechecksBeforeFirstPost_data();
	// Задержанный ответ не разрешает start после смены эпохи, контекста, связи или обнаружения Session.
	void selectedDestinationRechecksBeforeFirstPost();
	// Потерянный ответ повторяется теми же байтами после появления Session, без новой пригодности.
	void selectedDestinationRetryKeepsSingletonIntent();
	// Конфликт не переключает выбранный create в add и не принимает чужую Session за собственный успех.
	void selectedDestinationConflictDoesNotChangeBranch();
};

void RestreamOperationsTest::explicitAddOutputAcceptsDisabledDestinationWithoutClaimingPublication()
{
	// rig/scenario возвращают disabled Destination и отсутствие runtime при сохранённом новом Output.
	OperationRig rig({500, 5, 20});
	QVERIFY(rig.initialize());
	AddOutputScenario scenario(rig);
	QVERIFY(rig.operations.addOutput(rig.context(), QStringLiteral("session"), QStringLiteral("destination")));
	QTRY_VERIFY_WITH_TIMEOUT(rig.finished(), 2000);
	QCOMPARE(rig.state.operations().current->outcome, OperationOutcome::Completed);
	QVERIFY(rig.state.operations().current->stateConfirmed);
	QCOMPARE(rig.state.operations().current->outputId, QStringLiteral("created-output"));
	QCOMPARE(rig.posts().size(), 1);
	QCOMPARE(rig.posts().first().target, QByteArray("/api/v1/sessions/session/outputs"));
	QCOMPARE(QJsonDocument::fromJson(rig.posts().first().body)
			 .object()
			 .value(QStringLiteral("destination_id"))
			 .toString(),
		 QStringLiteral("destination"));
	QCOMPARE(rig.frontend.stopRequests, 0);
	QVERIFY(!rig.frontend.streaming);
}
void RestreamOperationsTest::addOutputRequiresExactReceiptAndDurableSnapshot_data()
{
	QTest::addColumn<QString>("violation");
	// violation меняет один контракт независимо от корректных остальных полей.
	for (const auto *violation :
	     {"initial-empty-output", "initial-wrong-target", "changed-receipt-output", "durable-wrong-session",
	      "durable-wrong-destination", "not-attached", "not-desired"})
		QTest::newRow(violation) << QString::fromLatin1(violation);
}
void RestreamOperationsTest::addOutputRequiresExactReceiptAndDurableSnapshot()
{
	// violation задаёт независимое нарушение ID или долговечного attached/desired состояния нового Output.
	QFETCH(QString, violation);
	// rig/scenario принимают команду, но не предоставляют доказательство нужного публичного результата.
	OperationRig rig({180, 5, 20});
	QVERIFY(rig.initialize());
	AddOutputScenario scenario(rig);
	scenario.violation = violation;
	QVERIFY(rig.operations.addOutput(rig.context(), QStringLiteral("session"), QStringLiteral("destination")));
	QTRY_VERIFY_WITH_TIMEOUT(rig.finished(), 1500);
	QCOMPARE(rig.state.operations().current->outcome, OperationOutcome::Unknown);
	QVERIFY(!rig.state.operations().current->stateConfirmed);
	QCOMPARE(rig.posts().size(), 1);
}
void RestreamOperationsTest::addOutputRejectsAlreadyAttachedDestination()
{
	// rig/scenario возвращают existing attached Output в полном списке до POST.
	OperationRig rig;
	QVERIFY(rig.initialize());
	AddOutputScenario scenario(rig);
	scenario.duplicateAttached = true;
	QVERIFY(rig.operations.addOutput(rig.context(), QStringLiteral("session"), QStringLiteral("destination")));
	QTRY_VERIFY_WITH_TIMEOUT(rig.finished(), 1000);
	QCOMPARE(rig.state.operations().current->outcome, OperationOutcome::Failed);
	QVERIFY(rig.posts().isEmpty());
}
void RestreamOperationsTest::lostAddOutputRetryKeepsExactIntent()
{
	// rig/scenario сохраняют принятую квитанцию, но теряют первый HTTP-ответ.
	OperationRig rig({500, 5, 20});
	QVERIFY(rig.initialize());
	AddOutputScenario scenario(rig);
	scenario.loseFirstResponse = true;
	QVERIFY(rig.operations.addOutput(rig.context(), QStringLiteral("session"), QStringLiteral("destination")));
	QTRY_VERIFY_WITH_TIMEOUT(rig.finished(), 1000);
	QCOMPARE(rig.state.operations().current->outcome, OperationOutcome::Unknown);
	QCOMPARE(rig.posts().size(), 1);
	// original фиксирует реальные байты первого намерения, а не заново собранный ожидаемый payload.
	const auto original = rig.posts().first();
	QVERIFY(rig.operations.retryUnknown(rig.context()));
	QTRY_VERIFY_WITH_TIMEOUT(rig.finished(), 1000);
	QCOMPARE(rig.state.operations().current->outcome, OperationOutcome::Completed);
	QCOMPARE(rig.posts().size(), 2);
	QCOMPARE(rig.posts().last().target, original.target);
	QCOMPARE(rig.posts().last().body, original.body);
}
void RestreamOperationsTest::addOutputDoesNotMoveToReplacementSession()
{
	// rig/scenario оставляют потерянное намерение для прежнего session ID.
	OperationRig rig;
	QVERIFY(rig.initialize());
	AddOutputScenario scenario(rig);
	scenario.loseFirstResponse = true;
	QVERIFY(rig.operations.addOutput(rig.context(), QStringLiteral("session"), QStringLiteral("destination")));
	QTRY_VERIFY_WITH_TIMEOUT(rig.finished(), 1000);
	// original содержит точную прежнюю цель и payload потерянного POST, включая request_id.
	const auto original = rig.posts().first();
	// replacement публикует следующий эфир того же Stream после неизвестного прежнего результата.
	auto replacement = rig.state.dockServer();
	replacement.session = DockSession{QStringLiteral("replacement-session"), QStringLiteral("RUNNING"), false};
	rig.state.setDockServer(replacement);
	QCOMPARE(rig.posts().size(), 1);
	QVERIFY(rig.operations.retryUnknown(rig.context()));
	QTRY_VERIFY_WITH_TIMEOUT(rig.finished(), 1000);
	QCOMPARE(rig.posts().size(), 2);
	QCOMPARE(rig.posts().last().target, original.target);
	QCOMPARE(rig.posts().last().body, original.body);
	QVERIFY(!rig.posts().last().target.contains("replacement-session"));
}
void RestreamOperationsTest::configurationGenerationInvalidatesCompletedAndInFlightPrechecks()
{
	// rig сначала получает Ready, затем начинает новую проверку, удерживая ответ из старой конфигурации.
	OperationRig rig({500, 5, 20});
	QVERIFY(rig.initialize(false));
	QVERIFY(rig.operations.checkFallback(rig.context()));
	QTRY_COMPARE_WITH_TIMEOUT(rig.state.operations().fallback.readiness, FallbackReadiness::Ready, 1000);
	// oldRevision — показанное пользователю подтверждение, которое теряет пригодность до записи.
	const auto oldRevision = rig.state.operations().fallback.revision;
	rig.state.invalidateConfiguration();
	QVERIFY(rig.state.operations().fallback.readiness != FallbackReadiness::Ready);
	QVERIFY(!rig.operations.startFallback(rig.context(), oldRevision));
	// held сохраняет GET fallback-policy, начатый до следующего изменения конфигурационного поколения.
	std::optional<HttpServer::Request> held;
	rig.intercept = [&](const HttpServer::Request &request) {
		if (request.target != "/api/v1/streams/stream/fallback-policy")
			return false;
		held = request;
		return true;
	};
	QVERIFY(rig.operations.checkFallback(rig.context()));
	QTRY_VERIFY_WITH_TIMEOUT(held.has_value(), 1000);
	rig.state.invalidateConfiguration();
	rig.intercept = {};
	rig.respond(*held);
	QTRY_VERIFY_WITH_TIMEOUT(rig.finished(), 1000);
	QVERIFY(rig.state.operations().fallback.readiness != FallbackReadiness::Ready);
	QVERIFY(rig.posts().isEmpty());
}
void RestreamOperationsTest::configurationRefreshDefersPrecheckUntilCommandFinishes()
{
	// rig удерживает GET квитанции Pause; deferred precheck не имеет права отменить этот живой шаг.
	OperationRig rig({800, 5, 20});
	QVERIFY(rig.initialize());
	// held удерживает квитанцию Pause; policyReads считает свежие проверки; pauseCompleted фиксирует успех до смены workflow.
	std::optional<HttpServer::Request> held;
	int policyReads = 0;
	bool pauseCompleted = false;
	// Сигнал фиксирует подтверждённый Pause до того, как единственная модель перейдёт к CheckFallback.
	connect(&rig.state, &StateStore::operationsChanged, this, [&] {
		if (rig.state.operations().current &&
		    rig.state.operations().current->action == OperationAction::Pause &&
		    rig.state.operations().current->outcome == OperationOutcome::Completed)
			pauseCompleted = true;
	});
	rig.intercept = [&](const HttpServer::Request &request) {
		if (request.target.startsWith("/api/v1/commands/")) {
			held = request;
			return true;
		}
		if (request.target == "/api/v1/streams/stream/fallback-policy")
			++policyReads;
		return false;
	};
	QVERIFY(rig.operations.pause(rig.context(), QStringLiteral("session")));
	QTRY_VERIFY_WITH_TIMEOUT(held.has_value(), 1000);
	rig.operations.refreshFallbackAfterConfiguration();
	rig.operations.refreshFallbackAfterConfiguration();
	QCOMPARE(policyReads, 0);
	// terminal завершает уже принятую Pause, сохраняя request/session/command identity.
	auto terminal = rig.receipts.first();
	terminal.insert(QStringLiteral("status"), QStringLiteral("SUCCEEDED"));
	HttpServer::reply(*held, fixture::bytes(terminal));
	QTRY_VERIFY_WITH_TIMEOUT(pauseCompleted, 1000);
	QTRY_COMPARE_WITH_TIMEOUT(rig.state.operations().fallback.readiness, FallbackReadiness::Ready, 1000);
	QCOMPARE(policyReads, 1);
	QCOMPARE(rig.posts().size(), 1);
}

void RestreamOperationsTest::precheckCannotBecomeReadyWhileConfigurationNeedsConfirmation()
{
	// rig задаёт полное отсутствие Session и пригодные серверные fixtures проверки fallback.
	OperationRig rig({500, 5, 20});
	QVERIFY(rig.initialize(false));
	// editor фиксирует текущую конфигурационную запись; её начало предшествует началу precheck.
	ConfigurationEditorState editor;
	editor.context = {rig.context().origin, rig.context().profileUuid, rig.context().streamId};
	editor.kind = ConfigurationKind::Destination;
	editor.id = QStringLiteral("destination");
	editor.writing = true;
	rig.state.setConfigurationEditor(editor);
	// startedDuringWrite допускает локальный отказ или ограниченную проверку, но никогда Ready.
	const bool startedDuringWrite = rig.operations.checkFallback(rig.context());
	if (startedDuringWrite)
		QTRY_VERIFY_WITH_TIMEOUT(rig.finished(), 1000);
	QVERIFY(rig.state.operations().fallback.readiness != FallbackReadiness::Ready);
	QVERIFY(!rig.operations.startFallback(rig.context(), rig.state.operations().fallback.revision));
	editor.writing = false;
	editor.outcome = ConfigurationOutcome::Unknown;
	editor.reconciliationRequired = true;
	rig.state.setConfigurationEditor(editor);
	// startedAfterUnknown проверяет тот же запрет после завершения HTTP с неопределённым исходом.
	const bool startedAfterUnknown = rig.operations.checkFallback(rig.context());
	if (startedAfterUnknown)
		QTRY_VERIFY_WITH_TIMEOUT(rig.finished(), 1000);
	QVERIFY(rig.state.operations().fallback.readiness != FallbackReadiness::Ready);
	QVERIFY(!rig.operations.startFallback(rig.context(), rig.state.operations().fallback.revision));
	QVERIFY(rig.posts().isEmpty());
	editor.reconciliationRequired = false;
	rig.state.setConfigurationEditor(editor);
	rig.operations.refreshFallbackAfterConfiguration();
	QTRY_COMPARE_WITH_TIMEOUT(rig.state.operations().fallback.readiness, FallbackReadiness::Ready, 1000);
	QVERIFY(rig.posts().isEmpty());
}

void RestreamOperationsTest::configurationWriteDuringSessionCheckPreventsFallbackPost()
{
	// rig предоставляет Ready до начала неизменяемого намерения предварительного запуска.
	OperationRig rig({500, 5, 20});
	QVERIFY(rig.initialize(false));
	QVERIFY(rig.operations.checkFallback(rig.context()));
	QTRY_COMPARE_WITH_TIMEOUT(rig.state.operations().fallback.readiness, FallbackReadiness::Ready, 1000);
	// revision фиксирует показанную пригодность; held удерживает последний GET поиска отсутствующей Session.
	const auto revision = rig.state.operations().fallback.revision;
	std::optional<HttpServer::Request> held;
	rig.intercept = [&](const HttpServer::Request &request) {
		// query проверяет именно финальное STOPPING-чтение без зависимости от порядка заголовков/фильтров.
		const QUrlQuery query(QUrl::fromEncoded("http://local" + request.target));
		if (request.target.startsWith("/api/v1/streams/stream/sessions?") &&
		    query.queryItemValue(QStringLiteral("state")) == QStringLiteral("STOPPING")) {
			held = request;
			return true;
		}
		return false;
	};
	QVERIFY(rig.operations.startFallback(rig.context(), revision));
	QTRY_VERIFY_WITH_TIMEOUT(held.has_value(), 1000);
	// editor сообщает о новой записи после admission StartFallback, но до его первого POST.
	ConfigurationEditorState editor;
	editor.context = {rig.context().origin, rig.context().profileUuid, rig.context().streamId};
	editor.writing = true;
	rig.state.setConfigurationEditor(editor);
	HttpServer::reply(*held, fixture::page({}));
	QTRY_VERIFY_WITH_TIMEOUT(rig.finished(), 1000);
	QVERIFY(rig.state.operations().current->outcome != OperationOutcome::Completed);
	QVERIFY(rig.posts().isEmpty());
}

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
	// window закрывается во время ожидания команды; переводчик возвращает ключи без зависимости от локализации.
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

// Контракт явного выбора допускает enabled=false; allEnabled-проверка сохраняет отдельную область результата.
void RestreamOperationsTest::selectedDestinationStartsAloneAndPreservesLegacyPrecheck()
{
	// rig и scenario создают один готовый disabled Destination и enabled-соседа без подготовленной группы.
	OperationRig rig({1000, 5, 20});
	QVERIFY(rig.initialize(false));
	StartDestinationScenario scenario(rig);
	QVERIFY(rig.operations.checkFallback(rig.context()));
	QTRY_VERIFY_WITH_TIMEOUT(rig.finished(), 2000);
	QVERIFY(rig.state.operations().fallback.readiness != FallbackReadiness::Ready);
	// legacy сохраняет публичное allEnabled-заключение до приватной выбранной проверки.
	const auto legacy = rig.state.operations().fallback;
	// startIndex отделяет запросы нового намерения от прежнего allEnabled-прохождения.
	const auto startIndex = rig.server.requests.size();
	rig.paginateVariants = true;
	QVERIFY(rig.operations.startDestination(rig.context(), QStringLiteral("destination")));
	QTRY_VERIFY_WITH_TIMEOUT(rig.finished(), 2000);
	QCOMPARE(rig.state.operations().current->action, OperationAction::StartDestination);
	QCOMPARE(rig.state.operations().current->outcome, OperationOutcome::Completed);
	QVERIFY(rig.state.operations().current->stateConfirmed);
	QCOMPARE(rig.state.operations().current->sessionId, QStringLiteral("session"));
	QCOMPARE(rig.state.operations().current->outputId, QStringLiteral("created-output"));
	QCOMPARE(rig.state.operations().fallback.revision, legacy.revision);
	QCOMPARE(rig.state.operations().fallback.readiness, legacy.readiness);
	QCOMPARE(rig.state.operations().fallback.messageKey, legacy.messageKey);
	QCOMPARE(rig.state.operations().fallback.generationId, legacy.generationId);
	QCOMPARE(rig.state.operations().fallback.destinations.size(), legacy.destinations.size());
	QCOMPARE(rig.posts().size(), 1);
	QCOMPARE(rig.posts().first().target, QByteArray("/api/v1/streams/stream/start"));
	// payload адресует только исходное назначение; отсутствие пустого списка защищает от запуска всех Enabled.
	const auto payload = QJsonDocument::fromJson(rig.posts().first().body).object();
	QCOMPARE(payload.value(QStringLiteral("destination_ids")).toArray(), QJsonArray{QStringLiteral("destination")});
	QVERIFY(!payload.value(QStringLiteral("request_id")).toString().isEmpty());
	// variantsRead и detailRead подтверждают полный preflight и отдельный адресный оракул, а не один receipt.
	bool variantsRead = false;
	bool detailRead = false;
	// index адресует только новые запросы; request сохраняет исходный метод и маршрут без нормализации.
	for (qsizetype index = startIndex; index < rig.server.requests.size(); ++index) {
		const auto &request = rig.server.requests.at(index);
		QVERIFY(request.method == "GET" || request.target == "/api/v1/streams/stream/start");
		QVERIFY(!request.target.contains("unprepared-group"));
		QVERIFY(!request.target.contains("/generations/pending"));
		QVERIFY(!request.target.startsWith("/api/v1/streams/stream/destinations"));
		if (request.target.startsWith("/api/v1/fallback-assets/asset/variants")) {
			QCOMPARE(QUrlQuery(QUrl::fromEncoded("http://local" + request.target))
					 .queryItemValue(QStringLiteral("generation_id")),
				 QStringLiteral("current"));
			variantsRead = variantsRead || !QUrlQuery(QUrl::fromEncoded("http://local" + request.target))
								.queryItemValue(QStringLiteral("cursor"))
								.isEmpty();
		}
		detailRead = detailRead || request.target == "/api/v1/sessions/session/outputs/created-output";
	}
	QVERIFY(variantsRead && detailRead);
	QVERIFY(rig.state.runtime().sessions.isEmpty());
	QVERIFY(!rig.frontend.streaming);
	QCOMPARE(rig.frontend.stopRequests, 0);
}

// Каждая строка данных нарушает один опубликованный допуск ручного запуска.
void RestreamOperationsTest::selectedDestinationRequiresPreparedCurrentFallback_data()
{
	QTest::addColumn<QString>("violation");
	// violation обозначает отсутствие policy/current, чужой Stream либо неполный/чужой вариант.
	for (const auto *violation : {"no-policy", "no-current", "destination-stream", "pending-variant",
				      "partial-variants", "incompatible-profile"})
		QTest::newRow(violation) << QString::fromLatin1(violation);
}

// Выбранная предварительная проверка не изменяет конфигурацию и не вызывает команды подготовки заглушки.
void RestreamOperationsTest::selectedDestinationRequiresPreparedCurrentFallback()
{
	// violation выбирает один независимый неподготовленный или несовместимый ресурс.
	QFETCH(QString, violation);
	// rig и scenario оставляют все прочие ресурсы пригодными, чтобы отказ нельзя было объяснить соседом.
	OperationRig rig({500, 5, 20});
	QVERIFY(rig.initialize(false));
	StartDestinationScenario scenario(rig);
	scenario.violation = violation;
	if (violation == QStringLiteral("incompatible-profile")) {
		rig.variantProfile = fixture::groupProfile();
		rig.variantProfile.insert(QStringLiteral("live_audio_mode"), QStringLiteral("copy"));
	}
	QVERIFY(rig.operations.startDestination(rig.context(), QStringLiteral("destination")));
	QTRY_VERIFY_WITH_TIMEOUT(rig.finished(), 2000);
	QVERIFY(rig.state.operations().current->outcome != OperationOutcome::Completed);
	QVERIFY(!rig.state.operations().current->stateConfirmed);
	QVERIFY(rig.posts().isEmpty());
	QVERIFY(!rig.frontend.streaming);
	QCOMPARE(rig.frontend.stopRequests, 0);
}

// Квитанция, собственная Session и долговечный Output — отдельные источники требуемых доказательств.
void RestreamOperationsTest::selectedDestinationRequiresOwnReceiptAndUniqueDurableOutput_data()
{
	QTest::addColumn<QString>("violation");
	// violation меняет ровно один идентификатор, статус, связь либо полноту списка.
	for (const auto *violation : {"receipt-session",    "receipt-request", "receipt-command",
				      "receipt-kind",       "receipt-target",  "receipt-failed",
				      "session-starting",   "session-stream",  "session-snapshot-stream",
				      "missing-output",     "partial-outputs", "duplicate-attached",
				      "duplicate-detached", "output-id",       "output-session",
				      "output-destination", "output-stream",   "output-group",
				      "group-stream",       "not-attached",    "not-desired"})
		QTest::newRow(violation) << QString::fromLatin1(violation);
}

// Даже detached-история того же назначения не позволяет приписать новейший attached Output собственной команде.
void RestreamOperationsTest::selectedDestinationRequiresOwnReceiptAndUniqueDurableOutput()
{
	// violation задаёт независимый дефект после принятия команды с единственным назначением.
	QFETCH(QString, violation);
	// rig ограничивает десять последовательных preflight GET и чтение результата общим сроком в одну секунду.
	OperationRig rig({1000, 5, 20});
	QVERIFY(rig.initialize(false));
	// scenario принимает команду, затем возвращает только указанное противоречие публичного результата.
	StartDestinationScenario scenario(rig);
	scenario.violation = violation;
	// proofPath адресует ответ с выбранным нарушением; таймаут до его чтения не проверяет этот сценарий.
	const auto proofPath =
		violation.startsWith(QStringLiteral("receipt-"))   ? QStringLiteral("/api/v1/commands/command-1")
		: violation.startsWith(QStringLiteral("session-")) ? QStringLiteral("/api/v1/sessions/session")
		: (violation == QStringLiteral("missing-output") || violation == QStringLiteral("partial-outputs") ||
		   violation.startsWith(QStringLiteral("duplicate-")))
			? QStringLiteral("/api/v1/sessions/session/outputs")
			: QStringLiteral("/api/v1/sessions/session/outputs/created-output");
	// violationReached подтверждает получение дефектного ответа; partial-outputs требует отказа второй страницы.
	bool violationReached = false;
	// request проверяется до стандартного ответа сценария, без изменения его данных и порядка запросов.
	scenario.before = [&violation, &proofPath, &violationReached](const HttpServer::Request &request) {
		// url сохраняет query, чтобы полный первый ответ не подменил отказ следующей страницы.
		const auto url = QUrl::fromEncoded("http://local" + request.target);
		violationReached = violationReached ||
				   (request.method == "GET" && url.path() == proofPath &&
				    (violation != QStringLiteral("partial-outputs") ||
				     !QUrlQuery(url).queryItemValue(QStringLiteral("cursor")).isEmpty()));
		return false;
	};
	QVERIFY(rig.operations.startDestination(rig.context(), QStringLiteral("destination")));
	QTRY_VERIFY_WITH_TIMEOUT(rig.finished(), 2000);
	QVERIFY(violationReached);
	QCOMPARE(rig.state.operations().current->outcome,
		 violation == QStringLiteral("receipt-failed") ? OperationOutcome::Failed : OperationOutcome::Unknown);
	QVERIFY(!rig.state.operations().current->stateConfirmed);
	QCOMPARE(rig.posts().size(), 1);
	QCOMPARE(rig.posts().first().target, QByteArray("/api/v1/streams/stream/start"));
	QVERIFY(!rig.frontend.streaming);
	QCOMPARE(rig.frontend.stopRequests, 0);
}

// Между полным списком и адресным подтверждением старый Output отсоединяется, а новое исполнение становится пригодным.
void RestreamOperationsTest::selectedDestinationKeepsPinnedOutputAfterReplacement()
{
	// rig и scenario дают прежнему Output и замене одинаковые Destination/Session, различая только их долговечные ID.
	OperationRig rig({1000, 5, 20});
	QVERIFY(rig.initialize(false));
	StartDestinationScenario scenario(rig);
	// replacementAvailable означает, что detach/re-add уже произошёл после закрепления исходного ID.
	bool replacementAvailable = false;
	// originalDetailReads и replacementDetailReads различают повтор исходного GET и перенаправление на новый Output.
	int originalDetailReads = 0;
	int replacementDetailReads = 0;
	// pinnedBeforeEveryDetail сохраняет нарушение публичного закрепления даже при последующем восстановлении ID.
	bool pinnedBeforeEveryDetail = true;
	// request после первого адресного GET видит отсоединённую историю и готовую замену; остальные ответы не изменяются.
	scenario.before = [&](const HttpServer::Request &request) {
		// path отделяет адрес Output от параметров полного списка.
		const auto path = QUrl::fromEncoded("http://local" + request.target).path();
		if (request.method != "GET")
			return false;
		if (path == QStringLiteral("/api/v1/sessions/session/outputs/created-output")) {
			++originalDetailReads;
			pinnedBeforeEveryDetail = pinnedBeforeEveryDetail && rig.state.operations().current &&
						  rig.state.operations().current->outputId ==
							  QStringLiteral("created-output");
			replacementAvailable = true;
			// original сохраняет прежний ID и снимки, но больше не может подтвердить запущенный прикреплённый Output.
			auto original = scenario.output();
			original.insert(QStringLiteral("attached"), false);
			original.insert(QStringLiteral("desired_running"), false);
			HttpServer::reply(request, fixture::bytes(original));
			return true;
		}
		if (replacementAvailable && path == QStringLiteral("/api/v1/sessions/session/outputs")) {
			// original остаётся в истории; replacement имеет корректные связи и желаемый запуск, но другой ID.
			auto original = scenario.output();
			original.insert(QStringLiteral("attached"), false);
			original.insert(QStringLiteral("desired_running"), false);
			const auto replacement = scenario.output(QStringLiteral("replacement-output"));
			HttpServer::reply(request, fixture::page({original, replacement}));
			return true;
		}
		if (path == QStringLiteral("/api/v1/sessions/session/outputs/replacement-output")) {
			++replacementDetailReads;
			HttpServer::reply(request,
					  fixture::bytes(scenario.output(QStringLiteral("replacement-output"))));
			return true;
		}
		return false;
	};
	QVERIFY(rig.operations.startDestination(rig.context(), QStringLiteral("destination")));
	QTRY_VERIFY_WITH_TIMEOUT(rig.finished(), 2000);
	QVERIFY(replacementAvailable);
	QVERIFY(originalDetailReads >= 2);
	QVERIFY(pinnedBeforeEveryDetail);
	QCOMPARE(replacementDetailReads, 0);
	QCOMPARE(rig.state.operations().current->outputId, QStringLiteral("created-output"));
	QCOMPARE(rig.state.operations().current->sessionId, QStringLiteral("session"));
	QCOMPARE(rig.state.operations().current->outcome, OperationOutcome::Unknown);
	QVERIFY(!rig.state.operations().current->stateConfirmed);
	QVERIFY(rig.state.operations().current->receipt);
	QCOMPARE(rig.state.operations().current->receipt->status, QStringLiteral("SUCCEEDED"));
	QCOMPARE(rig.posts().size(), 1);
	QCOMPARE(rig.posts().first().target, QByteArray("/api/v1/streams/stream/start"));
	QVERIFY(!rig.frontend.streaming);
	QCOMPARE(rig.frontend.stopRequests, 0);
}

// Локальное отсутствие Session должно быть полным, свежим и относиться к тому же контексту.
void RestreamOperationsTest::selectedDestinationRejectsUnsafeAdmission_data()
{
	QTest::addColumn<QString>("condition");
	// condition выбирает недостоверное наблюдение или запрет конфигурационной записи.
	for (const auto *condition : {"unknown", "partial", "stale", "STARTING", "RUNNING", "STOPPING", "disconnected",
				      "writing", "reconciliation", "empty-destination", "old-context", "stale-catalog",
				      "partial-catalog", "missing-destination"})
		QTest::newRow(condition) << QString::fromLatin1(condition);
}

// Ни один отказ локального допуска не создаёт POST или побочную команду OBS.
void RestreamOperationsTest::selectedDestinationRejectsUnsafeAdmission()
{
	// condition определяет единственное препятствие при прочем пригодном окружении.
	QFETCH(QString, condition);
	// rig и scenario обслужили бы корректный singleton start, если бы допуск ошибочно прошёл.
	OperationRig rig;
	QVERIFY(rig.initialize(false));
	StartDestinationScenario scenario(rig);
	// context фиксирует исходный выбор; metadata содержит доказательство отсутствия активной Session.
	const auto context = rig.context();
	auto metadata = rig.state.dockServer();
	if (condition == QStringLiteral("unknown"))
		metadata.sessionStatus.availability = RuntimeAvailability::Unknown;
	if (condition == QStringLiteral("stale"))
		metadata.sessionStatus.availability = RuntimeAvailability::Stale;
	if (condition == QStringLiteral("partial"))
		metadata.sessionStatus.complete = false;
	if (condition == QStringLiteral("stale-catalog"))
		metadata.destinationsStatus.availability = RuntimeAvailability::Stale;
	if (condition == QStringLiteral("partial-catalog"))
		metadata.destinationsStatus.complete = false;
	if (condition == QStringLiteral("missing-destination"))
		metadata.destinations.clear();
	if (condition == QStringLiteral("STARTING") || condition == QStringLiteral("RUNNING") ||
	    condition == QStringLiteral("STOPPING"))
		metadata.session = DockSession{QStringLiteral("other-session"), condition, false};
	rig.state.setDockServer(metadata);
	if (condition == QStringLiteral("disconnected"))
		rig.state.setConnection(rig.server.origin().toString(), ConnectionStatus::Reconnecting);
	if (condition == QStringLiteral("old-context"))
		rig.state.setSelection(QStringLiteral("other-stream"), true, {}, {}, QStringLiteral("profile-uuid"));
	if (condition == QStringLiteral("writing") || condition == QStringLiteral("reconciliation")) {
		// editor сообщает незавершённую запись либо необходимость её последующей сверки.
		ConfigurationEditorState editor;
		editor.context = {context.origin, context.profileUuid, context.streamId};
		editor.writing = condition == QStringLiteral("writing");
		editor.reconciliationRequired = condition == QStringLiteral("reconciliation");
		rig.state.setConfigurationEditor(editor);
	}
	QVERIFY(!rig.operations.startDestination(
		context, condition == QStringLiteral("empty-destination") ? QString{} : QStringLiteral("destination")));
	QVERIFY(rig.posts().isEmpty());
	QVERIFY(!rig.frontend.streaming);
}

// Гонка возникает после начала приватной проверки и до первого неизменяемого start POST.
void RestreamOperationsTest::selectedDestinationRechecksBeforeFirstPost_data()
{
	QTest::addColumn<QString>("change");
	// change задаёт новую эпоху, контекст, потерю связи либо подтверждённую чужую Session.
	for (const auto *change :
	     {"configuration", "context", "connection", "session", "server-session", "sending-configuration"})
		QTest::newRow(change) << QString::fromLatin1(change);
}

// Удержанный GET даёт детерминированное окно для изменения вместо временной задержки в тесте.
void RestreamOperationsTest::selectedDestinationRechecksBeforeFirstPost()
{
	// change выбирает событие, которое отзывает допуск ещё не отправленного запроса.
	QFETCH(QString, change);
	// rig и scenario начали выбранную проверку в безопасном контексте.
	OperationRig rig({500, 5, 20});
	QVERIFY(rig.initialize(false));
	StartDestinationScenario scenario(rig);
	// held удерживает последнее фильтрованное чтение отсутствия активной Session.
	std::optional<HttpServer::Request> held;
	// before получает request и задерживает только STOPPING-фильтр, оставляя пригодность полностью проверенной.
	scenario.before = [&](const HttpServer::Request &request) {
		if (request.target.startsWith("/api/v1/streams/stream/sessions?") &&
		    QUrlQuery(QUrl::fromEncoded("http://local" + request.target))
				    .queryItemValue(QStringLiteral("state")) == QStringLiteral("STOPPING")) {
			held = request;
			return true;
		}
		return false;
	};
	QVERIFY(rig.operations.startDestination(rig.context(), QStringLiteral("destination")));
	QTRY_VERIFY_WITH_TIMEOUT(held.has_value(), 2000);
	if (change == QStringLiteral("configuration"))
		rig.state.invalidateConfiguration();
	if (change == QStringLiteral("context"))
		rig.state.setSelection(QStringLiteral("other-stream"), true, {}, {}, QStringLiteral("profile-uuid"));
	if (change == QStringLiteral("connection"))
		rig.state.setConnection(rig.server.origin().toString(), ConnectionStatus::Reconnecting);
	if (change == QStringLiteral("session")) {
		// metadata публикует параллельный эфир, который не является результатом нашего ещё не отправленного POST.
		auto metadata = rig.state.dockServer();
		metadata.session = DockSession{QStringLiteral("foreign-session"), QStringLiteral("RUNNING"), false};
		rig.state.setDockServer(metadata);
	}
	// invalidated ограничивает синхронное изменение эпохи одним сигналом Sending до передачи первого POST.
	bool invalidated = false;
	if (change == QStringLiteral("sending-configuration")) {
		// Обработчик отзывает пригодность внутри publish, когда старый preflight уже завершён.
		connect(&rig.state, &StateStore::operationsChanged, this, [&] {
			if (!invalidated && rig.state.operations().current &&
			    rig.state.operations().current->phase == OperationPhase::Sending) {
				invalidated = true;
				rig.state.invalidateConfiguration();
			}
		});
	}
	// discovered сообщает активную Session в ответе последнего серверного фильтра вместо локального наблюдения.
	auto discovered = fixture::session();
	discovered.insert(QStringLiteral("state"), QStringLiteral("STOPPING"));
	HttpServer::reply(*held, fixture::page(change == QStringLiteral("server-session") ? QJsonArray{discovered}
											  : QJsonArray{}));
	QTRY_VERIFY_WITH_TIMEOUT(rig.finished() || rig.state.operations().lastInterrupted.has_value(), 2000);
	if (change == QStringLiteral("sending-configuration"))
		QVERIFY(invalidated);
	QVERIFY(rig.posts().isEmpty());
	QVERIFY(!rig.frontend.streaming);
}

// Повтор уже отправленного запроса восстанавливает прежнее намерение, даже если пригодность впоследствии изменилась.
void RestreamOperationsTest::selectedDestinationRetryKeepsSingletonIntent()
{
	// rig и scenario принимают первый start и теряют его HTTP-ответ.
	OperationRig rig({500, 5, 20});
	QVERIFY(rig.initialize(false));
	StartDestinationScenario scenario(rig);
	scenario.loseFirstResponse = true;
	QVERIFY(rig.operations.startDestination(rig.context(), QStringLiteral("destination")));
	QTRY_VERIFY_WITH_TIMEOUT(rig.finished(), 2000);
	QCOMPARE(rig.state.operations().current->outcome, OperationOutcome::Unknown);
	QCOMPARE(rig.posts().size(), 1);
	// original фиксирует реальные байты принятого запроса, metadata отражает уже созданную собственную Session.
	const auto original = rig.posts().first();
	auto metadata = rig.state.dockServer();
	metadata.session = DockSession{QStringLiteral("session"), QStringLiteral("RUNNING"), false};
	rig.state.setDockServer(metadata);
	rig.state.invalidateConfiguration();
	scenario.violation = QStringLiteral("no-policy");
	// retryIndex отделяет восстановление потерянного ответа от первоначального preflight.
	const auto retryIndex = rig.server.requests.size();
	QVERIFY(rig.operations.retryUnknown(rig.context()));
	QTRY_VERIFY_WITH_TIMEOUT(rig.finished(), 2000);
	QCOMPARE(rig.state.operations().current->outcome, OperationOutcome::Completed);
	QCOMPARE(rig.posts().size(), 2);
	QCOMPARE(rig.posts().last().target, original.target);
	QCOMPARE(rig.posts().last().body, original.body);
	QCOMPARE(rig.receipts.size(), 1);
	// index проверяет отсутствие нового допуска по конфигурации или отсутствию Session после принятого первого POST.
	for (qsizetype index = retryIndex; index < rig.server.requests.size(); ++index) {
		QVERIFY(!rig.server.requests.at(index).target.endsWith("/fallback-policy"));
		QVERIFY(!rig.server.requests.at(index).target.startsWith("/api/v1/streams/stream/sessions?"));
	}
	QVERIFY(!rig.frontend.streaming);
}

// Серверный 409 окончательно отклоняет первое намерение, не создавая иной команды или нового request_id.
void RestreamOperationsTest::selectedDestinationConflictDoesNotChangeBranch()
{
	// rig и scenario моделируют атомарный конфликт с чужой публикацией.
	OperationRig rig({500, 5, 20});
	QVERIFY(rig.initialize(false));
	StartDestinationScenario scenario(rig);
	scenario.conflict = true;
	QVERIFY(rig.operations.startDestination(rig.context(), QStringLiteral("destination")));
	QTRY_VERIFY_WITH_TIMEOUT(rig.finished(), 2000);
	QCOMPARE(rig.state.operations().current->outcome, OperationOutcome::Failed);
	QVERIFY(!rig.state.operations().current->retryAvailable);
	QVERIFY(!rig.state.operations().current->stateConfirmed);
	// metadata отражает чужой результат и не даёт повода автоматически прикрепить выбранное назначение.
	auto metadata = rig.state.dockServer();
	metadata.session = DockSession{QStringLiteral("foreign-session"), QStringLiteral("RUNNING"), false};
	rig.state.setDockServer(metadata);
	QVERIFY(!rig.operations.retryUnknown(rig.context()));
	QCOMPARE(rig.posts().size(), 1);
	QCOMPARE(rig.posts().first().target, QByteArray("/api/v1/streams/stream/start"));
	QVERIFY(!rig.frontend.streaming);
}

// Все три общие команды различают уже запрошенный запуск, остановленный Output и новое назначение.
void RestreamOperationsTest::visibleBatchCommandsRespectFrozenTargets_data()
{
	QTest::addColumn<int>("actionValue");
	QTest::newRow("start") << int(OperationAction::StartAllOutputs);
	QTest::newRow("stop") << int(OperationAction::StopAllOutputs);
	QTest::newRow("restart") << int(OperationAction::RestartAllOutputs);
}

// Проверяет точные POST вместо одного общего флага успеха; сосед сервера в намерение не входит.
void RestreamOperationsTest::visibleBatchCommandsRespectFrozenTargets()
{
	// actionValue хранит сериализуемое значение строки Qt Test, action выбирает смысл общей кнопки.
	QFETCH(int, actionValue);
	const auto action = OperationAction(actionValue);
	// rig и scenario содержат два прежних исполнения и новое выключенное назначение.
	OperationRig rig({1500, 5, 20});
	QVERIFY(rig.initialize());
	VisibleBatchScenario scenario(rig);
	if (action == OperationAction::StopAllOutputs) {
		rig.frontend.streaming = true;
		rig.obs.handleEvent(FrontendEvent::StreamingStarted);
	}
	// request фиксирует ровно три показанные строки с раздельными ID и безопасными именами.
	auto request =
		scenario.request({{QStringLiteral("one"), QStringLiteral("destination"), QStringLiteral("Первый")},
				  {QStringLiteral("two"), QStringLiteral("second"), QStringLiteral("Второй")},
				  {{}, QStringLiteral("third"), QStringLiteral("Третий")}});
	QVERIFY(rig.operations.controlOutputs(request, action));
	request.targets.clear();
	QTRY_VERIFY_WITH_TIMEOUT(rig.finished(), 6000);
	// result сохраняет весь исходный состав, несмотря на изменение переданного request после вызова.
	const auto result = *rig.state.operations().current;
	QCOMPARE(result.outputs.size(), 3);
	QCOMPARE(result.outcome, OperationOutcome::Completed);
	QCOMPARE(result.outputs.at(0).outputId, QStringLiteral("one"));
	QCOMPARE(result.outputs.at(1).destinationId, QStringLiteral("second"));
	QCOMPARE(result.outputs.at(2).destinationId, QStringLiteral("third"));
	QCOMPARE(result.outputs.at(0).name, QStringLiteral("Первый"));
	// routes и requestIds сохраняют фактическую последовательность и уникальность независимых команд.
	QList<QByteArray> routes;
	QSet<QString> requestIds;
	// post — только реальная команда; конфигурационные PATCH и Session stop не скрываются среди GET.
	for (const auto &post : rig.posts()) {
		routes.append(post.target);
		requestIds.insert(
			QJsonDocument::fromJson(post.body).object().value(QStringLiteral("request_id")).toString());
		QVERIFY(!post.body.contains("neighbor"));
	}
	if (action == OperationAction::StartAllOutputs) {
		QCOMPARE(routes, QList<QByteArray>({"/api/v1/sessions/session/outputs/two/start",
						    "/api/v1/sessions/session/outputs"}));
		QVERIFY(result.outputs.at(0).noOp);
		QCOMPARE(result.outputs.at(0).messageKey, QStringLiteral("Operation.AlreadyRequested"));
	}
	if (action == OperationAction::StopAllOutputs) {
		QCOMPARE(routes, QList<QByteArray>({"/api/v1/sessions/session/outputs/one/stop",
						    "/api/v1/sessions/session/outputs/two/stop"}));
		QVERIFY(result.outputs.at(2).noOp);
		QCOMPARE(result.outputs.at(2).messageKey, QStringLiteral("Operation.NoExecution"));
	}
	if (action == OperationAction::RestartAllOutputs) {
		QCOMPARE(routes, QList<QByteArray>({"/api/v1/sessions/session/outputs/one/stop",
						    "/api/v1/sessions/session/outputs/one/start",
						    "/api/v1/sessions/session/outputs/two/stop",
						    "/api/v1/sessions/session/outputs/two/start",
						    "/api/v1/sessions/session/outputs"}));
		QCOMPARE(result.outputs.at(0).stopOutcome, OperationOutcome::Completed);
		QCOMPARE(result.outputs.at(0).startOutcome, OperationOutcome::Completed);
		QCOMPARE(result.outputs.at(1).stopOutcome, OperationOutcome::Completed);
		QCOMPARE(result.outputs.at(1).startOutcome, OperationOutcome::Completed);
	}
	QCOMPARE(requestIds.size(), routes.size());
	QVERIFY(!requestIds.contains(QString{}));
	if (action != OperationAction::StopAllOutputs)
		QCOMPARE(QJsonDocument::fromJson(rig.posts().last().body)
				 .object()
				 .value(QStringLiteral("destination_id"))
				 .toString(),
			 QStringLiteral("third"));
	QVERIFY(rig.state.runtime().sessions.isEmpty());
	QCOMPARE(rig.frontend.stopRequests, 0);
	QCOMPARE(rig.frontend.streaming, action == OperationAction::StopAllOutputs);
	// requestOnWire проверяет отсутствие скрытых конфигурационных изменений и Session-команд.
	for (const auto &requestOnWire : rig.server.requests)
		QVERIFY(requestOnWire.method == "GET" || routes.contains(requestOnWire.target));
}

// Полное свежее отсутствие Session превращает Stop Outputs в подтверждённый no-op без создания эфира.
void RestreamOperationsTest::visibleStopWithoutSessionDoesNotCreateOrStopAnything()
{
	// rig и scenario оставляют Stream с тремя настроенными площадками, но без текущей Session.
	OperationRig rig;
	QVERIFY(rig.initialize(false));
	VisibleBatchScenario scenario(rig, false);
	QVERIFY(rig.operations.controlOutputs(
		scenario.request({{{}, QStringLiteral("destination"), QStringLiteral("Первый")},
				  {{}, QStringLiteral("second"), QStringLiteral("Второй")}}),
		OperationAction::StopAllOutputs));
	QTRY_VERIFY_WITH_TIMEOUT(rig.finished(), 1000);
	QCOMPARE(rig.state.operations().current->outcome, OperationOutcome::Completed);
	QCOMPARE(rig.state.operations().current->outputs.size(), 2);
	QVERIFY(rig.state.operations().current->outputs.at(0).noOp);
	QVERIFY(rig.state.operations().current->outputs.at(1).noOp);
	QVERIFY(rig.posts().isEmpty());
	QCOMPARE(rig.frontend.stopRequests, 0);
	QVERIFY(!rig.frontend.streaming);
	QVERIFY(!rig.state.dockServer().session);
}

// Определённый отказ предварительной проверки первой площадки не блокирует создание Session с первым пригодным назначением.
void RestreamOperationsTest::visibleBootstrapSkipsUnpreparedAndUsesOwnSessionWithLaggingCache()
{
	// rig и scenario не устанавливают собственные GET в кеш наблюдателя, который до конца остаётся без Session.
	OperationRig rig({1500, 5, 20});
	QVERIFY(rig.initialize(false));
	VisibleBatchScenario scenario(rig, false);
	scenario.unprepared.insert(QStringLiteral("destination"));
	QVERIFY(rig.operations.controlOutputs(
		scenario.request({{{}, QStringLiteral("destination"), QStringLiteral("Неготовый")},
				  {{}, QStringLiteral("second"), QStringLiteral("Готовый")},
				  {{}, QStringLiteral("third"), QStringLiteral("Следующий")}}),
		OperationAction::StartAllOutputs));
	QTRY_VERIFY_WITH_TIMEOUT(rig.finished(), 7000);
	QCOMPARE(rig.posts().size(), 2);
	QCOMPARE(rig.posts().at(0).target, QByteArray("/api/v1/streams/stream/start"));
	QCOMPARE(QJsonDocument::fromJson(rig.posts().at(0).body)
			 .object()
			 .value(QStringLiteral("destination_ids"))
			 .toArray(),
		 QJsonArray{QStringLiteral("second")});
	QCOMPARE(rig.posts().at(1).target, QByteArray("/api/v1/sessions/session/outputs"));
	QCOMPARE(QJsonDocument::fromJson(rig.posts().at(1).body)
			 .object()
			 .value(QStringLiteral("destination_id"))
			 .toString(),
		 QStringLiteral("third"));
	QVERIFY(rig.state.operations().current->outputs.at(0).outcome == OperationOutcome::Failed ||
		rig.state.operations().current->outputs.at(0).outcome == OperationOutcome::NotSent);
	QVERIFY(!rig.state.operations().current->outputs.at(0).messageKey.isEmpty());
	QCOMPARE(rig.state.operations().current->outputs.at(1).outcome, OperationOutcome::Completed);
	QCOMPARE(rig.state.operations().current->outputs.at(2).outcome, OperationOutcome::Completed);
	QVERIFY(rig.state.operations().current->outcome != OperationOutcome::Completed);
	QVERIFY(!rig.state.dockServer().session);
	QVERIFY(rig.state.runtime().sessions.isEmpty());
	QCOMPARE(rig.frontend.stopRequests, 0);
}

// Неизвестность первого Output не равна неизвестности общей собственной Session.
void RestreamOperationsTest::visibleBootstrapOutputUnknownAllowsIndependentAdd()
{
	// rig выделяет достаточно времени для полной предварительной проверки и доказательства общей Session до локального таймаута.
	OperationRig rig({1200, 5, 20});
	QVERIFY(rig.initialize(false));
	VisibleBatchScenario scenario(rig, false);
	scenario.omitBootstrapOutput = true;
	QVERIFY(rig.operations.controlOutputs(
		scenario.request({{{}, QStringLiteral("destination"), QStringLiteral("Первый")},
				  {{}, QStringLiteral("second"), QStringLiteral("Второй")}}),
		OperationAction::RestartAllOutputs));
	QTRY_VERIFY_WITH_TIMEOUT(rig.finished(), 5000);
	QCOMPARE(rig.posts().size(), 2);
	QCOMPARE(rig.posts().at(1).target, QByteArray("/api/v1/sessions/session/outputs"));
	QCOMPARE(rig.state.operations().current->outputs.at(0).outcome, OperationOutcome::Unknown);
	QCOMPARE(rig.state.operations().current->outputs.at(1).outcome, OperationOutcome::Completed);
	QCOMPARE(rig.state.operations().current->outcome, OperationOutcome::Unknown);
	QVERIFY(rig.state.operations().current->retryAvailable);
	QCOMPARE(rig.state.operations().current->retryTargetName, QStringLiteral("Первый"));
	// proofRead удостоверяет достижение проверяемой стадии, а не исчерпание срока в предварительной проверке.
	bool proofRead = false;
	// request адресует реальные чтения после собственной квитанции.
	for (const auto &request : rig.server.requests)
		proofRead = proofRead || request.target.startsWith("/api/v1/sessions/session/outputs?");
	QVERIFY(proofRead);
	QVERIFY(!rig.state.dockServer().session);
}

// Общая неизвестность первого создания Session и чужой Stream общей Session закрывают независимый остаток.
void RestreamOperationsTest::visibleBootstrapUncertaintyStopsRemainder_data()
{
	QTest::addColumn<QString>("condition");
	QTest::newRow("lost-bootstrap") << QStringLiteral("lost");
	QTest::newRow("foreign-session-snapshot") << QStringLiteral("foreign");
	QTest::newRow("conflicting-bootstrap") << QStringLiteral("conflict");
	QTest::newRow("failed-bootstrap-receipt") << QStringLiteral("failed");
}

// Retry принятого создания Session восстанавливает только прежний шаг и не прикрепляет неотправленный остаток.
void RestreamOperationsTest::visibleBootstrapUncertaintyStopsRemainder()
{
	// condition выбирает отказ, потерю 202 либо несовместимый Stream; lostResponse отмечает отсутствие commandId.
	QFETCH(QString, condition);
	const bool lostResponse = condition == QStringLiteral("lost");
	// rig и scenario обслуживают готовую площадку до фактического POST создания Session.
	OperationRig rig({1200, 5, 20});
	QVERIFY(rig.initialize(false));
	VisibleBatchScenario scenario(rig, false);
	scenario.foreignSession = condition == QStringLiteral("foreign");
	scenario.bootstrapConflict = condition == QStringLiteral("conflict");
	if (condition == QStringLiteral("failed"))
		scenario.failedOutputs.insert(QStringLiteral("bootstrap-output"));
	if (lostResponse)
		scenario.lostOutputs.insert(QStringLiteral("bootstrap-output"));
	// violationReached доказывает достижение именно дефектного Session/receipt либо отправленного bootstrap.
	bool violationReached = false;
	// request наблюдается до сценарного ответа; чужой Stream проверяется только адресным GET общей Session.
	scenario.before = [&](const HttpServer::Request &request) {
		violationReached =
			violationReached ||
			(condition == QStringLiteral("foreign") ? request.target == "/api/v1/sessions/session"
			 : condition == QStringLiteral("failed")
				 ? request.target.startsWith("/api/v1/commands/")
				 : request.method == "POST" && request.target == "/api/v1/streams/stream/start");
		return false;
	};
	QVERIFY(rig.operations.controlOutputs(
		scenario.request({{{}, QStringLiteral("destination"), QStringLiteral("Первый")},
				  {{}, QStringLiteral("second"), QStringLiteral("Неотправленный")}}),
		OperationAction::StartAllOutputs));
	QTRY_VERIFY_WITH_TIMEOUT(rig.finished(), 4000);
	QVERIFY(violationReached);
	QCOMPARE(rig.posts().size(), 1);
	QCOMPARE(rig.state.operations().current->outputs.at(0).outcome,
		 condition == QStringLiteral("conflict") || condition == QStringLiteral("failed")
			 ? OperationOutcome::Failed
			 : OperationOutcome::Unknown);
	QCOMPARE(rig.state.operations().current->outputs.at(1).outcome, OperationOutcome::NotSent);
	if (condition == QStringLiteral("conflict") || condition == QStringLiteral("failed")) {
		QVERIFY(!rig.state.operations().current->retryAvailable);
		QVERIFY(!rig.operations.retryUnknown(rig.context()));
		QCOMPARE(rig.posts().first().target, QByteArray("/api/v1/streams/stream/start"));
		QCOMPARE(rig.frontend.stopRequests, 0);
		return;
	}
	// original хранит фактический payload с единственным назначением, retryIndex отделяет новый явный повтор от исходного допуска.
	const auto original = rig.posts().first();
	const auto retryIndex = rig.server.requests.size();
	scenario.lostOutputs.clear();
	scenario.foreignSession = false;
	rig.state.invalidateConfiguration();
	QVERIFY(rig.operations.retryUnknown(rig.context()));
	QTRY_VERIFY_WITH_TIMEOUT(rig.finished(), 4000);
	QCOMPARE(rig.posts().size(), lostResponse ? 2 : 1);
	if (lostResponse) {
		QCOMPARE(rig.posts().last().body, original.body);
		QCOMPARE(rig.posts().last().target, original.target);
	}
	QCOMPARE(rig.state.operations().current->outputs.at(1).outcome, OperationOutcome::NotSent);
	QVERIFY(rig.state.operations().current->outcome != OperationOutcome::Completed);
	// index перечисляет только повтор: нового поиска отсутствия и нового проверки пригодности заглушки быть не должно.
	for (qsizetype index = retryIndex; index < rig.server.requests.size(); ++index) {
		QVERIFY(!rig.server.requests.at(index).target.startsWith("/api/v1/streams/stream/sessions?"));
		QVERIFY(!rig.server.requests.at(index).target.endsWith("/fallback-policy"));
	}
	QCOMPARE(rig.frontend.stopRequests, 0);
}

// Отказ и потерянный ответ первой stop-команды не отменяют независимую вторую строку.
void RestreamOperationsTest::visibleRestartContinuesRowsWithoutStartingAfterUnconfirmedStop_data()
{
	QTest::addColumn<bool>("unknown");
	QTest::newRow("terminal-failed") << false;
	QTest::newRow("lost-response") << true;
}

// Долговечный desired=false сам по себе не заменяет успешную stop-квитанцию перед зависимым start.
void RestreamOperationsTest::visibleRestartContinuesRowsWithoutStartingAfterUnconfirmedStop()
{
	// unknown задаёт неопределённый POST либо определённый серверный FAILED.
	QFETCH(bool, unknown);
	// rig и scenario готовы выполнить остальные строки в той же RUNNING Session.
	OperationRig rig({1200, 5, 20});
	QVERIFY(rig.initialize());
	VisibleBatchScenario scenario(rig);
	if (unknown)
		scenario.lostOutputs.insert(QStringLiteral("one"));
	else
		scenario.failedOutputs.insert(QStringLiteral("one"));
	QVERIFY(rig.operations.controlOutputs(
		scenario.request({{QStringLiteral("one"), QStringLiteral("destination"), QStringLiteral("Первый")},
				  {QStringLiteral("two"), QStringLiteral("second"), QStringLiteral("Второй")}}),
		OperationAction::RestartAllOutputs));
	QTRY_VERIFY_WITH_TIMEOUT(rig.finished(), 5000);
	QCOMPARE(rig.posts().size(), 3);
	QCOMPARE(rig.posts().at(0).target, QByteArray("/api/v1/sessions/session/outputs/one/stop"));
	QCOMPARE(rig.posts().at(1).target, QByteArray("/api/v1/sessions/session/outputs/two/stop"));
	QCOMPARE(rig.posts().at(2).target, QByteArray("/api/v1/sessions/session/outputs/two/start"));
	QCOMPARE(rig.state.operations().current->outputs.at(0).outcome,
		 unknown ? OperationOutcome::Unknown : OperationOutcome::Failed);
	QCOMPARE(rig.state.operations().current->outputs.at(0).stopOutcome,
		 unknown ? OperationOutcome::Unknown : OperationOutcome::Failed);
	QCOMPARE(rig.state.operations().current->outputs.at(0).startOutcome, OperationOutcome::NotSent);
	QCOMPARE(rig.state.operations().current->outputs.at(1).outcome, OperationOutcome::Completed);
	QVERIFY(rig.state.operations().current->outcome != OperationOutcome::Completed);
	QVERIFY(!rig.state.operations().current->messageKey.contains(QStringLiteral("SECRET_SERVER_MESSAGE")));
	if (unknown) {
		// original хранит неизвестный stop, которому нужен один явный повтор с прежними байтами.
		const auto original = rig.posts().first();
		scenario.lostOutputs.clear();
		QVERIFY(rig.operations.retryUnknown(rig.context()));
		QTRY_VERIFY_WITH_TIMEOUT(rig.finished(), 3000);
		QCOMPARE(rig.posts().size(), 4);
		QCOMPARE(rig.posts().last().body, original.body);
		QCOMPARE(rig.posts().last().target, original.target);
		QCOMPARE(rig.state.operations().current->outputs.at(0).stopOutcome, OperationOutcome::Completed);
		QCOMPARE(rig.state.operations().current->outputs.at(0).startOutcome, OperationOutcome::NotSent);
		QVERIFY(rig.state.operations().current->outputs.at(0).outcome != OperationOutcome::Completed);
		QVERIFY(rig.state.operations().current->outcome != OperationOutcome::Completed);
	}
}

// Два неизвестных stop сохраняются независимо, а последнее удачное выполнение не подменяет общий итог.
void RestreamOperationsTest::visibleRetryChoosesFirstUnknownAndOnlyOneSavedStep()
{
	// rig и scenario принимают обе stop-команды, теряя только их первоначальные HTTP-ответы.
	OperationRig rig({1000, 5, 20});
	QVERIFY(rig.initialize());
	VisibleBatchScenario scenario(rig);
	scenario.lostOutputs = {QStringLiteral("one"), QStringLiteral("two")};
	QVERIFY(rig.operations.controlOutputs(
		scenario.request({{QStringLiteral("one"), QStringLiteral("destination"), QStringLiteral("Первый")},
				  {QStringLiteral("two"), QStringLiteral("second"), QStringLiteral("Второй")}}),
		OperationAction::StopAllOutputs));
	QTRY_VERIFY_WITH_TIMEOUT(rig.finished(), 4000);
	QCOMPARE(rig.posts().size(), 2);
	// originals сохраняет реальные два намерения для проверки точной цели и request_id каждого повтора.
	const auto originals = rig.posts();
	QCOMPARE(rig.state.operations().current->retryTargetName, QStringLiteral("Первый"));
	scenario.lostOutputs.clear();
	QVERIFY(rig.operations.retryUnknown(rig.context()));
	QTRY_VERIFY_WITH_TIMEOUT(rig.finished(), 3000);
	QCOMPARE(rig.posts().size(), 3);
	QCOMPARE(rig.posts().last().body, originals.at(0).body);
	QCOMPARE(rig.posts().last().target, originals.at(0).target);
	QCOMPARE(rig.state.operations().current->outputs.at(0).outcome, OperationOutcome::Completed);
	QCOMPARE(rig.state.operations().current->outputs.at(1).outcome, OperationOutcome::Unknown);
	QCOMPARE(rig.state.operations().current->outcome, OperationOutcome::Unknown);
	QCOMPARE(rig.state.operations().current->retryTargetName, QStringLiteral("Второй"));
	QVERIFY(rig.operations.retryUnknown(rig.context()));
	QTRY_VERIFY_WITH_TIMEOUT(rig.finished(), 3000);
	QCOMPARE(rig.posts().size(), 4);
	QCOMPARE(rig.posts().last().body, originals.at(1).body);
	QCOMPARE(rig.posts().last().target, originals.at(1).target);
	QCOMPARE(rig.state.operations().current->outcome, OperationOutcome::Completed);
	QVERIFY(!rig.operations.retryUnknown(rig.context()));
	QCOMPARE(scenario.receipts.size(), 2);
}

// Неадресуемая runtime-строка остаётся в исходном отчёте; успешный сосед не превращает весь пакет в Completed.
void RestreamOperationsTest::visibleUnaddressableRowRemainsInAggregate()
{
	// rig и scenario доказывают один успешный stop и одну строку без безопасной связи с Destination.
	OperationRig rig({1000, 5, 20});
	QVERIFY(rig.initialize());
	VisibleBatchScenario scenario(rig);
	QVERIFY(rig.operations.controlOutputs(
		scenario.request({{QStringLiteral("runtime-output"), {}, QStringLiteral("Без адреса")},
				  {QStringLiteral("one"), QStringLiteral("destination"), QStringLiteral("Исполнение")}}),
		OperationAction::StopAllOutputs));
	QTRY_VERIFY_WITH_TIMEOUT(rig.finished(), 3000);
	QCOMPARE(rig.posts().size(), 1);
	QCOMPARE(rig.posts().first().target, QByteArray("/api/v1/sessions/session/outputs/one/stop"));
	QCOMPARE(rig.state.operations().current->outputs.size(), 2);
	QCOMPARE(rig.state.operations().current->outputs.at(0).outcome, OperationOutcome::NotSent);
	QCOMPARE(rig.state.operations().current->outputs.at(0).outputId, QStringLiteral("runtime-output"));
	QVERIFY(!rig.state.operations().current->outputs.at(0).messageKey.isEmpty());
	QCOMPARE(rig.state.operations().current->outputs.at(1).outcome, OperationOutcome::Completed);
	QVERIFY(rig.state.operations().current->outcome != OperationOutcome::Completed);
}

// Подтверждение Session, адресация и верхняя граница проверяются до первого сетевого побочного эффекта.
void RestreamOperationsTest::visibleBatchRejectsUnsafeAdmission_data()
{
	QTest::addColumn<QString>("condition");
	// condition задаёт ровно одно нарушение независимого контракта допуска.
	for (const auto *condition :
	     {"unknown-session", "partial-session", "starting-session", "stopping-session", "replacement-session",
	      "old-context", "disconnected", "duplicate-output", "too-many"})
		QTest::newRow(condition) << QString::fromLatin1(condition);
}

// Нельзя трактовать неизвестность или переходную Session как отсутствие и нельзя скрытно усекать состав.
void RestreamOperationsTest::visibleBatchRejectsUnsafeAdmission()
{
	// condition выбирает недопустимое наблюдение либо неверный входной состав.
	QFETCH(QString, condition);
	// rig и scenario обслужили бы обычные безопасные stop-команды, если бы вход ошибочно прошёл.
	OperationRig rig;
	QVERIFY(rig.initialize());
	VisibleBatchScenario scenario(rig);
	// request сохраняется до изменения состояния; metadata вводит проверяемое расхождение исходной Session.
	auto request =
		scenario.request({{QStringLiteral("one"), QStringLiteral("destination"), QStringLiteral("Первый")}});
	auto metadata = rig.state.dockServer();
	if (condition == QStringLiteral("unknown-session"))
		metadata.sessionStatus.availability = RuntimeAvailability::Unknown;
	if (condition == QStringLiteral("partial-session"))
		metadata.sessionStatus.complete = false;
	if (condition == QStringLiteral("starting-session"))
		metadata.session->state = QStringLiteral("STARTING");
	if (condition == QStringLiteral("stopping-session"))
		metadata.session->state = QStringLiteral("STOPPING");
	if (condition == QStringLiteral("replacement-session"))
		metadata.session->id = QStringLiteral("foreign-session");
	rig.state.setDockServer(metadata);
	if (condition == QStringLiteral("old-context"))
		rig.state.setSelection(QStringLiteral("other-stream"), true, {}, {}, QStringLiteral("profile-uuid"));
	if (condition == QStringLiteral("disconnected"))
		rig.state.setConnection(rig.server.origin().toString(), ConnectionStatus::Reconnecting);
	if (condition == QStringLiteral("duplicate-output"))
		request.targets.append(request.targets.first());
	if (condition == QStringLiteral("too-many")) {
		request.targets.clear();
		// index создаёт 10001 уникальную строку, чтобы запрет был именно ограничением размера, а не дубликатом.
		for (int index = 0; index < 10001; ++index)
			request.targets.append(
				{{}, QStringLiteral("destination-%1").arg(index), QStringLiteral("Площадка")});
	}
	QVERIFY(!rig.operations.controlOutputs(request, OperationAction::StopAllOutputs));
	QVERIFY(rig.server.requests.isEmpty());
	QCOMPARE(rig.frontend.stopRequests, 0);
}

// Новые конфигурационные снимки требуют исходной эпохи, но старый исторический Output не зависит от каталога.
void RestreamOperationsTest::visibleBatchConfigurationEpochStopsNewSnapshotsOnly()
{
	// rig и scenario сначала запускают точный остановленный Output, затем рассматривают каталог и исторический работающий Output.
	OperationRig rig({1200, 5, 20});
	QVERIFY(rig.initialize());
	VisibleBatchScenario scenario(rig);
	// invalidated доказывает смену эпохи уже после отправленного первого POST, а не отказ исходного допуска.
	bool invalidated = false;
	// request меняет конфигурационную эпоху во время чтения собственной первой квитанции.
	scenario.before = [&](const HttpServer::Request &request) {
		if (!invalidated && request.target.startsWith("/api/v1/commands/")) {
			invalidated = true;
			rig.state.invalidateConfiguration();
		}
		return false;
	};
	QVERIFY(rig.operations.controlOutputs(
		scenario.request(
			{{QStringLiteral("two"), QStringLiteral("second"), QStringLiteral("Первый")},
			 {{}, QStringLiteral("third"), QStringLiteral("Новый снимок")},
			 {QStringLiteral("one"), QStringLiteral("destination"), QStringLiteral("Исторический")}}),
		OperationAction::StartAllOutputs));
	QTRY_VERIFY_WITH_TIMEOUT(rig.finished(), 4000);
	QVERIFY(invalidated);
	QCOMPARE(rig.posts().size(), 1);
	QCOMPARE(rig.posts().first().target, QByteArray("/api/v1/sessions/session/outputs/two/start"));
	QCOMPARE(rig.state.operations().current->outputs.at(0).outcome, OperationOutcome::Completed);
	QCOMPARE(rig.state.operations().current->outputs.at(1).outcome, OperationOutcome::NotSent);
	QCOMPARE(rig.state.operations().current->outputs.at(2).outcome, OperationOutcome::Completed);
	QVERIFY(rig.state.operations().current->outputs.at(2).noOp);
	QVERIFY(rig.state.operations().current->outcome != OperationOutcome::Completed);
}

// Потеря общей связи или замена Session останавливает новые строки; возврат связи не воспроизводит остаток.
void RestreamOperationsTest::visibleBatchInterruptionsDoNotResume_data()
{
	QTest::addColumn<bool>("replaceSession");
	QTest::newRow("connection") << false;
	QTest::newRow("session") << true;
}

// Принятый stop дочитывается отдельно, но start того же restart и соседняя строка не отправляются.
void RestreamOperationsTest::visibleBatchInterruptionsDoNotResume()
{
	// replaceSession выбирает разрыв наблюдения либо подтверждённый следующий эфир того же Stream.
	QFETCH(bool, replaceSession);
	// rig и scenario дают точную первую квитанцию, чтобы прерывание нельзя было объяснить локальной ошибкой строки.
	OperationRig rig({1000, 5, 20});
	QVERIFY(rig.initialize());
	VisibleBatchScenario scenario(rig);
	// interrupted ограничивает изменение одним событием после принятого POST.
	bool interrupted = false;
	// request во время GET квитанции меняет общий контекст исполнения, не подменяя саму квитанцию.
	scenario.before = [&](const HttpServer::Request &request) {
		if (!interrupted && request.target.startsWith("/api/v1/commands/")) {
			interrupted = true;
			if (replaceSession) {
				// replacement моделирует следующий эфир; старое намерение не должно адресовать его.
				auto replacement = rig.state.dockServer();
				replacement.session->id = QStringLiteral("next-session");
				rig.state.setDockServer(replacement);
			} else {
				rig.state.setConnection(rig.server.origin().toString(), ConnectionStatus::Reconnecting);
			}
		}
		return false;
	};
	QVERIFY(rig.operations.controlOutputs(
		scenario.request({{QStringLiteral("one"), QStringLiteral("destination"), QStringLiteral("Первый")},
				  {QStringLiteral("two"), QStringLiteral("second"), QStringLiteral("Второй")}}),
		OperationAction::RestartAllOutputs));
	QTRY_VERIFY_WITH_TIMEOUT(rig.finished(), 4000);
	QVERIFY(interrupted);
	QCOMPARE(rig.posts().size(), 1);
	QCOMPARE(rig.posts().first().target, QByteArray("/api/v1/sessions/session/outputs/one/stop"));
	QCOMPARE(rig.state.operations().current->outputs.at(1).outcome, OperationOutcome::NotSent);
	QVERIFY(rig.state.operations().current->outcome != OperationOutcome::Completed);
	QVERIFY(rig.state.operations().current->remainingStopped);
	rig.state.setConnection(rig.server.origin().toString(), ConnectionStatus::Connected);
	QCoreApplication::sendPostedEvents(nullptr, QEvent::MetaCall);
	QCOMPARE(rig.posts().size(), 1);
	QCOMPARE(rig.frontend.stopRequests, 0);
}

// Истёкший receipt первой строки не может завершить или испортить следующую строку того же намерения.
void RestreamOperationsTest::visibleLateReceiptCannotCrossStepAndKnownRetryUsesGet()
{
	// rig и scenario моделируют две точные stop-команды; каждый шаг получает собственный ограниченный срок.
	OperationRig rig({700, 5, 20});
	QVERIFY(rig.initialize());
	VisibleBatchScenario scenario(rig);
	// oldReceipt удерживает ответ первой команды, nextOutput даёт детерминированную границу нового шага.
	std::optional<HttpServer::Request> oldReceipt;
	std::optional<HttpServer::Request> nextOutput;
	// release разрешает обычный GET при явном повторе, secondHeld ограничивает удержание новой строки одним запросом.
	bool release = false;
	bool secondHeld = false;
	// request задерживает только первую квитанцию и первый адресный GET второй строки после таймаута.
	scenario.before = [&](const HttpServer::Request &request) {
		if (!release && request.target.startsWith("/api/v1/commands/") && rig.posts().size() == 1) {
			oldReceipt = request;
			return true;
		}
		if (oldReceipt && !secondHeld && request.target == "/api/v1/sessions/session/outputs/two") {
			secondHeld = true;
			nextOutput = request;
			return true;
		}
		return false;
	};
	QVERIFY(rig.operations.controlOutputs(
		scenario.request({{QStringLiteral("one"), QStringLiteral("destination"), QStringLiteral("Первый")},
				  {QStringLiteral("two"), QStringLiteral("second"), QStringLiteral("Второй")}}),
		OperationAction::StopAllOutputs));
	QTRY_VERIFY_WITH_TIMEOUT(oldReceipt.has_value(), 1500);
	QTRY_VERIFY_WITH_TIMEOUT(nextOutput.has_value(), 2500);
	QVERIFY(!rig.finished());
	QCOMPARE(rig.state.operations().current->outputs.at(0).outcome, OperationOutcome::Unknown);
	// late содержит настоящую старую команду, но ложный итоговый FAILED: он не должен испортить следующую строку.
	auto late = scenario.receipts.value(
		QString::fromUtf8(oldReceipt->target.mid(oldReceipt->target.lastIndexOf('/') + 1)));
	late.insert(QStringLiteral("status"), QStringLiteral("FAILED"));
	HttpServer::reply(*oldReceipt, fixture::bytes(late));
	HttpServer::reply(*nextOutput, fixture::bytes(scenario.outputs.value(QStringLiteral("two"))));
	QTRY_VERIFY_WITH_TIMEOUT(rig.finished(), 2500);
	QCOMPARE(rig.posts().size(), 2);
	QCOMPARE(rig.state.operations().current->outputs.at(0).outcome, OperationOutcome::Unknown);
	QCOMPARE(rig.state.operations().current->outputs.at(1).outcome, OperationOutcome::Completed);
	QCOMPARE(rig.state.operations().current->outcome, OperationOutcome::Unknown);
	QVERIFY(rig.state.operations().current->retryAvailable);
	// retryIndex фиксирует границу ручного повтора: известный commandId не разрешает новый POST.
	const auto retryIndex = rig.server.requests.size();
	release = true;
	QVERIFY(rig.operations.retryUnknown(rig.context()));
	QTRY_VERIFY_WITH_TIMEOUT(rig.finished(), 2500);
	QCOMPARE(rig.posts().size(), 2);
	QVERIFY(rig.server.requests.size() > retryIndex);
	QCOMPARE(rig.server.requests.at(retryIndex).method, QByteArray("GET"));
	QCOMPARE(rig.server.requests.at(retryIndex).target, oldReceipt->target);
	QCOMPARE(rig.state.operations().current->outcome, OperationOutcome::Completed);
}

// Разные исторические Outputs одного Destination остаются разными адресатами массовой остановки.
void RestreamOperationsTest::visibleBatchDoesNotCollapseExecutionsOfSameDestination()
{
	// rig и scenario задают два Outputs одной площадки с различными точными Output ID.
	OperationRig rig({1000, 5, 20});
	QVERIFY(rig.initialize());
	VisibleBatchScenario scenario(rig);
	scenario.outputs[QStringLiteral("two")] =
		scenario.makeOutput(QStringLiteral("two"), QStringLiteral("destination"), true);
	// metadata подтверждает исторические связи списка независимо от текущих изменяемых назначений.
	auto metadata = rig.state.dockServer();
	metadata.outputs[QStringLiteral("two")].destinationId = QStringLiteral("destination");
	rig.state.setDockServer(metadata);
	QVERIFY(rig.operations.controlOutputs(
		scenario.request(
			{{QStringLiteral("one"), QStringLiteral("destination"), QStringLiteral("История один")},
			 {QStringLiteral("two"), QStringLiteral("destination"), QStringLiteral("История два")}}),
		OperationAction::StopAllOutputs));
	QTRY_VERIFY_WITH_TIMEOUT(rig.finished(), 3000);
	QCOMPARE(rig.posts().size(), 2);
	QCOMPARE(rig.posts().at(0).target, QByteArray("/api/v1/sessions/session/outputs/one/stop"));
	QCOMPARE(rig.posts().at(1).target, QByteArray("/api/v1/sessions/session/outputs/two/stop"));
	QCOMPARE(rig.state.operations().current->outputs.size(), 2);
	QCOMPARE(rig.state.operations().current->outcome, OperationOutcome::Completed);
}
