#include "plugin-lifecycle.hpp"

#include "api/api-client.hpp"
#include "api/event-client.hpp"
#include "core/connection-controller.hpp"
#include "core/settings-store.hpp"
#include "core/state-store.hpp"
#include "core/restream-controller.hpp"
#include "core/configuration-controller.hpp"
#include "obs/frontend.hpp"
#include "obs/obs-controller.hpp"
#include "ui/management-window.hpp"
#include "ui/restream-dock.hpp"
#include "ui/operation-error-dialog.hpp"

#include <QAction>
#include <QApplication>
#include <QDebug>
#include <QPointer>
#include <QThread>

#include <utility>
#include <optional>

namespace gost {

// Runtime владеет сервисами плагина; контроллеры и окна обращаются к ним по заимствованным ссылкам.
// Поля объявлены так, чтобы контроллеры уничтожались раньше сервисов, которые они используют.
// Удаление Runtime также отменяет адресованные ему отложенные вызовы Qt. Dock принадлежит OBS
// и только отслеживается через QPointer; ManagementWindow удаляется явно до StateStore и SettingsStore.
class PluginLifecycle::Runtime final : public QObject {
public:
	// Создаёт три независимые области отмены с суммарным REST-бюджетом 4/32.
	// frontend переживает Runtime; options и text копируются в необходимые значения сервисов/presenter.
	explicit Runtime(Frontend &frontend, const PluginOptions &options, TextLookup text)
		: logSink(options.logSink),
		  presentationFrontend(frontend),
		  notificationText(std::move(text))
	{
		stateStore = std::make_unique<StateStore>();
		settings = std::make_unique<SettingsStore>(options.settingsFile);
		// observationLimits оставляет наблюдению два активных запроса и шестнадцать ожидающих.
		api::Limits observationLimits;
		observationLimits.maxActiveRequests = 2;
		observationLimits.maxQueuedRequests = 16;
		apiClient = std::make_unique<api::APIClient>(observationLimits);
		// commandLimits выделяет workflow отдельную последовательную очередь, независимую от recovery SSE.
		api::Limits commandLimits;
		commandLimits.maxActiveRequests = 1;
		commandLimits.maxQueuedRequests = 8;
		commandApi = std::make_unique<api::APIClient>(commandLimits);
		// configurationLimits отделяет сохранение формы от завершения команды и восстановления SSE.
		api::Limits configurationLimits;
		configurationLimits.maxActiveRequests = 1;
		configurationLimits.maxQueuedRequests = 8;
		configurationApi = std::make_unique<api::APIClient>(configurationLimits);
		eventClient = std::make_unique<api::EventClient>();
		connection = std::make_unique<ConnectionController>(*stateStore, *settings, *apiClient, *eventClient);
		controller = std::make_unique<OBSController>(frontend, *stateStore);
		restream = std::make_unique<RestreamController>(*stateStore, *commandApi, *connection, *controller);
		configuration = std::make_unique<ConfigurationController>(*stateStore, *configurationApi, *connection,
									  *restream);
		// Оба контроллера публикуют отказ после проверки владельца шага; наблюдение StateStore окна не создаёт.
		connect(restream.get(), &RestreamController::operationFailed, this,
			[this](const OperationFailure &failure) { presentOperationFailure(failure); });
		connect(configuration.get(), &ConfigurationController::operationFailed, this,
			[this](const OperationFailure &failure) { presentOperationFailure(failure); });
		connect(controller.get(), &OBSController::profileObserved, connection.get(),
			&ConnectionController::observeProfile);
		// Изменение предпочтений действует на будущие HTTP-задания всех трёх областей.
		connect(settings.get(), &SettingsStore::changed, this, [this] { applyClientPreferences(); });
		applyClientPreferences();
		// diagnostic принимает только закрытые категории и числа, без адресов, payload и серверных сообщений.
		const api::DiagnosticCallback diagnostic = [this](const api::HttpDiagnostic &event) {
			logHttp(event);
		};
		apiClient->setDiagnosticCallback(diagnostic);
		commandApi->setDiagnosticCallback(diagnostic);
		configurationApi->setDiagnosticCallback(diagnostic);
		// Изменение состояния связи вызывает logConnection; повтор той же фазы не создаёт запись.
		connect(stateStore.get(), &StateStore::serverStateChanged, this, [this] { logConnection(); });
		// Изменение workflow передаётся logOperation без повторных записей при обновлении метрик.
		connect(stateStore.get(), &StateStore::operationsChanged, this, [this] { logOperation(); });
		// Результат редактора передаётся logConfiguration без записи содержимого полей.
		connect(stateStore.get(), &StateStore::configurationEditorChanged, this,
			[this] { logConfiguration(); });
		logConnection();
	}
	// Прекращает workflow раньше OBS и сети; удаление скрытого окна предшествует удалению StateStore.
	~Runtime() override
	{
		shutdownPresenter();
		configuration->shutdown();
		restream->shutdown();
		connection->shutdown();
		// Окно хранит ссылки на общее состояние, поэтому удаляется до уничтожения сервисов.
		delete managementWindow.data();
	}

