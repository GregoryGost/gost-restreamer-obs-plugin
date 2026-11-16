#include "fixtures/fake-frontend.hpp"
#include "fixtures/api-json.hpp"
#include "fixtures/http-server.hpp"
#include "fixtures/workspace-temp.hpp"
#include "core/plugin-lifecycle.hpp"
#include "core/connection-controller.hpp"
#include "core/restream-controller.hpp"
#include "obs/obs-controller.hpp"
#include "core/settings-store.hpp"
#include "core/state-store.hpp"
#include "ui/management-window.hpp"
#include "ui/restream-dock.hpp"
#include "ui/status-label.hpp"
#include "api/api-client.hpp"
#include "api/event-client.hpp"
#include "obs/obs-stats-collector.hpp"

#include <QAbstractItemView>
#include <QAbstractTextDocumentLayout>
#include <QApplication>
#include <QCheckBox>
#include <QComboBox>
#include <QCoreApplication>
#include <QEvent>
#include <QElapsedTimer>
#include <QLineEdit>
#include <QLabel>
#include <QListView>
#include <QListWidget>
#include <QMap>
#include <QFile>
#include <QMessageBox>
#include <QMenu>
#include <QAction>
#include <QAbstractButton>
#include <QPalette>
#include <QPixmap>
#include <QPushButton>
#include <QScopeGuard>
#include <QSettings>
#include <QSignalSpy>
#include <QScrollBar>
#include <QScrollArea>
#include <QSpinBox>
#include <QStackedWidget>
#include <QTabWidget>
#include <QTest>
#include <QTextBlock>
#include <QTextBoundaryFinder>
#include <QTextDocument>
#include <QTextLayout>
#include <QTextOption>
#include <QTextTable>
#include <QToolButton>
#include <QTimer>

#include <algorithm>
#include <utility>
#include <thread>

namespace {

gost::TextLookup localeLookup(const QString &locale = QStringLiteral("ru-RU"))
{
	// Словарь копируется из файлов поставки, поэтому отсутствие реально используемого ключа
	// проваливает тест сразу. Возвращаемая функция владеет строками и не зависит от QSettings.
	QSettings settings(QStringLiteral(GOST_TEST_LOCALE_DIR) + QLatin1Char('/') + locale + QStringLiteral(".ini"),
			   QSettings::IniFormat);
	QMap<QString, QString> strings;
	for (const auto &key : settings.allKeys())
		strings.insert(key, settings.value(key).toString());
	return [strings = std::move(strings)](const char *key) {
		const auto found = strings.constFind(QString::fromUtf8(key));
		if (found == strings.cend() || found->isEmpty()) {
			QTest::qFail("Отсутствует используемый перевод интерфейса.", __FILE__, __LINE__);
			return QString::fromUtf8(key);
		}
		return *found;
	};
}

QList<gost::ManagementWindow *> managementWindows()
{
	QList<gost::ManagementWindow *> result;
	for (auto *widget : QApplication::allWidgets()) {
		if (auto *window = dynamic_cast<gost::ManagementWindow *>(widget))
			result.append(window);
	}
	return result;
}

void dispatchQueuedEvents()
{
	// Явно выполняем отложенные вызовы и удаления, чтобы проверить очистку без ожидания по таймеру.
	QCoreApplication::sendPostedEvents(nullptr, QEvent::MetaCall);
	QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
}

QString displayedText(const QLabel *label)
{
	if (label->textFormat() == Qt::PlainText)
		return label->text();
	QTextDocument document;
	document.setHtml(label->text());
	return document.toPlainText();
}

// Читает видимый текст только статистики входа label, игнорируя добавленный представлением U+200B.
// Остальные управляющие символы и исходные данные не нормализуются; геометрия проверяется по исходному HTML.
QString ingestStatisticsText(const QLabel *label)
{
	// visible содержит полный текст этой карточки; удаляются только разрешённые точки переноса.
	auto visible = displayedText(label);
	visible.remove(QChar(0x200b));
	return visible;
}

// Находит expected после from в документе статистики входа, сохраняя реальные позиции UTF-16.
// Удаление U+200B из копии текста служит только поиску: document и его раскладка остаются неизменными.
QTextCursor findIngestText(QTextDocument &document, const QString &expected, const QTextCursor &from = {})
{
	// minimumPosition исключает совпадения перед уже найденной подписью.
	const int minimumPosition = from.isNull() ? 0 : from.selectionEnd();
	if (expected.isEmpty())
		return {};
	// block перебирает настоящие абзацы документа, сохраняя их абсолютные смещения.
	for (auto block = document.begin(); block.isValid(); block = block.next()) {
		// source содержит исходные символы абзаца вместе с точками переноса.
		const auto source = block.text();
		// visible содержит только копию для поиска, positions связывает её символы с source.
		QString visible;
		QList<int> positions;
		// index адресует кодовую единицу UTF-16; пары суррогатов сохраняют обе исходные позиции.
		for (int index = 0; index < source.size(); ++index) {
			if (source.at(index) == QChar(0x200b))
				continue;
			visible.append(source.at(index));
			positions.append(index);
		}
		// match перебирает буквальные совпадения без изменения кодека, пробелов или знаков HTML.
		for (auto match = visible.indexOf(expected); match >= 0; match = visible.indexOf(expected, match + 1)) {
			if (block.position() + positions.at(match) < minimumPosition)
				continue;
			// cursor выделяет всё совпадение в настоящем документе, включая внутренние U+200B.
			QTextCursor cursor(&document);
			cursor.setPosition(block.position() + positions.at(match));
			cursor.setPosition(block.position() + positions.at(match + expected.size() - 1) + 1,
					   QTextCursor::KeepAnchor);
			return cursor;
		}
	}
	return {};
}

// Возвращает обе видимые колонки локальных метрик Dock для проверки показанных значений.
QString obsStatisticsText(const gost::RestreamDock &dock)
{
	// result собирает обе видимые колонки; name адресует одну из них.
	QStringList result;
	for (const auto *name : {"obsStatistics", "obsDropStatistics"}) {
		// label содержит очередную часть; отсутствие обязательного элемента проваливает сценарий.
		const auto *label = dock.findChild<QLabel *>(QString::fromLatin1(name));
		if (!label) {
			QTest::qFail("Отсутствует часть локальной статистики OBS.", __FILE__, __LINE__);
			return {};
		}
		result.append(displayedText(label));
	}
	return result.join(QLatin1Char('\n'));
}

// Читает байты файла path для сравнения состояния диска до и после действий формы.
QByteArray settingsFileBytes(const QString &path)
{
	// file открывает только сценарный документ; ошибка даёт пустые байты и обнаруживается сравнением с исходным файлом.
	QFile file(path);
	return file.open(QIODevice::ReadOnly) ? file.readAll() : QByteArray{};
}

// Обслуживает полный read-only handshake request без выбранного Stream или медиакоманд.
void replyConnectionRead(const HttpServer::Request &request)
{
	if (request.target.startsWith("/api/v1/health/ready"))
		HttpServer::reply(request, R"({"status":"ready","reason":""})");
	else if (request.target.startsWith("/api/v1/system/capabilities"))
		HttpServer::reply(request, fixture::capabilities());
	else if (request.target.startsWith("/api/v1/streams"))
		HttpServer::reply(request, fixture::page({fixture::stream()}));
	else if (request.target.startsWith("/api/v1/events/snapshot"))
		HttpServer::reply(request, R"({"cursor":"settings-anchor","sessions":[],"gpu":null})");
	else if (request.target.startsWith("/api/v1/events?"))
		HttpServer::stream(request, ": heartbeat\n\n");
	else
		HttpServer::reply(request, "{}", 404);
}

// Предоставляет Dock настоящие контроллеры и локальный сервер, не подключая наблюдатель к backend.
// Это позволяет проверять UI-намерение и точный POST без имитации успешного завершения команды.
class OutputDockRig final {
public:
	// Создаёт заимствованные зависимости в порядке, который сохраняет их до удаления Dock и исполнителя.
	OutputDockRig()
		: settings(temp.filePath(QStringLiteral("settings.json"))),
		  connection(state, settings, observer, events),
		  obs(frontend, state),
		  commands(state, commandApi, connection, obs, gost::RestreamController::Limits{500, 5, 30})
	{
		// request проходит настоящие предварительные GET; POST получает отказ без успешной квитанции.
		server.handler = [this](const auto &request) {
			if (request.method == "GET" && request.target == "/api/v1/sessions/session") {
				HttpServer::reply(request, fixture::bytes(fixture::session()));
				return;
			}
			if (request.method == "GET" && (request.target == "/api/v1/sessions/session/outputs/one" ||
							request.target == "/api/v1/sessions/session/outputs/two")) {
				// id адресует ровно запрошенный Output; metadata задаёт его текущее сохранённое намерение.
				const auto id =
					QString::fromUtf8(request.target.mid(request.target.lastIndexOf('/') + 1));
				const auto metadata = state.dockServer().outputs.value(id);
				// output содержит полный серверный DTO и явно отсутствующее runtime-наблюдение.
				auto output = fixture::output();
				// destination отражает точную связь сценарных метаданных, не одноимённое чужое назначение.
				auto destination = fixture::destination();
				destination.insert(QStringLiteral("id"), metadata.destinationId);
				output.insert(QStringLiteral("destination"), destination);
				output.insert(QStringLiteral("id"), id);
				output.insert(QStringLiteral("attached"), metadata.attached);
				output.insert(QStringLiteral("desired_running"), metadata.desiredRunning);
				output.insert(QStringLiteral("runtime"), QJsonValue::Null);
				output.insert(QStringLiteral("runtime_availability"), QStringLiteral("unavailable"));
				output.insert(QStringLiteral("runtime_reason"), QString{});
				HttpServer::reply(request, fixture::bytes(output));
				return;
			}
			HttpServer::reply(request, "{}", 503);
		};
	}
	// Завершает workflows до сетевых зависимостей, не оставляя запросов следующему сценарию.
	~OutputDockRig()
	{
		commands.shutdown();
		connection.shutdown();
		obs.shutdown();
	}
	// Устанавливает полные сохранённые метаданные двух прикреплённых выходов без runtime.
	bool initialize()
	{
		if (!temp.isValid() || !server.listen())
			return false;
		settings.setAutoConnect(false);
		state.setConnection(server.origin().toString(), gost::ConnectionStatus::Connected);
		// stream нужен только для явного выбранного контекста, имя не участвует в адресации команд.
		gost::api::Stream stream;
		stream.id = QStringLiteral("stream");
		stream.name = QStringLiteral("Selected");
		state.setStreams({stream}, true);
		state.setSelection(stream.id, true, {}, {}, QStringLiteral("profile"));
		// metadata задаёт независимые намерения Outputs и полный пустой каталог дополнительных назначений.
		gost::DockServerState metadata;
		metadata.sessionStatus = {gost::RuntimeAvailability::Fresh, true, {}};
		metadata.session = gost::DockSession{QStringLiteral("session"), QStringLiteral("RUNNING"), false};
		metadata.outputsStatus = {gost::RuntimeAvailability::Fresh, true, {}};
		metadata.destinationsStatus = {gost::RuntimeAvailability::Fresh, true, {}};
		metadata.outputs.insert(QStringLiteral("one"),
					{QStringLiteral("Первый"), true, true, QStringLiteral("dest-one")});
		metadata.outputs.insert(QStringLiteral("two"),
					{QStringLiteral("Второй"), true, false, QStringLiteral("dest-two")});
		state.setDockServer(metadata);
		return true;
	}
	// Возвращает только полученные медиакоманды в их фактическом порядке.
	QList<HttpServer::Request> posts() const
	{
		// result содержит команды, request перебирает ограниченный журнал локального сервера.
		QList<HttpServer::Request> result;
		for (const auto &request : server.requests)
			if (request.method == "POST")
				result.append(request);
		return result;
	}
	// temp ограничивает все сценарные файлы каталогом workspace.
	WorkspaceTemp temp{QStringLiteral("dock-output-menu")};
	// server принимает только локальные запросы на автоматически выбранном порту.
	HttpServer server;
	// state содержит подтверждённые сценарные метаданные и наблюдаемый результат пользовательской команды.
	gost::StateStore state;
	// settings хранит только локальные предпочтения сценария.
	gost::SettingsStore settings;
	// observer/events существуют для ConnectionController, но подключение в этой фикстуре не запускается.
	gost::api::APIClient observer;
	gost::api::EventClient events;
	// connection предоставляет исполнителю штатное перечитывание, не заменяя API команд.
	gost::ConnectionController connection;
	// frontend моделирует интерфейс OBS; obs выполняет штатный контракт остановки при необходимости.
	FakeFrontend frontend;
	gost::OBSController obs;
	// commandApi обслуживает реальные POST, commands повторно проверяет скопированные UI-цели.
	gost::api::APIClient commandApi;
	gost::RestreamController commands;
};

} // namespace

// QApplication/offscreen проверяет настоящее владение Qt, сигналы, виджеты и цикл событий при
// управляемом Frontend. Сценарии API используют одноразовый HttpServer и WorkspaceTemp.
// Это не загрузка DLL в OBS и не доказательство нативного фокуса, DPI или вида окна; для них нужны отдельные проверки.
class PluginLifecycleTest final : public QObject {
	Q_OBJECT

private slots:
	// Общая кнопка Start фиксирует весь показанный каталог без дополнительного диалога.
	void outputStartHeaderUsesShownCatalogWithoutConfirmation();
	// Задаёт обе локали и изменение исходного контекста Details.
	void catalogDetailsUseDestinationWithoutSession_data();
	// Details использует исходный Destination без Session и не дублирует старый Output-сигнал.
	void catalogDetailsUseDestinationWithoutSession();
	// Задаёт две поставляемые локализации явных действий расширенных параметров.
	void advancedSettingsDraftRequiresSaveAndCancel_data();
	// Черновик переживает обновления и скрытие; Cancel локален, Save применяет весь набор без переподключения.
	void advancedSettingsDraftRequiresSaveAndCancel();
	// Задаёт локализованные результаты отказа диска и успешного повторного сохранения.
	void advancedSettingsFailedSaveCanRetryWithoutEditing_data();
	// Отказ сохраняет рабочую RAM, Cancel возвращает её, повторный Save пишет неизменённый набор.
	void advancedSettingsFailedSaveCanRetryWithoutEditing();
	// Без контроллера расширенные параметры и оба действия остаются недоступными.
	void advancedSettingsWithoutControllerAreDisabled();
	// Проверяет обе локализации формы адреса и локальной ошибки.
	void settingsDraftRequiresSaveAndCancelIsLocal_data();
	// Телеметрия, неверный Save и Cancel не сохраняют черновик и не прерывают действующее подключение.
	void settingsDraftRequiresSaveAndCancelIsLocal();
	// Save канонизирует адрес, подтверждает смену сервера одним нажатием и сохраняет расширенные параметры отдельно.
	void settingsSaveAppliesChangedOriginWithoutReconnectForEquivalentUrl();
	// Терминальный отказ и штатный 204 разрешают явное повторное сохранение того же адреса.
	void settingsSaveRetriesSameOriginAfterTerminalStop_data();
	// Повторное сохранение запускает новый handshake, сохраняя автоматическую терминальную политику.
	void settingsSaveRetriesSameOriginAfterTerminalStop();
	// Задаёт подтверждённое отсутствие эфира, завершённую Session и независимые наблюдения OBS.
	void dockOmitsStartupGuidance_data();
	// Проверяет отсутствие подсказки запуска и отчёта при разных состояниях OBS и Session.
	void dockOmitsStartupGuidance();
	// Задаёт две терминальные причины отказа, остающиеся в общей модели команд.
	void commandFailureRemainsInStateStore_data();
	// Результат и причина отказа остаются в StateStore без блока отчёта в Dock.
	void commandFailureRemainsInStateStore();
	// До первого события StateStore не выдаёт неизвестные наблюдения OBS за полученные значения.
	void stateStartsUnobserved();
	// Сигнал изменения OBS доставляется после установки новых значений состояния.
	void stateChangesAreObservable();
	// Загрузка регистрирует Dock и callback без раннего чтения неподготовленного Frontend.
	void registersBeforeFrontendReady();
	// Задаёт уже работающий эфир и неактивный OBS при загрузке плагина.
	void observesCurrentProfileAndStreaming_data();
	// Готовность и события OBS перечитывают фактические профиль и активность штатного выхода.
	void observesCurrentProfileAndStreaming();
	// Расчёт скорости использует монотонный интервал и сохраняет независимые пары счётчиков кадров.
	void obsStatisticsUseMonotonicIntervalsAndIndependentFramePairs();
	// Замена выхода, регресс счётчика и переходные фазы разрывают неподтверждённую скорость передачи.
	void obsStatisticsLoseTheRateAtOutputReplacementAndTransitions();
	// EXIT синхронно освобождает статистику и запрещает поздние обращения к OBS.
	void obsStatisticsStopBeforeExitReturns();
	// Задаёт обе локализации совместного отображения наблюдений.
	void bothViewsFollowObservations_data();
	// Dock и диагностика отражают общий StateStore; имя профиля остаётся буквальным текстом диагностики.
	void bothViewsFollowObservations();
	// Задаёт все фазы подключения и отсутствие данных в обеих поставляемых локалях.
	void dockConnectionStatusUsesUppercase_data();
	// Только шапка Dock использует верхний регистр, сохраняя смысл, PlainText и отсутствие индикатора.
	void dockConnectionStatusUsesUppercase();
	// Цвет статуса реагирует на изменение смысла и палитры приложения.
	void statusAppearanceFollowsStateAndPalette();
	// Задаёт обе локализации повторного открытия единственного окна управления.
	void managementWindowIsLazyAndReused_data();
	// Окно создаётся лениво, переживает закрытие и повторно используется без новых общих сервисов.
	void managementWindowIsLazyAndReused();
	// Компактный Dock не содержит настройки и не превращается в окно управления.
	void dockStaysCompact();
	// Задаёт обе локали постоянного списка назначений и независимых Output.
	void outputRowsKeepDestinationAndExecutionIdentities_data();
	// Каталог дополняет фактические выходы без схлопывания имени, ID назначения или истории снимка.
	void outputRowsKeepDestinationAndExecutionIdentities();
	// Задаёт свежесть, полноту и намерение выхода для проверки четырёх поддерживаемых действий.
	void outputMenuUsesDurableMetadata_data();
	// Меню доступно без runtime и разрешает только подтверждённые действия текущего Output.
	void outputMenuUsesDurableMetadata();
	// Задаёт команды и смену цели или удаление Dock во вложенном цикле событий.
	void outputMenuKeepsOriginalTargetAndLifetime_data();
	// Выбранное действие не перенаправляется на новую строку, Session или удалённый Dock.
	void outputMenuKeepsOriginalTargetAndLifetime();
	// Задаёт остановку и перезапуск показанного набора с подтверждением либо отменой.
	void outputBulkConfirmationFreezesShownRows_data();
	// Фиксирует все показанные строки до подтверждения Stop/Restart, включая строки за прокруткой.
	void outputBulkConfirmationFreezesShownRows();
	// Задаёт минимальную и узкую ширины в двух локалях для списка до начала эфира.
	void catalogRowsFitNarrowDockWithoutSession_data();
	// Длинные назначения сохраняют одну горизонтальную строку, доступные полные имена и внутреннюю прокрутку.
	void catalogRowsFitNarrowDockWithoutSession();
	// Задаёт три ветви Start и причины запрета каталогового запуска в обеих локалях.
	void catalogStartMenuUsesConfirmedMode_data();
	// Меню свободного Destination содержит Start и Details; запуск передаёт исполнителю точный ID.
	void catalogStartMenuUsesConfirmedMode();
	// Задаёт замену Session, контекста, строки и удаление Dock внутри каталогового меню.
	void catalogStartKeepsFrozenModeAndDestination_data();
	// Вложенный цикл не переключает create в add и не перенаправляет выбранный Destination.
	void catalogStartKeepsFrozenModeAndDestination();

	// Задаёт узкие и широкую ширины, число выходов и обе локализации карточек Dock.
	void dockCardsFitNarrowWidths_data();
	// Карточки и команды помещаются в узкий Dock без горизонтальной прокрутки или исполнения разметки имён.
	void dockCardsFitNarrowWidths();
	// Без выбранного Stream команды недоступны; клавиатура открывает управление из меню «Сервис».
	void dockActionsRemainLocalAndKeyboardAccessible();
	// Задаёт несовпадающие состояния Session и Output с ожидаемыми смысловыми цветами.
	void dockStatusAxesRemainIndependent_data();
	// OBS, вход, Session и Output сохраняют независимые статусы при смене состояния и темы.
	void dockStatusAxesRemainIndependent();
	// Локальный сброс меняет только отображаемые накопления, сохраняя исходные скорости и счётчики.
	void dockResetChangesOnlyDisplayedAccumulators();
	// Задаёт обе локализации смены выхода и регресса сетевого счётчика.
	void dockCounterPeriodsFollowIndependentSources_data();
	// Независимые источники счётчиков сохраняют собственные периоды отсчёта после сброса и замены выхода.
	void dockCounterPeriodsFollowIndependentSources();
	// Задаёт отдельное ограничение каждого штатного счётчика кадров OBS.
	void dockShowsNativeCounterLimitsIndependently_data();
	// Ограничение одного счётчика не скрывает доступные измерения остальных этапов.
	void dockShowsNativeCounterLimitsIndependently();
	// Задаёт локализованные ожидания свежих и последних известных значений.
	void dockKeepsLastValuesOnlyInTheSameContext_data();
	// Кеш последних измерений переживает обрыв только в прежнем контексте и очищается при смене идентичности.
	void dockKeepsLastValuesOnlyInTheSameContext();
	// Задаёт локализованный порядок разделов единого окна управления.
	void managementNavigationHasLocalizedPagesAndIcons_data();
	// Навигация выбирает соответствующую страницу с изображением и показывает предупреждение несохранённых настроек.
	void managementNavigationHasLocalizedPagesAndIcons();
	// Отказ регистрации Dock освобождает частично созданные ресурсы без передачи владения OBS.
	void failedDockRegistrationReleasesResources();
	// Задаёт отказы регистрации Dock, пункта меню и callback.
	void registrationExceptionReleasesAcquiredResources_data();
	// Откат загрузки освобождает только действительно приобретённые ресурсы каждого этапа.
	void registrationExceptionReleasesAcquiredResources();
	// Исключение чтения Frontend не выходит через callback C API и не блокирует последующее достоверное событие.
	void callbackExceptionDoesNotEscape();
	// EXIT синхронно удаляет сервисы и окна, сохраняя безопасную инертную оболочку для поздних событий.
	void exitCleansSynchronouslyAndLeavesInertShell();
	// Поставленное в очередь открытие окна не создаёт сервисы заново после EXIT.
	void queuedOpenCannotResurrectRuntime();
	// Отложенный сигнал QAction теряет получателя при завершении runtime.
	void queuedToolsMenuSignalCannotResurrectRuntime();
	// Событие рабочего потока, ожидающее GUI, не обращается к Frontend после EXIT.
	void queuedFrontendEventIsDiscardedOnExit();
	// Предварительное удаление окон родителем OBS не вызывает повторное освобождение при завершении плагина.
	void hostDestructionDoesNotDoubleDelete();
	// Поставляемые локализации имеют одинаковые непустые наборы ключей.
	void localesAreComplete();
	// Скрытие окна сохраняет один контроллер и подписку, а версия отображается из метаданных загрузки.
	void hiddenManagementKeepsOneSubscriptionAndMetadataVersion();
	// Большой список Streams и пачки SSE сохраняют продвижение GUI и корректные конечные данные.
	void largeStreamListAndRuntimeTrafficAllowGuiProgress();
	// Оперативное обновление сохраняет раскрытие, выделение и прокрутку списка Streams.
	void runtimeUpdatePreservesOpenStreamPopup();
	// Сохранённый Stream различает неподтверждённость и отсутствие без удаления пользовательского выбора.
	void savedStreamIsUnconfirmedUntilListIsComplete();
	// Задаёт обе локализации предупреждения о неполном списке Streams.
	void partialStreamListWarnsAndAllowsSelection_data();
	// Неполный список предупреждает пользователя и допускает явный выбор уже полученного Stream.
	void partialStreamListWarnsAndAllowsSelection();
};

// Задаёт реальные словари, чтобы кнопки и сообщения проверялись в обеих локализациях поставки.
void PluginLifecycleTest::advancedSettingsDraftRequiresSaveAndCancel_data()
{
	QTest::addColumn<QString>("locale");
	QTest::newRow("russian") << QStringLiteral("ru-RU");
	QTest::newRow("english") << QStringLiteral("en-US");
}

// Телеметрия не заменяет незавершённый ввод; единственный Save публикует полный набор и сохраняет активную SSE.
void PluginLifecycleTest::advancedSettingsDraftRequiresSaveAndCancel()
{
	// locale выбирает поставляемые подписи действий и результата сохранения.
	QFETCH(QString, locale);
	// temp и path задают единственный локальный файл, server обслуживает синтетическое подключение.
	WorkspaceTemp temp(QStringLiteral("advanced-settings-draft"));
	QVERIFY(temp.isValid());
	const auto path = temp.filePath(QStringLiteral("settings.json"));
	HttpServer server;
	QVERIFY(server.listen());
	server.handler = replyConnectionRead;
	// settings содержит применённые предпочтения, state — наблюдаемое GUI-состояние того же подключения.
	gost::SettingsStore settings(path);
	gost::StateStore state;
	// api и events выполняют настоящие асинхронные запросы только к петлевому серверу.
	gost::api::APIClient api;
	gost::api::EventClient events;
	// controller координирует SSE независимо от черновика формы.
	gost::ConnectionController controller(state, settings, api, events);
	controller.observeProfile(gost::FrontendEvent::ProfileChanged,
				  {QStringLiteral("A"), QStringLiteral("profiles/a"), {QStringLiteral("A")}, true});
	controller.connectToServer(server.origin().toString());
	QTRY_COMPARE_WITH_TIMEOUT(state.connectionStatus(), gost::ConnectionStatus::Connected, 3000);
	// text читает реальные переводы; window владеет обеими независимыми формами настроек.
	const auto text = localeLookup(locale);
	gost::ManagementWindow window(state, text, nullptr, &controller);
	window.show();
	// navigation открывает страницу; retries/timeout/log содержат только локальный ввод до Save.
	auto *navigation = window.findChild<QListWidget *>(QStringLiteral("managementNavigation"));
	auto *retries = window.findChild<QSpinBox *>(QStringLiteral("sseRejectionRetries"));
	auto *timeout = window.findChild<QSpinBox *>(QStringLiteral("httpTimeoutSeconds"));
	auto *log = window.findChild<QComboBox *>(QStringLiteral("clientLogMode"));
	// save и cancel обозначают общие действия ровно этой страницы; message остаётся доступным как QLabel.
	auto *save = window.findChild<QPushButton *>(QStringLiteral("advancedSettingsSaveButton"));
	auto *cancel = window.findChild<QPushButton *>(QStringLiteral("advancedSettingsCancelButton"));
	auto *message = window.findChild<QLabel *>(QStringLiteral("advancedSettingsMessage"));
	// url остаётся независимым черновиком другой страницы даже при Cancel или Save расширенных параметров.
	auto *url = window.findChild<QLineEdit *>(QStringLiteral("apiUrl"));
	QVERIFY(navigation && retries && timeout && log && save && cancel && message && url);
	navigation->setCurrentRow(7);
	QVERIFY(save->isVisible() && cancel->isVisible());
	QCOMPARE(save->text(),
		 locale == QStringLiteral("ru-RU") ? QStringLiteral("Сохранить") : QStringLiteral("Save"));
	QCOMPARE(cancel->text(),
		 locale == QStringLiteral("ru-RU") ? QStringLiteral("Отмена") : QStringLiteral("Cancel"));
	QVERIFY(dynamic_cast<gost::StatusLabel *>(message));
	QCOMPARE(message->contentsMargins().left(), 0);
	QCOMPARE(message->textFormat(), Qt::PlainText);
	QVERIFY(message->isHidden());
	QVERIFY(!save->isEnabled() && !cancel->isEnabled());
	QVERIFY(retries->isEnabled() && timeout->isEnabled() && log->isEnabled());
	QCOMPARE(retries->minimum(), -1);
	QCOMPARE(retries->value(), 3);
	QCOMPARE(timeout->minimum(), 5);
	QCOMPARE(timeout->maximum(), 60);
	QCOMPARE(timeout->value(), 15);
	QCOMPARE(log->currentData().toBool(), false);
	// original и requestCount фиксируют диск и готовую SSE до любых изменений черновика.
	const auto original = settingsFileBytes(path);
	const auto requestCount = server.requests.size();
	QVERIFY(!original.isEmpty());
	// changed фиксирует каждую запись, disconnected — скрытое нарушение действующего подключения.
	QSignalSpy changed(&settings, &gost::SettingsStore::changed);
	QSignalSpy disconnected(&controller, &gost::ConnectionController::explicitlyDisconnected);
	QVERIFY(changed.isValid() && disconnected.isValid());
	url->setText(QStringLiteral("http://other-server"));
	retries->setValue(-1);
	timeout->setValue(25);
	log->setCurrentIndex(1);
	state.setObsState(QStringLiteral("Updated profile"), true);
	navigation->setCurrentRow(5);
	navigation->setCurrentRow(7);
	window.hide();
	window.show();
	QCOMPARE(retries->value(), -1);
	QCOMPARE(timeout->value(), 25);
	QVERIFY(log->currentData().toBool());
	QVERIFY(save->isEnabled() && cancel->isEnabled());
	QCOMPARE(settings.sseRejectionRetries(), 3);
	QCOMPARE(settings.httpTimeoutSeconds(), 15);
	QVERIFY(!settings.detailedLogging());
	QCOMPARE(state.sseRejectionRetries(), 3);
	QCOMPARE(state.httpTimeoutSeconds(), 15);
	QVERIFY(!state.detailedLogging());
	QCOMPARE(settingsFileBytes(path), original);
	QCOMPARE(changed.count(), 0);
	QCOMPARE(url->text(), QStringLiteral("http://other-server"));
	QTest::mouseClick(cancel, Qt::LeftButton);
	QCOMPARE(retries->value(), 3);
	QCOMPARE(timeout->value(), 15);
	QVERIFY(!log->currentData().toBool());
	QVERIFY(!save->isEnabled() && !cancel->isEnabled());
	QCOMPARE(settingsFileBytes(path), original);
	QCOMPARE(changed.count(), 0);
	QCOMPARE(url->text(), QStringLiteral("http://other-server"));
	// timeoutInput позволяет проверить клавиатурный ввод без valueChanged и потери фокуса spinbox.
	auto *timeoutInput = timeout->findChild<QLineEdit *>();
	QVERIFY(timeoutInput);
	timeoutInput->setFocus();
	timeoutInput->selectAll();
	QTest::keyClicks(timeoutInput, "60");
	QCOMPARE(timeoutInput->text(), QStringLiteral("60"));
	QVERIFY(save->isEnabled() && cancel->isEnabled());
	state.setObsState(QStringLiteral("Another profile update"), false);
	QCOMPARE(timeoutInput->text(), QStringLiteral("60"));
	QCOMPARE(settings.httpTimeoutSeconds(), 15);
	QCOMPARE(settingsFileBytes(path), original);
	QTest::mouseClick(cancel, Qt::LeftButton);
	QCOMPARE(timeoutInput->text(), QStringLiteral("15"));
	QCOMPARE(changed.count(), 0);
	// Новый набор проверяет сохранение набранного срока вместе с двумя другими изменениями.
	retries->setValue(-1);
	log->setCurrentIndex(1);
	timeoutInput->setFocus();
	timeoutInput->selectAll();
	QTest::keyClicks(timeoutInput, "60");
	state.setObsState(QStringLiteral("Final profile update"), true);
	QCOMPARE(timeoutInput->text(), QStringLiteral("60"));
	QTest::mouseClick(save, Qt::LeftButton);
	QCOMPARE(changed.count(), 1);
	QCOMPARE(settings.sseRejectionRetries(), -1);
	QCOMPARE(settings.httpTimeoutSeconds(), 60);
	QVERIFY(settings.detailedLogging());
	QCOMPARE(state.sseRejectionRetries(), -1);
	QCOMPARE(state.httpTimeoutSeconds(), 60);
	QVERIFY(state.detailedLogging());
	QVERIFY(!state.settingsUnsaved());
	QVERIFY(!save->isEnabled() && !cancel->isEnabled());
	QVERIFY(message->isVisible());
	QCOMPARE(message->text(), text("Settings.AdvancedSaved"));
	QCOMPARE(message->property("statusTone").toString(), QStringLiteral("active"));
	// restored подтверждает запись всех полей и сохранение выбранного origin на диске.
	gost::SettingsStore restored(path);
	QVERIFY(restored.load());
	QCOMPARE(restored.serverOrigin(), server.origin().toString());
	QCOMPARE(restored.sseRejectionRetries(), -1);
	QCOMPARE(restored.httpTimeoutSeconds(), 60);
	QVERIFY(restored.detailedLogging());
	QCOMPARE(url->text(), QStringLiteral("http://other-server"));
	QCOMPARE(server.requests.size(), requestCount);
	QCOMPARE(disconnected.count(), 0);
	QCOMPARE(state.connectionStatus(), gost::ConnectionStatus::Connected);
	controller.shutdown();
}

