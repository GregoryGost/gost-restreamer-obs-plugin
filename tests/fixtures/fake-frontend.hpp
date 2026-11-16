#pragma once

#include "obs/frontend.hpp"

#include <QDockWidget>
#include <QMainWindow>
#include <QMenu>
#include <QMenuBar>
#include <QAction>
#include <QPointer>
#include <QStringList>
#include <functional>

#include <memory>
#include <stdexcept>

// Управляемая граница Frontend с настоящими QWidget/QDockWidget, меню QAction и владением Qt.
// Поля statistics/profile/streaming задают наблюдения, флаги отказов — точки отката. Счётчики
// доказывают отсутствие ранних/поздних вызовов API и порядок очистки. Нужен QApplication GUI-потока.
// host принадлежит фикстуре; Dock и его содержимое — host/Qt. Заимствованные cookie не удерживаются живыми.
class FakeFrontend final : public gost::Frontend {
public:
	// Удаляет главное окно до полей журнала, которые используются наблюдателем удаления QAction.
	~FakeFrontend() override { host.reset(); }

	// Возвращает заимствованное главное окно host и отмечает недопустимый поздний вызов.
	QWidget *mainWindow() const override
	{
		recordCall();
		return host.get();
	}

	// Регистрирует widget в Dock с id/title; искусственный отказ сохраняет владение у вызывающего кода.
	bool addDock(const char *id, const QString &title, QWidget *widget) override
	{
		// Передача владения происходит лишь после всех искусственных отказов. content — QPointer,
		// поэтому отказ addDock не делает фикстуру вторым владельцем ещё локального unique_ptr виджета.
		recordCall();
		++dockAdds;
		registrationOrder.append(QStringLiteral("dock"));
		dockId = QString::fromUtf8(id);
		dockTitle = title;
		content = widget;
		if (throwOnDockAdd)
			throw std::runtime_error("Ошибка регистрации Dock.");
		if (!acceptDock)
			return false;
		dock = new QDockWidget(title, host.get());
		dock->setWidget(widget);
		host->addDockWidget(Qt::LeftDockWidgetArea, dock);
		return true;
	}

	// Добавляет title в настоящее меню «Сервис»; отказ не передаёт вызывающему коду частичный QAction.
	QAction *addToolsMenuAction(const QString &title) override
	{
		recordCall();
		++toolsMenuAdds;
		registrationOrder.append(QStringLiteral("toolsMenu"));
		toolsMenuTitle = title;
		if (throwOnToolsMenuAdd)
			throw std::runtime_error("Ошибка регистрации меню Сервис.");
		if (!acceptToolsMenu)
			return nullptr;
		if (!toolsMenu)
			toolsMenu = host->menuBar()->addMenu(QStringLiteral("Сервис"));
		managementAction = toolsMenu->addAction(title);
		// Наблюдатель фиксирует синхронное удаление пункта, в том числе при уничтожении главного окна.
		QObject::connect(managementAction.data(), &QObject::destroyed, [this] {
			++toolsMenuRemoves;
			cleanupOrder.append(QStringLiteral("toolsMenu"));
		});
		return managementAction.data();
	}

	// Синхронно удаляет Dock id вместе с дочерним содержимым и записывает порядок очистки.
	void removeDock(const char *id) override
	{
		recordCall();
		++dockRemoves;
		removedId = QString::fromUtf8(id);
		cleanupOrder.append(QStringLiteral("dock"));
		delete dock.data();
	}

	// Сохраняет callback/cookie до снятия подписки; счётчик выявляет повторную регистрацию.
	void addEventCallback(EventCallback callback, void *cookie) override
	{
		recordCall();
		++callbackAdds;
		registrationOrder.append(QStringLiteral("callback"));
		if (throwOnCallbackAdd)
			throw std::runtime_error("Ошибка регистрации callback.");
		if (activeCallback)
			++invalidCallbackOperations;
		activeCallback = callback;
		activeCookie = cookie;
		lastCallback = callback;
		lastCookie = cookie;
	}