	// Запрещает новые уведомления до отмены сервисов и синхронно удаляет принадлежащее Runtime окно.
	void shutdownPresenter()
	{
		presenterEnabled = false;
		delete operationErrorDialog.data();
	}

	// Принимает только отказ нынешнего сервера/профиля/рабочего выбора, пишет его один раз в обоих
	// режимах журнала и заменяет содержимое единственного окна. Payload в истории не сохраняется.
	void presentOperationFailure(const OperationFailure &failure)
	{
		if (!presenterEnabled || failure.context.origin != stateStore->serverOrigin() ||
		    !stateStore->profileResolved() ||
		    failure.context.profileUuid != stateStore->dockContext().profileUuid ||
		    failure.context.streamId != stateStore->selectedStreamId())
			return;
		if (lastFailureSource == failure.source && lastFailureGeneration == failure.generation &&
		    lastFailureStep == failure.stepIdentity && lastFailureContext == failure.context)
			return;
		lastFailureSource = failure.source;
		lastFailureGeneration = failure.generation;
		lastFailureStep = failure.stepIdentity;
		lastFailureContext = failure.context;
		// reason и action содержат серверный текст либо перевод доверенных локальных ключей.
		// Копии переживают вызов внешнего адаптера журнала и события показа окна.
		const auto reason = !failure.reasonKey.isEmpty()
					    ? notificationText(failure.reasonKey.toUtf8().constData())
				    : failure.reason.isEmpty() ? notificationText("Operation.ErrorUnknownReason")
							       : failure.reason;
		const auto action = failure.actionKey.isEmpty()
					    ? failure.action
					    : notificationText(failure.actionKey.toUtf8().constData());
		// message содержит причину отказа и полученную диагностику, без запроса, конфигурации или успешного тела.
		QString message = QStringLiteral("Отказ операции: ") + failure.code + QLatin1Char('\n') + reason;
		if (!action.isEmpty())
			message += QStringLiteral("\nДействие: ") + action;
		if (!failure.field.isEmpty())
			message += QStringLiteral("\nПоле: ") + failure.field;
		// id перечисляет только проверенные декодером идентификаторы контекста отказа.
		for (auto id = failure.ids.cbegin(); id != failure.ids.cend(); ++id)
			message += QLatin1Char('\n') + id.key() + QStringLiteral(": ") + id.value();
		if (!failure.diagnosticBody.isEmpty())
			message += QStringLiteral("\nДиагностика: ") + QString::fromUtf8(failure.diagnosticBody);
		// OBS принимает C-строку: явное представление NUL сохраняет следующую часть диагностики в журнале.
		message.replace(QChar::Null, QStringLiteral("\\u0000"));
		// guard замечает удаление Runtime из внешнего callback или вложенного события Qt.
		const QPointer<Runtime> guard = this;
		writeLog(LogSeverity::Warning, message);
		if (!guard || !presenterEnabled)
			return;
		try {
			if (!operationErrorDialog)
				operationErrorDialog =
					new OperationErrorDialog(notificationText, presentationFrontend.mainWindow());
			operationErrorDialog->present(reason, action);
		} catch (...) {
			// Отказ окна не прерывает обработку исходного результата операции, уже записанного в OBS log.
			qWarning("Не удалось показать отказ операции GOST Restreamer.");
		}
	}

	// Передаёт готовое message с уровнем severity в адаптер; отказ журнала не выходит в event loop Qt.
	void writeLog(LogSeverity severity, const QString &message) const noexcept
	{
		if (!logSink)
			return;
		try {
			logSink(severity, message);
		} catch (...) {
			// Диагностический callback не меняет результат пользовательской операции при собственном отказе.
		}
	}