// Две локализации должны одинаково отличать отказ записи от подтверждённого сохранения.
void PluginLifecycleTest::advancedSettingsFailedSaveCanRetryWithoutEditing_data()
{
	QTest::addColumn<QString>("locale");
	QTest::newRow("russian") << QStringLiteral("ru-RU");
	QTest::newRow("english") << QStringLiteral("en-US");
}

// Отказ файла не откатывает принятые предпочтения; Cancel и повтор Save используют текущую рабочую RAM.
void PluginLifecycleTest::advancedSettingsFailedSaveCanRetryWithoutEditing()
{
	// locale выбирает настоящий текст предупреждения об отказе локального сохранения.
	QFETCH(QString, locale);
	// temp/path/settings создают исходный документ, который останется неизменным после отказа записи.
	WorkspaceTemp temp(QStringLiteral("advanced-settings-write-failure"));
	QVERIFY(temp.isValid());
	const auto path = temp.filePath(QStringLiteral("settings.json"));
	gost::SettingsStore settings(path);
	QVERIFY(settings.save());
	// original фиксирует диск до установки запрета записи.
	const auto original = settingsFileBytes(path);
	// state/api/events/controller воспроизводят применение предпочтений без запуска соединения с сервером.
	gost::StateStore state;
	gost::api::APIClient api;
	gost::api::EventClient events;
	gost::ConnectionController controller(state, settings, api, events);
	// text и window предоставляют реальную локализованную форму.
	const auto text = localeLookup(locale);
	gost::ManagementWindow window(state, text, nullptr, &controller);
	window.show();
	// navigation открывает форму; retries/timeout/log задают проверяемый набор рабочих предпочтений.
	auto *navigation = window.findChild<QListWidget *>(QStringLiteral("managementNavigation"));
	auto *retries = window.findChild<QSpinBox *>(QStringLiteral("sseRejectionRetries"));
	auto *timeout = window.findChild<QSpinBox *>(QStringLiteral("httpTimeoutSeconds"));
	auto *log = window.findChild<QComboBox *>(QStringLiteral("clientLogMode"));
	// save допускает повтор той же записи; cancel отменяет только новые отличия от рабочей RAM.
	auto *save = window.findChild<QPushButton *>(QStringLiteral("advancedSettingsSaveButton"));
	auto *cancel = window.findChild<QPushButton *>(QStringLiteral("advancedSettingsCancelButton"));
	// message показывает отказ записи обычным текстом и ошибочным смысловым цветом.
	auto *message = window.findChild<QLabel *>(QStringLiteral("advancedSettingsMessage"));
	QVERIFY(navigation && retries && timeout && log && save && cancel && message);
	navigation->setCurrentRow(7);
	QVERIFY(QFile::setPermissions(path, QFileDevice::ReadOwner));
	// restorePermissions снимает запрет даже после раннего выхода из проверки, сохраняя очистку WorkspaceTemp.
	const auto restorePermissions =
		qScopeGuard([&] { QFile::setPermissions(path, QFileDevice::ReadOwner | QFileDevice::WriteOwner); });
	// changed обнаруживает лишние публикации при Cancel и несколько публикаций одного явного Save.
	QSignalSpy changed(&settings, &gost::SettingsStore::changed);
	QVERIFY(changed.isValid());
	retries->setValue(-1);
	timeout->setValue(60);
	log->setCurrentIndex(1);
	QTest::mouseClick(save, Qt::LeftButton);
	QCOMPARE(changed.count(), 1);
	QCOMPARE(settingsFileBytes(path), original);
	QCOMPARE(settings.sseRejectionRetries(), -1);
	QCOMPARE(settings.httpTimeoutSeconds(), 60);
	QVERIFY(settings.detailedLogging());
	QVERIFY(settings.unsaved());
	QCOMPARE(state.sseRejectionRetries(), -1);
	QCOMPARE(state.httpTimeoutSeconds(), 60);
	QVERIFY(state.detailedLogging());
	QVERIFY(state.settingsUnsaved());
	QVERIFY(save->isEnabled());
	QVERIFY(message->isVisible());
	QCOMPARE(message->textFormat(), Qt::PlainText);
	QCOMPARE(message->text(), text("Settings.WriteError"));
	QCOMPARE(message->property("statusTone").toString(), QStringLiteral("error"));
	retries->setValue(0);
	timeout->setValue(5);
	log->setCurrentIndex(0);
	QVERIFY(cancel->isEnabled());
	QTest::mouseClick(cancel, Qt::LeftButton);
	QCOMPARE(retries->value(), -1);
	QCOMPARE(timeout->value(), 60);
	QVERIFY(log->currentData().toBool());
	QCOMPARE(settingsFileBytes(path), original);
	QCOMPARE(changed.count(), 1);
	QVERIFY(save->isEnabled());
	QVERIFY(message->text().contains(text("Settings.Unsaved")));
	QCOMPARE(message->property("statusTone").toString(), QStringLiteral("error"));
	QVERIFY(QFile::setPermissions(path, QFileDevice::ReadOwner | QFileDevice::WriteOwner));
	QTest::mouseClick(save, Qt::LeftButton);
	QCOMPARE(changed.count(), 2);
	QVERIFY(!settings.unsaved());
	QVERIFY(!state.settingsUnsaved());
	QVERIFY(!save->isEnabled() && !cancel->isEnabled());
	QVERIFY(!message->text().contains(text("Settings.Unsaved")));
	QCOMPARE(message->text(), text("Settings.AdvancedSaved"));
	QCOMPARE(message->property("statusTone").toString(), QStringLiteral("active"));
	// restored проверяет, что повтор без правок записал именно уже действовавший набор.
	gost::SettingsStore restored(path);
	QVERIFY(restored.load());
	QCOMPARE(restored.sseRejectionRetries(), -1);
	QCOMPARE(restored.httpTimeoutSeconds(), 60);
	QVERIFY(restored.detailedLogging());
	controller.shutdown();
}

// Инертное представление не предлагает изменить значения при отсутствии применяющего контроллера.
void PluginLifecycleTest::advancedSettingsWithoutControllerAreDisabled()
{
	// state содержит defaults, window показывает только представление без сетевых и файловых зависимостей.
	gost::StateStore state;
	gost::ManagementWindow window(state, localeLookup());
	window.show();
	// navigation выбирает расширенные параметры; name адресует каждое обязательное поле или действие.
	auto *navigation = window.findChild<QListWidget *>(QStringLiteral("managementNavigation"));
	QVERIFY(navigation);
	navigation->setCurrentRow(7);
	for (const auto *name : {"sseRejectionRetries", "httpTimeoutSeconds", "clientLogMode",
				 "advancedSettingsSaveButton", "advancedSettingsCancelButton"}) {
		// widget принадлежит показанной форме и обязан быть недоступен без контроллера.
		auto *widget = window.findChild<QWidget *>(QString::fromLatin1(name));
		QVERIFY(widget);
		QVERIFY(!widget->isEnabled());
	}
}

// Задаёт реальные ресурсы двух поддерживаемых локализаций.
void PluginLifecycleTest::settingsDraftRequiresSaveAndCancelIsLocal_data()
{
	QTest::addColumn<QString>("locale");
	QTest::newRow("russian") << QStringLiteral("ru-RU");
	QTest::newRow("english") << QStringLiteral("en-US");
}

// Неверный адрес остаётся локальной ошибкой; Cancel восстанавливает рабочий origin без записи или отключения.
void PluginLifecycleTest::settingsDraftRequiresSaveAndCancelIsLocal()
{
	// locale выбирает поставляемые подписи и безопасное сообщение валидации.
	QFETCH(QString, locale);
	// temp/path изолируют preferences; server обслуживает настоящее действующее подключение.
	WorkspaceTemp temp(QStringLiteral("settings-draft"));
	QVERIFY(temp.isValid());
	// path задаёт единственный сценарный файл настроек.
	const auto path = temp.filePath(QStringLiteral("settings.json"));
	// server обслуживает синтетический handshake без медиакоманд.
	HttpServer server;
	QVERIFY(server.listen());
	server.handler = replyConnectionRead;
	// settings/state/api/events/controller сохраняют прежний рабочий адрес независимо от черновика окна.
	gost::SettingsStore settings(path);
	// state хранит рабочий origin и наблюдаемые состояния подключения.
	gost::StateStore state;
	// api выполняет асинхронные локальные REST-чтения.
	gost::api::APIClient api;
	// events выполняет локальную SSE-подписку.
	gost::api::EventClient events;
	// controller координирует действующее подключение независимо от черновика UI.
	gost::ConnectionController controller(state, settings, api, events);
	controller.observeProfile(gost::FrontendEvent::ProfileChanged,
				  {QStringLiteral("A"), QStringLiteral("profiles/a"), {QStringLiteral("A")}, true});
	controller.connectToServer(server.origin().toString());
	QTRY_COMPARE_WITH_TIMEOUT(state.connectionStatus(), gost::ConnectionStatus::Connected, 3000);
	// text переводит интерфейс, window предоставляет настоящую форму Settings.
	const auto text = localeLookup(locale);
	// window показывает настоящую форму Settings на рабочем состоянии.
	gost::ManagementWindow window(state, text, nullptr, &controller);
	window.show();
	// navigation открывает форму; url/save/cancel/message доступны по устойчивым именам виджетов.
	auto *navigation = window.findChild<QListWidget *>(QStringLiteral("managementNavigation"));
	// url содержит редактируемый черновик адреса.
	auto *url = window.findChild<QLineEdit *>(QStringLiteral("apiUrl"));
	// save явно применяет допустимый черновик или повторяет остановленное подключение.
	auto *save = window.findChild<QPushButton *>(QStringLiteral("settingsSaveButton"));
	// cancel возвращает текущий рабочий origin без сетевого действия.
	auto *cancel = window.findChild<QPushButton *>(QStringLiteral("settingsCancelButton"));
	// message показывает локализованную ошибку формы обычным текстом.
	auto *message = window.findChild<QLabel *>(QStringLiteral("settingsMessage"));
	QVERIFY(navigation && url && save && cancel && message);
	navigation->setCurrentRow(6);
	// original и requestCount фиксируют состояние диска и сети после подтверждённой подписки.
	const auto original = settingsFileBytes(path);
	// requestCount сохраняет количество запросов до редактирования формы.
	const auto requestCount = server.requests.size();
	QVERIFY(!original.isEmpty());
	// changed/disconnected обнаруживают даже повторное сохранение прежних значений и скрытое отключение.
	QSignalSpy changed(&settings, &gost::SettingsStore::changed);
	// disconnected обнаруживает нежелательное явное отключение контроллера.
	QSignalSpy disconnected(&controller, &gost::ConnectionController::explicitlyDisconnected);
	QVERIFY(changed.isValid() && disconnected.isValid());
	url->setText(QStringLiteral("http://example.invalid/draft"));
	state.setObsState(QStringLiteral("Updated profile"), true);
	state.setPreferences(true, true, 3, false, {}, {});
	QCOMPARE(url->text(), QStringLiteral("http://example.invalid/draft"));
	QVERIFY(save->isEnabled() && cancel->isEnabled());
	QTest::mouseClick(save, Qt::LeftButton);
	QVERIFY(message->isVisible());
	QVERIFY(message->text().contains(text("Error.InvalidServerUrl")));
	QCOMPARE(message->textFormat(), Qt::PlainText);
	QCOMPARE(state.serverOrigin(), server.origin().toString());
	QCOMPARE(state.connectionStatus(), gost::ConnectionStatus::Connected);
	QVERIFY(state.safeErrorKey().isEmpty());
	QCOMPARE(settings.serverOrigin(), server.origin().toString());
	QCOMPARE(settingsFileBytes(path), original);
	QCOMPARE(server.requests.size(), requestCount);
	QCOMPARE(changed.count(), 0);
	QCOMPARE(disconnected.count(), 0);
	QTest::mouseClick(cancel, Qt::LeftButton);
	QCOMPARE(url->text(), state.serverOrigin());
	QVERIFY(!message->text().contains(text("Error.InvalidServerUrl")));
	QCOMPARE(settingsFileBytes(path), original);
	QCOMPARE(server.requests.size(), requestCount);
	QCOMPARE(changed.count(), 0);
	QCOMPARE(disconnected.count(), 0);
	QCOMPARE(state.connectionStatus(), gost::ConnectionStatus::Connected);
	controller.shutdown();
}

// Эквивалентный URL сохраняет SSE, изменённый URL применяет явный Save без дополнительного диалога.
void PluginLifecycleTest::settingsSaveAppliesChangedOriginWithoutReconnectForEquivalentUrl()
{
	// temp/path изолируют настройки; first/second представляют два независимых сервера.
	WorkspaceTemp temp(QStringLiteral("settings-save"));
	QVERIFY(temp.isValid());
	// path задаёт единственный сценарный файл настроек.
	const auto path = temp.filePath(QStringLiteral("settings.json"));
	// first обслуживает исходный сервер на петлевом интерфейсе.
	HttpServer first;
	// second обслуживает новый сервер на другом локальном порту.
	HttpServer second;
	QVERIFY(first.listen() && second.listen());
	first.handler = replyConnectionRead;
	second.handler = replyConnectionRead;
	// settings/state/api/events/controller составляют рабочее подключение до открытия формы.
	gost::SettingsStore settings(path);
	// state хранит рабочий origin и наблюдаемые состояния подключения.
	gost::StateStore state;
	// api выполняет асинхронные локальные REST-чтения.
	gost::api::APIClient api;
	// events выполняет локальную SSE-подписку.
	gost::api::EventClient events;
	// controller координирует действующее подключение независимо от черновика UI.
	gost::ConnectionController controller(state, settings, api, events);
	controller.observeProfile(gost::FrontendEvent::ProfileChanged,
				  {QStringLiteral("A"), QStringLiteral("profiles/a"), {QStringLiteral("A")}, true});
	controller.connectToServer(first.origin().toString());
	QTRY_COMPARE_WITH_TIMEOUT(state.connectionStatus(), gost::ConnectionStatus::Connected, 3000);
	// window открывает реальные Settings и Advanced, сохраняя единый черновик адреса.
	gost::ManagementWindow window(state, localeLookup(), nullptr, &controller);
	window.show();
	// navigation выбирает страницу; url/save/cancel управляют адресом; retries/timeout/log сохраняются отдельно.
	auto *navigation = window.findChild<QListWidget *>(QStringLiteral("managementNavigation"));
	// url содержит редактируемый черновик адреса.
	auto *url = window.findChild<QLineEdit *>(QStringLiteral("apiUrl"));
	// save явно применяет допустимый черновик или повторяет остановленное подключение.
	auto *save = window.findChild<QPushButton *>(QStringLiteral("settingsSaveButton"));
	// cancel возвращает текущий рабочий origin без сетевого действия.
	auto *cancel = window.findChild<QPushButton *>(QStringLiteral("settingsCancelButton"));
	// retries редактирует число повторов SSE до отдельного сохранения расширенных параметров.
	auto *retries = window.findChild<QSpinBox *>(QStringLiteral("sseRejectionRetries"));
	// timeout редактирует срок REST в секундах до отдельного сохранения расширенных параметров.
	auto *timeout = window.findChild<QSpinBox *>(QStringLiteral("httpTimeoutSeconds"));
	// log выбирает обычный либо подробный режим штатного журнала.
	auto *log = window.findChild<QComboBox *>(QStringLiteral("clientLogMode"));
	// advancedSave применяет три дополнительных предпочтения независимо от черновика URL.
	auto *advancedSave = window.findChild<QPushButton *>(QStringLiteral("advancedSettingsSaveButton"));
	QVERIFY(navigation && url && save && cancel && retries && timeout && log && advancedSave);
	navigation->setCurrentRow(6);
	// firstRequests фиксирует исходную подписку; original фиксирует точные байты настроек до нормализации.
	const auto firstRequests = first.requests.size();
	// original сохраняет байты preferences до проверяемых действий формы.
	const auto original = settingsFileBytes(path);
	// status перебирает подтверждённое соединение и два незавершённых состояния того же рабочего origin.
	for (const auto status : {gost::ConnectionStatus::Connected, gost::ConnectionStatus::Connecting,
				  gost::ConnectionStatus::Reconnecting}) {
		state.setConnection(first.origin().toString(), status);
		url->setText(QStringLiteral("HTTP://127.0.0.1:%1/").arg(first.origin().port()));
		QTest::mouseClick(save, Qt::LeftButton);
		QCOMPARE(url->text(), first.origin().toString());
		QCOMPARE(state.connectionStatus(), status);
		QCOMPARE(first.requests.size(), firstRequests);
		QCOMPARE(settingsFileBytes(path), original);
	}
	state.setConnection(first.origin().toString(), gost::ConnectionStatus::Connected);
	url->setText(second.origin().toString());
	navigation->setCurrentRow(7);
	QVERIFY(retries->isEnabled() && timeout->isEnabled() && log->isEnabled());
	retries->setValue(-1);
	timeout->setValue(25);
	log->setCurrentIndex(1);
	QCOMPARE(settings.sseRejectionRetries(), 3);
	QCOMPARE(settings.httpTimeoutSeconds(), 15);
	QVERIFY(!settings.detailedLogging());
	QCOMPARE(settingsFileBytes(path), original);
	QTest::mouseClick(advancedSave, Qt::LeftButton);
	// restored перечитывает диск, исключая подтверждение только значений RAM.
	gost::SettingsStore restored(path);
	QCOMPARE(restored.serverOrigin(), first.origin().toString());
	QCOMPARE(restored.sseRejectionRetries(), -1);
	QCOMPARE(restored.httpTimeoutSeconds(), 25);
	QVERIFY(restored.detailedLogging());
	QCOMPARE(url->text(), second.origin().toString());
	navigation->setCurrentRow(6);
	QTest::mouseClick(cancel, Qt::LeftButton);
	QCOMPARE(url->text(), first.origin().toString());
	QCOMPARE(settings.sseRejectionRetries(), -1);
	QCOMPARE(settings.httpTimeoutSeconds(), 25);
	QVERIFY(settings.detailedLogging());
	// modalObserved фиксирует нежелательное дополнительное подтверждение; dismissModal закрывает его,
	// чтобы дефект завершился проверяемым отказом, а не зависанием вложенного цикла QMessageBox.
	bool modalObserved = false;
	QTimer dismissModal;
	connect(&dismissModal, &QTimer::timeout, &window, [&] {
		// dialog адресует только активный диалог тестового QApplication.
		if (auto *dialog = qobject_cast<QMessageBox *>(QApplication::activeModalWidget())) {
			modalObserved = true;
			dialog->reject();
		}
	});
	dismissModal.start(0);
	url->setText(QStringLiteral("HTTP://127.0.0.1:%1/").arg(second.origin().port()));
	QTest::mouseClick(save, Qt::LeftButton);
	QTRY_VERIFY_WITH_TIMEOUT(state.serverOrigin() == second.origin().toString() || modalObserved, 1000);
	QVERIFY(!modalObserved);
	QTRY_COMPARE_WITH_TIMEOUT(state.connectionStatus(), gost::ConnectionStatus::Connected, 3000);
	dismissModal.stop();
	QCOMPARE(url->text(), second.origin().toString());
	QVERIFY(restored.load());
	QCOMPARE(restored.serverOrigin(), second.origin().toString());
	QCOMPARE(first.requests.size(), firstRequests);
	QCOMPARE(second.requests.size(), 5);
	// server/request проходят оба журнала для проверки отсутствия управляющих медиа-запросов.
	for (const auto *server : {&first, &second})
		for (const auto &request : server->requests)
			QCOMPARE(request.method, QByteArray("GET"));
	controller.shutdown();
}

// Задаёт окончательный HTTP-отказ и штатное завершение SSE.
void PluginLifecycleTest::settingsSaveRetriesSameOriginAfterTerminalStop_data()
{
	QTest::addColumn<int>("status");
	QTest::newRow("rejected") << 403;
	QTest::newRow("ended") << 204;
}

// После terminal SSE сохранение неизменённого адреса является явным повтором и восстанавливает наблюдение.
void PluginLifecycleTest::settingsSaveRetriesSameOriginAfterTerminalStop()
{
	// status задаёт терминальный результат первой SSE-подписки.
	QFETCH(int, status);
	// temp/settings изолируют preferences, server после первой подписки обслуживает исправленное подключение.
	WorkspaceTemp temp(QStringLiteral("settings-retry"));
	QVERIFY(temp.isValid());
	// server обслуживает синтетический handshake без медиакоманд.
	HttpServer server;
	QVERIFY(server.listen());
	// subscriptions считает SSE-попытки, не смешивая их с read-only handshake.
	int subscriptions = 0;
	// Обработчик request завершает первую SSE терминально, затем допускает исправленную подписку.
	server.handler = [&](const auto &request) {
		if (request.target.startsWith("/api/v1/events?") && ++subscriptions == 1)
			HttpServer::reply(request, {}, status);
		else
			replyConnectionRead(request);
	};
	// settings читает и сохраняет preferences только сценарного файла.
	gost::SettingsStore settings(temp.filePath(QStringLiteral("settings.json")));
	// state/api/events/controller выполняют реальный переход в терминальное состояние.
	gost::StateStore state;
	// api выполняет асинхронные локальные REST-чтения.
	gost::api::APIClient api;
	// events выполняет локальную SSE-подписку.
	gost::api::EventClient events;
	// controller координирует действующее подключение независимо от черновика UI.
	gost::ConnectionController controller(state, settings, api, events);
	controller.connectToServer(server.origin().toString());
	QTRY_COMPARE_WITH_TIMEOUT(state.connectionStatus(),
				  status == 204 ? gost::ConnectionStatus::Disconnected : gost::ConnectionStatus::Error,
				  3000);
	// window показывает сохранённый адрес без редактирования; navigation открывает страницу Settings.
	gost::ManagementWindow window(state, localeLookup(), nullptr, &controller);
	window.show();
	// navigation открывает страницу Settings либо Advanced.
	auto *navigation = window.findChild<QListWidget *>(QStringLiteral("managementNavigation"));
	// url подтверждает неизменность адреса; save остаётся доступным после окончательного отказа.
	auto *url = window.findChild<QLineEdit *>(QStringLiteral("apiUrl"));
	// save явно применяет допустимый черновик или повторяет остановленное подключение.
	auto *save = window.findChild<QPushButton *>(QStringLiteral("settingsSaveButton"));
	QVERIFY(navigation && url && save);
	navigation->setCurrentRow(6);
	QCOMPARE(url->text(), server.origin().toString());
	QVERIFY(save->isEnabled());
	QCOMPARE(subscriptions, 1);
	QTest::mouseClick(save, Qt::LeftButton);
	QTRY_COMPARE_WITH_TIMEOUT(state.connectionStatus(), gost::ConnectionStatus::Connected, 3000);
	QCOMPARE(subscriptions, 2);
	QCOMPARE(url->text(), server.origin().toString());
	controller.shutdown();
}

void PluginLifecycleTest::stateStartsUnobserved()
{
	gost::StateStore state;
	QVERIFY(!state.profileName().has_value());
	QVERIFY(!state.streamingActive().has_value());
	QCOMPARE(state.thread(), qApp->thread());
}

void PluginLifecycleTest::stateChangesAreObservable()
{
	// Проверяется не только число сигналов: после каждого уведомления должны быть установлены
	// соответствующие значения. Неопределённое исходное состояние нельзя заранее считать остановленным.
	gost::StateStore state;
	QSignalSpy changes(&state, &gost::StateStore::obsStateChanged);
	QVERIFY(changes.isValid());
	state.setObsState(QStringLiteral("Профиль A"), true);
	QCOMPARE(changes.count(), 1);
	QVERIFY(state.profileName().has_value());
	QVERIFY(state.streamingActive().has_value());
	QCOMPARE(state.profileName().value(), QStringLiteral("Профиль A"));
	QVERIFY(state.streamingActive().value());
	state.setObsState(QStringLiteral("Профиль B"), false);
	QCOMPARE(changes.count(), 2);
	QCOMPARE(state.profileName().value(), QStringLiteral("Профиль B"));
	QVERIFY(!state.streamingActive().value());
}

// Меню и Dock доступны до FinishedLoading; повторный load не дублирует регистрацию и не читает OBS.
void PluginLifecycleTest::registersBeforeFrontendReady()
{
	// frontend удерживает настоящее дерево Qt и считает обращения к ещё не готовому OBS.
	FakeFrontend frontend;
	// lifecycle владеет runtime, но передаёт виджеты и QAction главному окну frontend.
	gost::PluginLifecycle lifecycle(frontend, localeLookup());
	QVERIFY(lifecycle.load());
	QCOMPARE(frontend.dockAdds, 1);
	QCOMPARE(frontend.toolsMenuAdds, 1);
	QCOMPARE(frontend.callbackAdds, 1);
	QCOMPARE(frontend.registrationOrder,
		 QStringList({QStringLiteral("dock"), QStringLiteral("toolsMenu"), QStringLiteral("callback")}));
	QCOMPARE(frontend.dockId, QStringLiteral("gost-restreamer-obs-plugin"));
	QCOMPARE(frontend.dockTitle, QStringLiteral("GOST Restreamer"));
	QCOMPARE(frontend.toolsMenuTitle, QStringLiteral("GOST Restreamer"));
	QVERIFY(frontend.activeCallback && frontend.activeCookie && frontend.content && frontend.dock);
	QVERIFY(frontend.toolsMenu && frontend.managementAction);
	QCOMPARE(frontend.toolsMenu->actions().size(), 1);
	QCOMPARE(frontend.toolsMenu->actions().front(), frontend.managementAction.data());
	QCOMPARE(frontend.managementAction->objectName(), QStringLiteral("gostRestreamerManagementAction"));
	QVERIFY(frontend.managementAction->isEnabled());
	QVERIFY(!frontend.dock->isVisible());
	QVERIFY(lifecycle.load());
	QCOMPARE(frontend.dockAdds, 1);
	QCOMPARE(frontend.toolsMenuAdds, 1);
	QCOMPARE(frontend.callbackAdds, 1);
	QVERIFY(managementWindows().isEmpty());
	frontend.managementAction->trigger();
	QCOMPARE(managementWindows().size(), 1);
	frontend.emitEvent(gost::FrontendEvent::ProfileChanged);
	frontend.emitEvent(gost::FrontendEvent::StreamingStarted);
	QCOMPARE(frontend.profileReads, 0);
	QCOMPARE(frontend.profilePathReads, 0);
	QCOMPARE(frontend.inventoryReads, 0);
	QCOMPARE(frontend.streamingReads, 0);
	QCOMPARE(frontend.statisticsReads, 0);
	QVERIFY(lifecycle.stateStore());
	QVERIFY(!lifecycle.stateStore()->profileName().has_value());
	QVERIFY(!lifecycle.stateStore()->streamingActive().has_value());
}

void PluginLifecycleTest::observesCurrentProfileAndStreaming_data()
{
	QTest::addColumn<bool>("initialStreaming");
	QTest::newRow("already-streaming") << true;
	QTest::newRow("idle") << false;
}

void PluginLifecycleTest::observesCurrentProfileAndStreaming()
{
	// Начальный OBS может уже транслировать. FinishedLoading должен прочитать текущие факты,
	// а последующие события — снова опросить адаптер, не подставить состояние из имени события.
	QFETCH(bool, initialStreaming);
	FakeFrontend frontend;
	frontend.streaming = initialStreaming;
	gost::PluginLifecycle lifecycle(frontend, localeLookup());
	QVERIFY(lifecycle.load());
	QSignalSpy changes(lifecycle.stateStore(), &gost::StateStore::obsStateChanged);
	QVERIFY(changes.isValid());
	frontend.emitEvent(gost::FrontendEvent::FinishedLoading);
	QVERIFY(lifecycle.stateStore()->profileName().has_value());
	QVERIFY(lifecycle.stateStore()->streamingActive().has_value());
	QCOMPARE(lifecycle.stateStore()->profileName().value(), frontend.profile);
	QCOMPARE(lifecycle.stateStore()->streamingActive().value(), initialStreaming);
	QCOMPARE(changes.count(), 1);
	frontend.profile = QStringLiteral("Другой профиль");
	frontend.emitEvent(gost::FrontendEvent::ProfileChanged);
	QCOMPARE(lifecycle.stateStore()->profileName().value(), QStringLiteral("Другой профиль"));
	frontend.streaming = !initialStreaming;
	frontend.emitEvent(gost::FrontendEvent::StreamingStarted);
	QCOMPARE(lifecycle.stateStore()->streamingActive().value(), !initialStreaming);
	frontend.streaming = initialStreaming;
	frontend.emitEvent(gost::FrontendEvent::StreamingStopped);
	QCOMPARE(lifecycle.stateStore()->streamingActive().value(), initialStreaming);
}

void PluginLifecycleTest::obsStatisticsUseMonotonicIntervalsAndIndependentFramePairs()
{
	// Время передаётся явно: 1000 байт за 250 мс дают 32000 бит/с без реального ожидания.
	// Повтор того же момента не допускает деление на ноль; поздняя неизменная выборка уже даёт 0 бит/с.
	// Три разные пары кадров обнаруживают случайную подстановку чужого total.
	using namespace std::chrono_literals;
	gost::OBSStatsCollector collector;
	const gost::OBSStatsCollector::Clock::time_point start{};
	gost::ObsStatisticsSample sample{.videoAvailable = true,
					 .videoIdentity = 1,
					 .outputIdentity = 2,
					 .fps = 59.5,
					 .renderTimeNs = 240000,
					 .sentBytes = 700,
					 .render = gost::FrameCounters{9, 300},
					 .encode = gost::FrameCounters{3, 120},
					 .network = gost::FrameCounters{1, 25}};
	auto observed = collector.sample(sample, true, start);
	QCOMPARE(observed.fps, std::optional{59.5});
	QCOMPARE(observed.renderTimeNs, std::optional<quint64>{240000});
	QVERIFY(!observed.bitrate);
	QVERIFY(observed.bitrateWarming);

	sample.sentBytes = 1700;
	observed = collector.sample(sample, true, start + 250ms);
	QCOMPARE(observed.bitrate, std::optional{32000.0});
	QVERIFY(observed.render && observed.encode && observed.network);
	QCOMPARE(observed.render->total, quint64{300});
	QCOMPARE(observed.render->skipped, quint64{9});
	QCOMPARE(observed.encode->total, quint64{120});
	QCOMPARE(observed.encode->skipped, quint64{3});
	QCOMPARE(observed.network->total, quint64{25});
	QCOMPARE(observed.network->skipped, quint64{1});

	observed = collector.sample(sample, true, start + 250ms);
	QVERIFY(!observed.bitrate);
	observed = collector.sample(sample, true, start + 750ms);
	QCOMPARE(observed.bitrate, std::optional{0.0});
}

