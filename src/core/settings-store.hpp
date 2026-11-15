#pragma once

#include "obs/frontend.hpp"

#include <QMap>
#include <QObject>
#include <QString>

#include <optional>

namespace gost {

// Плагин назначает каждому профилю OBS устойчивый UUID. Привязки к потокам на всех серверах хранятся
// по этому UUID, поэтому переименование профиля их не сбрасывает. Имя и каталог могут измениться;
// locator хранит путь каталога профиля.
struct ProfileRecord {
	QString uuid;
	QString name;
	QString locator;
};

struct ProfileResolution {
	// resolved=true означает, что для текущего снимка профиля определён UUID и можно читать его привязки.
	// При false использовать uuid для выбора потока нельзя, даже если здесь сохранился прежний UUID.
	// changed сообщает смену идентичности или потерю её подтверждения; renamed подтверждает сохранение
	// прежнего UUID при смене имени/каталога.
	QString uuid;
	bool resolved = false;
	bool changed = false;
	bool renamed = false;
};

/**
 * Хранит локальные предпочтения и выбранный Stream для каждой пары «канонический origin + UUID профиля OBS».
 * Работает в GUI-потоке, не владеет OBS Profile и не хранит серверные ключи/endpoint. Ссылки методов чтения
 * заимствованы до следующего изменения. Конструктор сразу пытается прочитать указанный файл.
 *
 * Изменения сначала действуют в RAM, затем сохраняются атомарной заменой JSON формата v3.
 * Ошибка записи оставляет новые рабочие значения и unsaved=true, прежний файл сохраняется.
 * Неподдерживаемый/повреждённый вход блокирует запись, чтобы дальнейший метод изменения не уничтожил его.
 * changed сообщает как изменения данных, так и результат сохранения/ошибку; это не подтверждение записи.
 */
class SettingsStore final : public QObject {
	Q_OBJECT

public:
	// Значение -1 оставляет восстановление SSE без конечного числа попыток.
	static constexpr int UnlimitedSseRejectionRetries = -1;
	// Старые настройки и новые установки используют три повтора после отказа SSE.
	static constexpr int DefaultSseRejectionRetries = 3;
	// Общий срок обычного REST-запроса по умолчанию, в секундах.
	static constexpr int DefaultHttpTimeoutSeconds = 15;
	// Нижняя граница пользовательской настройки общего срока REST-запроса.
	static constexpr int MinHttpTimeoutSeconds = 5;
	// Верхняя граница пользовательской настройки общего срока REST-запроса.
	static constexpr int MaxHttpTimeoutSeconds = 60;