	// Снимает точную пару callback/cookie и обнаруживает несовпадающую пару.
	void removeEventCallback(EventCallback callback, void *cookie) override
	{
		recordCall();
		++callbackRemoves;
		if (callback != activeCallback || cookie != activeCookie)
			++invalidCallbackOperations;
		activeCallback = nullptr;
		activeCookie = nullptr;
		cleanupOrder.append(QStringLiteral("callback"));
	}

	// Возвращает заданное имя профиля или внедряет отказ чтения.
	QString currentProfile() const override
	{
		recordCall();
		++profileReads;
		if (throwOnProfileRead)
			throw std::runtime_error("Ошибка чтения профиля.");
		return profile;
	}

	// Возвращает заданный каталог текущего профиля для разрешения устойчивой идентичности.
	QString currentProfilePath() const override
	{
		recordCall();
		++profilePathReads;
		return profilePath;
	}

	// Возвращает заданный список профилей либо единственный текущий профиль.
	QStringList profiles() const override
	{
		recordCall();
		++inventoryReads;
		return profileInventory.isEmpty() ? QStringList{profile} : profileInventory;
	}

	// Возвращает заданную активность передачи, отдельно от подтверждения остановки.
	bool streamingActive() const override
	{
		recordCall();
		++streamingReads;
		return streaming;
	}

	// Возвращает доступность и флаги текущего штатного выхода без изменения его состояния.
	gost::StreamingState streamingState() const override
	{
		recordCall();
		++streamingStateReads;
		return {streamingStateAvailable, streaming, reconnecting};
	}

	// Учитывает запрос stop; onStopRequest позволяет сценарию отдельно подтвердить или отвергнуть остановку.
	void requestStreamingStop() override
	{
		recordCall();
		++stopRequests;
		if (onStopRequest)
			onStopRequest();
	}

	// Возвращает независимую синтетическую выборку статистики и учитывает чтение.
	gost::ObsStatisticsSample readStatistics() override
	{
		recordCall();
		++statisticsReads;
		return statistics;
	}

	// Учитывает освобождение подписок статистики до недоступности Frontend.
	void releaseStatistics() noexcept override
	{
		recordCall();
		++statisticsReleases;
	}

	// Синхронно доставляет event зарегистрированному callback; после Exit запрещает дальнейшие чтения.
	void emitEvent(gost::FrontendEvent event)
	{
		// Frontend становится недоступным сразу после завершения рассылки Exit. Поэтому очистка
		// через отложенный вызов или deleteLater была бы обнаружена callsAfterExit, даже без аварии настоящего OBS.
		// Обработчик Exit может снять собственную подписку прямо во время вызова.
		// callback и cookie фиксируют исходную пару, которую обработчик может снять во время рассылки.
		const auto callback = activeCallback;
		const auto cookie = activeCookie;
		if (callback)
			callback(event, cookie);
		if (event == gost::FrontendEvent::Exit)
			frontendUnavailable = true;
	}

	// Учитывает обращение к уже недоступному Frontend, не обращаясь к настоящему OBS.
	void recordCall() const
	{
		if (frontendUnavailable)
			++callsAfterExit;
	}