void PluginLifecycleTest::obsStatisticsLoseTheRateAtOutputReplacementAndTransitions()
{
	// Замена output и регресс байтов независимо разрывают bitrate, сохраняя эпоху видео.
	// Starting/Stopping показываются до совпадения active, а отсутствие output после StreamingStarted
	// не превращается в измеренные сетевые нули.
	using namespace std::chrono_literals;
	using gost::FrontendEvent;
	using gost::ObsStreamingPhase;
	gost::OBSStatsCollector collector;
	const gost::OBSStatsCollector::Clock::time_point start{};
	gost::ObsStatisticsSample sample{.videoAvailable = true,
					 .videoIdentity = 1,
					 .outputIdentity = 2,
					 .fps = 60,
					 .sentBytes = 1000,
					 .network = gost::FrameCounters{0, 30}};
	auto observed = collector.sample(sample, false, start, FrontendEvent::StreamingStarting);
	QCOMPARE(observed.phase, ObsStreamingPhase::Starting);
	QVERIFY(!observed.sentBytes && !observed.network && !observed.bitrate);
	observed = collector.sample(sample, true, start + 100ms);
	QCOMPARE(observed.phase, ObsStreamingPhase::Streaming);
	QVERIFY(observed.bitrateWarming);
	const auto firstOutput = observed.outputEpoch;
	const auto video = observed.videoEpoch;

	sample.sentBytes = 1500;
	sample.outputIdentity = 3;
	observed = collector.sample(sample, true, start + 200ms);
	QVERIFY(observed.outputEpoch != firstOutput);
	QCOMPARE(observed.videoEpoch, video);
	QVERIFY(!observed.bitrate && observed.bitrateWarming);
	const auto replacement = observed.outputEpoch;

	sample.sentBytes = 100;
	observed = collector.sample(sample, true, start + 300ms);
	QVERIFY(observed.outputEpoch != replacement);
	QVERIFY(!observed.bitrate && observed.bitrateWarming);
	observed = collector.sample(sample, true, start + 400ms, FrontendEvent::StreamingStopping);
	QCOMPARE(observed.phase, ObsStreamingPhase::Stopping);
	observed = collector.sample(sample, false, start + 500ms);
	QCOMPARE(observed.phase, ObsStreamingPhase::Stopped);
	QVERIFY(!observed.sentBytes && !observed.network && !observed.bitrate);
	QCOMPARE(observed.fps, std::optional{60.0});

	sample.outputIdentity.reset();
	observed = collector.sample(sample, true, start + 600ms, FrontendEvent::StreamingStarted);
	QCOMPARE(observed.phase, ObsStreamingPhase::Streaming);
	QVERIFY(!observed.sentBytes && !observed.network && !observed.bitrate);
}

void PluginLifecycleTest::obsStatisticsStopBeforeExitReturns()
{
	// Проверяется синхронная граница: releaseStatistics ровно один раз до возврата Exit.
	// Затем имитируются поздний обработчик и отложенные события, пока инертная оболочка ещё жива.
	FakeFrontend frontend;
	frontend.statistics.videoAvailable = true;
	frontend.statistics.fps = 48.0;
	gost::PluginLifecycle lifecycle(frontend, localeLookup());
	QVERIFY(lifecycle.load());
	QCOMPARE(frontend.statisticsReads, 0);
	frontend.emitEvent(gost::FrontendEvent::FinishedLoading);
	QTRY_COMPARE_WITH_TIMEOUT(lifecycle.stateStore()->obsStatistics().fps, std::optional{48.0}, 1500);
	QVERIFY(frontend.statisticsReads > 0);
	QCOMPARE(frontend.statisticsReleases, 0);
	const int reads = frontend.statisticsReads;

	frontend.emitEvent(gost::FrontendEvent::Exit);
	QCOMPARE(frontend.statisticsReleases, 1);
	QVERIFY(!lifecycle.stateStore());
	frontend.lastCallback(gost::FrontendEvent::StreamingStarted, frontend.lastCookie);
	lifecycle.shutdown();
	dispatchQueuedEvents();
	QCOMPARE(frontend.statisticsReads, reads);
	QCOMPARE(frontend.statisticsReleases, 1);
	QCOMPARE(frontend.callsAfterExit, 0);
}

void PluginLifecycleTest::managementWindowIsLazyAndReused_data()
{
	QTest::addColumn<QString>("locale");
	QTest::newRow("russian") << QStringLiteral("ru-RU");
	QTest::newRow("english") << QStringLiteral("en-US");
}

void PluginLifecycleTest::bothViewsFollowObservations_data()
{
	QTest::addColumn<QString>("locale");
	QTest::newRow("russian") << QStringLiteral("ru-RU");
	QTest::newRow("english") << QStringLiteral("en-US");
}

// Проверяет общие наблюдения Dock и диагностики с отдельным регистром текста подключения.
void PluginLifecycleTest::bothViewsFollowObservations()
{
	// Оба окна используют один StateStore, даже когда ManagementWindow скрыто. Имя с HTML-подобным
	// текстом проверяет PlainText, а неизвестные сервер/Stream остаются отдельно от локального OBS.
	// locale выбирает реальные подписи и числовой формат текущей локализации.
	QFETCH(QString, locale);
	// text читает переводы из поставляемого файла локали.
	const auto text = localeLookup(locale);
	// frontend моделирует принадлежащие OBS события и владение Dock без настоящего процесса OBS.
	FakeFrontend frontend;
	// lifecycle связывает поддельный Frontend с настоящими сервисами и окнами плагина.
	gost::PluginLifecycle lifecycle(frontend, text);
	QVERIFY(lifecycle.load());
	lifecycle.openManagementWindow();
	QCOMPARE(managementWindows().size(), 1);
	// window предоставляет единое окно управления на том же общем состоянии.
	auto *window = managementWindows().front();
	// windowProfile отражает буквальное имя профиля в диагностике.
	auto *windowProfile = window->findChild<QLabel *>(QStringLiteral("obsProfile"));
	// dockStreaming показывает локальный статус OBS в Dock.
	auto *dockStreaming = frontend.content->findChild<QLabel *>(QStringLiteral("obsStreaming"));
	// windowStreaming показывает тот же локальный статус в диагностике.
	auto *windowStreaming = window->findChild<QLabel *>(QStringLiteral("obsStreaming"));
	// server показывает состояние подключения независимо от OBS.
	auto *server = frontend.content->findChild<QLabel *>(QStringLiteral("serverObservation"));
	// stream содержит имя и точный ID потока, полученного для сценария.
	auto *stream = frontend.content->findChild<QComboBox *>(QStringLiteral("streamSelector"));
	QVERIFY(windowProfile && dockStreaming && windowStreaming && server && stream);
	// label адресует очередную проверяемую подпись без изменения её состояния.
	for (auto *label : {windowProfile, dockStreaming, windowStreaming})
		QCOMPARE(label->text(), text("Observation.NoData"));
	QCOMPARE(server->text(), text("Observation.NoData").toUpper());
	QVERIFY(stream->currentData().toString().isEmpty());
	frontend.profile = QStringLiteral("<b>Профиль & OBS</b>");
	frontend.streaming = true;
	frontend.emitEvent(gost::FrontendEvent::FinishedLoading);
	QCOMPARE(windowProfile->text(), frontend.profile);
	QCOMPARE(windowProfile->textFormat(), Qt::PlainText);
	QCOMPARE(dockStreaming->text(), text("Dock.ObsStreaming"));
	QCOMPARE(windowStreaming->text(), text("OBS.StreamingActive"));
	window->close();
	frontend.profile = QStringLiteral("Переименованный профиль");
	frontend.streaming = false;
	frontend.emitEvent(gost::FrontendEvent::ProfileChanged);
	QCOMPARE(windowProfile->text(), frontend.profile);
	QCOMPARE(dockStreaming->text(), text("Dock.ObsStopped"));
	QCOMPARE(windowStreaming->text(), text("OBS.StreamingInactive"));
	QCOMPARE(server->text(), text("Observation.NoData").toUpper());
	QVERIFY(stream->currentData().toString().isEmpty());
}

// Независимые буквальные ожидания проверяют регистр каждой локализованной фазы и отсутствие origin.
void PluginLifecycleTest::dockConnectionStatusUsesUppercase_data()
{
	QTest::addColumn<QString>("locale");
	QTest::addColumn<int>("status");
	QTest::addColumn<bool>("hasOrigin");
	QTest::addColumn<QString>("expected");
	QTest::addColumn<QString>("ordinary");
	QTest::addColumn<QString>("tone");
	// locale выбирает набор буквальных переводов; status адресует значение публичного перечисления.
	for (const auto &locale : {QStringLiteral("ru-RU"), QStringLiteral("en-US")}) {
		// russian выбирает точные ожидаемые строки поставляемой локализации.
		const bool russian = locale == QStringLiteral("ru-RU");
		// expected и ordinary содержат независимые видимые ожидания Dock и общего StatusLabel по порядку enum.
		const QStringList expected =
			russian ? QStringList{QStringLiteral("ОТКЛЮЧЁН"), QStringLiteral("ПОДКЛЮЧЕНИЕ"),
					      QStringLiteral("ПОДКЛЮЧЁН"), QStringLiteral("ВОССТАНОВЛЕНИЕ СВЯЗИ"),
					      QStringLiteral("ОШИБКА ПОДКЛЮЧЕНИЯ · НЕТ АКТУАЛЬНЫХ ДАННЫХ")}
				: QStringList{QStringLiteral("DISCONNECTED"), QStringLiteral("CONNECTING"),
					      QStringLiteral("CONNECTED"), QStringLiteral("RECONNECTING"),
					      QStringLiteral("CONNECTION ERROR · CURRENT DATA UNAVAILABLE")};
		const QStringList ordinary =
			russian ? QStringList{QStringLiteral("Отключён"), QStringLiteral("Подключение"),
					      QStringLiteral("Подключён"), QStringLiteral("Восстановление связи"),
					      QStringLiteral("Ошибка подключения · нет актуальных данных")}
				: QStringList{QStringLiteral("Disconnected"), QStringLiteral("Connecting"),
					      QStringLiteral("Connected"), QStringLiteral("Reconnecting"),
					      QStringLiteral("Connection error · current data unavailable")};
		// tones задаёт смысл подключения независимо от регистра и оформления текущей темы.
		const QStringList tones{QStringLiteral("neutral"), QStringLiteral("pending"), QStringLiteral("active"),
					QStringLiteral("pending"), QStringLiteral("error")};
		// status перебирает пять фаз; без origin каждая обязана показывать отсутствие наблюдения.
		for (int status = 0; status < expected.size(); ++status) {
			QTest::newRow(qPrintable(QStringLiteral("%1-%2").arg(locale).arg(status)))
				<< locale << status << true << expected.at(status) << ordinary.at(status)
				<< tones.at(status);
			QTest::newRow(qPrintable(QStringLiteral("%1-%2-without-origin").arg(locale).arg(status)))
				<< locale << status << false
				<< (russian ? QStringLiteral("НЕТ ДАННЫХ") : QStringLiteral("NO DATA"))
				<< (russian ? QStringLiteral("Нет данных") : QStringLiteral("No data"))
				<< QStringLiteral("neutral");
		}
		QTest::newRow(qPrintable(locale + QStringLiteral("-unobserved")))
			<< locale << -1 << false << (russian ? QStringLiteral("НЕТ ДАННЫХ") : QStringLiteral("NO DATA"))
			<< (russian ? QStringLiteral("Нет данных") : QStringLiteral("No data"))
			<< QStringLiteral("neutral");
	}
}

// Проверяет все фазы, сохраняя исходное enum в StateStore и обычный регистр общего StatusLabel.
void PluginLifecycleTest::dockConnectionStatusUsesUppercase()
{
	// locale выбирает перевод; status и hasOrigin задают подтверждённость наблюдения подключения.
	QFETCH(QString, locale);
	QFETCH(int, status);
	QFETCH(bool, hasOrigin);
	// expected задаёт точный текст Dock, ordinary — обычного StatusLabel, tone — независимый смысл статуса.
	QFETCH(QString, expected);
	QFETCH(QString, ordinary);
	QFETCH(QString, tone);
	// state хранит независимое наблюдение, text читает настоящие локализации поставки.
	gost::StateStore state;
	const auto text = localeLookup(locale);
	// dock показывает шапку, common проверяет неизменную семантику общей подписи вне Dock.
	gost::RestreamDock dock(state, text);
	gost::StatusLabel common(nullptr);
	// connection сохраняет исходную фазу только для сценариев с заданным наблюдением.
	const auto connection = static_cast<gost::ConnectionStatus>(qMax(status, 0));
	if (status >= 0)
		state.setConnection(hasOrigin ? QStringLiteral("http://127.0.0.1:8080") : QString{}, connection);
	common.setConnectionStatus(text, hasOrigin ? std::optional{connection} : std::nullopt);
	// label адресует только верхнюю подпись подключения, не статусы OBS, входа или Session.
	const auto *label = dock.findChild<QLabel *>(QStringLiteral("serverObservation"));
	QVERIFY(label);
	QCOMPARE(label->text(), expected);
	QCOMPARE(label->textFormat(), Qt::PlainText);
	QCOMPARE(label->contentsMargins().left(), 0);
	QCOMPARE(label->property("statusTone").toString(), tone);
	QCOMPARE(common.text(), ordinary);
	if (status >= 0)
		QCOMPARE(state.connectionStatus(), connection);
}

void PluginLifecycleTest::statusAppearanceFollowsStateAndPalette()
{
	// Палитра приложения заменяется только в тестовом процессе и восстанавливается qScopeGuard.
	// Проверяется как смена смысла статуса, так и смена темы при прежнем статусе: одного начального
	// выбора цвета в конструкторе недостаточно. Нейтральный цвет берётся из текущей палитры.
	// savedPalette сохраняет исходную палитру тестового приложения.
	const auto savedPalette = QApplication::palette();
	// restorePalette возвращает savedPalette при выходе; callback выполняется также при раннем отказе проверки.
	const auto restorePalette = qScopeGuard([&] { QApplication::setPalette(savedPalette); });
	// palette задаёт тёмное, затем светлое оформление тестового приложения.
	QPalette palette = savedPalette;
	palette.setColor(QPalette::Window, QColor(QStringLiteral("#1c1f26")));
	palette.setColor(QPalette::WindowText, QColor(QStringLiteral("#ecedf2")));
	QApplication::setPalette(palette);

	// state хранит исходные наблюдения, которые представления не должны изменять.
	gost::StateStore state;
	// text читает переводы из поставляемого файла локали.
	const auto text = localeLookup();
	// dock показывает настоящий Widgets-интерфейс на общем состоянии.
	gost::RestreamDock dock(state, text);
	// window предоставляет единое окно управления на том же общем состоянии.
	gost::ManagementWindow window(state, text);
	// server показывает состояние подключения независимо от OBS.
	auto *server = dock.findChild<QLabel *>(QStringLiteral("serverObservation"));
	// dockStreaming показывает локальный статус OBS в Dock.
	auto *dockStreaming = dock.findChild<QLabel *>(QStringLiteral("obsStreaming"));
	// windowStreaming показывает тот же локальный статус в диагностике.
	auto *windowStreaming = window.findChild<QLabel *>(QStringLiteral("obsStreaming"));
	QVERIFY(server && dockStreaming && windowStreaming);

	// label адресует очередную проверяемую подпись без изменения её состояния.
	for (auto *label : {server, dockStreaming, windowStreaming}) {
		label->ensurePolished();
		QVERIFY(label->font().bold());
		QCOMPARE(label->palette().color(QPalette::WindowText), palette.color(QPalette::WindowText));
	}

	// origin задаёт адрес контекста наблюдения без пользовательских настроек OBS.
	const QString origin = QStringLiteral("http://127.0.0.1:8080");
	using gost::ConnectionStatus;
	// status перебирает промежуточные состояния подключения с общим предупреждающим цветом.
	for (const auto status : {ConnectionStatus::Connecting, ConnectionStatus::Reconnecting}) {
		state.setConnection(origin, status);
		// label адресует очередную проверяемую подпись без изменения её состояния.
		for (auto *label : {server})
			QCOMPARE(label->palette().color(QPalette::WindowText), QColor(QStringLiteral("#ffd078")));
	}
	state.setConnection(origin, ConnectionStatus::Error);
	// label адресует очередную проверяемую подпись без изменения её состояния.
	for (auto *label : {server})
		QCOMPARE(label->palette().color(QPalette::WindowText), QColor(QStringLiteral("#ff8992")));

	state.setConnection(origin, ConnectionStatus::Connected);
	state.setObsState(QStringLiteral("Profile"), true);
	// label адресует очередную проверяемую подпись без изменения её состояния.
	for (auto *label : {server, dockStreaming, windowStreaming})
		QCOMPARE(label->palette().color(QPalette::WindowText), QColor(QStringLiteral("#6bdf91")));

	palette.setColor(QPalette::Window, QColor(QStringLiteral("#f6f7f8")));
	palette.setColor(QPalette::WindowText, QColor(QStringLiteral("#202226")));
	QApplication::setPalette(palette);
	// label адресует очередную проверяемую подпись без изменения её состояния.
	for (auto *label : {server, dockStreaming, windowStreaming})
		QTRY_COMPARE_WITH_TIMEOUT(label->palette().color(QPalette::WindowText),
					  QColor(QStringLiteral("#155d32")), 1000);

	state.setConnection(origin, ConnectionStatus::Disconnected);
	state.setObsState(QStringLiteral("Profile"), false);
	// label адресует очередную проверяемую подпись без изменения её состояния.
	for (auto *label : {server, dockStreaming, windowStreaming}) {
		QVERIFY(label->font().bold());
		QCOMPARE(label->palette().color(QPalette::WindowText), palette.color(QPalette::WindowText));
	}
}

// Пункт «Сервис» повторно показывает единственное окно после минимизации или скрытия, сохраняя runtime.
void PluginLifecycleTest::managementWindowIsLazyAndReused()
{
	// locale выбирает поставляемые подписи русского или английского интерфейса.
	QFETCH(QString, locale);
	// frontend создаёт меню с настоящим QAction и главное окно-владельца.
	FakeFrontend frontend;
	// lifecycle объединяет один runtime с лениво создаваемым окном управления.
	gost::PluginLifecycle lifecycle(frontend, localeLookup(locale));
	QVERIFY(lifecycle.load());
	QVERIFY(managementWindows().isEmpty());
	// state, dock и action позволяют обнаружить пересоздание общих ресурсов при повторном открытии.
	QPointer<gost::StateStore> state = lifecycle.stateStore();
	QPointer<QWidget> dock = frontend.content;
	QPointer<QAction> action = frontend.managementAction;
	QVERIFY(action);
	QCOMPARE(action->text(), QStringLiteral("GOST Restreamer"));
	frontend.emitEvent(gost::FrontendEvent::FinishedLoading);
	action->trigger();
	QCOMPARE(managementWindows().size(), 1);
	// window отслеживает именно первый экземпляр, включая закрытие без удаления.
	QPointer<gost::ManagementWindow> window = managementWindows().front();
	QVERIFY(window->isWindow() && window->isVisible());
	QVERIFY(!window->testAttribute(Qt::WA_DeleteOnClose));
	QVERIFY(!window->testAttribute(Qt::WA_QuitOnClose));
	action->trigger();
	QCOMPARE(managementWindows().size(), 1);
	QCOMPARE(managementWindows().front(), window.data());
	window->showMinimized();
	QTRY_VERIFY_WITH_TIMEOUT(window->isMinimized(), 1000);
	action->trigger();
	QTRY_VERIFY_WITH_TIMEOUT(!window->isMinimized(), 1000);
	QVERIFY(window->isVisible());
	QCOMPARE(managementWindows().size(), 1);
	QCOMPARE(managementWindows().front(), window.data());
	window->close();
	dispatchQueuedEvents();
	QVERIFY(window && state && dock && action);
	QVERIFY(!window->isVisible());
	QCOMPARE(lifecycle.stateStore(), state.data());
	QCOMPARE(state->profileName().value(), frontend.profile);
	frontend.profile = QStringLiteral("Профиль после закрытия");
	frontend.emitEvent(gost::FrontendEvent::ProfileChanged);
	QCOMPARE(state->profileName().value(), frontend.profile);
	action->trigger();
	QCOMPARE(managementWindows().size(), 1);
	QCOMPARE(managementWindows().front(), window.data());
	QVERIFY(window->isVisible());
	QCOMPARE(frontend.dockAdds, 1);
	QCOMPARE(frontend.toolsMenuAdds, 1);
	QCOMPARE(frontend.callbackAdds, 1);
}
// Сохраняет компактный Dock без входа в настройки, форм и внутренней навигации.
void PluginLifecycleTest::dockStaysCompact()
{
	// Проверяется удаление входа в настройки из Dock и отсутствие навигации/форм.
	// Геометрия, тема и читаемость на реальном Windows-дисплее этим набором типов не доказываются.
	// frontend моделирует принадлежащие OBS события и владение Dock без настоящего процесса OBS.
	FakeFrontend frontend;
	// lifecycle связывает поддельный Frontend с настоящими сервисами и окнами плагина.
	gost::PluginLifecycle lifecycle(frontend, localeLookup());
	QVERIFY(lifecycle.load());
	// dock показывает настоящий Widgets-интерфейс на общем состоянии.
	const auto *dock = frontend.content.data();
	QVERIFY(dock->findChildren<QToolButton *>(QStringLiteral("managementButton")).isEmpty());
	QVERIFY(dock->findChildren<QAction *>(QStringLiteral("gostRestreamerManagementAction")).isEmpty());
	QVERIFY(dock->findChildren<QTabWidget *>().isEmpty());
	QVERIFY(dock->findChildren<QLineEdit *>().isEmpty());
	QCOMPARE(dock->findChildren<QComboBox *>().size(), 1);
	QVERIFY(dock->findChild<QComboBox *>(QStringLiteral("streamSelector")));
	QVERIFY(dock->findChildren<QListWidget *>().isEmpty());
	QVERIFY(dock->findChildren<QStackedWidget *>().isEmpty());
}

// Задаёт ширины карточек, число выходов и локали для проверки переносов и трёх колонок входа.
void PluginLifecycleTest::dockCardsFitNarrowWidths_data()
{
	QTest::addColumn<int>("width");
	QTest::addColumn<int>("outputCount");
	QTest::addColumn<QString>("locale");
	QTest::addColumn<bool>("longCodec");
	QTest::addColumn<QString>("unicodeSuffix");
	// width задаёт ширину Dock, count — число выходов, locale — язык подписей каждой строки.
	for (const int width : {300, 360, 400, 700})
		for (const int count : {0, 1, 20})
			for (const auto &locale : {QStringLiteral("ru-RU"), QStringLiteral("en-US")})
				QTest::newRow(qPrintable(QStringLiteral("%1-%2-%3").arg(width).arg(count).arg(locale)))
					<< width << count << locale << false << QString{};
	// locale выбирает язык для длинного кодека без пробелов при минимальной ширине Dock.
	for (const auto &locale : {QStringLiteral("ru-RU"), QStringLiteral("en-US")})
		QTest::newRow(qPrintable(QStringLiteral("300-0-%1-long-codec").arg(locale)))
			<< 300 << 0 << locale << true << QString{};
	// locale выбирает язык; составные буквы и emoji проверяют переносы только между целыми графемами.
	for (const auto &locale : {QStringLiteral("ru-RU"), QStringLiteral("en-US")})
		QTest::newRow(qPrintable(QStringLiteral("300-0-%1-unicode-codec").arg(locale)))
			<< 300 << 0 << locale << false
			<< QStringLiteral("&A\u0301\U0001F469\u200d\U0001F4BB").repeated(16);
}

