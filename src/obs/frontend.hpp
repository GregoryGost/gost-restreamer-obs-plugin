#pragma once

#include "obs-statistics.hpp"

#include <QString>
#include <QStringList>
#include <QMetaType>

class QWidget;

namespace gost {

enum class FrontendEvent {
	FinishedLoading,
	ProfileChanging,
	ProfileChanged,
	ProfileRenamed,
	ProfileListChanged,
	StreamingStarting,
	StreamingStarted,
	StreamingStopping,
	StreamingStopped,
	Exit,
};

struct ProfileDescriptor {
	QString name;
	QString path;
	QStringList inventory;
	bool valid = false;
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
	virtual QString currentProfilePath() const = 0;
	virtual QStringList profiles() const = 0;
	virtual bool streamingActive() const = 0;
	virtual ObsStatisticsSample readStatistics() { return {}; }
	virtual void releaseStatistics() noexcept {}
};

} // namespace gost

Q_DECLARE_METATYPE(gost::FrontendEvent)
Q_DECLARE_METATYPE(gost::ProfileDescriptor)