	// Меняет сроки новых обычных запросов, не затрагивая SSE и уже принятые задания.
	void applyClientPreferences()
	{
		// timeoutMs переводит ограниченные секунды хранилища в миллисекунды APIClient.
		const int timeoutMs = settings->httpTimeoutSeconds() * 1000;
		apiClient->setHttpTimeoutMs(timeoutMs);
		commandApi->setHttpTimeoutMs(timeoutMs);
		configurationApi->setHttpTimeoutMs(timeoutMs);
	}

	// Возвращает имя закрытого HTTP-метода без пользовательских строк.
	static QString methodName(api::HttpMethod method)
	{
		switch (method) {
		case api::HttpMethod::Get:
			return QStringLiteral("GET");
		case api::HttpMethod::Post:
			return QStringLiteral("POST");
		case api::HttpMethod::Patch:
			return QStringLiteral("PATCH");
		case api::HttpMethod::Delete:
			return QStringLiteral("DELETE");
		}
		return QStringLiteral("UNKNOWN");
	}

	// Возвращает только тип маршрута; непрозрачные ID, URL и query сюда не попадают.
	static QString routeName(api::RouteType route)
	{
		switch (route) {
		case api::RouteType::Health:
			return QStringLiteral("health");
		case api::RouteType::Capabilities:
			return QStringLiteral("capabilities");
		case api::RouteType::Snapshot:
			return QStringLiteral("snapshot");
		case api::RouteType::Stream:
			return QStringLiteral("stream");
		case api::RouteType::Destination:
			return QStringLiteral("destination");
		case api::RouteType::Profile:
			return QStringLiteral("profile");
		case api::RouteType::Group:
			return QStringLiteral("group");
		case api::RouteType::Session:
			return QStringLiteral("session");
		case api::RouteType::Command:
			return QStringLiteral("command");
		case api::RouteType::Asset:
			return QStringLiteral("asset");
		case api::RouteType::Other:
			return QStringLiteral("other");
		}
		return QStringLiteral("other");
	}

	// Пишет HTTP-отказ из event в обоих режимах; успешные запросы и числовые подробности — только в detailed.
	void logHttp(const api::HttpDiagnostic &event) const
	{
		if (!event.error && !settings->detailedLogging())
			return;
		// message строится исключительно из закрытых категорий, HTTP-статуса и числовых измерений.
		QString message = QStringLiteral("REST %1 %2: HTTP %3")
					  .arg(methodName(event.method), routeName(event.route))
					  .arg(event.status);
		if (event.error)
			message += QStringLiteral(", ошибка %1").arg(static_cast<int>(*event.error));
		if (settings->detailedLogging())
			message += QStringLiteral(", %1 мс, принято %2 байт, активно %3, ожидает %4")
					   .arg(event.durationMs)
					   .arg(event.receivedBytes)
					   .arg(event.activeRequests)
					   .arg(event.queuedRequests);
		writeLog(event.error ? LogSeverity::Warning : LogSeverity::Info, message);
	}

	// Фиксирует изменение фазы подключения без origin и текста серверной ошибки.
	void logConnection()
	{
		// status сравнивается с последним записанным состоянием, чтобы телеметрия не размножала сообщения.
		const auto status = stateStore->connectionStatus();
		if (loggedConnection == status)
			return;
		loggedConnection = status;
		writeLog(status == ConnectionStatus::Error ? LogSeverity::Warning : LogSeverity::Info,
			 QStringLiteral("Состояние подключения API: %1.").arg(static_cast<int>(status)));
	}

	// Фиксирует переходы явного workflow, не записывая его payload или полученные тексты.
	void logOperation()
	{
		// operation содержит только текущую value-модель, message служит безопасным признаком изменения.
		const auto &operation = stateStore->operations().current;
		if (!operation)
			return;
		const auto message = QStringLiteral("Операция %1: действие %2, фаза %3, результат %4.")
					     .arg(operation->generation)
					     .arg(static_cast<int>(operation->action))
					     .arg(static_cast<int>(operation->phase))
					     .arg(static_cast<int>(operation->outcome));
		if (message == loggedOperation)
			return;
		loggedOperation = message;
		writeLog(operation->outcome == OperationOutcome::Failed ||
					 operation->outcome == OperationOutcome::Unknown
				 ? LogSeverity::Warning
				 : LogSeverity::Info,
			 message);
	}