// Проверяет ширину карточек, буквальные серверные строки и три подписанных столбца статистики потока.
// Проверяет компоновку оставшихся карточек, состояний и команд в узком Dock.
void PluginLifecycleTest::dockCardsFitNarrowWidths()
{
	// Настоящие layout и QTextDocument проверяют переносы и доступность областей без снимка экрана.
	// Длинные имена содержат буквальную HTML-разметку; ни имя, ни серверный статус не должны её выполнять.
	// width задаёт проверяемую ширину Dock в пикселях.
	QFETCH(int, width);
	// outputCount задаёт число синтетических выходов в карточке.
	QFETCH(int, outputCount);
	// locale выбирает реальные подписи и числовой формат текущей локализации.
	QFETCH(QString, locale);
	// longCodec добавляет непрерывный токен, который не должен вытеснять соседние метрики.
	QFETCH(bool, longCodec);
	// unicodeSuffix содержит составные Unicode-графемы либо пустую строку обычного сценария.
	QFETCH(QString, unicodeSuffix);
	// codecSuffix удлиняет имя кодека без пробелов, сохраняя исходную проверку экранирования.
	const QString codecSuffix = longCodec ? QString(96, QLatin1Char('X')) : unicodeSuffix;
	// originalCodec сохраняет точную серверную строку до преобразований представления.
	const QString originalCodec = QStringLiteral("h264<test>") + codecSuffix;
	using namespace gost;
	using namespace gost::api;
	// state хранит исходные наблюдения, которые представления не должны изменять.
	StateStore state;
	// text читает переводы из поставляемого файла локали.
	const auto text = localeLookup(locale);
	// longName содержит длинное имя с буквальным HTML для проверки сжатия и безопасного отображения.
	const QString longName = QStringLiteral("<b>Очень длинное название & канала с дополнительными словами</b>");
	// stream содержит имя и точный ID потока, полученного для сценария.
	Stream stream;
	stream.id = QStringLiteral("stream");
	stream.name = longName;
	state.setConnection(QStringLiteral("http://127.0.0.1:8080"), ConnectionStatus::Connected);
	state.setStreams({stream}, true);
	state.setSelection(stream.id, true, {}, {}, QStringLiteral("profile"));
	state.setObsState(longName, false);
	state.setObsStatistics({.phase = ObsStreamingPhase::Stopped,
				.fps = 59.75,
				.renderTimeNs = 340000,
				.render = FrameCounters{19, 483552},
				.encode = FrameCounters{3, 150336},
				.observed = true});
	// session содержит синтетическое наблюдение выбранной Session.
	RuntimeSession session;
	session.sessionId = QStringLiteral("session");
	session.observation = {FieldState::Value, Observation{.instanceId = QStringLiteral("session-instance"),
							      .state = QStringLiteral("RUNNING")}};
	// input задаёт независимо наблюдаемые входные параметры сервера.
	Input input;
	input.connected = true;
	input.metrics.state = FieldState::Value;
	// media изменяет формат и скорости входа внутри синтетической метрики.
	auto &media = input.metrics.value.media;
	media.format = {.videoCodec = originalCodec, .width = 2560, .height = 1440};
	media.rates.fps = {.status = QStringLiteral("available"), .value = {FieldState::Value, 47.25}};
	media.rates.payloadBitrate = {.status = QStringLiteral("available"), .value = {FieldState::Value, 8370000.0}};
	session.input = {FieldState::Value, input};
	// metadata связывает Session и её выходы с подтверждённым состоянием Dock.
	DockServerState metadata;
	metadata.session = DockSession{session.sessionId, QStringLiteral("RUNNING"), false};
	metadata.sessionStatus = {RuntimeAvailability::Fresh, true, {}};
	metadata.outputsStatus = {RuntimeAvailability::Fresh, true, {}};
	// index адресует очередной синтетический выход или элемент сценарного набора.
	for (int index = 0; index < outputCount; ++index) {
		// output содержит состояние одного синтетического выхода.
		RuntimeOutput output;
		output.outputId = QStringLiteral("output-%1").arg(index);
		output.instanceId = QStringLiteral("output-instance-%1").arg(index);
		output.state = index == 0 ? QStringLiteral("<b>UNRECOGNIZED</b>") : QStringLiteral("PUBLISHING");
		metadata.outputs.insert(output.outputId, {longName + QString::number(index), true, true});
		session.outputs.append(output);
	}
	QVERIFY(state.installAnchor({QStringLiteral("geometry-anchor"), {session}, {FieldState::Null, {}}}));
	state.setDockServer(metadata);
	// dock показывает настоящий Widgets-интерфейс на общем состоянии.
	RestreamDock dock(state, text);
	dock.resize(width, 560);
	dock.show();
	// Доставляет запланированное обновление после первой фактической компоновки Dock.
	dispatchQueuedEvents();
	// details предоставляет прокрутку подробных карточек Dock.
	auto *details = dock.findChild<QScrollArea *>(QStringLiteral("dockDetailsScroll"));
	// outputs предоставляет модель и виджеты строк выходов.
	auto *outputs = dock.findChild<QListView *>(QStringLiteral("outputList"));
	// reset выполняет локальный сброс отображаемых накоплений.
	auto *reset = dock.findChild<QPushButton *>(QStringLiteral("statsResetButton"));
	QVERIFY(details && outputs && reset);
	QVERIFY(!dock.findChild<QToolButton *>(QStringLiteral("managementButton")));
	QTRY_COMPARE_WITH_TIMEOUT(outputs->model()->rowCount(), qMax(outputCount, 1), 1000);
	QCOMPARE(dock.width(), width);
	QCOMPARE(dock.height(), 560);
	QCOMPARE(details->horizontalScrollBar()->maximum(), 0);
	QCOMPARE(outputs->horizontalScrollBar()->maximum(), 0);
	// outputCard и title показывают точный новый заголовок, не подменяя его старой строкой состава.
	auto *outputCard = dock.findChild<QWidget *>(QStringLiteral("outputsCard"));
	QVERIFY(outputCard);
	auto *title = outputCard->findChild<QLabel *>(QStringLiteral("cardTitle"));
	QVERIFY(title);
	QCOMPARE(title->text(), locale == QStringLiteral("ru-RU") ? QStringLiteral("Статистика выходов")
								  : QStringLiteral("Output statistics"));
	// startOutputs, stopOutputs и restartOutputs остаются видимыми иконками с полными локализованными именами.
	auto *startOutputs = outputCard->findChild<QToolButton *>(QStringLiteral("outputsStartButton"));
	auto *stopOutputs = outputCard->findChild<QToolButton *>(QStringLiteral("outputsStopButton"));
	auto *restartOutputs = outputCard->findChild<QToolButton *>(QStringLiteral("outputsRestartButton"));
	QVERIFY(startOutputs && stopOutputs && restartOutputs);
	// button перебирает три пакетные команды и проверяет доступность с клавиатуры.
	for (auto *button : {startOutputs, stopOutputs, restartOutputs}) {
		QCOMPARE(button->toolButtonStyle(), Qt::ToolButtonIconOnly);
		QVERIFY(!button->icon().isNull());
		QVERIFY(button->focusPolicy() != Qt::NoFocus);
		QVERIFY(!button->toolTip().isEmpty());
		QCOMPARE(button->accessibleName(), button->toolTip());
		// buttonRect и titleRect сравнивают поля одной шапки в координатах карточки.
		const QRect buttonRect(button->mapTo(outputCard, QPoint{}), button->size());
		const QRect titleRect(title->mapTo(outputCard, QPoint{}), title->size());
		QVERIFY(outputCard->rect().contains(buttonRect));
		QVERIFY(buttonRect.top() < titleRect.bottom() && titleRect.top() < buttonRect.bottom());
		QVERIFY(buttonRect.left() > titleRect.left());
	}
	QCOMPARE(startOutputs->toolTip(), text("Dock.StartOutputs"));
	QCOMPARE(stopOutputs->toolTip(), text("Dock.StopOutputs"));
	QCOMPARE(restartOutputs->toolTip(), text("Dock.RestartOutputs"));
	// label проверяет отсутствие удалённого подзаголовка среди реальных видимых подписей карточки.
	for (const auto *label : outputCard->findChildren<QLabel *>())
		QVERIFY(label->text() != (locale == QStringLiteral("ru-RU") ? QStringLiteral("Исходящие потоки")
									    : QStringLiteral("Outgoing streams")));

	// name адресует проверяемую карточку или подпись имени выхода.
	for (const auto *name : {"stateCard", "obsStatisticsCard", "inputCard", "outputsCard"}) {
		// card адресует очередную карточку, ширина которой ограничена Dock.
		auto *card = dock.findChild<QWidget *>(QString::fromLatin1(name));
		QVERIFY(card);
		QVERIFY(card->width() > 0 && card->width() <= dock.width());
	}
	// widget адресует очередную постоянную кнопку для проверки её доступности.
	for (auto *widget : {static_cast<QWidget *>(reset)}) {
		// mapped содержит прямоугольник кнопки в координатах Dock.
		const QRect mapped(widget->mapTo(&dock, QPoint{}), widget->size());
		QVERIFY(dock.rect().contains(mapped));
		QVERIFY(!widget->visibleRegion().isEmpty());
	}
	// selector отражает буквальное имя выбранного Stream в компактной шапке.
	const auto *selector = dock.findChild<QComboBox *>(QStringLiteral("streamSelector"));
	QVERIFY(selector);
	QCOMPARE(selector->currentText(), longName);
	QVERIFY(selector->width() > 0);
	// statistics адресует таблицу локальных метрик, проверяемую этим сценарием.
	const auto *statistics = dock.findChild<QLabel *>(QStringLiteral("obsStatistics"));
	QVERIFY(statistics);
	// local содержит отображаемый текст текущих локальных скоростей.
	const auto local = displayedText(statistics);
	QVERIFY(local.contains(locale == QStringLiteral("ru-RU") ? QStringLiteral("59,75") : QStringLiteral("59.75")));
	QVERIFY(local.contains(text("Dock.RenderLabel")));
	// ingest показывает формат и скорости серверного входа.
	const auto *ingest = dock.findChild<QLabel *>(QStringLiteral("ingestStatistics"));
	QVERIFY(ingest);
	QVERIFY(ingest->wordWrap());
	// В широкой карточке ждём обновления по новой ширине, а не оцениваем HTML до первой компоновки.
	if (width == 700 && !longCodec && unicodeSuffix.isEmpty())
		QTRY_VERIFY_WITH_TIMEOUT(!ingest->text().contains(QChar(0x200b)), 1000);
	// inputText содержит отображаемые значения входа без HTML-разметки.
	const auto inputText = ingestStatisticsText(ingest);
	QVERIFY(inputText.contains(QStringLiteral("H264<TEST>")));
	QVERIFY(inputText.contains(QStringLiteral("2560 × 1440")));
	QVERIFY(inputText.contains(locale == QStringLiteral("ru-RU") ? QStringLiteral("47,25")
								     : QStringLiteral("47.25")));
	QVERIFY(inputText.contains(locale == QStringLiteral("ru-RU") ? QStringLiteral("8,37")
								     : QStringLiteral("8.37")));
	// inputCard адресует серверную карточку, inputTitle — её собственный заголовок.
	auto *inputCard = dock.findChild<QWidget *>(QStringLiteral("inputCard"));
	QVERIFY(inputCard);
	const auto *inputTitle = inputCard->findChild<QLabel *>(QStringLiteral("cardTitle"));
	QVERIFY(inputTitle);
	QCOMPARE(inputTitle->text(), locale == QStringLiteral("ru-RU") ? QStringLiteral("Статистика потока")
								       : QStringLiteral("Stream statistics"));
	// captions задаёт независимый ожидаемый порядок трёх подписей на текущем языке.
	const QStringList captions =
		locale == QStringLiteral("ru-RU")
			? QStringList{QStringLiteral("Формат"), QStringLiteral("FPS"), QStringLiteral("Битрейт")}
			: QStringList{QStringLiteral("Format"), QStringLiteral("FPS"), QStringLiteral("Bitrate")};
	// verifyInputColumns проверяет координаты подписей и ожидаемых values после раскладки текста Qt.
	// Каждый столбец должен сохранять значение под своей подписью и помещаться в доступную ширину.
	const auto verifyInputColumns = [&](const QStringList &values) {
		QCOMPARE(values.size(), captions.size());
		// document раскладывает реально показанный текст с тем же шрифтом и доступной шириной QLabel.
		QTextDocument document;
		document.setDefaultFont(ingest->font());
		document.setDocumentMargin(0);
		// textOption повторяет WordWrap у QLabel, не разрешая скрытый перенос непрерывного слова.
		auto textOption = document.defaultTextOption();
		textOption.setWrapMode(QTextOption::WordWrap);
		document.setDefaultTextOption(textOption);
		document.setHtml(ingest->text());
		document.setTextWidth(ingest->contentsRect().width());
		QVERIFY(document.size().width() <= ingest->contentsRect().width() + 1);
		// textPosition возвращает начало выделения cursor в координатах документа с учётом переносов.
		const auto textPosition = [&](const QTextCursor &cursor) {
			// start указывает на первый символ выделения, даже если значение занимает несколько строк.
			auto start = cursor;
			start.setPosition(cursor.selectionStart());
			// block содержит выбранный символ; bounds учитывает положение блока внутри таблицы.
			const auto block = start.block();
			const auto bounds = document.documentLayout()->blockBoundingRect(block);
			// position задаёт смещение символа в блоке, line — его строку после переноса по ширине.
			const int position = start.position() - block.position();
			const auto line = block.layout()->lineForTextPosition(position);
			if (!line.isValid()) {
				QTest::qFail("Не удалось определить положение текста статистики.", __FILE__, __LINE__);
				return QPointF{-1, -1};
			}
			return bounds.topLeft() + QPointF(line.cursorToX(position), line.y());
		};
		// verifyTextInColumn проверяет каждую строку выделения cursor между границами left и right.
		// Проверка полного фрагмента обнаруживает выход длинного слова за пределы собственного столбца.
		const auto verifyTextInColumn = [&](const QTextCursor &cursor, qreal left, qreal right) {
			// block проходит все абзацы, пересекающие найденную подпись или значение.
			for (auto block = document.findBlock(cursor.selectionStart());
			     block.isValid() && block.position() < cursor.selectionEnd(); block = block.next()) {
				// bounds задаёт положение абзаца в документе, layout содержит его строки после переноса.
				const auto bounds = document.documentLayout()->blockBoundingRect(block);
				const auto *layout = block.layout();
				// index адресует очередную строку абзаца, в которую могло попасть выделение.
				for (int index = 0; index < layout->lineCount(); ++index) {
					// line содержит геометрию строки; first и last ограничивают выделенный фрагмент.
					const auto line = layout->lineAt(index);
					const int first =
						qMax(cursor.selectionStart() - block.position(), line.textStart());
					const int last = qMin(cursor.selectionEnd() - block.position(),
							      line.textStart() + line.textLength());
					if (first >= last)
						continue;
					QVERIFY(bounds.left() + line.cursorToX(first) >= left - 1);
					QVERIFY(bounds.left() + line.cursorToX(last) <= right + 1);
				}
			}
		};
		// captionPositions и valuePositions сохраняют начала подписей и значений для проверки соседства колонок.
		QList<QPointF> captionPositions;
		QList<QPointF> valuePositions;
		// column адресует формат, FPS или битрейт в пользовательском порядке слева направо.
		for (int column = 0; column < captions.size(); ++column) {
			// caption находит точную подпись, value — ожидаемое значение после этой подписи.
			const auto caption = findIngestText(document, captions.at(column));
			QVERIFY(!caption.isNull());
			const auto value = findIngestText(document, values.at(column), caption);
			QVERIFY(!value.isNull());
			// table содержит ячейку с подписью и полным значением текущего столбца.
			const auto *table = value.currentTable();
			QVERIFY(table);
			QCOMPARE(table->columns(), captions.size());
			// cell и cellCursor выделяют все символы ячейки, чтобы обнаружить лишний или потерянный текст.
			const auto cell = table->cellAt(value);
			auto cellCursor = cell.firstCursorPosition();
			cellCursor.setPosition(cell.lastCursorPosition().position(), QTextCursor::KeepAnchor);
			// presented сохраняет точки переноса, visibleCell удаляет только U+200B для буквального сравнения.
			const auto presented = cellCursor.selectedText();
			auto visibleCell = presented;
			visibleCell.remove(QChar(0x200b));
			// expectedCell содержит ровно подпись, разрыв строки HTML и полное ожидаемое значение.
			const auto expectedCell = captions.at(column) + QChar(0x2028) + values.at(column);
			QCOMPARE(visibleCell, expectedCell);
			// boundaries проверяет исходные графемы независимо от размещения U+200B в готовом HTML.
			QTextBoundaryFinder boundaries(QTextBoundaryFinder::Grapheme, expectedCell);
			// offset считает исходные кодовые единицы UTF-16 перед очередной точкой переноса.
			int offset = 0;
			// character адресует символ готовой ячейки, включая суррогаты и комбинируемые знаки.
			for (const QChar character : presented) {
				if (character == QChar(0x200b)) {
					boundaries.setPosition(offset);
					QVERIFY(boundaries.isAtBoundary());
				} else {
					++offset;
				}
			}
			if (width == 700 && !longCodec && unicodeSuffix.isEmpty())
				QCOMPARE(presented, expectedCell);
			captionPositions.append(textPosition(caption));
			valuePositions.append(textPosition(value));
			QVERIFY(captionPositions.back().x() >= 0);
			QVERIFY(valuePositions.back().y() > captionPositions.back().y());
			QVERIFY(valuePositions.back().x() >= captionPositions.back().x() - 1);
			QVERIFY(qAbs(captionPositions.back().y() - captionPositions.front().y()) <= 1);
			if (column > 0) {
				QVERIFY(captionPositions.at(column).x() > captionPositions.at(column - 1).x());
				QVERIFY(valuePositions.at(column - 1).x() < captionPositions.at(column).x());
			}
		}
		// column адресует пару подписи и значения, чьи строки не должны заходить в следующий столбец.
		for (int column = 0; column < captions.size(); ++column) {
			// left и right задают видимый интервал столбца между соседними подписями и краем QLabel.
			const qreal left = captionPositions.at(column).x();
			const qreal right = column + 1 < captions.size() ? captionPositions.at(column + 1).x()
									 : ingest->contentsRect().width();
			// caption и value находят проверяемые фрагменты в том же документе с режимом QLabel.
			const auto caption = findIngestText(document, captions.at(column));
			const auto value = findIngestText(document, values.at(column), caption);
			QVERIFY(left >= 0 && left < ingest->contentsRect().width());
			QVERIFY(right <= ingest->contentsRect().width() + 1);
			verifyTextInColumn(caption, left, right);
			verifyTextInColumn(value, left, right);
		}
	};
	verifyInputColumns(
		{QStringLiteral("H264<TEST>%1 2560 × 1440").arg(codecSuffix),
		 locale == QStringLiteral("ru-RU") ? QStringLiteral("47,25 FPS") : QStringLiteral("47.25 FPS"),
		 locale == QStringLiteral("ru-RU") ? QStringLiteral("8,37 Мбит/с") : QStringLiteral("8.37 Mbps")});
	if (outputCount == 0) {
		QCOMPARE(outputs->model()->index(0, 0).data().toString(), text("Observation.NoData"));
		QVERIFY(!outputs->indexWidget(outputs->model()->index(0, 0)));
		QVERIFY(outputs->isHidden() || outputs->height() <= 80);
	} else {
		// row адресует строку таблицы или виджет первого выхода.
		auto *row = outputs->indexWidget(outputs->model()->index(0, 0));
		QVERIFY(row);
		// name адресует проверяемую карточку или подпись имени выхода.
		auto *name = row->findChild<QLabel *>(QStringLiteral("outputName"));
		// status показывает независимое состояние первого серверного выхода.
		auto *status = row->findChild<QLabel *>(QStringLiteral("outputStatus"));
		// rate показывает скорость первого выхода.
		auto *rate = row->findChild<QLabel *>(QStringLiteral("outputBitrate"));
		QVERIFY(name && status && rate);
		QVERIFY(!row->findChild<QLabel *>(QStringLiteral("outputDetails")));
		QVERIFY(!name->wordWrap() && !rate->wordWrap());
		// nameRect, statusRect и rateRect задают фактические области трёх полей в одной строке.
		const QRect nameRect(name->mapTo(row, QPoint{}), name->size());
		const QRect statusRect(status->mapTo(row, QPoint{}), status->size());
		const QRect rateRect(rate->mapTo(row, QPoint{}), rate->size());
		QVERIFY(nameRect.right() < statusRect.left());
		QVERIFY(statusRect.right() < rateRect.left());
		QVERIFY(nameRect.top() < rateRect.bottom() && rateRect.top() < nameRect.bottom());
		QVERIFY(row->toolTip().contains(text("Dock.OutputDetails").section(QStringLiteral("%1"), 0, 0)));
		QVERIFY(!row->accessibleDescription().isEmpty());
		QCOMPARE(name->text(), longName + QStringLiteral("0"));
		QCOMPARE(name->textFormat(), Qt::PlainText);
		QVERIFY(name->font().bold());
		QCOMPARE(status->text(), text("Dock.UnknownStatus"));
		QCOMPARE(status->property("statusTone").toString(), QStringLiteral("neutral"));
		QCOMPARE(rate->text(), text("Observation.NoData"));
		// expectedDescription сохраняет полное буквальное имя и недоступность всех ненаблюдаемых показателей.
		const auto expectedDescription = text("Dock.OutputRow")
							 .arg(longName + QStringLiteral("0"),
							      text("Dock.UnknownStatus"), text("Observation.NoData"),
							      text("Observation.NoData"), text("Observation.NoData"),
							      text("Observation.NoData"), text("Observation.NoData"));
		// field перебирает три поля; tooltip проверяет полное описание после безопасного HTML-экранирования.
		for (const auto *field : {name, status, rate}) {
			QTextDocument tooltip;
			tooltip.setHtml(field->toolTip());
			QCOMPARE(tooltip.toPlainText(), expectedDescription);
		}
		QCOMPARE(row->accessibleDescription(), expectedDescription);
		// action открывает меню оперативных команд одного выхода.
		auto *action = row->findChild<QToolButton *>(QStringLiteral("outputActionsButton"));
		QVERIFY(action && !action->isEnabled());
		QVERIFY(action->x() > rate->x());
		QVERIFY(row->height() <= qMax(action->height(), name->fontMetrics().height()) + 20);
		if (outputCount > 1) {
			QTRY_VERIFY_WITH_TIMEOUT(outputs->verticalScrollBar()->maximum() > 0, 1000);
			outputs->scrollToBottom();
			QVERIFY(outputs->verticalScrollBar()->value() > 0);
		}
	}
	if (qEnvironmentVariableIsSet("GOST_TEST_CAPTURE_DOCK")) {
		// path задаёт путь необязательного снимка Dock внутри workspace.
		const auto path = QStringLiteral(GOST_TEST_WORKSPACE_DIR) + QStringLiteral("/dock-preview-") +
				  QString::fromLatin1(QTest::currentDataTag()) + QStringLiteral(".png");
		QVERIFY(dock.grab().save(path));
		// inputPath сохраняет саму карточку входа, даже если она находится ниже видимой области Dock.
		const auto inputPath = QStringLiteral(GOST_TEST_WORKSPACE_DIR) +
				       QStringLiteral("/dock-input-card-preview-") +
				       QString::fromLatin1(QTest::currentDataTag()) + QStringLiteral(".png");
		QVERIFY(inputCard->grab().save(inputPath));
		// outputsCard предоставляет карточку выходов для отдельного снимка.
		auto *outputsCard = dock.findChild<QWidget *>(QStringLiteral("outputsCard"));
		QVERIFY(outputsCard);
		details->ensureWidgetVisible(outputsCard);
		// outputsPath задаёт путь снимка Dock после прокрутки к выходам.
		const auto outputsPath = QStringLiteral(GOST_TEST_WORKSPACE_DIR) +
					 QStringLiteral("/dock-outputs-preview-") +
					 QString::fromLatin1(QTest::currentDataTag()) + QStringLiteral(".png");
		QVERIFY(dock.grab().save(outputsPath));
		// cardPath задаёт путь отдельного снимка карточки выходов.
		const auto cardPath = QStringLiteral(GOST_TEST_WORKSPACE_DIR) +
				      QStringLiteral("/dock-outputs-card-preview-") +
				      QString::fromLatin1(QTest::currentDataTag()) + QStringLiteral(".png");
		QVERIFY(outputsCard->grab().save(cardPath));
	}
	// Представление не изменяет исходный codec, разрешение и измерения в fixture или StateStore.
	QCOMPARE(media.format.videoCodec, originalCodec);
	QCOMPARE(media.format.width, quint32{2560});
	QCOMPARE(media.format.height, quint32{1440});
	// rawSession сохраняет копию наблюдения после событий геометрии и необязательных снимков.
	const auto rawSession = state.runtime().sessions.value(session.sessionId);
	// rawInput заимствует исходный вход из живущей до конца сценария копии Session.
	const auto &rawInput = rawSession.input;
	QVERIFY(rawInput);
	QVERIFY(rawInput->metrics);
	QCOMPARE(rawInput->metrics->media.format.videoCodec, originalCodec);
	QCOMPARE(rawInput->metrics->media.format.width, quint32{2560});
	QCOMPARE(rawInput->metrics->media.format.height, quint32{1440});
	QCOMPARE(rawInput->metrics->media.rates.fps.value.value, 47.25);
	QCOMPARE(rawInput->metrics->media.rates.payloadBitrate.value.value, 8370000.0);
	// Смена профиля очищает прежние измерения, сохраняя те же три столбца с явным отсутствием данных.
	state.setSelection(stream.id, true, {}, {}, QStringLiteral("other-profile"));
	QTRY_VERIFY_WITH_TIMEOUT(!ingestStatisticsText(ingest).contains(QStringLiteral("H264<TEST>")), 1000);
	verifyInputColumns({text("Observation.NoData"), text("Observation.NoData"), text("Observation.NoData")});
	QCOMPARE(dock.width(), width);
	QCOMPARE(details->horizontalScrollBar()->maximum(), 0);
}

// Проверяет отсутствие Dock Start и недоступность команд без Stream, сохраняя клавиатурный доступ к управлению.
// Проверяет запрет медиакоманд без выбора и открытие единственного окна клавиатурой через меню.
void PluginLifecycleTest::dockActionsRemainLocalAndKeyboardAccessible()
{
	// Без выбранного Stream команды недоступны и мышью, и клавиатурой. Проверяется реальная граница
	// lifecycle с локальным HTTP-сервером; меню «Сервис» открывает тот же экземпляр управления.
	// temp изолирует настройки lifecycle внутри workspace.
	WorkspaceTemp temp(QStringLiteral("dock-local-actions"));
	QVERIFY(temp.isValid());
	// server принимает только локальные запросы; handler отвечает на bootstrap и удерживает SSE.
	HttpServer server;
	QVERIFY(server.listen());
	server.handler = [&](const auto &request) {
		if (request.target.startsWith("/api/v1/health/ready"))
			HttpServer::reply(request, R"({"status":"ready","reason":""})");
		else if (request.target.startsWith("/api/v1/system/capabilities"))
			HttpServer::reply(request, fixture::capabilities());
		else if (request.target.startsWith("/api/v1/streams"))
			HttpServer::reply(request, fixture::page({}));
		else if (request.target.startsWith("/api/v1/events/snapshot"))
			HttpServer::reply(request, R"({"cursor":"actions-anchor","sessions":[],"gpu":null})");
		else
			HttpServer::stream(request);
	};
	// frontend предоставляет настоящее дерево QWidget с управляемыми наблюдениями OBS.
	FakeFrontend frontend;
	// lifecycle создаёт общий runtime и UI с явным файлом настроек.
	gost::PluginLifecycle lifecycle(frontend, localeLookup(),
					{.settingsFile = temp.filePath(QStringLiteral("settings.json"))});
	QVERIFY(lifecycle.load());
	lifecycle.settingsStore()->setAutoConnect(false);
	frontend.emitEvent(gost::FrontendEvent::FinishedLoading);
	lifecycle.connectionController()->connectToServer(server.origin().toString());
	QTRY_COMPARE_WITH_TIMEOUT(lifecycle.stateStore()->connectionStatus(), gost::ConnectionStatus::Connected, 3000);
	// dock заимствует зарегистрированное содержимое OBS Dock.
	auto *dock = frontend.content.data();
	frontend.host->show();
	frontend.dock->show();
	// requests фиксирует завершившийся bootstrap для проверки отсутствия команд.
	const auto requests = server.requests.size();
	// sample сохраняет локальную статистику до попыток недоступных действий.
	const auto sample = lifecycle.stateStore()->obsStatistics();
	QVERIFY(!dock->findChild<QPushButton *>(QStringLiteral("broadcastStartButton")));
	// name адресует конкретную доступную в Dock команду без выбора текущей строки.
	for (const auto *name : {"broadcastPauseButton", "broadcastResumeButton", "broadcastStopButton",
				 "outputsStartButton", "outputsStopButton", "outputsRestartButton"}) {
		// button адресует точную кнопку; она не должна порождать действие без выбора.
		auto *button = dock->findChild<QAbstractButton *>(QString::fromLatin1(name));
		QVERIFY(button);
		QVERIFY(!button->isHidden());
		QVERIFY(!button->isEnabled());
		// clicked обнаруживает недопустимый сигнал мыши или клавиатуры от отключённой кнопки.
		QSignalSpy clicked(button, &QAbstractButton::clicked);
		QVERIFY(clicked.isValid());
		QTest::mouseClick(button, Qt::LeftButton);
		QTest::keyClick(button, Qt::Key_Space);
		QCOMPARE(clicked.count(), 0);
	}
	dispatchQueuedEvents();
	QCOMPARE(server.requests.size(), requests);
	QVERIFY(lifecycle.stateStore()->obsStatistics() == sample);
	QVERIFY(!frontend.streaming);
	// menu и action обозначают штатное меню и его единственный пункт открытия управления.
	auto *menu = frontend.toolsMenu.data();
	auto *action = frontend.managementAction.data();
	// reset адресует кнопку локального baseline, не сетевую команду.
	auto *reset = dock->findChild<QPushButton *>(QStringLiteral("statsResetButton"));
	QVERIFY(menu && action && reset);
	QVERIFY(!dock->findChild<QToolButton *>(QStringLiteral("managementButton")));
	QVERIFY(reset->focusPolicy() != Qt::NoFocus);
	menu->popup(frontend.host->mapToGlobal(QPoint{}));
	menu->setActiveAction(action);
	QTest::keyClick(menu, Qt::Key_Return);
	QTRY_COMPARE_WITH_TIMEOUT(managementWindows().size(), 1, 1000);
	// window фиксирует экземпляр ManagementWindow, который должен пережить закрытие.
	auto *window = managementWindows().front();
	window->close();
	menu->popup(frontend.host->mapToGlobal(QPoint{}));
	menu->setActiveAction(action);
	QTest::keyClick(menu, Qt::Key_Return);
	QCOMPARE(managementWindows().size(), 1);
	QCOMPARE(managementWindows().front(), window);
	QVERIFY(window->isVisible());
	QCOMPARE(server.requests.size(), requests);
}

void PluginLifecycleTest::dockStatusAxesRemainIndependent_data()
{
	QTest::addColumn<QString>("sessionState");
	QTest::addColumn<QString>("outputState");
	QTest::addColumn<QString>("sessionTone");
	QTest::addColumn<QString>("outputTone");
	QTest::newRow("running-failed-output") << QStringLiteral("RUNNING") << QStringLiteral("FAILED")
					       << QStringLiteral("active") << QStringLiteral("error");
	QTest::newRow("starting-stopped-output") << QStringLiteral("STARTING") << QStringLiteral("STOPPED")
						 << QStringLiteral("pending") << QStringLiteral("neutral");
	QTest::newRow("failed-publishing-output") << QStringLiteral("FAILED") << QStringLiteral("PUBLISHING")
						  << QStringLiteral("error") << QStringLiteral("active");
}

void PluginLifecycleTest::dockStatusAxesRemainIndependent()
{
	QFETCH(QString, sessionState);
	QFETCH(QString, outputState);
	QFETCH(QString, sessionTone);
	QFETCH(QString, outputTone);
	using namespace gost;
	using namespace gost::api;
	const auto savedPalette = QApplication::palette();
	const auto restorePalette = qScopeGuard([&] { QApplication::setPalette(savedPalette); });
	StateStore state;
	Stream stream;
	stream.id = QStringLiteral("stream");
	state.setStreams({stream}, true);
	state.setConnection(QStringLiteral("http://127.0.0.1:8080"), ConnectionStatus::Connected);
	state.setSelection(stream.id, true, {}, {}, QStringLiteral("profile"));
	state.setObsStatistics({.phase = ObsStreamingPhase::Starting, .observed = true});
	RuntimeSession session;
	session.sessionId = QStringLiteral("session");
	session.observation = {FieldState::Value,
			       Observation{.instanceId = QStringLiteral("session-instance"), .state = sessionState}};
	Input input;
	input.connected = false;
	session.input = {FieldState::Value, input};
	RuntimeOutput output;
	output.outputId = QStringLiteral("output");
	output.instanceId = QStringLiteral("output-instance");
	output.state = outputState;
	session.outputs.append(output);
	QVERIFY(state.installAnchor({QStringLiteral("axes-anchor"), {session}, {FieldState::Null, {}}}));
	DockServerState metadata;
	metadata.session = DockSession{session.sessionId, sessionState, false};
	metadata.sessionStatus = {RuntimeAvailability::Fresh, true, {}};
	metadata.outputsStatus = {RuntimeAvailability::Fresh, true, {}};
	metadata.outputs.insert(output.outputId, {QStringLiteral("Channel"), true, true});
	state.setDockServer(metadata);
	RestreamDock dock(state, localeLookup());
	dock.show();
	auto *list = dock.findChild<QListView *>(QStringLiteral("outputList"));
	QVERIFY(list);
	QTRY_COMPARE_WITH_TIMEOUT(list->model()->rowCount(), 1, 1000);
	auto *row = list->indexWidget(list->model()->index(0, 0));
	QVERIFY(row);
	auto *outputLabel = row->findChild<QLabel *>(QStringLiteral("outputStatus"));
	auto *sessionLabel = dock.findChild<QLabel *>(QStringLiteral("sessionObservation"));
	auto *inputLabel = dock.findChild<QLabel *>(QStringLiteral("inputObservation"));
	auto *obsLabel = dock.findChild<QLabel *>(QStringLiteral("obsStreaming"));
	QVERIFY(outputLabel && sessionLabel && inputLabel && obsLabel);
	QCOMPARE(sessionLabel->property("statusTone").toString(), sessionTone);
	QCOMPARE(outputLabel->property("statusTone").toString(), outputTone);
	QCOMPARE(inputLabel->property("statusTone").toString(), QStringLiteral("neutral"));
	QCOMPARE(obsLabel->property("statusTone").toString(), QStringLiteral("pending"));
	for (const bool dark : {true, false}) {
		QPalette palette = savedPalette;
		palette.setColor(QPalette::Window, QColor(dark ? "#161b22" : "#f1ece6"));
		palette.setColor(QPalette::WindowText, QColor(dark ? "#ded9d3" : "#292d32"));
		QApplication::setPalette(palette);
		for (auto *label : {outputLabel, sessionLabel, inputLabel, obsLabel})
			label->ensurePolished();
		QTRY_COMPARE_WITH_TIMEOUT(inputLabel->palette().color(QPalette::WindowText),
					  palette.color(QPalette::WindowText), 1000);
		QVERIFY(obsLabel->palette().color(QPalette::WindowText) != palette.color(QPalette::WindowText));
		QCOMPARE(sessionLabel->property("statusTone").toString(), sessionTone);
		QCOMPARE(outputLabel->property("statusTone").toString(), outputTone);
		QVERIFY(!sessionLabel->text().isEmpty() && !outputLabel->text().isEmpty());
	}
	state.setObsStatistics({.phase = ObsStreamingPhase::Stopping, .observed = true});
	QTRY_COMPARE_WITH_TIMEOUT(obsLabel->property("statusTone").toString(), QStringLiteral("pending"), 1000);
	state.setObsStatistics({.phase = ObsStreamingPhase::Stopped, .observed = true});
	QTRY_COMPARE_WITH_TIMEOUT(obsLabel->property("statusTone").toString(), QStringLiteral("neutral"), 1000);
}

void PluginLifecycleTest::dockResetChangesOnlyDisplayedAccumulators()
{
	// После клика сравнивается исходный StateStore и неизменность FPS/bitrate/времени рендера.
	// Следующие разные дельты render/encode/network дают 20/5/0 процентов и обнаруживают смешение периодов.
	// state хранит исходные наблюдения, которые представления не должны изменять.
	gost::StateStore state;
	// text читает переводы из поставляемого файла локали.
	const auto text = localeLookup(QStringLiteral("en-US"));
	// dock показывает настоящий Widgets-интерфейс на общем состоянии.
	gost::RestreamDock dock(state, text);
	dock.show();
	// reset выполняет локальный сброс отображаемых накоплений.
	auto *reset = dock.findChild<QPushButton *>(QStringLiteral("statsResetButton"));
	// statistics адресует таблицу локальных метрик, проверяемую этим сценарием.
	const auto *statistics = dock.findChild<QLabel *>(QStringLiteral("obsDropStatistics"));
	QVERIFY(reset && statistics);
	QVERIFY(!reset->isEnabled());
	QVERIFY(obsStatisticsText(dock).contains(QStringLiteral("No data")));

	// sample задаёт исходные независимые счётчики и скорости OBS.
	gost::ObsStatistics sample{.phase = gost::ObsStreamingPhase::Streaming,
				   .videoEpoch = 1,
				   .outputEpoch = 1,
				   .fps = 58.75,
				   .bitrate = 6370000,
				   .renderTimeNs = 340000,
				   .sentBytes = 5000000,
				   .render = gost::FrameCounters{12, 150},
				   .encode = gost::FrameCounters{3, 40},
				   .network = gost::FrameCounters{1, 25},
				   .observed = true};
	state.setObsStatistics(sample);
	QTRY_VERIFY_WITH_TIMEOUT(reset->isEnabled(), 1000);
	// before сохраняет отображение до сброса для сравнения накоплений и скоростей.
	const auto before = obsStatisticsText(dock);
	// rates содержит ожидаемые мгновенные скорости, которые локальный сброс сохраняет.
	const QStringList rates{QStringLiteral("58.75"), QStringLiteral("6.37 Mbps"), QStringLiteral("0.34 ms")};
	// rate содержит очередную строку FPS, битрейта или времени рендера OBS до сброса.
	for (const auto &rate : rates)
		QVERIFY(before.contains(rate));
	// value содержит очередное ожидаемое значение отображения.
	for (const auto &value : {QStringLiteral("12 / 150 (8.0%)"), QStringLiteral("3 / 40 (7.5%)"),
				  QStringLiteral("1 / 25 (4.0%)"), QStringLiteral("4.77 MiB")})
		QVERIFY(before.contains(value));
	reset->setFocus();
	QTest::keyClick(reset, Qt::Key_Space);
	QVERIFY(obsStatisticsText(dock) != before);
	// rate содержит ту же локальную мгновенную метрику, которая должна сохраниться после сброса.
	for (const auto &rate : rates)
		QVERIFY(obsStatisticsText(dock).contains(rate));
	// Сброс меняет представление накоплений, сохраняя исходную выборку и мгновенные скорости.
	QVERIFY(state.obsStatistics() == sample);

	sample.render = gost::FrameCounters{14, 160};
	sample.encode = gost::FrameCounters{4, 60};
	sample.network = gost::FrameCounters{1, 29};
	state.setObsStatistics(sample);
	QTRY_VERIFY_WITH_TIMEOUT(obsStatisticsText(dock).contains(QStringLiteral("(20.0%)")), 1000);
	QVERIFY(obsStatisticsText(dock).contains(QStringLiteral("(5.0%)")));
	QVERIFY(obsStatisticsText(dock).contains(QStringLiteral("(0.0%)")));
}