	// Читает разрешённый файл filePath; parent управляет временем жизни QObject.
	explicit SettingsStore(QString filePath, QObject *parent = nullptr);
	// Принимает HTTP(S) адрес без userinfo/query/fragment; путь может быть только пустым или равным "/".
	// В результате схема и хост приведены к нижнему регистру, корневой путь и стандартный порт удалены.
	// Пробелы по краям отвергаются; псевдонимы DNS и IP не объединяются в один сервер.
	static std::optional<QString> canonicalOrigin(const QString &text);
	// Отсутствующий файл допустим. Существующий документ проверяется целиком до замены RAM;
	// Форматы v1/v2 получают недостающие настройки по умолчанию без записи при одном чтении.
	bool load();
	// Записывает текущую RAM в формат v3 через QSaveFile без прямой перезаписи. Возвращает факт commit;
	// ошибка загрузки/записи доступна в error/errorKey. Может создать родительский каталог filePath.
	bool save();
	// Возвращает выбранный канонический origin без ключей и адресов назначений.
	const QString &serverOrigin() const noexcept;
	// Возвращает прежнее поле auto_connect для совместимости файла; штатное подключение всегда автоматически.
	bool autoConnect() const noexcept;
	// Возвращает прежнее поле reconnect для совместимости файла; восстановление REST/SSE не отключается им.
	bool reconnect() const noexcept;
	// Возвращает число повторов SSE либо UnlimitedSseRejectionRetries.
	int sseRejectionRetries() const noexcept;
	// Возвращает общий срок новых обычных REST-запросов в секундах.
	int httpTimeoutSeconds() const noexcept;
	// Возвращает включение подробных безопасных записей в штатный журнал OBS.
	bool detailedLogging() const noexcept;
	// Показывает наличие принятых в RAM, но не сохранённых предпочтений.
	bool unsaved() const noexcept;
	// Возвращает безопасное пояснение отказа чтения или записи настроек.
	const QString &error() const noexcept;
	// Возвращает ключ локализации соответствующего пояснения ошибки.
	const QString &errorKey() const noexcept;
	// Возвращает текущий подтверждённый UUID профиля OBS либо пустую строку.
	const QString &profileUuid() const noexcept;
	// Возвращает заимствованный реестр UUID с изменяемыми именами и путями профилей.
	const QMap<QString, ProfileRecord> &profiles() const noexcept;
	// Ищет точную пару без дополнительной нормализации; пустая строка означает отсутствие привязки.
	QString selectedStream(const QString &origin, const QString &profileUuid) const;
	// bool у методов изменения означает принятие допустимого значения в RAM, а не успешный save().
	// Повтор прежнего значения не сохраняет файл; состояние записи проверяется через unsaved/error.
	bool setServerOrigin(const QString &origin);
	// Сохраняет enabled в прежнем поле auto_connect; политику подключения текущего клиента не меняет.
	void setAutoConnect(bool enabled);
	// Сохраняет enabled в прежнем поле reconnect; политику восстановления текущего клиента не меняет.
	void setReconnect(bool enabled);
	// Сохраняет retries, если оно не меньше UnlimitedSseRejectionRetries.
	void setSseRejectionRetries(int retries);
	// Принимает только 5–60 секунд; bool подтверждает RAM, а не успешную запись файла.
	bool setHttpTimeoutSeconds(int seconds);
	// Сохраняет detailed как выбор обычного или подробного штатного журнала OBS.
	void setDetailedLogging(bool detailed);
	// Проверяет retries (-1 либо >=0) и seconds (5–60) до изменения RAM, затем применяет их вместе
	// с detailed и один раз вызывает save(). Возвращает подтверждение записи, при отказе сохраняет
	// рабочую RAM. Повтор тех же значений также пытается записать несохранённые настройки.
	bool saveAdvancedPreferences(int retries, int seconds, bool detailed);
	// Требует уже канонический origin и известный UUID. Пустой streamId удаляет привязку пары;
	// наличие Stream на сервере здесь не проверяется, чтобы неполный список не стёр сохранённый выбор.
	bool setSelectedStream(const QString &origin, const QString &profileUuid, const QString &streamId);
	/**
	 * Определяет UUID текущего профиля по имени, пути и перечню профилей, захваченным в callback OBS.
	 * Для известного пути возвращает прежний UUID. Перенос UUID на новый путь считается переименованием
	 * только без ProfileChanging, при исчезновении прежнего имени и однозначной последовательности событий.
	 * Новый или дублированный профиль получает новый UUID. Если снимок ненадёжен, прежние привязки
	 * не переносятся на неизвестный профиль по догадке; результат возвращается с resolved=false.
	 * Обновление реестра и очистка отсутствующих профилей сохраняются вместе с привязками всех серверов.
	 */
	ProfileResolution observeProfile(FrontendEvent event, const ProfileDescriptor &descriptor);

signals:
	// Сообщает об изменении рабочих значений или результата их сохранения.
	void changed();

private:
	// Пытается атомарно сохранить RAM, не откатывая её при отказе диска.
	void persist();
	// Удаляет профили, чьих имён нет в достоверном перечне inventory, вместе с их привязками; текущий UUID сохраняет.
	void retireAbsent(const QStringList &inventory);
	// Разрешённый путь единственного локального файла предпочтений.
	QString filePath_;
	// Канонический origin управляющего API, пустой до выбора сервера.
	QString origin_;
	// Прежнее поле auto_connect, сохраняемое для совместимости формата настроек.
	bool autoConnect_ = true;
	// Прежнее поле reconnect; текущее штатное восстановление от этого значения не зависит.
	bool reconnect_ = true;
	// Бюджет повторов после отказа подписки SSE 400/409.
	int sseRejectionRetries_ = DefaultSseRejectionRetries;
	// Общий срок новых обычных REST-запросов, в секундах.
	int httpTimeoutSeconds_ = DefaultHttpTimeoutSeconds;
	// false выбирает обычные записи; true добавляет безопасные технические сведения.
	bool detailedLogging_ = false;
	// Рабочие изменения ещё не подтверждены атомарной записью.
	bool unsaved_ = false;
	// Неизвестный или повреждённый вход запрещает перезапись исходного файла.
	bool writeAllowed_ = true;
	// Безопасное пояснение последнего отказа локального хранилища.
	QString error_;
	// Ключ локализации error_.
	QString errorKey_;
	// Реестр устойчивых UUID и отображаемых атрибутов профилей OBS.
	QMap<QString, ProfileRecord> profiles_;
	// Выбор Stream по каждой паре origin/UUID профиля, без серверных секретов.
	QMap<QString, QMap<QString, QString>> bindings_;
	// UUID профиля последнего подтверждённого наблюдения.
	QString currentUuid_;
	// changing_ отмечает явное переключение профиля. После неполного или позднего снимка
	// transitionUncertain_ запрещает перенос прежнего UUID на неизвестный путь до распознавания известного профиля.
	bool changing_ = false;
	bool transitionUncertain_ = false;
};

} // namespace gost
