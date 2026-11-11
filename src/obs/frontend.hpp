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

// Один снимок текущего профиля: name — имя, path — каталог, inventory — перечень имён профилей OBS.
// SettingsStore использует их вместе, чтобы отличить переключение от переименования и определить UUID.
// valid=false означает, что снимок нельзя использовать для подтверждения привязки или переименования.
struct ProfileDescriptor {
	QString name;
	QString path;
	QStringList inventory;
	bool valid = false;
};

/**
 * Граница OBS API, заменяемая управляемым двойником в тестах логики и жизненного цикла.
 * Адаптер заимствует OBS; вызывающая сторона обеспечивает GUI-поток, чтение только после
 * FinishedLoading и освобождение подписок до возврата из Exit. Регистрация Dock/callback допустима
 * до FinishedLoading. Интерфейс намеренно не даёт создавать второй output или менять сервис трансляции.
 *
 * Все возвращаемые QString/списки/выборки являются собственными значениями. Исключение — mainWindow,
 * который принадлежит OBS. cookie обработчика заимствован; владелец обязан снять ту же пару до
 * уничтожения cookie. Реализация не должна выпускать исключение через границу EventCallback.
 */
class Frontend {
public:
	using EventCallback = void (*)(FrontendEvent event, void *cookie) noexcept;
	virtual ~Frontend() = default;

	// Главное окно заимствовано у OBS; адаптер не владеет им.
	virtual QWidget *mainWindow() const = 0;
	// Успешная регистрация передаёт владение виджетом OBS; удаление Dock уничтожает его синхронно.
	// При отказе регистрации владение остаётся у вызывающей стороны.
	virtual bool addDock(const char *id, const QString &title, QWidget *widget) = 0;
	virtual void removeDock(const char *id) = 0;
	// Регистрируется одна пара из обработчика и cookie; cookie действует до снятия подписки.
	virtual void addEventCallback(EventCallback callback, void *cookie) = 0;
	virtual void removeEventCallback(EventCallback callback, void *cookie) = 0;
	// Эти три значения читаются в одном GUI callback и складываются в ProfileDescriptor.
	// Чтения в разные моменты могут захватить разные переходы и ошибочно связать новый профиль со старым UUID.
	virtual QString currentProfile() const = 0;
	virtual QString currentProfilePath() const = 0;
	virtual QStringList profiles() const = 0;
	virtual bool streamingActive() const = 0;
	// Читает штатные источники статистики без изменения их счётчиков. Недоступные поля остаются optional.
	// Выборка может лениво создать подписки; releaseStatistics снимает их до возврата из Exit.
	// Базовая реализация допускает frontend без статистики (например, ограниченный тестовый двойник).
	virtual ObsStatisticsSample readStatistics() { return {}; }
	virtual void releaseStatistics() noexcept {}
};

} // namespace gost

Q_DECLARE_METATYPE(gost::FrontendEvent)
Q_DECLARE_METATYPE(gost::ProfileDescriptor)