void PluginLifecycleTest::dockCounterPeriodsFollowIndependentSources_data()
{
	QTest::addColumn<QString>("locale");
	QTest::addColumn<bool>("replaceOutput");
	for (const auto &locale : {QStringLiteral("ru-RU"), QStringLiteral("en-US")}) {
		QTest::newRow(qPrintable(locale + QStringLiteral("-output-replacement"))) << locale << true;
		QTest::newRow(qPrintable(locale + QStringLiteral("-network-regression"))) << locale << false;
	}
}

void PluginLifecycleTest::dockCounterPeriodsFollowIndependentSources()
{
	// Замена output не сбрасывает накопления видео, а регресс сети не сбрасывает байты.
	// Числовые дельты остаются раздельными, хотя период больше не выводится в Dock.
	// locale выбирает реальные подписи и числовой формат текущей локализации.
	QFETCH(QString, locale);
	// replaceOutput выбирает смену output либо отдельный регресс сетевого счётчика.
	QFETCH(bool, replaceOutput);
	using namespace gost;
	// state хранит исходные наблюдения, которые представления не должны изменять.
	StateStore state;
	// text читает переводы из поставляемого файла локали.
	const auto text = localeLookup(locale);
	// russian выбирает ожидаемые десятичные разделители и единицы.
	const bool russian = locale == QStringLiteral("ru-RU");
	// dock показывает настоящий Widgets-интерфейс на общем состоянии.
	RestreamDock dock(state, text);
	// reset выполняет локальный сброс отображаемых накоплений.
	auto *reset = dock.findChild<QPushButton *>(QStringLiteral("statsResetButton"));
	// statistics адресует таблицу локальных метрик, проверяемую этим сценарием.
	const auto *statistics = dock.findChild<QLabel *>(QStringLiteral("obsDropStatistics"));
	QVERIFY(reset && statistics);
	// rows callback извлекает значения строк по независимым подписям счётчиков.
	const auto rows = [&] {
		// result собирает значения таблицы по переведённым подписям строк.
		QMap<QString, QString> result;
		// document разбирает отображаемую HTML-таблицу без доступа к реализации расчёта.
		QTextDocument document;
		document.setHtml(statistics->text());
		// frame перебирает дочерние фреймы документа, среди которых находится таблица.
		for (auto *frame : document.rootFrame()->childFrames()) {
			// table адресует таблицу из двух колонок с подписью и значением.
			auto *table = qobject_cast<QTextTable *>(frame);
			if (!table || table->columns() != 2)
				continue;
			// row адресует строку таблицы или виджет первого выхода.
			for (int row = 0; row < table->rows(); ++row) {
				// cellText callback возвращает текст ячейки column текущей строки row.
				const auto cellText = [&](int column) {
					// cell адресует одну ячейку отображаемой таблицы.
					const auto cell = table->cellAt(row, column);
					// cursor выделяет полный текст ячейки для независимого сравнения.
					auto cursor = cell.firstCursorPosition();
					cursor.setPosition(cell.lastCursorPosition().position(),
							   QTextCursor::KeepAnchor);
					return cursor.selectedText();
				};
				result.insert(cellText(0), cellText(1));
			}
		}
		return result;
	};
	// verifyRows сравнивает ожидаемые render, encode, network и sent с видимыми строками по подписям.
	const auto verifyRows = [&](const QString &render, const QString &encode, const QString &network,
				    const QString &sent) {
		// values содержит прочитанные значения строк локальных накоплений.
		const auto values = rows();
		QCOMPARE(values.value(text("Dock.RenderDropsLabel")), render);
		QCOMPARE(values.value(text("Dock.EncodeDropsLabel")), encode);
		QCOMPARE(values.value(text("Dock.NetworkDropsLabel")), network);
		QCOMPARE(values.value(text("Dock.SentLabel")), sent);
	};
	// sample задаёт исходные независимые счётчики и скорости OBS.
	ObsStatistics sample{.phase = ObsStreamingPhase::Streaming,
			     .videoEpoch = 1,
			     .outputEpoch = 1,
			     .sentBytes = 1048576,
			     .render = FrameCounters{12, 150},
			     .encode = FrameCounters{3, 40},
			     .network = FrameCounters{8, 100},
			     .observed = true};
	state.setObsStatistics(sample);
	QTRY_VERIFY_WITH_TIMEOUT(reset->isEnabled(), 1000);
	verifyRows(russian ? QStringLiteral("12 / 150 (8,0%)") : QStringLiteral("12 / 150 (8.0%)"),
		   russian ? QStringLiteral("3 / 40 (7,5%)") : QStringLiteral("3 / 40 (7.5%)"),
		   russian ? QStringLiteral("8 / 100 (8,0%)") : QStringLiteral("8 / 100 (8.0%)"),
		   russian ? QStringLiteral("1,00 МиБ") : QStringLiteral("1.00 MiB"));
	QVERIFY(!dock.findChild<QLabel *>(QStringLiteral("obsStatisticsPeriod")));
	for (const auto *key : {"Dock.PeriodRun", "Dock.PeriodReset", "Dock.PeriodNewSample"}) {
		// key адресует подпись baseline, которая не должна попадать в локальные значения Dock.
		QVERIFY(!obsStatisticsText(dock).contains(text(key)));
	}
	reset->click();
	QVERIFY(!obsStatisticsText(dock).contains(text("Dock.PeriodReset")));
	QVERIFY(state.obsStatistics() == sample);

	sample.render = FrameCounters{14, 160};
	sample.encode = FrameCounters{4, 60};
	sample.network = FrameCounters{10, 110};
	sample.sentBytes = 3145728;
	state.setObsStatistics(sample);
	// twenty задаёт ожидаемые две пропущенные единицы из десяти новых кадров.
	const auto twenty = russian ? QStringLiteral("2 / 10 (20,0%)") : QStringLiteral("2 / 10 (20.0%)");
	QTRY_COMPARE_WITH_TIMEOUT(rows().value(text("Dock.RenderDropsLabel")), twenty, 1000);
	verifyRows(twenty, russian ? QStringLiteral("1 / 20 (5,0%)") : QStringLiteral("1 / 20 (5.0%)"), twenty,
		   russian ? QStringLiteral("2,00 МиБ") : QStringLiteral("2.00 MiB"));

	sample.render = FrameCounters{18, 190};
	sample.encode = FrameCounters{5, 80};
	sample.sentBytes = 4194304;
	if (replaceOutput) {
		sample.outputEpoch = 2;
		sample.network = FrameCounters{3, 50};
	} else {
		sample.network = FrameCounters{2, 120};
	}
	state.setObsStatistics(sample);
	// firstNetwork задаёт точное значение после смены output либо регресса сети.
	// Ожидание даёт Dock обработать отложенное обновление до чтения обеих ветвей.
	const auto firstNetwork =
		replaceOutput
			? (russian ? QStringLiteral("3 / 50 (6,0%)") : QStringLiteral("3 / 50 (6.0%)"))
			: text("Dock.Pair").arg(QStringLiteral("0"), QStringLiteral("0"), text("Observation.NoData"));
	QTRY_COMPARE_WITH_TIMEOUT(rows().value(text("Dock.NetworkDropsLabel")), firstNetwork, 1000);
	if (!replaceOutput) {
		QCOMPARE(rows().value(text("Dock.NetworkDropsLabel")),
			 text("Dock.Pair").arg(QStringLiteral("0"), QStringLiteral("0"), text("Observation.NoData")));
		sample.network = FrameCounters{5, 130};
		sample.sentBytes = 5242880;
		state.setObsStatistics(sample);
		QTRY_VERIFY_WITH_TIMEOUT(obsStatisticsText(dock).contains(russian ? QStringLiteral("30,0%")
										  : QStringLiteral("30.0%")),
					 1000);
	}
	verifyRows(russian ? QStringLiteral("6 / 40 (15,0%)") : QStringLiteral("6 / 40 (15.0%)"),
		   russian ? QStringLiteral("2 / 40 (5,0%)") : QStringLiteral("2 / 40 (5.0%)"),
		   replaceOutput ? (russian ? QStringLiteral("3 / 50 (6,0%)") : QStringLiteral("3 / 50 (6.0%)"))
				 : (russian ? QStringLiteral("3 / 10 (30,0%)") : QStringLiteral("3 / 10 (30.0%)")),
		   russian ? QStringLiteral("4,00 МиБ") : QStringLiteral("4.00 MiB"));
	for (const auto *key : {"Dock.PeriodRun", "Dock.PeriodReset", "Dock.PeriodNewSample"}) {
		// key остаётся внутренней меткой baseline и не является частью видимого значения счётчика.
		QVERIFY(!obsStatisticsText(dock).contains(text(key)));
	}
	QVERIFY(state.obsStatistics() == sample);
}

void PluginLifecycleTest::dockShowsNativeCounterLimitsIndependently_data()
{
	QTest::addColumn<int>("limitedCounter");
	QTest::newRow("render") << 0;
	QTest::newRow("encode") << 1;
	QTest::newRow("network") << 2;
}

void PluginLifecycleTest::dockShowsNativeCounterLimitsIndependently()
{
	// В каждой строке ограничен ровно один этап при доступных двух других. Это проверяет передачу
	// признака ограничения исходного API через сборщик в UI без обнуления всей статистики или скрытия доступного FPS.
	// limitedCounter выбирает единственный недоступный этап при доступности остальных.
	QFETCH(int, limitedCounter);
	// state хранит исходные наблюдения, которые представления не должны изменять.
	gost::StateStore state;
	// text читает переводы из поставляемого файла локали.
	const auto text = localeLookup(QStringLiteral("en-US"));
	// dock показывает настоящий Widgets-интерфейс на общем состоянии.
	gost::RestreamDock dock(state, text);
	// statistics адресует таблицу локальных метрик, проверяемую этим сценарием.
	const auto *statistics = dock.findChild<QLabel *>(QStringLiteral("obsDropStatistics"));
	QVERIFY(statistics);
	// sample задаёт исходные независимые счётчики и скорости OBS.
	const gost::ObsStatisticsSample sample{.videoAvailable = true,
					       .videoIdentity = 1,
					       .outputIdentity = 2,
					       .fps = 48.25,
					       .render = gost::FrameCounters{5, 100},
					       .encode = gost::FrameCounters{6, 100},
					       .network = gost::FrameCounters{7, 100},
					       .renderLimited = limitedCounter == 0,
					       .encodeLimited = limitedCounter == 1,
					       .networkLimited = limitedCounter == 2};
	// collector преобразует синтетическую выборку штатных OBS-счётчиков в наблюдение.
	gost::OBSStatsCollector collector;
	// observed содержит результат сборщика с независимыми признаками ограничения.
	const auto observed = collector.sample(sample, true, {});
	QCOMPARE(observed.renderLimited, sample.renderLimited);
	QCOMPARE(observed.encodeLimited, sample.encodeLimited);
	QCOMPARE(observed.networkLimited, sample.networkLimited);
	state.setObsStatistics(observed);
	QTRY_VERIFY_WITH_TIMEOUT(obsStatisticsText(dock).contains(QStringLiteral("48.25")), 1000);
	// counterValues содержит ожидаемые значения трёх независимых счётчиков.
	QStringList counterValues;
	// i адресует этап render, encode или network для отдельного ожидания.
	for (int i = 0; i < 3; ++i) {
		counterValues.append(i == limitedCounter ? text("Dock.ChangeUnavailable")
							 : text("Dock.Pair")
								   .arg(QString::number(i + 5), QStringLiteral("100"),
									QStringLiteral("%1.0%").arg(i + 5)));
	}
	// value содержит очередное ожидаемое значение отображения.
	for (const auto &value : counterValues)
		QCOMPARE(obsStatisticsText(dock).count(value), 1);
	QVERIFY(!dock.findChild<QLabel *>(QStringLiteral("obsStatisticsPeriod")));
}

void PluginLifecycleTest::dockKeepsLastValuesOnlyInTheSameContext_data()
{
	QTest::addColumn<QString>("locale");
	QTest::addColumn<QString>("currentRate");
	QTest::addColumn<QString>("lastRate");
	QTest::addColumn<QString>("resumedRate");
	QTest::addColumn<QString>("replacementRate");
	QTest::addColumn<QString>("currentSession");
	QTest::addColumn<QString>("noCurrentData");
	QTest::addColumn<QString>("lastValues");
	QTest::newRow("russian")
		<< QStringLiteral("ru-RU") << QStringLiteral("8,37") << QStringLiteral("9,37")
		<< QStringLiteral("10,37") << QStringLiteral("12,37") << QStringLiteral("Рестрим запущен")
		<< QStringLiteral("Данные отсутствуют") << QStringLiteral("Последние полученные значения");
	QTest::newRow("english")
		<< QStringLiteral("en-US") << QStringLiteral("8.37") << QStringLiteral("9.37")
		<< QStringLiteral("10.37") << QStringLiteral("12.37") << QStringLiteral("Restream running")
		<< QStringLiteral("No current data") << QStringLiteral("Last received values");
}

// Проверяет сохранение последних входных измерений только в том же исполнении и контексте Dock.
void PluginLifecycleTest::dockKeepsLastValuesOnlyInTheSameContext()
{
	// Один сценарий проходит полный жизненный цикл кеша: свежая метрика непосредственно перед
	// обрывом -> подписанное последнее значение -> возобновление без освежения -> новая точная метрика ->
	// неподтверждённый заменяющий снимок -> подтверждение -> другой UUID профиля. Отличающиеся
	// скорости позволяют заметить показ старого/преждевременно нового источника в обеих локализациях.
	// locale выбирает локализацию подписей и единиц.
	QFETCH(QString, locale);
	// currentRate задаёт первое подтверждённое измерение входа.
	QFETCH(QString, currentRate);
	// lastRate задаёт свежее значение непосредственно перед обрывом.
	QFETCH(QString, lastRate);
	// resumedRate задаёт отдельную метрику после восстановления транспорта.
	QFETCH(QString, resumedRate);
	// replacementRate отличает новое исполнение от прежнего кеша.
	QFETCH(QString, replacementRate);
	// currentSession задаёт подпись подтверждённой работающей Session.
	QFETCH(QString, currentSession);
	// noCurrentData обозначает отсутствие свежего наблюдения.
	QFETCH(QString, noCurrentData);
	// lastValues обозначает явное предупреждение о сохранённых метриках.
	QFETCH(QString, lastValues);
	using namespace gost;
	using namespace gost::api;
	// state хранит общие наблюдения на протяжении смены свежести и контекста.
	StateStore state;
	// origin задаёт синтетическую идентичность сервера без сетевых вызовов.
	const auto origin = QStringLiteral("http://127.0.0.1:8080");
	// streamId связывает выбор Dock с единственным сценарием потока.
	const auto streamId = QStringLiteral("stream");
	// sessionId задаёт долговечный идентификатор наблюдаемой Session.
	const auto sessionId = QStringLiteral("session");
	// profileId задаёт UUID исходного профиля OBS.
	const auto profileId = QStringLiteral("profile-a");
	// stream содержит имя и ID выбранного потока.
	Stream stream;
	stream.id = streamId;
	stream.name = QStringLiteral("Selected");
	state.setStreams({stream}, true);
	state.setConnection(origin, ConnectionStatus::Connected);
	state.setSelection(streamId, true, {}, {}, profileId);
	state.setObsState(QStringLiteral("OBS profile"), false);
	// text читает реальные переводы интерфейса из локализации поставки.
	const auto text = localeLookup(locale);
	// dock отображает настоящий Widgets-интерфейс без запуска OBS.
	RestreamDock dock(state, text);
	// outputs предоставляет строки и собственную прокрутку серверных выходов.
	auto *outputs = dock.findChild<QListView *>(QStringLiteral("outputList"));
	// reset меняет только локальную точку отсчёта накоплений.
	auto *reset = dock.findChild<QPushButton *>(QStringLiteral("statsResetButton"));
	// server показывает состояние транспорта отдельно от состояния Session.
	const auto *server = dock.findChild<QLabel *>(QStringLiteral("serverObservation"));
	// sessionLabel показывает подтверждение оперативного состояния Session.
	const auto *sessionLabel = dock.findChild<QLabel *>(QStringLiteral("sessionObservation"));
	// ingest содержит три столбца измерений входа с допустимыми точками переноса.
	const auto *ingest = dock.findChild<QLabel *>(QStringLiteral("ingestStatistics"));
	// notice показывает отсутствие свежести и наличие последних значений.
	const auto *notice = dock.findChild<QLabel *>(QStringLiteral("serverDataNotice"));
	// localStatistics позволяет проверить независимое обновление метрик OBS.
	const auto *localStatistics = dock.findChild<QLabel *>(QStringLiteral("obsStatistics"));
	QVERIFY(outputs && reset && server && sessionLabel && ingest && notice && localStatistics);

	// session содержит исходное исполнение и его серверные измерения.
	RuntimeSession session;
	session.sessionId = sessionId;
	session.observation = {FieldState::Value, Observation{.instanceId = QStringLiteral("epoch:1"),
							      .state = QStringLiteral("RUNNING")}};
	// input хранит исходные метрики входа независимо от представления.
	Input input;
	input.connected = true;
	input.metrics.state = FieldState::Value;
	input.metrics.value.sourceEpoch = 1;
	// inputBitrate изменяет только синтетический bitrate очередной серверной метрики.
	auto &inputBitrate = input.metrics.value.media.rates.payloadBitrate;
	inputBitrate.status = QStringLiteral("available");
	inputBitrate.value = {FieldState::Value, 8370000.0};
	session.input = {FieldState::Value, input};
	// metadata подтверждает Session и полный состав выходов выбранного потока.
	DockServerState metadata;
	metadata.sessionStatus = {RuntimeAvailability::Fresh, true, {}};
	metadata.session = DockSession{sessionId, QStringLiteral("RUNNING"), false};
	metadata.outputsStatus = {RuntimeAvailability::Fresh, true, {}};
	// i адресует один из двадцати выходов для проверки сохранения состава и прокрутки.
	for (int i = 0; i < 20; ++i) {
		// output содержит наблюдение отдельного синтетического выхода.
		RuntimeOutput output;
		output.instanceId = QStringLiteral("epoch:%1").arg(i + 2);
		output.outputId = QStringLiteral("out-%1").arg(i);
		output.state = QStringLiteral("PUBLISHING");
		if (i == 0) {
			output.metrics.state = FieldState::Value;
			output.metrics.value.counters.insert(QStringLiteral("reconnects"), 7);
			output.metrics.value.queueDrops.insert(QStringLiteral("video"), 11);
			output.metrics.value.deliveryDrops.insert(QStringLiteral("video"), 13);
		}
		metadata.outputs.insert(output.outputId, {QStringLiteral("Channel %1").arg(i), true, true});
		session.outputs.append(output);
	}
	QVERIFY(state.installAnchor({QStringLiteral("anchor"), {session}, {FieldState::Null, {}}}));
	state.setDockServer(metadata);
	dock.resize(360, 560);
	dock.show();
	QTRY_VERIFY_WITH_TIMEOUT(ingestStatisticsText(ingest).contains(currentRate), 1000);
	QVERIFY(sessionLabel->text().contains(currentSession));
	QVERIFY(!notice->text().contains(lastValues));
	QCOMPARE(outputs->model()->rowCount(), 20);
	// firstRow адресует первый выход без зависимости от текста его подробностей.
	const auto firstRow = outputs->model()->index(0, 0);
	// rowBeforeReset сохраняет видимые серверные накопления до локального сброса.
	const auto rowBeforeReset = firstRow.data().toString();
	QVERIFY(rowBeforeReset.contains(QStringLiteral("Channel 0")));
	QVERIFY(rowBeforeReset.contains(QStringLiteral("11")));
	QVERIFY(rowBeforeReset.contains(QStringLiteral("13")));
	QVERIFY(reset->isEnabled());
	reset->click();
	// rowAfterReset содержит новое числовое значение серверных накоплений без изменения источника.
	const auto rowAfterReset = firstRow.data().toString();
	QVERIFY(rowAfterReset.contains(text("Dock.PeriodReset")));
	QVERIFY(!rowAfterReset.contains(QStringLiteral("11")));
	QVERIFY(!rowAfterReset.contains(QStringLiteral("13")));
	// raw сохраняет серверные счётчики после сброса для проверки неизменности источника.
	const auto raw = state.runtime().sessions.value(sessionId).outputs.front().metrics.value;
	QCOMPARE(raw.counters.value(QStringLiteral("reconnects")), quint64{7});
	QCOMPARE(raw.queueDrops.value(QStringLiteral("video")), quint64{11});
	QCOMPARE(raw.deliveryDrops.value(QStringLiteral("video")), quint64{13});
	QTRY_VERIFY_WITH_TIMEOUT(outputs->verticalScrollBar()->maximum() > 0, 1000);
	QVERIFY(dock.width() >= 300 && dock.width() <= 400);
	outputs->scrollToBottom();
	QTRY_VERIFY_WITH_TIMEOUT(outputs->verticalScrollBar()->value() > 0, 1000);
	QVERIFY(!server->visibleRegion().isEmpty());
	QVERIFY(!sessionLabel->visibleRegion().isEmpty());

	// metric публикует управляемые обновления входа с возрастающими ревизиями.
	RuntimeEvent metric;
	metric.cursor = QStringLiteral("latest-before-disconnect");
	metric.kind = QStringLiteral("metrics");
	metric.owner = QStringLiteral("session");
	metric.sessionId = sessionId;
	metric.observation.instanceId = session.observation->instanceId;
	metric.observation.revision = 1;
	inputBitrate.value = {FieldState::Value, 9370000.0};
	metric.input = {FieldState::Value, input};
	QVERIFY(state.applyEvent(metric));
	// Свежая метрика приходит непосредственно перед обрывом и должна попасть в кеш последних значений Dock.
	state.markRuntimeStale();
	state.setConnection(origin, ConnectionStatus::Reconnecting);
	QTRY_VERIFY_WITH_TIMEOUT(notice->text().contains(noCurrentData), 1000);
	QVERIFY(notice->text().contains(lastValues));
	// lastBitrate содержит полное ожидаемое значение с подписью устаревшего измерения.
	const auto lastBitrate = text("Dock.LastValue").arg(text("Dock.Megabits").arg(lastRate));
	QVERIFY(ingestStatisticsText(ingest).contains(lastBitrate));
	QVERIFY(!ingestStatisticsText(ingest).contains(currentRate));
	state.setConnection(origin, ConnectionStatus::Connected);
	QTRY_VERIFY_WITH_TIMEOUT(notice->text().contains(lastValues), 1000);
	QVERIFY(ingestStatisticsText(ingest).contains(lastBitrate));

	metric.cursor = QStringLiteral("exact-metric-after-reconnect");
	metric.observation.revision = 2;
	inputBitrate.value = {FieldState::Value, 10370000.0};
	metric.input = {FieldState::Value, input};
	QVERIFY(state.applyEvent(metric));
	QCOMPARE(state.dockServer().sessionStatus.availability, RuntimeAvailability::Stale);
	QTRY_VERIFY_WITH_TIMEOUT(ingestStatisticsText(ingest).contains(resumedRate), 1000);
	// resumedBitrate задаёт новое значение после отдельного подтверждения метрики.
	const auto resumedBitrate = text("Dock.Megabits").arg(resumedRate);
	// lastResumedBitrate задаёт то же измерение с предупреждением после повторного обрыва.
	const auto lastResumedBitrate = text("Dock.LastValue").arg(resumedBitrate);
	QVERIFY(!ingestStatisticsText(ingest).contains(lastResumedBitrate));
	QVERIFY(!sessionLabel->text().contains(currentSession));

	state.markRuntimeStale();
	state.setConnection(origin, ConnectionStatus::Reconnecting);
	QTRY_VERIFY_WITH_TIMEOUT(ingestStatisticsText(ingest).contains(lastResumedBitrate), 1000);
	session.observation->instanceId = QStringLiteral("replacement:1");
	session.outputs.front().instanceId = QStringLiteral("replacement:2");
	inputBitrate.value = {FieldState::Value, 12370000.0};
	session.input = {FieldState::Value, input};
	QVERIFY(state.installAnchor({QStringLiteral("replacement-anchor"), {session}, {FieldState::Null, {}}}, true));
	state.setObsStatistics({.fps = 43.0, .observed = true});
	QTRY_VERIFY_WITH_TIMEOUT(displayedText(localStatistics).contains(QStringLiteral("43")), 1000);
	QVERIFY(ingestStatisticsText(ingest).contains(lastResumedBitrate));
	QVERIFY(!ingestStatisticsText(ingest).contains(replacementRate));
	state.setConnection(origin, ConnectionStatus::Connected);
	QTRY_VERIFY_WITH_TIMEOUT(ingestStatisticsText(ingest).contains(replacementRate), 1000);
	QVERIFY(!ingestStatisticsText(ingest).contains(resumedRate));

	state.setSelection(streamId, true, {}, {}, QStringLiteral("profile-b"));
	QTRY_VERIFY_WITH_TIMEOUT(!ingestStatisticsText(ingest).contains(replacementRate), 1000);
	QVERIFY(!notice->text().contains(lastValues));
	QVERIFY(!sessionLabel->text().contains(currentSession));
}

void PluginLifecycleTest::managementNavigationHasLocalizedPagesAndIcons_data()
{
	QTest::addColumn<QString>("locale");
	QTest::addColumn<QStringList>("pages");
	QTest::newRow("russian") << QStringLiteral("ru-RU") << QStringList{QStringLiteral("Потоки"),
									   QStringLiteral("Выходы"),
									   QStringLiteral("Группы кодирования"),
									   QStringLiteral("Профили"),
									   QStringLiteral("Заглушки"),
									   QStringLiteral("Диагностика"),
									   QStringLiteral("Настройки"),
									   QStringLiteral("Расширенные")};
	QTest::newRow("english") << QStringLiteral("en-US")
				 << QStringList{QStringLiteral("Streams"),         QStringLiteral("Outputs"),
						QStringLiteral("Encoding groups"), QStringLiteral("Profiles"),
						QStringLiteral("Fallback"),        QStringLiteral("Diagnostics"),
						QStringLiteral("Settings"),        QStringLiteral("Advanced")};
}

// Проверяет отдельные локализованные страницы и сохранение действующих сообщений настроек.
void PluginLifecycleTest::managementNavigationHasLocalizedPagesAndIcons()
{
	// locale выбирает словарь OBS, pages задаёт независимый ожидаемый порядок разделов.
	QFETCH(QString, locale);
	QFETCH(QStringList, pages);
	// text переводит настоящие ресурсы, state/window создают единственное изолированное окно управления.
	const auto text = localeLookup(locale);
	gost::StateStore state;
	gost::ManagementWindow window(state, text);
	// navigation/stack должны сопоставлять каждый самостоятельный раздел одной странице.
	auto *navigation = window.findChild<QListWidget *>();
	auto *stack = window.findChild<QStackedWidget *>();
	QVERIFY(navigation);
	QVERIFY(stack);
	QCOMPARE(navigation->count(), pages.size());
	QCOMPARE(stack->count(), pages.size());
	// index адресует каждую страницу; изображение проверяется и для резервной иконки Qt.
	for (int index = 0; index < pages.size(); ++index) {
		QCOMPARE(navigation->item(index)->text(), pages.at(index));
		QVERIFY(!navigation->item(index)->icon().pixmap(navigation->iconSize()).isNull());
		navigation->setCurrentRow(index);
		QCOMPARE(stack->currentIndex(), index);
		QVERIFY(stack->currentWidget());
	}
	QVERIFY(!stack->widget(2)->findChild<QTabWidget *>());
	QVERIFY(!stack->widget(3)->findChild<QTabWidget *>());
	QVERIFY(stack->widget(2) != stack->widget(3));
	// retries/message подтверждают сохранение настроек и реальных inline-сообщений в «Расширенных».
	auto *retries = stack->currentWidget()->findChild<QSpinBox *>(QStringLiteral("sseRejectionRetries"));
	auto *message = stack->currentWidget()->findChild<QLabel *>(QStringLiteral("advancedSettingsMessage"));
	QVERIFY(retries && message);
	QVERIFY(message->isHidden());
	state.setPreferences(false, true, 3, true);
	QVERIFY(!message->isHidden());
	QCOMPARE(message->text(), text("Settings.Unsaved"));
}

void PluginLifecycleTest::failedDockRegistrationReleasesResources()
{
	FakeFrontend frontend;
	frontend.acceptDock = false;
	{
		gost::PluginLifecycle lifecycle(frontend, localeLookup());
		// При отказе OBS не получает владение виджетом, поэтому его удаляет путь отката загрузки.
		QVERIFY(!lifecycle.load());
		QVERIFY(!frontend.content);
		QVERIFY(!lifecycle.stateStore());
		QVERIFY(!frontend.activeCallback);
		QCOMPARE(frontend.callbackRemoves, frontend.callbackAdds);
		QCOMPARE(frontend.dockRemoves, 0);
		lifecycle.shutdown();
	}
	QCOMPARE(frontend.callbackRemoves, frontend.callbackAdds);
	QCOMPARE(frontend.invalidCallbackOperations, 0);
	QVERIFY(managementWindows().isEmpty());
}

// EXIT удаляет QAction до Dock и runtime; удаление и поздние события не создают окно заново.
void PluginLifecycleTest::exitCleansSynchronouslyAndLeavesInertShell()
{
	// frontend сохраняет порядок удаления и последнюю пару callback/cookie только до жизни lifecycle.
	FakeFrontend frontend;
	{
		// lifecycle остаётся инертной оболочкой после синхронной обработки Exit.
		gost::PluginLifecycle lifecycle(frontend, localeLookup());
		QVERIFY(lifecycle.load());
		frontend.managementAction->trigger();
		QCOMPARE(managementWindows().size(), 1);
		QVERIFY(frontend.lastCallback);
		// state, dock, window и action отслеживают освобождение ресурсов до следующего оборота событий.
		QPointer<gost::StateStore> state = lifecycle.stateStore();
		QPointer<QWidget> dock = frontend.content;
		QPointer<gost::ManagementWindow> window = managementWindows().front();
		QPointer<QAction> action = frontend.managementAction;
		// unrelated представляет соседний пункт другого плагина, который очистка GOST Restreamer обязана сохранить.
		auto *unrelated = frontend.toolsMenu->addAction(QStringLiteral("Other plugin"));
		// Обработчик удаления пробует повторно открыть окно в середине shutdown.
		QObject::connect(action.data(), &QObject::destroyed, &lifecycle,
				 [&lifecycle] { lifecycle.openManagementWindow(); });
		frontend.emitEvent(gost::FrontendEvent::Exit);
		QVERIFY(!state && !dock && !window && !action);
		QVERIFY(!lifecycle.stateStore() && !frontend.activeCallback);
		QCOMPARE(frontend.toolsMenu->actions(), QList<QAction *>{unrelated});
		QCOMPARE(frontend.callbackRemoves, 1);
		QCOMPARE(frontend.toolsMenuRemoves, 1);
		QCOMPARE(frontend.dockRemoves, 1);
		QCOMPARE(frontend.removedId, QStringLiteral("gost-restreamer-obs-plugin"));
		QCOMPARE(frontend.cleanupOrder, QStringList({QStringLiteral("callback"), QStringLiteral("toolsMenu"),
							     QStringLiteral("dock")}));
		lifecycle.shutdown();
		lifecycle.openManagementWindow();
		QVERIFY(!lifecycle.load());
		frontend.lastCallback(gost::FrontendEvent::ProfileChanged, frontend.lastCookie);
		frontend.lastCallback(gost::FrontendEvent::Exit, frontend.lastCookie);
		dispatchQueuedEvents();
		QVERIFY(managementWindows().isEmpty());
		QCOMPARE(frontend.callbackRemoves, 1);
		QCOMPARE(frontend.toolsMenuRemoves, 1);
		QCOMPARE(frontend.dockRemoves, 1);
	}
	QCOMPARE(frontend.callsAfterExit, 0);
	QCOMPARE(frontend.invalidCallbackOperations, 0);
}

