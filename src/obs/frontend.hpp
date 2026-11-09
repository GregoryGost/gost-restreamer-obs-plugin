#pragma once

#include <QString>

class QWidget;

namespace gost {

enum class FrontendEvent {
	FinishedLoading,
	ProfileChanged,
	StreamingStarting,
	StreamingStarted,
	StreamingStopping,
	StreamingStopped,
	Exit,
};

class Frontend {
public:
	using EventCallback = void (*)(FrontendEvent event, void *cookie) noexcept;
	virtual ~Frontend() = default;

	virtual QWidget *mainWindow() const = 0;
	// Успешная регистрация передаёт владение виджетом OBS; удаление Dock уничтожает его синхронно.
	virtual bool addDock(const char *id, const QString &title, QWidget *widget) = 0;
	virtual void removeDock(const char *id) = 0;
	virtual void addEventCallback(EventCallback callback, void *cookie) = 0;
	virtual void removeEventCallback(EventCallback callback, void *cookie) = 0;
	virtual QString currentProfile() const = 0;
	virtual bool streamingActive() const = 0;
};

} // namespace gost
