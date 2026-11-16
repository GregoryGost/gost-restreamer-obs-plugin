#pragma once

#include "obs-statistics.hpp"

#include <QString>
#include <QStringList>
#include <QMetaType>

class QAction;
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

// Независимая выборка штатной трансляции без передачи указателей libobs вызывающему коду.
struct StreamingState {
	// true означает, что адаптер смог достоверно прочитать флаги; отсутствие output само по себе не ошибка.
	bool available = false;
	// Штатный выход активен; переподключение также может входить в активный жизненный цикл OBS.
	bool active = false;
	// Выход пытается восстановить подключение, даже если медиаданные сейчас не передаются.
	bool reconnecting = false;
};

/**
 * Граница OBS API, заменяемая управляемым двойником в тестах логики и жизненного цикла.
 * Адаптер заимствует OBS; вызывающая сторона обеспечивает GUI-поток, чтение только после
 * FinishedLoading и освобождение подписок до возврата из Exit. Dock, пункт «Сервис» и callback можно
 * регистрировать до FinishedLoading. Интерфейс не даёт создавать второй output или менять сервис трансляции.
 *
 * Все возвращаемые QString/списки/выборки являются собственными значениями. mainWindow и пункт
 * меню принадлежат OBS. cookie обработчика заимствован; владелец обязан снять ту же пару до
 * уничтожения cookie. Реализация не должна выпускать исключение через границу EventCallback.
 */
class Frontend {
public:
	// Обработчик получает событие и заимствованный cookie; исключения через границу OBS запрещены.
	using EventCallback = void (*)(FrontendEvent event, void *cookie) noexcept;
	// Разрушает адаптер; конкретная реализация освобождает принадлежащие ей подписки.
	virtual ~Frontend() = default;

	// Главное окно заимствовано у OBS; адаптер не владеет им.
	virtual QWidget *mainWindow() const = 0;
	// Добавляет title в меню «Сервис» и возвращает заимствованный QAction, принадлежащий меню OBS.
	// При отказе возвращает nullptr; владелец плагина удаляет пункт до конца Exit, если OBS ещё не удалил его.
	virtual QAction *addToolsMenuAction(const QString &title) = 0;
	// Успешная регистрация передаёт владение виджетом OBS; удаление Dock уничтожает его синхронно.
	// При отказе регистрации владение остаётся у вызывающей стороны.
	virtual bool addDock(const char *id, const QString &title, QWidget *widget) = 0;
	// Удаляет зарегистрированный Dock с данным id вместе с переданным OBS виджетом.
	virtual void removeDock(const char *id) = 0;
	// Регистрируется одна пара из обработчика и cookie; cookie действует до снятия подписки.
	virtual void addEventCallback(EventCallback callback, void *cookie) = 0;
	// Снимает ранее зарегистрированную пару callback/cookie до уничтожения получателя.
	virtual void removeEventCallback(EventCallback callback, void *cookie) = 0;
	// Эти три значения читаются в одном GUI callback и складываются в ProfileDescriptor.
	// Чтения в разные моменты могут захватить разные переходы и ошибочно связать новый профиль со старым UUID.
	virtual QString currentProfile() const = 0;
	// Возвращает каталог текущего профиля в том же GUI-вызове, что имя и список профилей.
	virtual QString currentProfilePath() const = 0;
	// Возвращает имена профилей для проверки переименования, без изменения выбора OBS.
	virtual QStringList profiles() const = 0;
	// Возвращает текущий флаг активности штатной трансляции OBS.
	virtual bool streamingActive() const = 0;
	// Читает за один GUI-вызов флаги active/reconnecting и доступность наблюдения; атомарности флагов не обещает.
	virtual StreamingState streamingState() const = 0;
	// Запрашивает штатный stop без обещания завершения; остановку подтверждают события и streamingState.
	virtual void requestStreamingStop() = 0;
	// Читает штатные источники статистики без изменения их счётчиков. Недоступные поля остаются optional.
	// Выборка может лениво создать подписки; releaseStatistics снимает их до возврата из Exit.
	// Базовая реализация допускает frontend без статистики (например, ограниченный тестовый двойник).
	virtual ObsStatisticsSample readStatistics() { return {}; }
	// Снимает подписки статистики до EXIT; базовый адаптер без статистики ничего не делает.
	virtual void releaseStatistics() noexcept {}
};

} // namespace gost

Q_DECLARE_METATYPE(gost::FrontendEvent)
Q_DECLARE_METATYPE(gost::ProfileDescriptor)