// Охватывает отказ до передачи Dock, null/исключение меню и исключение после создания QAction.
void PluginLifecycleTest::registrationExceptionReleasesAcquiredResources_data()
{
	QTest::addColumn<QString>("failure");
	QTest::newRow("dock-exception") << QStringLiteral("dock");
	QTest::newRow("menu-null") << QStringLiteral("menu-null");
	QTest::newRow("menu-exception") << QStringLiteral("menu-exception");
	QTest::newRow("callback-exception") << QStringLiteral("callback");
}

// Отказ каждого этапа освобождает только приобретённые ресурсы и не оставляет пункт меню в OBS.
void PluginLifecycleTest::registrationExceptionReleasesAcquiredResources()
{
	// failure выбирает управляемую точку отказа без настоящего OBS.
	QFETCH(QString, failure);
	// frontend удерживает меню даже после отката, позволяя проверить удаление QAction из actions().
	FakeFrontend frontend;
	frontend.throwOnDockAdd = failure == QStringLiteral("dock");
	frontend.acceptToolsMenu = failure != QStringLiteral("menu-null");
	frontend.throwOnToolsMenuAdd = failure == QStringLiteral("menu-exception");
	frontend.throwOnCallbackAdd = failure == QStringLiteral("callback");
	{
		// lifecycle создаёт ресурсы поэтапно и должен синхронно откатить любую неуспешную загрузку.
		gost::PluginLifecycle lifecycle(frontend, localeLookup());
		QVERIFY(!lifecycle.load());
		QVERIFY(!lifecycle.stateStore());
		QVERIFY(!frontend.content && !frontend.dock && !frontend.managementAction);
		QVERIFY(!frontend.activeCallback);
		QCOMPARE(frontend.toolsMenuAdds, frontend.throwOnDockAdd ? 0 : 1);
		QCOMPARE(frontend.dockRemoves, frontend.throwOnDockAdd ? 0 : 1);
		QCOMPARE(frontend.toolsMenuRemoves, frontend.throwOnCallbackAdd ? 1 : 0);
		if (frontend.toolsMenu)
			QVERIFY(frontend.toolsMenu->actions().isEmpty());
		if (frontend.throwOnCallbackAdd)
			QCOMPARE(frontend.cleanupOrder,
				 QStringList({QStringLiteral("toolsMenu"), QStringLiteral("dock")}));
		lifecycle.shutdown();
	}
	QCOMPARE(frontend.dockRemoves, frontend.throwOnDockAdd ? 0 : 1);
	QCOMPARE(frontend.toolsMenuRemoves, frontend.throwOnCallbackAdd ? 1 : 0);
	QCOMPARE(frontend.callbackRemoves, 0);
	QVERIFY(managementWindows().isEmpty());
}

void PluginLifecycleTest::callbackExceptionDoesNotEscape()
{
	// Исключение чтения профиля происходит внутри обработчика C API. Оно должно остановить только текущую
	// обработку, сохранить жизненный цикл и позволить следующему достоверному событию обновить состояние.
	FakeFrontend frontend;
	gost::PluginLifecycle lifecycle(frontend, localeLookup());
	QVERIFY(lifecycle.load());
	frontend.throwOnProfileRead = true;
	frontend.emitEvent(gost::FrontendEvent::FinishedLoading);
	QVERIFY(lifecycle.stateStore());
	QVERIFY(!lifecycle.stateStore()->profileName().has_value());
	QVERIFY(!lifecycle.stateStore()->streamingActive().has_value());
	frontend.throwOnProfileRead = false;
	frontend.emitEvent(gost::FrontendEvent::ProfileChanged);
	QVERIFY(lifecycle.stateStore()->profileName().has_value());
	QCOMPARE(lifecycle.stateStore()->profileName().value(), frontend.profile);
	frontend.emitEvent(gost::FrontendEvent::Exit);
	QVERIFY(!lifecycle.stateStore());
	QCOMPARE(frontend.callsAfterExit, 0);
}

// Отложенное обращение к ещё живой оболочке lifecycle не создаёт окно после завершения runtime.
void PluginLifecycleTest::queuedOpenCannotResurrectRuntime()
{
	// frontend считает обращения за допустимой фазой Exit без настоящего OBS.
	FakeFrontend frontend;
	// lifecycle переживает собственный runtime и остаётся получателем отложенного вызова.
	gost::PluginLifecycle lifecycle(frontend, localeLookup());
	QVERIFY(lifecycle.load());
	// Callback обращается к живой оболочке лишь после синхронного завершения runtime.
	QVERIFY(QMetaObject::invokeMethod(
		&lifecycle, [&lifecycle] { lifecycle.openManagementWindow(); }, Qt::QueuedConnection));
	frontend.emitEvent(gost::FrontendEvent::Exit);
	dispatchQueuedEvents();
	QVERIFY(managementWindows().isEmpty());
	QCOMPARE(frontend.toolsMenuRemoves, 1);
	QCOMPARE(frontend.callsAfterExit, 0);
}

// Доставленный после EXIT сигнал QAction не пересоздаёт runtime и не обращается к уже недоступному OBS.
void PluginLifecycleTest::queuedToolsMenuSignalCannotResurrectRuntime()
{
	// frontend удерживает QAction до синхронного Exit и считает поздние вызовы OBS.
	FakeFrontend frontend;
	// lifecycle сохраняет инертную оболочку после удаления получателя queued-сигнала.
	gost::PluginLifecycle lifecycle(frontend, localeLookup());
	QVERIFY(lifecycle.load());
	// action используется только до Exit: вызов triggered от рабочего потока ставит реальное соединение в очередь GUI.
	auto *action = frontend.managementAction.data();
	QVERIFY(action);
	// delivery испускает только сигнал без вызова методов виджета вне GUI-потока.
	std::thread delivery([action] { action->triggered(false); });
	delivery.join();
	QVERIFY(managementWindows().isEmpty());
	frontend.emitEvent(gost::FrontendEvent::Exit);
	dispatchQueuedEvents();
	QVERIFY(managementWindows().isEmpty());
	QCOMPARE(frontend.toolsMenuRemoves, 1);
	QCOMPARE(frontend.callsAfterExit, 0);
}

// Удаление главным окном меню, Dock и ManagementWindow не приводит к повторному освобождению.
void PluginLifecycleTest::hostDestructionDoesNotDoubleDelete()
{
	// frontend позволяет удалить Qt-родителя раньше завершения плагина.
	FakeFrontend frontend;
	{
		// lifecycle переживает родителя виджетов и должен безопасно освободить оставшийся runtime.
		gost::PluginLifecycle lifecycle(frontend, localeLookup());
		QVERIFY(lifecycle.load());
		frontend.managementAction->trigger();
		// dock, action и window наблюдают удаление родителем; state остаётся собственностью runtime.
		QPointer<QWidget> dock = frontend.content;
		QPointer<QAction> action = frontend.managementAction;
		QPointer<gost::ManagementWindow> window = managementWindows().front();
		QPointer<gost::StateStore> state = lifecycle.stateStore();
		frontend.host.reset();
		QVERIFY(!dock && !action && !window);
		QVERIFY(state);
		QCOMPARE(frontend.toolsMenuRemoves, 1);
		frontend.emitEvent(gost::FrontendEvent::Exit);
		QVERIFY(!state);
		QVERIFY(managementWindows().isEmpty());
	}
	QCOMPARE(frontend.toolsMenuRemoves, 1);
	QCOMPARE(frontend.callsAfterExit, 0);
	QCOMPARE(frontend.invalidCallbackOperations, 0);
}

void PluginLifecycleTest::queuedFrontendEventIsDiscardedOnExit()
{
	FakeFrontend frontend;
	gost::PluginLifecycle lifecycle(frontend, localeLookup());
	QVERIFY(lifecycle.load());
	frontend.emitEvent(gost::FrontendEvent::FinishedLoading);
	QPointer<gost::StateStore> state = lifecycle.stateStore();
	QVERIFY(state->profileName().has_value());
	const auto observedProfile = state->profileName().value();
	const int profileReads = frontend.profileReads;
	const int streamingReads = frontend.streamingReads;
	frontend.profile = QStringLiteral("Профиль после события из рабочего потока");
	// Поток только ставит событие в очередь и завершается до Exit; проверяется порядок доставки без гонки потоков.
	std::thread eventThread([&frontend] { frontend.emitEvent(gost::FrontendEvent::ProfileChanged); });
	eventThread.join();
	QCOMPARE(frontend.profileReads, profileReads);
	QCOMPARE(frontend.streamingReads, streamingReads);
	QCOMPARE(state->profileName().value(), observedProfile);
	frontend.emitEvent(gost::FrontendEvent::Exit);
	QVERIFY(!state);
	dispatchQueuedEvents();
	QVERIFY(!lifecycle.stateStore());
	QVERIFY(managementWindows().isEmpty());
	QCOMPARE(frontend.profileReads, profileReads);
	QCOMPARE(frontend.streamingReads, streamingReads);
	QCOMPARE(frontend.callsAfterExit, 0);
}

void PluginLifecycleTest::localesAreComplete()
{
	// Сравнение множеств ключей и непустых значений проверяет файлы обеих локализаций поставки.
	// Корректность выбранной подписи конкретного состояния проверяют сценарии виджетов выше.
	QSettings russian(QStringLiteral(GOST_TEST_LOCALE_DIR) + QStringLiteral("/ru-RU.ini"), QSettings::IniFormat);
	QSettings english(QStringLiteral(GOST_TEST_LOCALE_DIR) + QStringLiteral("/en-US.ini"), QSettings::IniFormat);
	QCOMPARE(russian.status(), QSettings::NoError);
	QCOMPARE(english.status(), QSettings::NoError);
	QVERIFY(!russian.allKeys().isEmpty());
	QCOMPARE(russian.allKeys(), english.allKeys());
	for (const auto &key : russian.allKeys()) {
		QVERIFY(!russian.value(key).toString().isEmpty());
		QVERIFY(!english.value(key).toString().isEmpty());
	}
}

void PluginLifecycleTest::hiddenManagementKeepsOneSubscriptionAndMetadataVersion()
{
	// Закрытие окна не должно пересоздать controller/SSE. Событие принимается именно во время
	// скрытого окна, затем проверяется тот же экземпляр UI и ровно одна подписка. Версия поступает
	// через PluginOptions и проверяется как текст метаданных, а не захардкоженное число виджета.
	WorkspaceTemp temp(QStringLiteral("runtime-ui"));
	QVERIFY(temp.isValid());
	HttpServer server;
	QVERIFY(server.listen());
	QList<HttpServer::Request> subscriptions;
	server.handler = [&](const auto &request) {
		if (request.target.startsWith("/api/v1/health/ready"))
			HttpServer::reply(request, R"({"status":"ready","reason":""})");
		else if (request.target.startsWith("/api/v1/system/capabilities"))
			HttpServer::reply(request, fixture::capabilities());
		else if (request.target.startsWith("/api/v1/streams"))
			HttpServer::reply(request, fixture::page({fixture::stream()}));
		else if (request.target.startsWith("/api/v1/events/snapshot"))
			HttpServer::reply(request, R"({"cursor":"anchor","sessions":[],"gpu":null})");
		else if (request.target.startsWith("/api/v1/events?")) {
			subscriptions.append(request);
			HttpServer::stream(request, ": heartbeat\n\n");
		}
	};
	FakeFrontend frontend;
	const QString version = QStringLiteral("7.8.9-metadata");
	gost::PluginLifecycle lifecycle(frontend, localeLookup(),
					{version, temp.filePath(QStringLiteral("settings.json"))});
	QVERIFY(lifecycle.load());
	lifecycle.settingsStore()->setAutoConnect(false);
	frontend.emitEvent(gost::FrontendEvent::FinishedLoading);
	QVERIFY(lifecycle.connectionController());
	lifecycle.connectionController()->connectToServer(server.origin().toString());
	QTRY_COMPARE_WITH_TIMEOUT(subscriptions.size(), 1, 3000);
	auto *versionLabel = frontend.content->findChild<QLabel *>(QStringLiteral("pluginVersion"));
	QVERIFY(versionLabel);
	QVERIFY(versionLabel->text().contains(version));
	QCOMPARE(versionLabel->textFormat(), Qt::PlainText);
	lifecycle.openManagementWindow();
	QCOMPARE(managementWindows().size(), 1);
	QPointer<gost::ManagementWindow> window = managementWindows().front();
	QPointer<gost::StateStore> state = lifecycle.stateStore();
	QPointer<gost::ConnectionController> controller = lifecycle.connectionController();
	window->close();
	dispatchQueuedEvents();
	QVERIFY(!window->isVisible());
	QVERIFY(state && controller);
	subscriptions.front().socket->write(
		fixture::frame(fixture::event(QStringLiteral("state"), QStringLiteral("while-hidden"))));
	QTRY_COMPARE_WITH_TIMEOUT(state->runtime().cursor, QStringLiteral("while-hidden"), 2000);
	lifecycle.openManagementWindow();
	QCOMPARE(managementWindows().size(), 1);
	QCOMPARE(managementWindows().front(), window.data());
	QCOMPARE(lifecycle.connectionController(), controller.data());
	QCOMPARE(subscriptions.size(), 1);
	QVERIFY(window->isVisible());
	frontend.emitEvent(gost::FrontendEvent::Exit);
	QVERIFY(!state && !controller && !window);
	dispatchQueuedEvents();
	QCOMPARE(frontend.callsAfterExit, 0);
	for (const auto &request : server.requests)
		QCOMPARE(request.method, QByteArray("GET"));
}

void PluginLifecycleTest::largeStreamListAndRuntimeTrafficAllowGuiProgress()
{
	// 100 страниц по 100 Stream исчерпывают штатный бюджет элементов и страниц, но последний пустой cursor
	// подтверждает полноту. Runtime содержит 64 Session с независимыми Group/Output/FFmpeg.
	// Таймер переключает настоящую навигацию во время чтения и SSE-пачек. Оракул — продвижение GUI,
	// правильные конечные DTO и выбор Stream; напечатанные длительности не являются порогом приёмки.
	using namespace gost::api;
	// streamCount задаёт полный штатный предел списка Streams.
	constexpr int streamCount = 10000;
	// sessionCount задаёт число независимых Session в нагрузочном снимке.
	constexpr int sessionCount = 64;
	// outputCount задаёт число синтетических выходов в карточке.
	constexpr int outputCount = 8;
	// eventCount задаёт число применяемых SSE-метрик.
	constexpr int eventCount = 256;
	// batchSize задаёт число событий в управляемой пачке.
	constexpr int batchSize = 32;
	// pages хранит сто синтетических страниц списка Streams.
	QVector<QByteArray> pages;
	// page адресует текущую сотню Streams при построении страниц.
	for (int page = 0; page < 100; ++page) {
		// items содержит Stream DTO одной страницы списка.
		QJsonArray items;
		// index адресует очередной синтетический выход или элемент сценарного набора.
		for (int index = page * 100; index < (page + 1) * 100; ++index) {
			// stream содержит имя и точный ID потока, полученного для сценария.
			auto stream = fixture::stream(QStringLiteral("stream-%1").arg(index));
			stream.insert(QStringLiteral("name"), QStringLiteral("Поток %1").arg(index));
			items.append(stream);
		}
		pages.append(fixture::page(items, page == 99 ? QString{} : QStringLiteral("page-%1").arg(page + 1)));
	}
	// anchor содержит исходное независимое runtime-дерево нагрузочного сценария.
	Anchor anchor{QStringLiteral("load-anchor"), {}, {FieldState::Null, {}}};
	// index адресует очередной синтетический выход или элемент сценарного набора.
	for (int index = 0; index < sessionCount; ++index) {
		// session определяет Session синтетического снимка или очередной метрики.
		RuntimeSession session;
		session.sessionId = QStringLiteral("session-%1").arg(index);
		session.observation = {
			FieldState::Value,
			{QStringLiteral("session-instance-%1").arg(index), 1, {}, QStringLiteral("RUNNING"), {}}};
		// group адресует очередную группу внутри Session.
		for (int group = 0; group < 4; ++group) {
			// node содержит очередной узел группы либо выхода синтетического снимка.
			RuntimeGroup node;
			node.instanceId = QStringLiteral("group-instance-%1-%2").arg(index).arg(group);
			node.groupId = QStringLiteral("group-%1").arg(group);
			node.profileId = QStringLiteral("profile-%1").arg(group);
			node.state = QStringLiteral("READY");
			node.revision = 1;
			node.metrics = {FieldState::Value, {}};
			session.groups.append(node);
			// role выбирает независимое исполнение live либо fallback в группе.
			for (const auto &role : {QStringLiteral("live"), QStringLiteral("fallback")}) {
				// ffmpeg содержит узел исполнения одной роли в синтетическом снимке.
				RuntimeFFmpeg ffmpeg;
				ffmpeg.instanceId = node.instanceId + QLatin1Char('-') + role;
				ffmpeg.groupInstanceId = node.instanceId;
				ffmpeg.role = role;
				ffmpeg.state = QStringLiteral("RUNNING");
				ffmpeg.metrics = {FieldState::Value, {}};
				session.ffmpeg.append(ffmpeg);
			}
		}
		// output определяет очередной выход внутри Session.
		for (int output = 0; output < outputCount; ++output) {
			// node содержит очередной узел группы либо выхода синтетического снимка.
			RuntimeOutput node;
			node.outputId = QStringLiteral("output-%1").arg(output);
			node.instanceId = QStringLiteral("output-instance-%1-%2").arg(index).arg(output);
			node.state = QStringLiteral("PUBLISHING");
			node.revision = 1;
			node.metrics = {FieldState::Value, {}};
			session.outputs.append(node);
		}
		anchor.sessions.append(session);
	}
	// state хранит исходные наблюдения, которые представления не должны изменять.
	gost::StateStore state;
	// window предоставляет единое окно управления на том же общем состоянии.
	gost::ManagementWindow window(state, localeLookup());
	// dock отражает большой рабочий список отдельно от навигации окна управления.
	gost::RestreamDock dock(state, localeLookup());
	window.show();
	// selector отражает выбранный Stream и точные ID вариантов.
	auto *selector = dock.findChild<QComboBox *>(QStringLiteral("streamSelector"));
	// navigation переключает разделы окна во время сетевой нагрузки.
	auto *navigation = window.findChild<QListWidget *>(QStringLiteral("managementNavigation"));
	// stack показывает страницу, которую должна выбрать навигация.
	auto *stack = window.findChild<QStackedWidget *>(QStringLiteral("managementPages"));
	QVERIFY(selector && navigation && stack);
	// server обслуживает страницы и SSE только на петлевом интерфейсе.
	HttpServer server;
	QVERIFY(server.listen());
	// pageIndex адресует следующую синтетическую HTTP-страницу списка.
	int pageIndex = 0;
	// Обработчик request возвращает очередную страницу либо открывает поток SSE.
	server.handler = [&](const auto &request) {
		if (request.target.startsWith("/api/v1/streams") && pageIndex < pages.size())
			HttpServer::reply(request, pages[pageIndex++]);
		else
			HttpServer::stream(request, ": heartbeat\n\n");
	};
	// clock измеряет продвижение event loop без требования универсальной задержки.
	QElapsedTimer clock;
	clock.start();
	// last сохраняет время предыдущего GUI-tick в наносекундах.
	qint64 last = 0;
	// maximumGap сохраняет максимальный наблюдавшийся интервал между GUI-tick.
	qint64 maximumGap = 0;
	// uiProgress считает фактические переключения навигации.
	int uiProgress = 0;
	// progressDuringEvents считает продвижения GUI между первым и последним SSE-событием.
	int progressDuringEvents = 0;
	// received считает обработанные SSE-события.
	int received = 0;
	// guiConsistent сохраняет соответствие текущей страницы и строки навигации.
	bool guiConsistent = true;
	// ticker инициирует переключения навигации; callback измеряет интервал и увеличивает счётчики продвижения.
	QTimer ticker;
	connect(&ticker, &QTimer::timeout, &window, [&] {
		// now содержит монотонное время текущего GUI-tick в наносекундах.
		const auto now = clock.nsecsElapsed();
		maximumGap = qMax(maximumGap, now - last);
		last = now;
		navigation->setCurrentRow((navigation->currentRow() + 1) % navigation->count());
		guiConsistent = guiConsistent && stack->currentIndex() == navigation->currentRow();
		++uiProgress;
		if (received > 0 && received < eventCount)
			++progressDuringEvents;
	});
	ticker.start(0);
	// api читает список через настоящий асинхронный REST-клиент.
	APIClient api;
	api.configure(server.origin(), {1, 1});
	// loaded сигнализирует завершение обхода страниц.
	bool loaded = false;
	// listValid отражает независимую проверку полноты и числа полученных Streams.
	bool listValid = false;
	// listElapsed сохраняет фактическую длительность чтения списка в миллисекундах.
	qint64 listElapsed = 0;
	// Callback получает result списка, проверяет полноту и переносит Stream DTO в общее состояние.
	api.getAllStreams(&window, [&](auto, auto result) {
		listValid = result && result.value->complete && result.value->items.size() == streamCount;
		if (result)
			state.setStreams(std::move(result.value->items), result.value->complete);
		loaded = true;
		listElapsed = clock.elapsed();
	});
	QTRY_VERIFY_WITH_TIMEOUT(loaded, 10000);
	QVERIFY(listValid);
	QCOMPARE(selector->count(), streamCount + 1);
	state.setSelection(QStringLiteral("stream-9999"), true);
	QCOMPARE(selector->currentData().toString(), QStringLiteral("stream-9999"));
	// anchorStart сохраняет начало установки исходного снимка в наносекундах.
	const auto anchorStart = clock.nsecsElapsed();
	QVERIFY(state.installAnchor(anchor));
	// anchorElapsed содержит измеренную длительность установки снимка.
	const auto anchorElapsed = clock.nsecsElapsed() - anchorStart;
	QCOMPARE(state.runtime().sessions.size(), sessionCount);
	// events принимает настоящие локальные SSE-фрагменты.
	EventClient events;
	// failed фиксирует отказ подписки для ограниченного ожидания.
	bool failed = false;
	// applied сохраняет успешность применения всех полученных событий.
	bool applied = true;
	// Callback event применяет метрику и считает её; callback отказа прерывает ограниченные ожидания.
	events.subscribe(
		server.origin(), anchor.cursor, {1, 1}, &window,
		[&](auto, const auto &event) {
			// accepted содержит результат применения конкретной SSE-метрики.
			const bool accepted = state.applyEvent(event);
			applied = applied && accepted;
			++received;
			return accepted;
		},
		[&](auto, auto, bool) { failed = true; });
	QTRY_VERIFY_WITH_TIMEOUT(server.requests.size() == 101 || failed, 3000);
	QVERIFY(!failed);
	// trafficStart сохраняет начало передачи SSE-пачек в наносекундах.
	const auto trafficStart = clock.nsecsElapsed();
	// batch адресует текущую пачку синтетических событий.
	for (int batch = 0; batch < eventCount / batchSize; ++batch) {
		// wire собирает SSE-кадры одной пачки в исходные байты.
		QByteArray wire;
		// number задаёт номер события для уникального cursor и независимого ожидаемого счётчика.
		for (int number = batch * batchSize; number < (batch + 1) * batchSize; ++number) {
			// session определяет Session синтетического снимка или очередной метрики.
			const int session = number % sessionCount;
			// output определяет очередной выход внутри Session.
			const int output = (number / sessionCount) % outputCount;
			// event содержит метрику точного исполнения выбранного выхода.
			auto event =
				fixture::event(QStringLiteral("metrics"), QStringLiteral("load-event-%1").arg(number));
			event.insert(QStringLiteral("owner"), QStringLiteral("output"));
			event.insert(QStringLiteral("session_id"), QStringLiteral("session-%1").arg(session));
			event.insert(QStringLiteral("output_id"), QStringLiteral("output-%1").arg(output));
			event.insert(
				QStringLiteral("observation"),
				fixture::observation(QStringLiteral("output-instance-%1-%2").arg(session).arg(output),
						     2, QString{}));
			// metrics содержит независимый набор полей метрики выхода.
			auto metrics = fixture::outputMetrics();
			// media содержит счётчики медиапотока текущего серверного выхода.
			auto media = fixture::media();
			media.insert(QStringLiteral("video_frames"), QString::number(number + 1));
			metrics.insert(QStringLiteral("media"), media);
			event.insert(QStringLiteral("output"), metrics);
			wire += fixture::frame(event);
		}
		QVERIFY(server.requests.back().socket);
		server.requests.back().socket->write(wire);
		QTRY_VERIFY_WITH_TIMEOUT(received == (batch + 1) * batchSize || failed, 10000);
		QVERIFY(!failed);
	}
	// trafficElapsed содержит измеренную длительность передачи всех SSE-пачек.
	const auto trafficElapsed = clock.nsecsElapsed() - trafficStart;
	// Проверяется продвижение GUI во время нагрузки; измеренная задержка выводится без универсального порога.
	maximumGap = qMax(maximumGap, clock.nsecsElapsed() - last);
	ticker.stop();
	events.shutdown();
	QVERIFY(applied && guiConsistent);
	QVERIFY(uiProgress > 0);
	QVERIFY(progressDuringEvents > 0);
	QCOMPARE(events.confirmedCursor(), QStringLiteral("load-event-255"));
	QCOMPARE(state.runtime().sessions.size(), sessionCount);
	// number задаёт номер события для уникального cursor и независимого ожидаемого счётчика.
	for (int number = 0; number < eventCount; ++number) {
		// session определяет Session синтетического снимка или очередной метрики.
		const auto &session = state.runtime().sessions[QStringLiteral("session-%1").arg(number % sessionCount)];
		QCOMPARE(session.groups.size(), 4);
		QCOMPARE(session.ffmpeg.size(), 8);
		QCOMPARE(session.outputs.size(), outputCount);
		// output определяет очередной выход внутри Session.
		const auto &output = session.outputs[(number / sessionCount) % outputCount];
		QCOMPARE(output.state, QStringLiteral("PUBLISHING"));
		QCOMPARE(output.metrics.value.media.videoFrames, quint64(number + 1));
	}
	QCOMPARE(selector->count(), streamCount + 1);
	QCOMPARE(selector->currentData().toString(), QStringLiteral("stream-9999"));
	qInfo("Нагрузка GUI: %d Stream/100 страниц, %d Session, 256 групп, 512 Output, 512 FFmpeg; %d SSE metrics в пачках %d; список %lld мс, установка anchor %.3f мс, SSE %.3f мс, максимальный разрыв event loop %.3f мс, продвижений GUI %d (при SSE %d); Qt %s.",
	      streamCount, sessionCount, eventCount, batchSize, listElapsed, double(anchorElapsed) / 1000000.0,
	      double(trafficElapsed) / 1000000.0, double(maximumGap) / 1000000.0, uiProgress, progressDuringEvents,
	      qVersion());
}

void PluginLifecycleTest::runtimeUpdatePreservesOpenStreamPopup()
{
	// Выделенный пункт раскрытого списка отличается от сохранённого выбора. После обычного оперативного события
	// обязаны сохраниться видимость, выделение и прокрутка, а не только currentData комбобокса.
	// temp изолирует preferences; state хранит наблюдение, settings читает только сценарный файл.
	WorkspaceTemp temp(QStringLiteral("stream-popup"));
	QVERIFY(temp.isValid());
	// state хранит исходные наблюдения, которые представления не должны изменять.
	gost::StateStore state;
	// settings хранит preferences и привязки в сценарном файле.
	gost::SettingsStore settings(temp.filePath(QStringLiteral("settings.json")));
	// api/events создают независимые REST/SSE-каналы, controller координирует рабочий выбор.
	gost::api::APIClient api;
	// events принимает настоящие локальные SSE-фрагменты.
	gost::api::EventClient events;
	// controller координирует асинхронное подключение и точный выбор Stream.
	gost::ConnectionController controller(state, settings, api, events);
	// streams содержит тысячу вариантов; index задаёт номер, stream — очередной синтетический DTO.
	QVector<gost::api::Stream> streams;
	// index адресует очередной синтетический выход или элемент сценарного набора.
	for (int index = 0; index < 1000; ++index) {
		// stream содержит имя и точный ID потока, полученного для сценария.
		gost::api::Stream stream;
		stream.id = QStringLiteral("stream-%1").arg(index);
		stream.name = QStringLiteral("Поток %1").arg(index);
		streams.append(stream);
	}
	state.setStreams(streams, true);
	state.setSelection(QStringLiteral("stream-10"), true);
	state.setConnection(QStringLiteral("http://127.0.0.1:8080"), gost::ConnectionStatus::Connected);
	QVERIFY(state.installAnchor({QStringLiteral("popup-anchor"), {}, {gost::api::FieldState::Null, {}}}));
	// dock показывает рабочий список без настоящего OBS; selector открывает выбор потока.
	gost::RestreamDock dock(state, localeLookup(), nullptr, {}, nullptr, &controller);
	dock.show();
	// selector отражает выбранный Stream и точные ID вариантов.
	auto *selector = dock.findChild<QComboBox *>(QStringLiteral("streamSelector"));
	QVERIFY(selector);
	QVERIFY(selector->isEnabled());
	selector->showPopup();
	// view предоставляет раскрытый popup; highlighted/scroll фиксируют выделение и прокрутку до SSE.
	auto *view = selector->view();
	view->setCurrentIndex(view->model()->index(500, 0));
	view->scrollTo(view->currentIndex(), QAbstractItemView::PositionAtCenter);
	QTRY_VERIFY_WITH_TIMEOUT(view->isVisible(), 1000);
	const int highlighted = view->currentIndex().row();
	const int scroll = view->verticalScrollBar()->value();
	QVERIFY(scroll > 0);
	// Последующее SSE-обновление не должно пересоздать список, потерять выделение или прокрутку открытого меню.
	// update несёт новое наблюдение сеанса, не изменение списка конфигурации.
	gost::api::RuntimeEvent update;
	update.cursor = QStringLiteral("popup-event");
	update.kind = QStringLiteral("state");
	update.owner = QStringLiteral("session");
	update.sessionId = QStringLiteral("session");
	update.observation.instanceId = QStringLiteral("popup-session-instance");
	update.observation.revision = 1;
	update.observation.state = QStringLiteral("RUNNING");
	QVERIFY(state.applyEvent(update));
	QVERIFY(view->isVisible());
	QCOMPARE(view->currentIndex().row(), highlighted);
	QCOMPARE(view->verticalScrollBar()->value(), scroll);
	QCOMPARE(selector->currentData().toString(), QStringLiteral("stream-10"));
	selector->hidePopup();
}

void PluginLifecycleTest::savedStreamIsUnconfirmedUntilListIsComplete()
{
	// Один сохранённый ID проходит Unknown/неполный/полный пустой/полный содержащий список.
	// «Неподтверждён» и «Недоступен» должны различаться без удаления предпочтения пользователя.
	// state хранит исходные наблюдения, которые представления не должны изменять.
	gost::StateStore state;
	// text читает переводы из поставляемого файла локали.
	const auto text = localeLookup();
	// dock показывает настоящий Widgets-интерфейс на общем состоянии.
	gost::RestreamDock dock(state, text);
	// selector отражает выбранный Stream и точные ID вариантов.
	auto *selector = dock.findChild<QComboBox *>(QStringLiteral("streamSelector"));
	QVERIFY(selector);
	state.setSelection(QStringLiteral("saved-stream"), true);
	QCOMPARE(selector->currentText(), text("Stream.Unconfirmed").arg(QStringLiteral("saved-stream")));
	state.setStreams({}, false);
	QCOMPARE(selector->currentText(), text("Stream.Unconfirmed").arg(QStringLiteral("saved-stream")));
	state.setStreams({}, true);
	QCOMPARE(selector->currentText(), text("Stream.Unavailable").arg(QStringLiteral("saved-stream")));
	// stream содержит имя и точный ID потока, полученного для сценария.
	gost::api::Stream stream;
	stream.id = QStringLiteral("saved-stream");
	stream.name = QStringLiteral("Подтверждённый поток");
	state.setStreams({stream}, true);
	QCOMPARE(selector->currentText(), stream.name);
}