	// Записывает подтверждённый результат редактора без содержимого base/draft и ошибок полей.
	void logConfiguration()
	{
		// editor содержит результат и вид сущности; message не включает её ID или поля.
		const auto &editor = stateStore->configurationEditor();
		if (editor.outcome == ConfigurationOutcome::None) {
			loggedConfiguration.clear();
			return;
		}
		const auto message = QStringLiteral("Редактор конфигурации: вид %1, результат %2.")
					     .arg(static_cast<int>(editor.kind))
					     .arg(static_cast<int>(editor.outcome));
		if (message == loggedConfiguration)
			return;
		loggedConfiguration = message;
		writeLog(editor.outcome == ConfigurationOutcome::Saved ||
					 editor.outcome == ConfigurationOutcome::Deleted
				 ? LogSeverity::Info
				 : LogSeverity::Warning,
			 message);
	}

	// logSink и признаки последних записей переживают все принадлежащие Runtime сервисы.
	LogSink logSink;
	// presentationFrontend заимствован до shutdown и даёт родительское главное окно OBS.
	Frontend &presentationFrontend;
	// notificationText переводит подписи окна без зависимости от ManagementWindow.
	TextLookup notificationText;
	// presenterEnabled отзывается до shutdown сервисов, исключая уведомления из callbacks отмены.
	bool presenterEnabled = true;
	// operationErrorDialog отслеживает единственное окно; удаление главным окном OBS обнуляет указатель.
	QPointer<OperationErrorDialog> operationErrorDialog;
	// lastFailureSource, lastFailureGeneration, lastFailureStep и lastFailureContext описывают только
	// последний доставленный шаг для защиты от повторного сигнала; диагностического payload здесь нет.
	std::optional<OperationFailureSource> lastFailureSource;
	quint64 lastFailureGeneration = 0;
	QString lastFailureStep;
	DockContext lastFailureContext;
	// loggedConnection подавляет повторение одной фазы при поступлении метрик.
	std::optional<ConnectionStatus> loggedConnection;
	// loggedOperation и loggedConfiguration содержат только подготовленные безопасные сообщения.
	QString loggedOperation;
	QString loggedConfiguration;
	// Обратный порядок уничтожения полей освобождает контроллеры раньше их зависимостей.
	// stateStore хранит общие наблюдения, settings читает и записывает разрешённый файл предпочтений.
	std::unique_ptr<StateStore> stateStore;
	std::unique_ptr<SettingsStore> settings;
	// apiClient владеет наблюдением 2/16, commandApi — командами 1/8, configurationApi — CRUD 1/8.
	std::unique_ptr<api::APIClient> apiClient;
	std::unique_ptr<api::APIClient> commandApi;
	std::unique_ptr<api::APIClient> configurationApi;
	// eventClient обслуживает единственную подписку SSE.
	std::unique_ptr<api::EventClient> eventClient;
	// connection управляет наблюдением, controller читает OBS, restream координирует явные намерения.
	std::unique_ptr<ConnectionController> connection;
	std::unique_ptr<OBSController> controller;
	std::unique_ptr<RestreamController> restream;
	// configuration управляет серверными редакторами и завершается до заимствованных сервисов.
	std::unique_ptr<ConfigurationController> configuration;
	// dock отслеживает переданный OBS виджет; managementWindow отслеживает скрываемое расширенное окно.
	// Оба окна могут быть удалены родителем OBS; QPointer не продлевает их жизнь.
	QPointer<RestreamDock> dock;
	QPointer<ManagementWindow> managementWindow;
	// managementAction отслеживает пункт меню OBS; shutdown удаляет его до освобождения Runtime.
	// Удаление меню самим OBS обнуляет указатель и не оставляет второго владельца QAction.
	QPointer<QAction> managementAction;
};

PluginLifecycle::PluginLifecycle(Frontend &frontend, TextLookup text, PluginOptions options)
	: frontend_(frontend),
	  text_(std::move(text)),
	  options_(std::move(options))
{
}

PluginLifecycle::~PluginLifecycle()
{
	shutdown();
}

// Создаёт GUI Runtime и передаёт Dock OBS только при успешной регистрации.
// Возвращает true после регистрации или при уже загруженном Runtime; отсутствие GUI и отказ дают false.
// Все обработчики привязаны к Runtime; исключение освобождает ресурсы через shutdown.
bool PluginLifecycle::load()
{
	// application проверяет существование GUI и принадлежность вызова его потоку.
	// Локальный unique_ptr удерживает Dock до подтверждения передачи OBS. Если addDock вернул false,
	// shutdown очищает Runtime, а локальный указатель освобождает ещё не переданный виджет.
	auto *application = qobject_cast<QApplication *>(QCoreApplication::instance());
	if (shuttingDown_ || !application || QThread::currentThread() != application->thread()) {
		return false;
	}
	if (runtime_) {
		return true;
	}
	try {
		runtime_ = std::make_unique<Runtime>(frontend_, options_, text_);
		// dock удерживает виджет до успешной передачи Frontend; отказ регистрации не оставляет утечки.
		auto dock = std::make_unique<RestreamDock>(*runtime_->stateStore, text_, nullptr, options_.version,
							   runtime_->restream.get(), runtime_->connection.get());
		runtime_->dock = dock.get();
		// Маршрут Details передаёт только исходный context и Destination ID, без требования Session.
		connect(dock.get(), &RestreamDock::destinationDetailsRequested, runtime_.get(),
			[this](DockContext context, QString destinationId) {
				if (!runtime_ || runtime_->stateStore->dockContext() != context)
					return;
				openManagementWindow();
				if (runtime_ && runtime_->managementWindow &&
				    runtime_->stateStore->dockContext() == context)
					runtime_->managementWindow->selectDestination(context, destinationId);
			});
		// Прежний сигнал остаётся адаптером для точных IDs исходной Session и Output.
		connect(dock.get(), &RestreamDock::outputDetailsRequested, runtime_.get(),
			[this](DockContext context, QString sessionId, QString outputId) {
				openManagementWindow();
				if (runtime_ && runtime_->managementWindow)
					runtime_->managementWindow->selectOutput(context, sessionId, outputId);
			});
		if (!frontend_.addDock(DockId, text_("Plugin.Name"), dock.get())) {
			shutdown();
			return false;
		}
		// Только успешная регистрация передаёт виджет OBS; при отказе его удаляет локальный unique_ptr.
		dock.release();
		dockRegistered_ = true;
		runtime_->managementAction = frontend_.addToolsMenuAction(text_("Plugin.Name"));
		if (!runtime_->managementAction) {
			shutdown();
			return false;
		}
		runtime_->managementAction->setObjectName(QStringLiteral("gostRestreamerManagementAction"));
		// Пункт «Сервис» открывает прежнее единственное окно; удаление Runtime отключает обработчик.
		connect(runtime_->managementAction.data(), &QAction::triggered, runtime_.get(),
			[this] { openManagementWindow(); });
		frontend_.addEventCallback(&PluginLifecycle::frontendEvent, this);
		callbackRegistered_ = true;
		return true;
	} catch (...) {
		qWarning("Не удалось создать интерфейс GOST Restreamer.");
		shutdown();
		return false;
	}
}

void PluginLifecycle::shutdown() noexcept
{
	if (shuttingDown_) {
		return;
	}
	// Запрет новых действий устанавливается до очистки, которая сама может вызвать сигналы Qt.
	shuttingDown_ = true;
	Q_ASSERT(!runtime_ || QThread::currentThread() == thread());
	if (runtime_) {
		// Окно и доставка отказов завершаются до сервисов и любых callbacks отмены.
		runtime_->shutdownPresenter();
		// Останавливаем чтение OBS и сетевые обновления, пока общее состояние ещё существует.
		runtime_->configuration->shutdown();
		runtime_->restream->shutdown();
		runtime_->controller->shutdown();
		runtime_->connection->shutdown();
	}
	if (callbackRegistered_) {
		callbackRegistered_ = false;
		try {
			frontend_.removeEventCallback(&PluginLifecycle::frontendEvent, this);
		} catch (...) {
			qWarning("Не удалось снять обработчик событий GOST Restreamer.");
		}
	}
	// Пункт исчезает из меню синхронно, пока ещё допустима работа с GUI OBS.
	// QPointer также допускает, что родитель уже удалил меню вместе с главным окном.
	if (runtime_)
		delete runtime_->managementAction.data();
	// Обработчик OBS снят до удаления Dock; его сигналы также не должны повторно открыть окно.
	if (runtime_ && runtime_->dock) {
		QObject::disconnect(runtime_->dock.data(), nullptr, nullptr, nullptr);
	}
	if (dockRegistered_) {
		dockRegistered_ = false;
		try {
			frontend_.removeDock(DockId);
		} catch (...) {
			qWarning("Не удалось удалить Dock GOST Restreamer.");
		}
	}
	// Освобождаем рабочие ресурсы синхронно; оболочка остаётся неактивной до выгрузки модуля.
	runtime_.reset();
}

// Показывает одно окно; вложенные события показа могут завершить Runtime и удалить виджет.
void PluginLifecycle::openManagementWindow()
{
	Q_ASSERT(QThread::currentThread() == thread());
	if (shuttingDown_ || !runtime_) {
		return;
	}
	try {
		// Закрытие ManagementWindow скрывает его; QPointer дополнительно учитывает удаление главным окном OBS.
		if (!runtime_->managementWindow) {
			runtime_->managementWindow = new ManagementWindow(
				*runtime_->stateStore, text_, frontend_.mainWindow(), runtime_->connection.get(),
				runtime_->restream.get(), runtime_->configuration.get());
		}
		// window замечает удаление родителем OBS или shutdown во вложенном подтверждении showEvent.
		const QPointer<ManagementWindow> window = runtime_->managementWindow;
		if (window->isMinimized()) {
			window->setWindowState(window->windowState() & ~Qt::WindowMinimized);
		}
		if (!window)
			return;
		window->show();
		if (window)
			window->raise();
		if (window)
			window->activateWindow();
	} catch (...) {
		qWarning("Не удалось открыть окно управления GOST Restreamer.");
	}
}

StateStore *PluginLifecycle::stateStore() const noexcept
{
	return runtime_ ? runtime_->stateStore.get() : nullptr;
}

SettingsStore *PluginLifecycle::settingsStore() const noexcept
{
	return runtime_ ? runtime_->settings.get() : nullptr;
}

ConnectionController *PluginLifecycle::connectionController() const noexcept
{
	return runtime_ ? runtime_->connection.get() : nullptr;
}

// Возвращает фасад текущего Runtime без передачи владения; закрытие ManagementWindow его не уничтожает.
RestreamController *PluginLifecycle::restreamController() const noexcept
{
	return runtime_ ? runtime_->restream.get() : nullptr;
}

// Возвращает текущий контроллер редакторов без передачи владения вызывающей стороне.
ConfigurationController *PluginLifecycle::configurationController() const noexcept
{
	return runtime_ ? runtime_->configuration.get() : nullptr;
}

void PluginLifecycle::frontendEvent(FrontendEvent event, void *cookie) noexcept
{
	auto *self = static_cast<PluginLifecycle *>(cookie);
	// OBS хранит заимствованный cookie; оболочка остаётся жива до снятия обработчика и выгрузки модуля.
	// Исключения обработчика перехватываются здесь, чтобы не пересечь границу C API.
	try {
		self->handleEvent(event);
	} catch (...) {
		qWarning("Не удалось обработать событие OBS в GOST Restreamer.");
	}
}

void PluginLifecycle::handleEvent(FrontendEvent event, bool reliableProfile)
{
	// EXIT является последней разрешённой фазой Frontend API. Остальные события можно поставить
	// в очередь GUI, но тогда имя/путь/inventory уже нельзя выдавать за снимок момента исходного callback.
	if (shuttingDown_ || !runtime_) {
		return;
	}
	if (event == FrontendEvent::Exit) {
		// OBS отправляет EXIT из GUI-потока до уничтожения Frontend API.
		// Откладывать очистку нельзя: OBS отключает Frontend API сразу после рассылки EXIT.
		shutdown();
		return;
	}
	if (QThread::currentThread() != thread()) {
		// Контекст Runtime исключает доставку после его удаления. Перенесённое событие уже не даёт
		// достоверного снимка профиля на момент исходного вызова, поэтому reliableProfile сбрасывается.
		QMetaObject::invokeMethod(
			runtime_.get(),
			[this, event] {
				try {
					handleEvent(event, false);
				} catch (...) {
					qWarning("Не удалось обработать отложенное событие OBS.");
				}
			},
			Qt::QueuedConnection);
		return;
	}
	runtime_->controller->handleEvent(event, reliableProfile);
}

} // namespace gost
