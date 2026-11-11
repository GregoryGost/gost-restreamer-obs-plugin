#pragma once

#include "obs/frontend.hpp"

#include <QDockWidget>
#include <QMainWindow>
#include <QPointer>
#include <QStringList>

#include <memory>
#include <stdexcept>

// Управляемая граница Frontend с настоящими QWidget/QDockWidget и владением Qt, а не заглушкой UI.
// Поля statistics/profile/streaming задают наблюдения, флаги отказов — точки отката. Счётчики
// доказывают отсутствие ранних/поздних вызовов API и порядок очистки. Нужен QApplication GUI-потока.
// host принадлежит фикстуре; Dock и его содержимое — host/Qt. Заимствованные cookie не удерживаются живыми.
class FakeFrontend final : public gost::Frontend {
public:
	QWidget *mainWindow() const override
	{
		recordCall();
		return host.get();
	}

	bool addDock(const char *id, const QString &title, QWidget *widget) override
	{
		// Передача владения происходит лишь после всех искусственных отказов. content — QPointer,
		// поэтому отказ addDock не делает фикстуру вторым владельцем ещё локального unique_ptr виджета.
		recordCall();
		++dockAdds;
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

	void removeDock(const char *id) override
	{
		recordCall();
		++dockRemoves;
		removedId = QString::fromUtf8(id);
		cleanupOrder.append(QStringLiteral("dock"));
		delete dock.data();
	}

	void addEventCallback(EventCallback callback, void *cookie) override
	{
		recordCall();
		++callbackAdds;
		if (throwOnCallbackAdd)
			throw std::runtime_error("Ошибка регистрации callback.");
		if (activeCallback)
			++invalidCallbackOperations;
		activeCallback = callback;
		activeCookie = cookie;
		lastCallback = callback;
		lastCookie = cookie;
	}

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

	QString currentProfile() const override
	{
		recordCall();
		++profileReads;
		if (throwOnProfileRead)
			throw std::runtime_error("Ошибка чтения профиля.");
		return profile;
	}

	QString currentProfilePath() const override
	{
		recordCall();
		++profilePathReads;
		return profilePath;
	}

	QStringList profiles() const override
	{
		recordCall();
		++inventoryReads;
		return profileInventory.isEmpty() ? QStringList{profile} : profileInventory;
	}

	bool streamingActive() const override
	{
		recordCall();
		++streamingReads;
		return streaming;
	}

	gost::ObsStatisticsSample readStatistics() override
	{
		recordCall();
		++statisticsReads;
		return statistics;
	}

	void releaseStatistics() noexcept override
	{
		recordCall();
		++statisticsReleases;
	}

	void emitEvent(gost::FrontendEvent event)
	{
		// Frontend становится недоступным сразу после завершения рассылки Exit. Поэтому очистка
		// через отложенный вызов или deleteLater была бы обнаружена callsAfterExit, даже без аварии настоящего OBS.
		// Обработчик Exit может снять собственную подписку прямо во время вызова.
		const auto callback = activeCallback;
		const auto cookie = activeCookie;
		if (callback)
			callback(event, cookie);
		if (event == gost::FrontendEvent::Exit)
			frontendUnavailable = true;
	}

	void recordCall() const
	{
		if (frontendUnavailable)
			++callsAfterExit;
	}

	std::unique_ptr<QMainWindow> host = std::make_unique<QMainWindow>();
	QPointer<QDockWidget> dock;
	QPointer<QWidget> content;
	QString profile = QStringLiteral("Основной профиль");
	QString profilePath = QStringLiteral("profiles/main");
	QStringList profileInventory;
	QString dockId;
	QString dockTitle;
	QString removedId;
	QStringList cleanupOrder;
	gost::ObsStatisticsSample statistics;
	bool streaming = false;
	bool acceptDock = true;
	bool frontendUnavailable = false;
	bool throwOnDockAdd = false;
	bool throwOnCallbackAdd = false;
	bool throwOnProfileRead = false;
	int dockAdds = 0;
	int dockRemoves = 0;
	int callbackAdds = 0;
	int callbackRemoves = 0;
	int invalidCallbackOperations = 0;
	int statisticsReads = 0;
	int statisticsReleases = 0;
	mutable int profileReads = 0;
	mutable int profilePathReads = 0;
	mutable int inventoryReads = 0;
	mutable int streamingReads = 0;
	mutable int callsAfterExit = 0;
	EventCallback activeCallback = nullptr;
	void *activeCookie = nullptr;
	// Сохранённая пара позволяет имитировать поздний вызов, пока оболочка PluginLifecycle ещё жива.
	// lastCookie допустимо вызывать лишь пока внешняя оболочка lifecycle ещё существует;
	// это моделирует задержанную доставку, а не разрешает разыменование после её деструктора.
	EventCallback lastCallback = nullptr;
	void *lastCookie = nullptr;
};