void PluginLifecycleTest::partialStreamListWarnsAndAllowsSelection_data()
{
	QTest::addColumn<QString>("locale");
	QTest::newRow("russian") << QStringLiteral("ru-RU");
	QTest::newRow("english") << QStringLiteral("en-US");
}

// Неполный список сохраняет точный выбор; безлимитный SSE применяется отдельным явным сохранением формы.
void PluginLifecycleTest::partialStreamListWarnsAndAllowsSelection()
{
	// Вторая страница по размеру не входит в общий бюджет, сохранённый Stream находится именно там.
	// Пока SSE-заголовки удерживаются, UI предупреждает о неполноте и позволяет выбрать уже полученный
	// Stream. Новый выбор сохраняется на диске, отменяет старую подписку и начинает собственные снимок и SSE.
	// locale определяет реальные ресурсы страницы; temp/server изолируют файлы и петлевые HTTP-запросы.
	QFETCH(QString, locale);
	WorkspaceTemp temp(QStringLiteral("partial-stream-list"));
	QVERIFY(temp.isValid());
	// server обслуживает петлевые HTTP-запросы частичного списка потоков.
	HttpServer server;
	QVERIFY(server.listen());
	// first содержит полученный Stream и cursor, second — не вошедший в бюджет сохранённый Stream.
	const auto first = fixture::page({fixture::stream(QStringLiteral("received"))}, QStringLiteral("next-page"));
	const auto second = fixture::page({fixture::stream(QStringLiteral("saved"))});
	// readyRequests/pageRequests/snapshotRequests/eventRequests считают независимые фазы подключения.
	int readyRequests = 0;
	int pageRequests = 0;
	int snapshotRequests = 0;
	int eventRequests = 0;
	// firstSubscription удерживает исходную подписку, selectedSubscription — подписку нового выбора.
	HttpServer::Request firstSubscription;
	HttpServer::Request selectedSubscription;
	// Обработчик request отвечает только синтетическими чтениями и сохраняет адреса двух SSE-соединений.
	server.handler = [&](const auto &request) {
		if (request.target.startsWith("/api/v1/health/ready")) {
			++readyRequests;
			HttpServer::reply(request, R"({"status":"ready","reason":""})");
		} else if (request.target.startsWith("/api/v1/system/capabilities")) {
			HttpServer::reply(request, fixture::capabilities());
		} else if (request.target.startsWith("/api/v1/streams")) {
			++pageRequests;
			HttpServer::reply(request, request.target.contains("cursor=") ? second : first);
		} else if (request.target.startsWith("/api/v1/events/snapshot")) {
			++snapshotRequests;
			HttpServer::reply(request, R"({"cursor":"anchor","sessions":[],"gpu":null})");
		} else if (request.target.startsWith("/api/v1/events?")) {
			if (++eventRequests == 1)
				firstSubscription = request;
			else
				selectedSubscription = request;
		}
	};
	// path/settings адресуют локальные preferences, state отражает только наблюдения этой проверки.
	const auto path = temp.filePath(QStringLiteral("settings.json"));
	// settings хранит preferences и привязки в сценарном файле.
	gost::SettingsStore settings(path);
	settings.setAutoConnect(false);
	// state хранит исходные наблюдения, которые представления не должны изменять.
	gost::StateStore state;
	// limits ограничивает общий объём страниц, api/events выполняют запросы, controller управляет выбором.
	gost::api::Limits limits;
	limits.pageWalkBytes = first.size() + second.size() - 1;
	// api читает список через настоящий асинхронный REST-клиент.
	gost::api::APIClient api(limits);
	// events принимает настоящие локальные SSE-фрагменты.
	gost::api::EventClient events;
	// controller координирует асинхронное подключение и точный выбор Stream.
	gost::ConnectionController controller(state, settings, api, events);
	controller.observeProfile(gost::FrontendEvent::FinishedLoading,
				  {QStringLiteral("A"), QStringLiteral("profiles/a"), {QStringLiteral("A")}, true});
	QVERIFY(state.profileResolved());
	// origin/profileId задают точную привязку сервера и устойчивого профиля OBS.
	const auto origin = server.origin().toString();
	const auto profileId = settings.profileUuid();
	QVERIFY(settings.setSelectedStream(origin, profileId, QStringLiteral("saved")));
	// text читает локализацию; window показывает расширенные параметры, dock — рабочий выбор.
	const auto text = localeLookup(locale);
	// window предоставляет единое окно управления на том же общем состоянии.
	gost::ManagementWindow window(state, text, nullptr, &controller);
	window.show();
	// dock показывает настоящий Widgets-интерфейс на общем состоянии.
	gost::RestreamDock dock(state, text, nullptr, {}, nullptr, &controller);
	dock.show();
	// navigation переключает страницы; selector/message показывают выбор и предупреждение непосредственно в Dock.
	// retries задаёт лимит повторов SSE на самостоятельной странице расширенных параметров.
	// navigation переключает разделы окна во время сетевой нагрузки.
	auto *navigation = window.findChild<QListWidget *>(QStringLiteral("managementNavigation"));
	// selector отражает выбранный Stream и точные ID вариантов.
	auto *selector = dock.findChild<QComboBox *>(QStringLiteral("streamSelector"));
	// message показывает предупреждение о неполноте списка непосредственно в Dock.
	auto *message = dock.findChild<QLabel *>(QStringLiteral("dockNotice"));
	// retries содержит ещё не применённый лимит повторов подписки.
	auto *retries = window.findChild<QSpinBox *>(QStringLiteral("sseRejectionRetries"));
	// advancedSave явно применяет лимит перед первой сетевой подпиской сценария.
	auto *advancedSave = window.findChild<QPushButton *>(QStringLiteral("advancedSettingsSaveButton"));
	QVERIFY(navigation && selector && message && retries && advancedSave);
	navigation->setCurrentRow(6);
	QVERIFY(selector->isVisible());
	QVERIFY(!retries->isVisible());
	navigation->setCurrentRow(7);
	QVERIFY(retries->isVisible());
	QVERIFY(retries->isEnabled());
	QCOMPARE(retries->value(), 3);
	retries->setValue(-1);
	QCOMPARE(settings.sseRejectionRetries(), 3);
	QTest::mouseClick(advancedSave, Qt::LeftButton);
	QCOMPARE(settings.sseRejectionRetries(), -1);
	// restored независимо читает preferences после записи из интерфейса.
	gost::SettingsStore restored(path);
	QCOMPARE(restored.sseRejectionRetries(), -1);
	navigation->setCurrentRow(6);

	controller.connectToServer(origin);
	QTRY_VERIFY_WITH_TIMEOUT(firstSubscription.socket, 3000);
	QCOMPARE(state.connectionStatus(), gost::ConnectionStatus::Connecting);
	QCOMPARE(readyRequests, 1);
	QCOMPARE(pageRequests, 2);
	QCOMPARE(snapshotRequests, 1);
	QCOMPARE(eventRequests, 1);
	QVERIFY(!state.streamsComplete());
	QCOMPARE(state.selectedStreamId(), QStringLiteral("saved"));
	QVERIFY(!state.selectionAvailable());
	QVERIFY(restored.load());
	QCOMPARE(restored.selectedStream(origin, profileId), QStringLiteral("saved"));
	QVERIFY(message->isVisible());
	QVERIFY(message->text().contains(text("Stream.ListIncomplete")));
	QCOMPARE(selector->currentText(), text("Stream.Unconfirmed").arg(QStringLiteral("saved")));
	QVERIFY(selector->isEnabled());
	// receivedIndex адресует подтверждённый уже полученный Stream без обращения к неполной второй странице.
	const int receivedIndex = selector->findData(QStringLiteral("received"));
	QVERIFY(receivedIndex > 0);
	selector->setCurrentIndex(receivedIndex);
	// Программное отражение не должно сохранять выбор; activated обозначает явное действие пользователя.
	QCOMPARE(state.selectedStreamId(), QStringLiteral("saved"));
	QVERIFY(restored.load());
	QCOMPARE(restored.selectedStream(origin, profileId), QStringLiteral("saved"));
	QVERIFY(QMetaObject::invokeMethod(selector, "activated", Qt::DirectConnection, Q_ARG(int, receivedIndex)));
	QCOMPARE(state.selectedStreamId(), QStringLiteral("received"));
	QVERIFY(state.selectionAvailable());
	QVERIFY(restored.load());
	QCOMPARE(restored.selectedStream(origin, profileId), QStringLiteral("received"));
	QTRY_VERIFY_WITH_TIMEOUT(firstSubscription.socket.isNull(), 2000);
	QTRY_VERIFY_WITH_TIMEOUT(selectedSubscription.socket, 3000);
	QCOMPARE(eventRequests, 2);
	QCOMPARE(snapshotRequests, 2);
	QCOMPARE(state.connectionStatus(), gost::ConnectionStatus::Connecting);
	HttpServer::stream(selectedSubscription);
	QTRY_COMPARE_WITH_TIMEOUT(state.connectionStatus(), gost::ConnectionStatus::Connected, 2000);
	QCOMPARE(state.selectedStreamId(), QStringLiteral("received"));
	QVERIFY(message->text().contains(text("Stream.ListIncomplete")));
	controller.shutdown();
}

QTEST_MAIN(PluginLifecycleTest)

#include "plugin-lifecycle-test.moc"

void PluginLifecycleTest::dockOmitsStartupGuidance_data()
{
	QTest::addColumn<QString>("locale");
	QTest::addColumn<QString>("sessionState");
	QTest::addColumn<bool>("obsActive");
	QTest::addColumn<bool>("obsStarting");
	QTest::addColumn<bool>("partialStop");
	QTest::addColumn<bool>("known");
	// locale выбирает настоящий поставляемый словарь для каждого независимого сценария.
	for (const auto &locale : {QStringLiteral("ru-RU"), QStringLiteral("en-US")}) {
		QTest::newRow(qPrintable(locale + QStringLiteral("-no-session-active")))
			<< locale << QString{} << true << false << false << true;
		QTest::newRow(qPrintable(locale + QStringLiteral("-stopped-active")))
			<< locale << QStringLiteral("STOPPED") << true << false << false << true;
		QTest::newRow(qPrintable(locale + QStringLiteral("-failed-active")))
			<< locale << QStringLiteral("FAILED") << true << false << false << true;
		QTest::newRow(qPrintable(locale + QStringLiteral("-interrupted-active")))
			<< locale << QStringLiteral("INTERRUPTED") << true << false << false << true;
		QTest::newRow(qPrintable(locale + QStringLiteral("-starting-false-flags")))
			<< locale << QString{} << false << true << false << true;
		QTest::newRow(qPrintable(locale + QStringLiteral("-stopped-inactive")))
			<< locale << QStringLiteral("STOPPED") << false << false << false << true;
		QTest::newRow(qPrintable(locale + QStringLiteral("-unknown-session")))
			<< locale << QString{} << true << false << false << false;
		QTest::newRow(qPrintable(locale + QStringLiteral("-partial-stop-false-flags")))
			<< locale << QStringLiteral("STOPPED") << false << false << true << true;
	}
}

void PluginLifecycleTest::dockOmitsStartupGuidance()
{
	// locale выбирает словарь поставки; sessionState задаёт точную сохранённую Session либо полное отсутствие.
	QFETCH(QString, locale);
	QFETCH(QString, sessionState);
	// obsActive задаёт булевое наблюдение, obsStarting — независимую фазу запуска с временно ложными флагами.
	QFETCH(bool, obsActive);
	QFETCH(bool, obsStarting);
	// partialStop задаёт прежнее неподтверждённое завершение OBS; known определяет достоверность поиска Session.
	QFETCH(bool, partialStop);
	QFETCH(bool, known);
	// text использует настоящий перевод, state хранит раздельные наблюдения Session, OBS и результаты команды.
	const auto text = localeLookup(locale);
	gost::StateStore state;
	// stream подтверждает текущий выбранный Stream без обращения к backend.
	gost::api::Stream stream;
	stream.id = QStringLiteral("stream");
	state.setConnection(QStringLiteral("http://127.0.0.1:8080"), gost::ConnectionStatus::Connected);
	state.setStreams({stream}, true);
	state.setSelection(stream.id, true, {}, {}, QStringLiteral("profile"));
	state.setObsState(QStringLiteral("Профиль"), obsActive);
	state.setObsStatistics({.phase = obsStarting ? gost::ObsStreamingPhase::Starting
						     : (obsActive ? gost::ObsStreamingPhase::Streaming
								  : gost::ObsStreamingPhase::Stopped),
				.observed = true});
	// metadata задаёт независимо полное подтверждение отсутствия эфира либо неизвестный результат поиска.
	gost::DockServerState metadata;
	metadata.sessionStatus = {known ? gost::RuntimeAvailability::Fresh : gost::RuntimeAvailability::Unknown,
				  known,
				  {}};
	if (!sessionState.isEmpty())
		metadata.session = gost::DockSession{QStringLiteral("session"), sessionState, false};
	state.setDockServer(metadata);
	if (partialStop) {
		// operations/workflow сохраняют серверный успех отдельно от неизвестного локального stop.
		gost::OperationsState operations;
		gost::WorkflowResult workflow;
		workflow.context = state.dockContext();
		workflow.action = gost::OperationAction::StopAll;
		workflow.phase = gost::OperationPhase::Finished;
		workflow.outcome = gost::OperationOutcome::Unknown;
		workflow.obsOutcome = gost::OperationOutcome::Unknown;
		workflow.serverOutcome = gost::OperationOutcome::Completed;
		operations.current = workflow;
		state.setOperations(operations);
	}
	// dock показывает те же состояния, что и рабочий интерфейс, без сетевого контроллера.
	gost::RestreamDock dock(state, text);
	QVERIFY(!dock.findChild<QLabel *>(QStringLiteral("nativeStartGuidance")));
	QVERIFY(!dock.findChild<QLabel *>(QStringLiteral("operationStatus")));
}

void PluginLifecycleTest::commandFailureRemainsInStateStore_data()
{
	QTest::addColumn<QString>("receiptStatus");
	// status задаёт одну из двух терминальных причин отказа, сохраняемых в StateStore.
	for (const auto &status : {QStringLiteral("FAILED"), QStringLiteral("INTERRUPTED")})
		QTest::newRow(qPrintable(status)) << status;
}

void PluginLifecycleTest::commandFailureRemainsInStateStore()
{
	// receiptStatus задаёт терминальную квитанцию отказа.
	QFETCH(QString, receiptStatus);
	// state хранит проверяемый результат команды независимо от существования Dock.
	gost::StateStore state;
	// code и message проверяют сохранение полей отказа без их переноса в Dock.
	const auto code = QStringLiteral("cleanup_<b>failure</b>&");
	const auto message = QStringLiteral("private-server-message-marker");
	// receipt содержит подтверждённый код и сообщение отказа для внутреннего результата команды.
	gost::api::Receipt receipt;
	receipt.status = receiptStatus;
	receipt.failure = {gost::api::FieldState::Value, {code, message}};
	// operations/workflow хранят подтверждённый отказ, а не предположение по состоянию площадки.
	gost::OperationsState operations;
	gost::WorkflowResult workflow;
	workflow.action = gost::OperationAction::Pause;
	workflow.phase = gost::OperationPhase::Finished;
	workflow.outcome = gost::OperationOutcome::Failed;
	workflow.receipt = receipt;
	operations.current = workflow;
	state.setOperations(operations);
	// dock не создаёт блок отчёта и не меняет результат, который хранит StateStore.
	// text предоставляет подписи, необходимые для создания виджетов Dock.
	const auto text = localeLookup(QStringLiteral("en-US"));
	gost::RestreamDock dock(state, text);
	QVERIFY(!dock.findChild<QLabel *>(QStringLiteral("operationStatus")));
	QVERIFY(state.operations().current);
	QCOMPARE(state.operations().current->receipt->failure->code, code);
	QCOMPARE(state.operations().current->receipt->failure->message, message);
}

// Проверяет обе поставляемые локали, не подменяя ими правила идентичности Destination и Output.
void PluginLifecycleTest::outputRowsKeepDestinationAndExecutionIdentities_data()
{
	QTest::addColumn<QString>("locale");
	QTest::newRow("ru") << QStringLiteral("ru-RU");
	QTest::newRow("en") << QStringLiteral("en-US");
}

// Два Outputs одного назначения сохраняют отдельные строки и исторические имена.
// Runtime без REST-связи и одноимённый каталог не считаются одним выходом; detach возвращает строку назначения.
void PluginLifecycleTest::outputRowsKeepDestinationAndExecutionIdentities()
{
	// locale выбирает подписи интерфейса, text предоставляет фактические переводы поставки.
	QFETCH(QString, locale);
	const auto text = localeLookup(locale);
	// rig предоставляет полноценные контроллеры для проверки доступности меню без живого OBS.
	OutputDockRig rig;
	QVERIFY(rig.initialize());
	// metadata содержит два Outputs одного назначения, один отсоединённый Output и три свободных назначения.
	auto metadata = rig.state.dockServer();
	metadata.outputs.clear();
	metadata.outputs.insert(QStringLiteral("a-one"),
				{QStringLiteral("Историческое первое"), true, true, QStringLiteral("shared")});
	metadata.outputs.insert(QStringLiteral("a-two"),
				{QStringLiteral("Историческое второе"), true, false, QStringLiteral("shared")});
	metadata.outputs.insert(QStringLiteral("detached"), {QStringLiteral("Старый отсоединённый"), false, false,
							     QStringLiteral("detached-destination")});
	metadata.destinations.insert(QStringLiteral("shared"), {QStringLiteral("Переименованное назначение")});
	metadata.destinations.insert(QStringLiteral("catalog-only"), {QStringLiteral("Историческое первое")});
	metadata.destinations.insert(QStringLiteral("runtime-only"), {QStringLiteral("Отдельное назначение")});
	metadata.destinations.insert(QStringLiteral("detached-destination"), {QStringLiteral("После отсоединения")});
	// session содержит известное runtime-исполнение без доказанной связи с одноимённым Destination ID.
	gost::api::RuntimeSession session;
	session.sessionId = QStringLiteral("session");
	session.observation = {gost::api::FieldState::Value,
			       gost::api::Observation{.instanceId = QStringLiteral("session-instance"),
						      .state = QStringLiteral("RUNNING")}};
	// runtimeOnly имеет ID, совпадающий с ID назначения; разные пространства идентичности не схлопываются.
	gost::api::RuntimeOutput runtimeOnly;
	runtimeOnly.outputId = QStringLiteral("runtime-only");
	runtimeOnly.instanceId = QStringLiteral("runtime-instance");
	runtimeOnly.state = QStringLiteral("PUBLISHING");
	session.outputs.append(runtimeOnly);
	QVERIFY(rig.state.installAnchor(
		{QStringLiteral("union-anchor"), {session}, {gost::api::FieldState::Null, {}}}));
	rig.state.setDockServer(metadata);
	// dock показывает объединённый список; list предоставляет строки и команды каждой из них.
	gost::RestreamDock dock(rig.state, text, nullptr, {}, &rig.commands);
	dock.resize(360, 560);
	dock.show();
	auto *list = dock.findChild<QListView *>(QStringLiteral("outputList"));
	QVERIFY(list);
	QTRY_COMPARE_WITH_TIMEOUT(list->model()->rowCount(), 6, 1000);
	// names сохраняет все строки, disabledNames выявляет runtime-строку без доказанной связи с Destination.
	QStringList names;
	QStringList disabledNames;
	// index адресует текущую строку, row/name/actions дают её публичные поля без чтения приватного кеша.
	for (int index = 0; index < list->model()->rowCount(); ++index) {
		auto *row = list->indexWidget(list->model()->index(index, 0));
		QVERIFY(row);
		auto *name = row->findChild<QLabel *>(QStringLiteral("outputName"));
		auto *actions = row->findChild<QToolButton *>(QStringLiteral("outputActionsButton"));
		QVERIFY(name && actions);
		names.append(name->text());
		if (!actions->isEnabled())
			disabledNames.append(name->text());
		// status и rate не выдают каталог или долговечные намерения за runtime-публикацию.
		auto *status = row->findChild<QLabel *>(QStringLiteral("outputStatus"));
		auto *rate = row->findChild<QLabel *>(QStringLiteral("outputBitrate"));
		QVERIFY(status && rate);
		if (name->text() != text("Dock.UnnamedOutput")) {
			QCOMPARE(status->text(), text("Observation.NoData"));
			QCOMPARE(rate->text(), text("Observation.NoData"));
		}
	}
	QCOMPARE(names.count(QStringLiteral("Историческое первое")), 2);
	QCOMPARE(names.count(QStringLiteral("Историческое второе")), 1);
	QCOMPARE(names.count(text("Dock.UnnamedOutput")), 1);
	QCOMPARE(names.count(QStringLiteral("Отдельное назначение")), 1);
	QCOMPARE(names.count(QStringLiteral("После отсоединения")), 1);
	QVERIFY(!names.contains(QStringLiteral("Переименованное назначение")));
	QVERIFY(!names.contains(QStringLiteral("Старый отсоединённый")));
	QCOMPARE(disabledNames, QStringList{text("Dock.UnnamedOutput")});
	// summary не должен представлять дополнительные каталоговые или неопределённые строки как полный состав.
	auto *summary = dock.findChild<QLabel *>(QStringLiteral("outputSummary"));
	QVERIFY(summary);
	QVERIFY(summary->isHidden() || summary->text().isEmpty());
	// После подтверждённого detach обоих Outputs остаётся ровно одна строка сохранённого назначения.
	metadata.outputs[QStringLiteral("a-one")].attached = false;
	metadata.outputs[QStringLiteral("a-two")].attached = false;
	rig.state.setDockServer(metadata);
	QTRY_COMPARE_WITH_TIMEOUT(list->model()->rowCount(), 5, 1000);
	// allRows собирает представление после detach, index адресует каждую строку без предположения о сортировке.
	QString allRows;
	for (int index = 0; index < list->model()->rowCount(); ++index)
		allRows += list->model()->index(index, 0).data().toString();
	QVERIFY(allRows.contains(QStringLiteral("Переименованное назначение")));
	QVERIFY(!allRows.contains(QStringLiteral("Историческое второе")));
	QVERIFY(rig.posts().isEmpty());
	// Удаление Session не удаляет четыре назначения выбранного Stream.
	metadata.session.reset();
	metadata.outputs.clear();
	rig.state.setDockServer(metadata);
	QTRY_COMPARE_WITH_TIMEOUT(list->model()->rowCount(), 4, 1000);
	QVERIFY(rig.posts().isEmpty());
}

// Проверяет доступность по сохранённым метаданным, включая runtime=null и обе локали.
void PluginLifecycleTest::outputMenuUsesDurableMetadata_data()
{
	QTest::addColumn<QString>("locale");
	QTest::addColumn<QString>("condition");
	// locale выбирает перевод; condition задаёт подтверждённость, намерение или конфликтующий workflow.
	for (const auto &locale : {QStringLiteral("ru-RU"), QStringLiteral("en-US")})
		for (const auto &condition :
		     {QStringLiteral("running"), QStringLiteral("stopped"), QStringLiteral("stale"),
		      QStringLiteral("partial"), QStringLiteral("busy")})
			QTest::newRow(qPrintable(locale + QLatin1Char('-') + condition)) << locale << condition;
}

// Открывает настоящее меню клавиатурой и сравнивает полный набор поддерживаемых действий.
void PluginLifecycleTest::outputMenuUsesDurableMetadata()
{
	// locale и condition определяют перевод и независимую границу разрешения команд.
	QFETCH(QString, locale);
	QFETCH(QString, condition);
	// rig сохраняет runtime пустым; text читает реальные подписи.
	OutputDockRig rig;
	QVERIFY(rig.initialize());
	const auto text = localeLookup(locale);
	// metadata изменяет только желаемое состояние либо полноту/свежесть REST, не создавая runtime.
	auto metadata = rig.state.dockServer();
	metadata.outputs[QStringLiteral("one")].desiredRunning = condition != QStringLiteral("stopped");
	if (condition == QStringLiteral("stale"))
		metadata.outputsStatus.availability = gost::RuntimeAvailability::Stale;
	if (condition == QStringLiteral("partial"))
		metadata.outputsStatus.complete = false;
	rig.state.setDockServer(metadata);
	if (condition == QStringLiteral("busy")) {
		// busy моделирует уже исполняемую команду; просмотр подробностей остаётся разрешён.
		gost::OperationsState busy;
		busy.current = gost::WorkflowResult{};
		busy.current->phase = gost::OperationPhase::PollingReceipt;
		rig.state.setOperations(busy);
	}
	// dock/list/button предоставляют настоящий путь открытия меню без runtime-исполнения.
	gost::RestreamDock dock(rig.state, text, nullptr, {}, &rig.commands);
	dock.resize(360, 560);
	dock.show();
	auto *list = dock.findChild<QListView *>(QStringLiteral("outputList"));
	QVERIFY(list);
	QTRY_COMPARE_WITH_TIMEOUT(list->model()->rowCount(), 2, 1000);
	// row предоставляет виджет исходного Output; отсутствие строки должно дать проверяемый отказ, а не обращение через null.
	auto *row = list->indexWidget(list->model()->index(0, 0));
	QVERIFY(row);
	auto *button = row->findChild<QToolButton *>(QStringLiteral("outputActionsButton"));
	QVERIFY(button && button->isEnabled());
	QVERIFY(button->focusPolicy() != Qt::NoFocus);
	QVERIFY(!button->accessibleName().isEmpty());
	// labels/enabled сохраняют публичное содержимое меню; observed отличает настоящий popup от пропущенного вызова.
	QStringList labels;
	QList<bool> enabled;
	bool observed = false;
	// Обработчик вызывается внутри вложенного цикла menu.exec и закрывает меню после чтения каждого QAction.
	QTimer::singleShot(0, &dock, [&] {
		// menu заимствован у popup; action перебирает ровно видимые поддерживаемые действия.
		auto *menu = qobject_cast<QMenu *>(QApplication::activePopupWidget());
		if (!menu)
			return;
		observed = true;
		for (auto *action : menu->actions()) {
			if (!action->isSeparator()) {
				labels.append(action->text());
				enabled.append(action->isEnabled());
			}
		}
		menu->close();
	});
	button->setFocus();
	QTest::keyClick(button, Qt::Key_Space);
	QVERIFY(observed);
	QCOMPARE(labels, QStringList({text("Operation.StartOutput"), text("Operation.StopOutput"),
				      text("Operation.RestartOutput"), text("Operation.OutputDetails")}));
	// allowed выражает контракт пользователя: только полный свежий состав без конфликтующей команды.
	const bool allowed = condition == QStringLiteral("running") || condition == QStringLiteral("stopped");
	QCOMPARE(enabled, QList<bool>({allowed && condition == QStringLiteral("stopped"),
				       allowed && condition == QStringLiteral("running"), allowed, true}));
	QVERIFY(rig.state.runtime().sessions.isEmpty());
	QVERIFY(rig.posts().isEmpty());
}

// Сценарии отделяют реальную команду от просмотра, смены цели и удаления владельца вложенного меню.
void PluginLifecycleTest::outputMenuKeepsOriginalTargetAndLifetime_data()
{
	QTest::addColumn<QString>("mode");
	QTest::newRow("start-stopped") << QStringLiteral("start");
	QTest::newRow("stop-running") << QStringLiteral("stop");
	QTest::newRow("restart-confirmed") << QStringLiteral("restart");
	QTest::newRow("details-without-runtime") << QStringLiteral("details");
	QTest::newRow("session-replaced-during-menu") << QStringLiteral("replace");
	QTest::newRow("session-replaced-during-confirmation") << QStringLiteral("replace-confirmation");
	QTest::newRow("dock-deleted-during-menu") << QStringLiteral("delete-menu");
	QTest::newRow("dock-deleted-during-confirmation") << QStringLiteral("delete-confirmation");
}

// Выбирает действие настоящего меню и проверяет точные Session/Output в POST либо в запросе подробностей.
// Отказ синтетического сервера не трактуется как успех; здесь оракулом служит адресованное намерение UI.
void PluginLifecycleTest::outputMenuKeepsOriginalTargetAndLifetime()
{
	// mode выбирает действие и момент изменения цели; rig предоставляет настоящую повторную проверку контроллера.
	QFETCH(QString, mode);
	OutputDockRig rig;
	QVERIFY(rig.initialize());
	// text задаёт реальные переводы, metadata разрешает запуск только остановленного исходного выхода.
	const auto text = localeLookup();
	auto metadata = rig.state.dockServer();
	if (mode == QStringLiteral("start"))
		metadata.outputs[QStringLiteral("one")].desiredRunning = false;
	rig.state.setDockServer(metadata);
	// dock защищён QPointer: часть сценариев намеренно удаляет его внутри menu.exec или QMessageBox::exec.
	QPointer<gost::RestreamDock> dock = new gost::RestreamDock(rig.state, text, nullptr, {}, &rig.commands);
	// cleanup освобождает оставшийся Dock до зависимостей rig даже при раннем выходе проверки.
	const auto cleanup = qScopeGuard([&] {
		if (dock)
			delete dock;
	});
	dock->resize(360, 560);
	dock->show();
	// details наблюдает точный контракт перехода без создания нового окна управления.
	QSignalSpy details(dock, &gost::RestreamDock::destinationDetailsRequested);
	QVERIFY(details.isValid());
	// originalContext сохраняет допустимый контекст до любого вложенного event loop.
	const auto originalContext = rig.state.dockContext();
	// list/button адресуют исходный Output one; последующее обновление строки не меняет этот выбор.
	auto *list = dock->findChild<QListView *>(QStringLiteral("outputList"));
	QVERIFY(list);
	QTRY_COMPARE_WITH_TIMEOUT(list->model()->rowCount(), 2, 1000);
	// row предоставляет виджет исходного Output; отсутствие строки должно дать проверяемый отказ, а не обращение через null.
	auto *row = list->indexWidget(list->model()->index(0, 0));
	QVERIFY(row);
	auto *button = row->findChild<QToolButton *>(QStringLiteral("outputActionsButton"));
	QVERIFY(button && button->isEnabled());
	// opened и confirmed фиксируют фактическое прохождение меню и отдельного подтверждения.
	bool opened = false;
	bool confirmed = false;
	// callback выбирает только исходное действие и при необходимости меняет Session или удаляет Dock.
	QTimer::singleShot(0, &rig.state, [&] {
		// menu заимствован только до закрытия popup или удаления родительского Dock.
		auto *menu = qobject_cast<QMenu *>(QApplication::activePopupWidget());
		if (!menu)
			return;
		opened = true;
		if (mode == QStringLiteral("delete-menu")) {
			delete dock;
			return;
		}
		// key задаёт поддерживаемое действие; action хранит найденную текущую QAction до её выбора.
		const char *key = mode == QStringLiteral("start")                 ? "Operation.StartOutput"
				  : mode == QStringLiteral("restart")             ? "Operation.RestartOutput"
				  : mode.endsWith(QStringLiteral("confirmation")) ? "Operation.RestartOutput"
				  : mode == QStringLiteral("details")             ? "Operation.OutputDetails"
										  : "Operation.StopOutput";
		QAction *action = nullptr;
		// candidate перебирает пользовательские действия меню по их локализованному смыслу.
		for (auto *candidate : menu->actions())
			if (candidate->text() == text(key))
				action = candidate;
		if (!action || !action->isEnabled()) {
			menu->close();
			return;
		}
		if (mode == QStringLiteral("replace")) {
			// replacement меняет точную Session и строку до выбора пункта прежнего меню.
			auto replacement = rig.state.dockServer();
			replacement.session->id = QStringLiteral("new-session");
			replacement.outputs.clear();
			replacement.outputs.insert(QStringLiteral("new-output"),
						   {QStringLiteral("Новая строка"), true, true,
						    QStringLiteral("new-destination")});
			rig.state.setDockServer(replacement);
		}
		if (mode == QStringLiteral("restart") || mode.endsWith(QStringLiteral("confirmation"))) {
			// Второй callback выполняется уже внутри подтверждения, после закрытия меню исходного Output.
			QTimer::singleShot(0, &rig.state, [&] {
				// confirmation — единственное модальное подтверждение выбранной прерывающей команды.
				auto *confirmation = qobject_cast<QMessageBox *>(QApplication::activeModalWidget());
				if (!confirmation)
					return;
				confirmed = true;
				if (mode == QStringLiteral("delete-confirmation")) {
					delete dock;
					return;
				}
				if (mode == QStringLiteral("replace-confirmation")) {
					// replacement запрещает перенос команды после положительного ответа пользователем.
					auto replacement = rig.state.dockServer();
					replacement.session->id = QStringLiteral("new-session");
					rig.state.setDockServer(replacement);
				}
				confirmation->done(QMessageBox::Yes);
			});
		}
		menu->setActiveAction(action);
		QTest::keyClick(menu, Qt::Key_Return);
	});
	button->click();
	QVERIFY(opened);
	if (mode == QStringLiteral("delete-menu") || mode == QStringLiteral("delete-confirmation")) {
		QVERIFY(dock.isNull());
		QVERIFY(rig.posts().isEmpty());
		QCOMPARE(confirmed, mode == QStringLiteral("delete-confirmation"));
	} else if (mode == QStringLiteral("replace") || mode == QStringLiteral("replace-confirmation")) {
		dispatchQueuedEvents();
		QVERIFY(rig.posts().isEmpty());
		QCOMPARE(rig.state.dockServer().session->id, QStringLiteral("new-session"));
		QCOMPARE(confirmed, mode == QStringLiteral("replace-confirmation"));
	} else if (mode == QStringLiteral("details")) {
		QCOMPARE(details.size(), 1);
		QCOMPARE(qvariant_cast<gost::DockContext>(details.at(0).at(0)), originalContext);
		QCOMPARE(details.at(0).size(), 2);
		QCOMPARE(details.at(0).at(1).toString(), QStringLiteral("dest-one"));
		QVERIFY(rig.posts().isEmpty());
	} else {
		QTRY_VERIFY_WITH_TIMEOUT(!rig.posts().isEmpty(), 1000);
		QCOMPARE(rig.posts().size(), 1);
		// expectedRoute имеет точные исходные ID; restart начинает с отдельного stop.
		const QByteArray expectedRoute = mode == QStringLiteral("start")
							 ? "/api/v1/sessions/session/outputs/one/start"
							 : "/api/v1/sessions/session/outputs/one/stop";
		QCOMPARE(rig.posts().front().target, expectedRoute);
		QCOMPARE(confirmed, mode == QStringLiteral("restart"));
		QVERIFY(details.isEmpty());
	}
}