	// Главное окно принадлежит фикстуре и переживает зарегистрированные виджеты.
	std::unique_ptr<QMainWindow> host = std::make_unique<QMainWindow>();
	// Заимствованный Dock автоматически обнуляется после удаления Qt.
	QPointer<QDockWidget> dock;
	// Заимствованное содержимое позволяет наблюдать передачу владения без второго владельца.
	QPointer<QWidget> content;
	// Меню принадлежит главному окну; наблюдатель не мешает его синхронному удалению родителем.
	QPointer<QMenu> toolsMenu;
	// Зарегистрированный пункт управления принадлежит меню, а фикстура только отслеживает его жизнь.
	QPointer<QAction> managementAction;
	// Последняя подпись, переданная на регистрацию в «Сервис».
	QString toolsMenuTitle;
	// Порядок приобретения Dock, пункта меню и подписки.
	QStringList registrationOrder;
	// Отображаемое имя текущего профиля для сценариев переключения и переименования.
	QString profile = QStringLiteral("Основной профиль");
	// Каталог профиля, отличающий его идентичность от отображаемого имени.
	QString profilePath = QStringLiteral("profiles/main");
	// Явный список имён профилей; пустой список использует текущий профиль.
	QStringList profileInventory;
	// Последний идентификатор зарегистрированного Dock.
	QString dockId;
	// Последний заголовок зарегистрированного Dock.
	QString dockTitle;
	// Идентификатор, переданный последнему removeDock.
	QString removedId;
	// Порядок снятия callback, удаления пункта меню и Dock для проверки lifecycle.
	QStringList cleanupOrder;
	// Независимая выборка OBS с управляемыми доступностью и накоплениями.
	gost::ObsStatisticsSample statistics;
	// Наблюдаемый флаг активности штатного выхода.
	bool streaming = false;
	// reconnecting моделирует восстановление соединения при временно неактивной передаче.
	bool reconnecting = false;
	// false означает недостоверные флаги, а не подтверждённую остановку.
	bool streamingStateAvailable = true;
	// Число вызовов void stop позволяет отличить запрос от подтверждённой остановки.
	int stopRequests = 0;
	// Число независимых выборок streamingState.
	mutable int streamingStateReads = 0;
	// Сценарный callback выполняется при stop и может доставить отдельное событие OBS.
	std::function<void()> onStopRequest;
	// false моделирует отказ регистрации Dock без исключения.
	bool acceptDock = true;
	// false моделирует отсутствие QAction при отказе регистрации меню.
	bool acceptToolsMenu = true;
	// Исключение на границе регистрации меню проверяет откат уже переданного Dock.
	bool throwOnToolsMenuAdd = false;
	// Количество попыток добавить пункт меню управления.
	int toolsMenuAdds = 0;
	// Количество фактических удалений QAction, включая удаление родителем.
	int toolsMenuRemoves = 0;
	// true после Exit обнаруживает обращения за пределами допустимого lifecycle.
	bool frontendUnavailable = false;
	// Исключение до передачи владения проверяет откат загрузки.
	bool throwOnDockAdd = false;
	// Исключение регистрации callback проверяет откат подписки.
	bool throwOnCallbackAdd = false;
	// Исключение чтения профиля проверяет обработку отказа Frontend.
	bool throwOnProfileRead = false;
	// Количество попыток регистрации Dock.
	int dockAdds = 0;
	// Количество удалений Dock.
	int dockRemoves = 0;
	// Количество попыток регистрации callback.
	int callbackAdds = 0;
	// Количество попыток снятия callback.
	int callbackRemoves = 0;
	// Количество повторных регистраций или несовпадающих снятий пары.
	int invalidCallbackOperations = 0;
	// Количество чтений статистики.
	int statisticsReads = 0;
	// Количество освобождений подписок статистики.
	int statisticsReleases = 0;
	// Количество чтений отображаемого имени профиля.
	mutable int profileReads = 0;
	// Количество чтений каталога профиля.
	mutable int profilePathReads = 0;
	// Количество чтений перечня профилей.
	mutable int inventoryReads = 0;
	// Количество чтений активности без выборки reconnecting.
	mutable int streamingReads = 0;
	// Количество недопустимых вызовов после завершения Frontend.
	mutable int callsAfterExit = 0;
	// Действующая подписка на события Frontend.
	EventCallback activeCallback = nullptr;
	// Заимствованный контекст действующей подписки.
	void *activeCookie = nullptr;
	// Сохранённая пара позволяет имитировать поздний вызов, пока оболочка PluginLifecycle ещё жива.
	// lastCookie допустимо вызывать лишь пока внешняя оболочка lifecycle ещё существует;
	// это моделирует задержанную доставку, а не разрешает разыменование после её деструктора.
	// Последний обработчик для управляемой доставки после снятия подписки.
	EventCallback lastCallback = nullptr;
	// Заимствованный контекст последней подписки, допустимый только пока lifecycle ещё существует.
	void *lastCookie = nullptr;
};