// Проверяет оба прерывающих действия с подтверждением и безопасным отказом по умолчанию.
void PluginLifecycleTest::outputBulkConfirmationFreezesShownRows_data()
{
	QTest::addColumn<bool>("restart");
	QTest::addColumn<bool>("approve");
	QTest::newRow("restart-yes") << true << true;
	QTest::newRow("restart-no") << true << false;
	QTest::newRow("stop-yes") << false << true;
	QTest::newRow("stop-no") << false << false;
}

// Все строки модели, включая находящиеся за прокруткой, фиксируются до вложенного подтверждения.
void PluginLifecycleTest::outputBulkConfirmationFreezesShownRows()
{
	// restart выбирает смысл общей команды, approve задаёт явный ответ пользовательского подтверждения.
	QFETCH(bool, restart);
	QFETCH(bool, approve);
	// rig содержит два attached Output; text читает поставляемые переводы.
	OutputDockRig rig;
	QVERIFY(rig.initialize());
	const auto text = localeLookup();
	// metadata добавляет 26 ещё не исполняющихся назначений; index задаёт устойчивый исходный порядок ID.
	auto metadata = rig.state.dockServer();
	for (int index = 0; index < 26; ++index)
		metadata.destinations.insert(QStringLiteral("catalog-%1").arg(index, 2, 10, QLatin1Char('0')),
					     {QStringLiteral("Площадка %1").arg(index)});
	rig.state.setDockServer(metadata);
	// dock и list дают настоящий прокручиваемый список из 28 строк.
	gost::RestreamDock dock(rig.state, text, nullptr, {}, &rig.commands);
	dock.resize(360, 560);
	dock.show();
	auto *list = dock.findChild<QListView *>(QStringLiteral("outputList"));
	QVERIFY(list);
	QTRY_COMPARE_WITH_TIMEOUT(list->model()->rowCount(), 28, 1000);
	QTRY_VERIFY_WITH_TIMEOUT(list->verticalScrollBar()->maximum() > 0, 1000);
	// button выбирает одну из двух команд выходов, а не завершение эфира OBS+Session.
	auto *button = dock.findChild<QToolButton *>(restart ? QStringLiteral("outputsRestartButton")
							     : QStringLiteral("outputsStopButton"));
	QVERIFY(button && button->isEnabled());
	// confirmed и safeDefault доказывают обязательное подтверждение с ответом No; callback меняет состав перед ответом.
	bool confirmed = false;
	bool safeDefault = false;
	QTimer::singleShot(0, &dock, [&] {
		// confirmation существует только во вложенном цикле; changed расширяет показанный состав после фиксации намерения.
		auto *confirmation = qobject_cast<QMessageBox *>(QApplication::activeModalWidget());
		if (!confirmation)
			return;
		confirmed = true;
		safeDefault = confirmation->defaultButton() == confirmation->button(QMessageBox::No);
		auto changed = rig.state.dockServer();
		changed.outputs.insert(QStringLiteral("three"),
				       {QStringLiteral("Поздний"), true, true, QStringLiteral("dest-three")});
		changed.destinations.insert(QStringLiteral("late-catalog"), {QStringLiteral("Позднее назначение")});
		rig.state.setDockServer(changed);
		confirmation->done(approve ? QMessageBox::Yes : QMessageBox::No);
	});
	button->click();
	QVERIFY(confirmed && safeDefault);
	if (!approve) {
		QVERIFY(!rig.state.operations().current);
		QVERIFY(rig.server.requests.isEmpty());
		return;
	}
	QVERIFY(rig.state.operations().current);
	QCOMPARE(rig.state.operations().current->action,
		 restart ? gost::OperationAction::RestartAllOutputs : gost::OperationAction::StopAllOutputs);
	QCOMPARE(rig.state.operations().current->outputs.size(), 28);
	// outputIds и destinationIds собирают публичный отчёт, а не текущий изменившийся серверный каталог.
	QStringList outputIds;
	QStringList destinationIds;
	// result представляет одну исходную строку с раздельными пространствами ID.
	for (const auto &result : rig.state.operations().current->outputs) {
		outputIds.append(result.outputId);
		destinationIds.append(result.destinationId);
	}
	QCOMPARE(outputIds.count(QStringLiteral("one")), 1);
	QCOMPARE(outputIds.count(QStringLiteral("two")), 1);
	QVERIFY(destinationIds.contains(QStringLiteral("catalog-25")));
	QVERIFY(!outputIds.contains(QStringLiteral("three")));
	QVERIFY(!destinationIds.contains(QStringLiteral("late-catalog")));
	QTRY_VERIFY_WITH_TIMEOUT(!rig.posts().isEmpty(), 1000);
	QCOMPARE(rig.posts().front().target, QByteArray("/api/v1/sessions/session/outputs/one/stop"));
	QCOMPARE(rig.frontend.stopRequests, 0);
}

// Проверяет каталог без Session в обеих локалях на двух узких ширинах Dock.
void PluginLifecycleTest::catalogRowsFitNarrowDockWithoutSession_data()
{
	QTest::addColumn<QString>("locale");
	QTest::addColumn<int>("width");
	// locale определяет перевод, width ограничивает всю карточку выбранным размером.
	for (const auto &locale : {QStringLiteral("ru-RU"), QStringLiteral("en-US")})
		for (const int width : {300, 400})
			QTest::newRow(qPrintable(locale + QString::number(width))) << locale << width;
}

// Каталоговые строки сохраняют три поля и отсутствие runtime; многоточие предоставляет явный одиночный запуск.
void PluginLifecycleTest::catalogRowsFitNarrowDockWithoutSession()
{
	// locale и width задают язык и фактическую ширину настоящего QWidget.
	QFETCH(QString, locale);
	QFETCH(int, width);
	// rig сохраняет отдельный командный API, чтобы обнаружить ошибочный POST каталога.
	OutputDockRig rig;
	QVERIFY(rig.initialize());
	// text предоставляет реальные переводы; fullName содержит длинное имя с буквальной разметкой и Unicode-графемами.
	const auto text = localeLookup(locale);
	const QString fullName = QStringLiteral("<b>Назначение & A\u0301 \U0001F469\u200d\U0001F4BB</b>").repeated(5);
	// metadata не содержит Session/Outputs; index создаёт 25 независимых сохранённых назначений.
	auto metadata = rig.state.dockServer();
	metadata.session.reset();
	metadata.outputs.clear();
	for (int index = 0; index < 25; ++index)
		metadata.destinations.insert(QStringLiteral("destination-%1").arg(index, 2, 10, QLatin1Char('0')),
					     {fullName + QString::number(index)});
	rig.state.setDockServer(metadata);
	// dock/list/scroll предоставляют реальную геометрию и две независимые области прокрутки.
	gost::RestreamDock dock(rig.state, text, nullptr, {}, &rig.commands);
	dock.resize(width, 560);
	dock.show();
	auto *list = dock.findChild<QListView *>(QStringLiteral("outputList"));
	auto *scroll = dock.findChild<QScrollArea *>(QStringLiteral("dockDetailsScroll"));
	QVERIFY(list && scroll);
	QTRY_COMPARE_WITH_TIMEOUT(list->model()->rowCount(), 25, 1000);
	QCOMPARE(dock.width(), width);
	QCOMPARE(list->horizontalScrollBar()->maximum(), 0);
	QCOMPARE(scroll->horizontalScrollBar()->maximum(), 0);
	QTRY_VERIFY_WITH_TIMEOUT(list->verticalScrollBar()->maximum() > 0, 1000);
	// row/name/status/rate/action адресуют первую строку по точному ID назначения, не по визуальному сокращению.
	auto *row = list->indexWidget(list->model()->index(0, 0));
	QVERIFY(row);
	auto *name = row->findChild<QLabel *>(QStringLiteral("outputName"));
	auto *status = row->findChild<QLabel *>(QStringLiteral("outputStatus"));
	auto *rate = row->findChild<QLabel *>(QStringLiteral("outputBitrate"));
	auto *action = row->findChild<QToolButton *>(QStringLiteral("outputActionsButton"));
	QVERIFY(name && status && rate && action);
	QCOMPARE(name->text(), fullName + QStringLiteral("0"));
	QCOMPARE(name->textFormat(), Qt::PlainText);
	// expectedDescription содержит буквальное имя и «Нет данных» для состояния, скорости и всех счётчиков.
	const auto expectedDescription = text("Dock.OutputRow")
						 .arg(fullName + QStringLiteral("0"), text("Observation.NoData"),
						      text("Observation.NoData"), text("Observation.NoData"),
						      text("Observation.NoData"), text("Observation.NoData"),
						      text("Observation.NoData"));
	// field перебирает все видимые поля, tooltip раскрывает исходное описание без выполнения серверной разметки.
	for (const auto *field : {name, status, rate}) {
		QTextDocument tooltip;
		tooltip.setHtml(field->toolTip());
		QCOMPARE(tooltip.toPlainText(), expectedDescription);
	}
	QCOMPARE(row->accessibleDescription(), expectedDescription);
	QVERIFY(row->accessibleName().contains(name->text()));
	QCOMPARE(status->text(), text("Observation.NoData"));
	QCOMPARE(rate->text(), text("Observation.NoData"));
	QVERIFY(!name->wordWrap() && !rate->wordWrap());
	QVERIFY(name->x() < status->x() && status->x() < rate->x() && rate->x() < action->x());
	QVERIFY(row->height() <= qMax(action->height(), name->fontMetrics().height()) + 20);
	QVERIFY(!action->isHidden() && action->isEnabled());
	list->scrollToBottom();
	QTRY_VERIFY_WITH_TIMEOUT(list->verticalScrollBar()->value() > 0, 1000);
	QVERIFY(rig.posts().isEmpty());
	QVERIFY(rig.state.runtime().sessions.isEmpty());
}

// Запуск свободной строки зависит от подтверждённых данных, а не от runtime или enabled назначения.
void PluginLifecycleTest::catalogStartMenuUsesConfirmedMode_data()
{
	QTest::addColumn<QString>("locale");
	QTest::addColumn<QString>("condition");
	// locale выбирает поставляемый перевод; condition задаёт отдельную границу допуска create/add.
	for (const auto &locale : {QStringLiteral("ru-RU"), QStringLiteral("en-US")})
		for (const auto *condition :
		     {"absent", "terminal", "running", "unknown-session", "stale-session", "partial-session",
		      "STARTING", "STOPPING", "stale-catalog", "partial-catalog", "partial-outputs", "busy"})
			QTest::newRow(qPrintable(locale + QLatin1Char('-') + QString::fromLatin1(condition)))
				<< locale << QString::fromLatin1(condition);
}

// Настоящее меню передаёт выбранный Destination контроллеру; 503 останавливает приватный GET до медиакоманды.
void PluginLifecycleTest::catalogStartMenuUsesConfirmedMode()
{
	// locale и condition задают язык и независимое состояние серверных метаданных.
	QFETCH(QString, locale);
	QFETCH(QString, condition);
	// rig предоставляет настоящие Qt-контроллеры; text читает поставляемые словари.
	OutputDockRig rig;
	QVERIFY(rig.initialize());
	const auto text = localeLookup(locale);
	// metadata содержит одну свободную строку назначения с ID, отличным от любого Output.
	auto metadata = rig.state.dockServer();
	metadata.outputs.clear();
	metadata.destinations.insert(QStringLiteral("selected-destination"), {QStringLiteral("Одинаковое имя")});
	if (condition != QStringLiteral("running") && condition != QStringLiteral("partial-outputs"))
		metadata.session.reset();
	if (condition == QStringLiteral("terminal"))
		metadata.session =
			gost::DockSession{QStringLiteral("historic-session"), QStringLiteral("STOPPED"), false};
	if (condition == QStringLiteral("STARTING") || condition == QStringLiteral("STOPPING"))
		metadata.session = gost::DockSession{QStringLiteral("transitional-session"), condition, false};
	if (condition == QStringLiteral("unknown-session"))
		metadata.sessionStatus.availability = gost::RuntimeAvailability::Unknown;
	if (condition == QStringLiteral("stale-session"))
		metadata.sessionStatus.availability = gost::RuntimeAvailability::Stale;
	if (condition == QStringLiteral("partial-session"))
		metadata.sessionStatus.complete = false;
	if (condition == QStringLiteral("stale-catalog"))
		metadata.destinationsStatus.availability = gost::RuntimeAvailability::Stale;
	if (condition == QStringLiteral("partial-catalog"))
		metadata.destinationsStatus.complete = false;
	if (condition == QStringLiteral("partial-outputs"))
		metadata.outputsStatus.complete = false;
	rig.state.setDockServer(metadata);
	if (condition == QStringLiteral("busy")) {
		// busy моделирует живую чужую последовательность, не создавая HTTP-намерения.
		gost::OperationsState busy;
		busy.current = gost::WorkflowResult{};
		busy.current->phase = gost::OperationPhase::PollingReceipt;
		rig.state.setOperations(busy);
	}
	// dock и list предоставляют единственную каталоговую строку независимо от Session.
	gost::RestreamDock dock(rig.state, text, nullptr, {}, &rig.commands);
	dock.resize(360, 560);
	dock.show();
	auto *list = dock.findChild<QListView *>(QStringLiteral("outputList"));
	QVERIFY(list);
	QTRY_COMPARE_WITH_TIMEOUT(list->model()->rowCount(), 1, 1000);
	// row и button открывают настоящее меню, сохраняя доступ клавиатуры к каталоговой строке.
	auto *row = list->indexWidget(list->model()->index(0, 0));
	QVERIFY(row);
	auto *button = row->findChild<QToolButton *>(QStringLiteral("outputActionsButton"));
	QVERIFY(button && button->isEnabled());
	QVERIFY(button->focusPolicy() != Qt::NoFocus);
	QVERIFY(!button->accessibleName().isEmpty());
	// allowed выражает независимый контракт свежей полной пустоты либо точной RUNNING Session для add.
	const bool allowed = condition == QStringLiteral("absent") || condition == QStringLiteral("terminal") ||
			     condition == QStringLiteral("running");
	// labels и enabled сохраняют полный наблюдаемый состав меню; opened подтверждает вход во вложенный цикл.
	QStringList labels;
	QList<bool> enabled;
	bool opened = false;
	// callback читает все действия и выбирает Start только при контрактно разрешённом состоянии.
	QTimer::singleShot(0, &dock, [&] {
		// menu заимствован только до завершения menu.exec; start указывает на пункт по реальной локализации.
		auto *menu = qobject_cast<QMenu *>(QApplication::activePopupWidget());
		if (!menu)
			return;
		opened = true;
		QAction *start = nullptr;
		// action перечисляет все видимые пункты, включая запрещённые Stop/Restart свободного Destination.
		for (auto *action : menu->actions()) {
			if (action->isSeparator())
				continue;
			labels.append(action->text());
			enabled.append(action->isEnabled());
			if (action->text() == text("Operation.StartOutput"))
				start = action;
		}
		if (allowed && start && start->isEnabled()) {
			menu->setActiveAction(start);
			QTest::keyClick(menu, Qt::Key_Return);
		} else {
			menu->close();
		}
	});
	button->setFocus();
	QTest::keyClick(button, Qt::Key_Space);
	QVERIFY(opened);
	QCOMPARE(labels, QStringList({text("Operation.StartOutput"), text("Operation.StopOutput"),
				      text("Operation.RestartOutput"), text("Operation.OutputDetails")}));
	QCOMPARE(enabled, QList<bool>({allowed, false, false, true}));
	if (allowed) {
		// Запрос выбранного Destination обязателен; Add сначала отдельно подтверждает точную Session.
		// Предикат читает сохранённый HTTP-запрос request и ждёт точный ID назначения в его маршруте.
		QTRY_VERIFY_WITH_TIMEOUT(std::any_of(rig.server.requests.cbegin(), rig.server.requests.cend(),
						     [](const auto &request) {
							     return request.target ==
								    "/api/v1/destinations/selected-destination";
						     }),
					 1000);
		QVERIFY(rig.state.operations().current);
		QCOMPARE(rig.state.operations().current->action, condition == QStringLiteral("running")
									 ? gost::OperationAction::AddOutput
									 : gost::OperationAction::StartDestination);
		QCOMPARE(rig.state.operations().current->destinationId, QStringLiteral("selected-destination"));
		QCOMPARE(rig.server.requests.first().method, QByteArray("GET"));
		QCOMPARE(rig.server.requests.first().target,
			 condition == QStringLiteral("running")
				 ? QByteArray("/api/v1/sessions/session")
				 : QByteArray("/api/v1/destinations/selected-destination"));
		QVERIFY(rig.state.operations().current->outputId.isEmpty());
		if (condition == QStringLiteral("running"))
			QCOMPARE(rig.state.operations().current->sessionId, QStringLiteral("session"));
	} else {
		QVERIFY(rig.server.requests.isEmpty());
	}
	QVERIFY(rig.posts().isEmpty());
	QVERIFY(!rig.frontend.streaming);
	QCOMPARE(rig.frontend.stopRequests, 0);
}

// Замена состояния происходит после открытия меню, когда context, режим и Destination уже выбраны.
void PluginLifecycleTest::catalogStartKeepsFrozenModeAndDestination_data()
{
	QTest::addColumn<QString>("change");
	// change задаёт появление/замену Session, смену контекста, перестройку строк или удаление владельца.
	for (const auto *change : {"none", "row-reordered", "session-appeared", "session-replaced", "context",
				   "deleted", "destination-removed"})
		QTest::newRow(change) << QString::fromLatin1(change);
}

// Новая Session не превращает старый create в add; новое имя или индекс не становятся адресом выбранной команды.
void PluginLifecycleTest::catalogStartKeepsFrozenModeAndDestination()
{
	// change определяет одно событие внутри вложенного event loop меню.
	QFETCH(QString, change);
	// rig задаёт только локальные фикстуры, text предоставляет реальные подписи.
	OutputDockRig rig;
	QVERIFY(rig.initialize());
	const auto text = localeLookup();
	// metadata содержит два одинаковых имени, но разные Destination ID; первый выбран по порядку точных ключей.
	auto metadata = rig.state.dockServer();
	metadata.outputs.clear();
	metadata.session.reset();
	metadata.destinations.insert(QStringLiteral("selected-destination"), {QStringLiteral("Одинаковое имя")});
	metadata.destinations.insert(QStringLiteral("zz-neighbor"), {QStringLiteral("Одинаковое имя")});
	if (change == QStringLiteral("session-replaced"))
		metadata.session = gost::DockSession{QStringLiteral("session"), QStringLiteral("RUNNING"), false};
	rig.state.setDockServer(metadata);
	// dock защищён QPointer; cleanup удаляет оставшийся объект до зависимостей даже после раннего отказа проверки.
	QPointer<gost::RestreamDock> dock = new gost::RestreamDock(rig.state, text, nullptr, {}, &rig.commands);
	const auto cleanup = qScopeGuard([&] {
		if (dock)
			delete dock;
	});
	dock->resize(360, 560);
	dock->show();
	// list, row и button адресуют исходную строку до последующего переиспользования её QWidget.
	auto *list = dock->findChild<QListView *>(QStringLiteral("outputList"));
	QVERIFY(list);
	QTRY_COMPARE_WITH_TIMEOUT(list->model()->rowCount(), 2, 1000);
	auto *row = list->indexWidget(list->model()->index(0, 0));
	QVERIFY(row);
	auto *button = row->findChild<QToolButton *>(QStringLiteral("outputActionsButton"));
	QVERIFY(button && button->isEnabled());
	// opened подтверждает реальный popup, selected — наличие разрешённого исходного Start до изменения состояния.
	bool opened = false;
	bool selected = false;
	// callback меняет только наблюдение и затем выбирает исходный QAction старого меню.
	QTimer::singleShot(0, &rig.state, [&] {
		// menu и start заимствованы у popup до закрытия или уничтожения Dock.
		auto *menu = qobject_cast<QMenu *>(QApplication::activePopupWidget());
		if (!menu)
			return;
		opened = true;
		QAction *start = nullptr;
		// action находит исходное намерение по переведённой подписи до изменения данных.
		for (auto *action : menu->actions())
			if (action->text() == text("Operation.StartOutput"))
				start = action;
		if (!start || !start->isEnabled()) {
			menu->close();
			return;
		}
		selected = true;
		if (change == QStringLiteral("deleted")) {
			delete dock;
			return;
		}
		// changed сохраняет первоначальное наблюдение и вносит ровно выбранное изменение внутри меню.
		auto changed = rig.state.dockServer();
		if (change == QStringLiteral("row-reordered")) {
			changed.destinations.insert(QStringLiteral("a-new-neighbor"),
						    {QStringLiteral("Новая первая строка")});
			changed.destinations[QStringLiteral("selected-destination")].name =
				QStringLiteral("Новое имя выбранного");
		}
		if (change == QStringLiteral("session-appeared") || change == QStringLiteral("session-replaced"))
			changed.session = gost::DockSession{QStringLiteral("replacement-session"),
							    QStringLiteral("RUNNING"), false};
		if (change == QStringLiteral("destination-removed"))
			changed.destinations.remove(QStringLiteral("selected-destination"));
		if (change == QStringLiteral("context"))
			rig.state.setSelection(QStringLiteral("other-stream"), true, {}, {}, QStringLiteral("profile"));
		else
			rig.state.setDockServer(changed);
		menu->setActiveAction(start);
		QTest::keyClick(menu, Qt::Key_Return);
	});
	button->click();
	QVERIFY(opened && selected);
	if (change == QStringLiteral("none") || change == QStringLiteral("row-reordered")) {
		QTRY_VERIFY_WITH_TIMEOUT(!rig.server.requests.isEmpty(), 1000);
		QCOMPARE(rig.server.requests.first().target, QByteArray("/api/v1/destinations/selected-destination"));
		QVERIFY(rig.state.operations().current);
		QCOMPARE(rig.state.operations().current->action, gost::OperationAction::StartDestination);
		QCOMPARE(rig.state.operations().current->destinationId, QStringLiteral("selected-destination"));
	} else {
		dispatchQueuedEvents();
		QVERIFY(rig.server.requests.isEmpty());
	}
	if (change == QStringLiteral("deleted"))
		QVERIFY(dock.isNull());
	QVERIFY(rig.posts().isEmpty());
	QVERIFY(!rig.frontend.streaming);
}

// Проверяет прямой переход в обеих локалях и смену контекста внутри вложенного меню.
void PluginLifecycleTest::catalogDetailsUseDestinationWithoutSession_data()
{
	QTest::addColumn<QString>("locale");
	QTest::addColumn<QString>("change");
	// locale определяет реальные подписи, change задаёт событие до выбора Details.
	for (const auto &locale : {QStringLiteral("ru-RU"), QStringLiteral("en-US")})
		for (const auto &change :
		     {QStringLiteral("none"), QStringLiteral("reorder"), QStringLiteral("context")})
			QTest::newRow(qPrintable(locale + change)) << locale << change;
}

// Ещё не запущенная площадка адресуется по исходному Destination, без Session/Output и без второго сигнала-адаптера.
void PluginLifecycleTest::catalogDetailsUseDestinationWithoutSession()
{
	// locale выбирает перевод, change определяет изменение состояния во вложенном menu.exec.
	QFETCH(QString, locale);
	QFETCH(QString, change);
	// rig содержит только настройки выбранного Stream; text предоставляет действительную локализацию.
	OutputDockRig rig;
	QVERIFY(rig.initialize());
	const auto text = localeLookup(locale);
	// metadata не подтверждает даже отсутствие Session: чтение карточки не требует runtime-доказательства.
	auto metadata = rig.state.dockServer();
	metadata.session.reset();
	metadata.sessionStatus = {};
	metadata.outputs.clear();
	metadata.destinations.insert(QStringLiteral("selected-destination"), {QStringLiteral("Площадка до эфира")});
	rig.state.setDockServer(metadata);
	// originalContext фиксирует ссылку до открытия меню; dock сохраняет единственный UI-источник сигнала.
	const auto originalContext = rig.state.dockContext();
	gost::RestreamDock dock(rig.state, text, nullptr, {}, &rig.commands);
	dock.show();
	// details и legacy различают новый прямой контракт и прежний адаптер для attached Output.
	QSignalSpy details(&dock, &gost::RestreamDock::destinationDetailsRequested);
	QSignalSpy legacy(&dock, &gost::RestreamDock::outputDetailsRequested);
	QVERIFY(details.isValid() && legacy.isValid());
	// list/row/button открывают настоящий пункт Details единственной каталоговой строки.
	auto *list = dock.findChild<QListView *>(QStringLiteral("outputList"));
	QVERIFY(list);
	QTRY_COMPARE_WITH_TIMEOUT(list->model()->rowCount(), 1, 1000);
	auto *row = list->indexWidget(list->model()->index(0, 0));
	QVERIFY(row);
	auto *button = row->findChild<QToolButton *>(QStringLiteral("outputActionsButton"));
	QVERIFY(button && button->isEnabled());
	// selected подтверждает доступность самого пункта, callback изменяет модель перед активацией старого QAction.
	bool selected = false;
	QTimer::singleShot(0, &dock, [&] {
		// menu и action существуют до возврата из menu.exec; candidate перебирает исходные пункты.
		auto *menu = qobject_cast<QMenu *>(QApplication::activePopupWidget());
		if (!menu)
			return;
		QAction *action = nullptr;
		for (auto *candidate : menu->actions())
			if (candidate->text() == text("Operation.OutputDetails"))
				action = candidate;
		if (!action || !action->isEnabled()) {
			menu->close();
			return;
		}
		selected = true;
		if (change == QStringLiteral("context"))
			rig.state.setSelection(QStringLiteral("other-stream"), true, {}, {}, QStringLiteral("profile"));
		if (change == QStringLiteral("reorder")) {
			// reordered меняет первую строку, но не исходный ID уже открытого меню.
			auto reordered = rig.state.dockServer();
			reordered.destinations.insert(QStringLiteral("a-new"), {QStringLiteral("Новый первый")});
			rig.state.setDockServer(reordered);
		}
		menu->setActiveAction(action);
		QTest::keyClick(menu, Qt::Key_Return);
	});
	button->click();
	QVERIFY(selected);
	QCOMPARE(details.size(), change == QStringLiteral("context") ? 0 : 1);
	if (!details.isEmpty()) {
		QCOMPARE(details.first().size(), 2);
		QCOMPARE(qvariant_cast<gost::DockContext>(details.first().at(0)), originalContext);
		QCOMPARE(details.first().at(1).toString(), QStringLiteral("selected-destination"));
	}
	QVERIFY(legacy.isEmpty());
	QVERIFY(rig.server.requests.isEmpty());
	QVERIFY(!rig.state.dockServer().session);
	QCOMPARE(rig.frontend.stopRequests, 0);
}

// Start All сразу принимает показанный каталог целиком и не открывает подтверждение перед первой проверкой.
void PluginLifecycleTest::outputStartHeaderUsesShownCatalogWithoutConfirmation()
{
	// rig оставляет Session отсутствующей и предоставляет только локальную границу команд.
	OutputDockRig rig;
	QVERIFY(rig.initialize());
	// metadata содержит 28 независимых назначений за пределами видимой области списка; index задаёт их ID.
	auto metadata = rig.state.dockServer();
	metadata.session.reset();
	metadata.outputs.clear();
	for (int index = 0; index < 28; ++index)
		metadata.destinations.insert(QStringLiteral("destination-%1").arg(index, 2, 10, QLatin1Char('0')),
					     {QStringLiteral("Площадка %1").arg(index)});
	rig.state.setDockServer(metadata);
	// dock, list и button представляют реальный путь общей команды до OBS/Session.
	gost::RestreamDock dock(rig.state, localeLookup(), nullptr, {}, &rig.commands);
	dock.resize(360, 560);
	dock.show();
	auto *list = dock.findChild<QListView *>(QStringLiteral("outputList"));
	auto *button = dock.findChild<QToolButton *>(QStringLiteral("outputsStartButton"));
	QVERIFY(list && button && button->isEnabled());
	QTRY_COMPARE_WITH_TIMEOUT(list->model()->rowCount(), 28, 1000);
	QTRY_VERIFY_WITH_TIMEOUT(list->verticalScrollBar()->maximum() > 0, 1000);
	// confirmed обнаруживает неожиданное подтверждение; callback закрывает его, чтобы дефект не зависал в exec.
	bool confirmed = false;
	QTimer::singleShot(0, &dock, [&] {
		// confirmation заимствован у текущего модального окна только на время этого оборота Qt.
		auto *confirmation = qobject_cast<QMessageBox *>(QApplication::activeModalWidget());
		if (confirmation) {
			confirmed = true;
			confirmation->done(QMessageBox::No);
		}
	});
	button->click();
	QVERIFY(!confirmed);
	QVERIFY(rig.state.operations().current);
	QCOMPARE(rig.state.operations().current->action, gost::OperationAction::StartAllOutputs);
	QCOMPARE(rig.state.operations().current->outputs.size(), 28);
	QCOMPARE(rig.state.operations().current->outputs.last().destinationId, QStringLiteral("destination-27"));
	QVERIFY(!rig.state.dockServer().session);
	QVERIFY(rig.posts().isEmpty());
	QCOMPARE(rig.frontend.stopRequests, 0);
}
