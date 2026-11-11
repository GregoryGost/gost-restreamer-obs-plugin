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
 * Изменения сначала действуют в RAM, затем сохраняются атомарной заменой JSON формата v2.
 * Ошибка записи оставляет новые рабочие значения и unsaved=true, прежний файл сохраняется.
 * Неподдерживаемый/повреждённый вход блокирует запись, чтобы дальнейший метод изменения не уничтожил его.
 * changed сообщает как изменения данных, так и результат сохранения/ошибку; это не подтверждение записи.
 */
class SettingsStore final : public QObject {
	Q_OBJECT

public:
	static constexpr int UnlimitedSseRejectionRetries = -1;
	static constexpr int DefaultSseRejectionRetries = 3;

	explicit SettingsStore(QString filePath, QObject *parent = nullptr);
	// Принимает HTTP(S) адрес без userinfo/query/fragment; путь может быть только пустым или равным "/".
	// В результате схема и хост приведены к нижнему регистру, корневой путь и стандартный порт удалены.
	// Пробелы по краям отвергаются; псевдонимы DNS и IP не объединяются в один сервер.
	static std::optional<QString> canonicalOrigin(const QString &text);
	// Отсутствующий файл допустим. Существующий документ проверяется целиком до замены RAM;
	// формат v1 читает прежние поля с тремя повторами SSE и не переписывается при одном чтении.
	bool load();
	// Записывает текущую RAM в формат v2 через QSaveFile без прямой перезаписи. Возвращает факт commit;
	// ошибка загрузки/записи доступна в error/errorKey. Может создать родительский каталог filePath.
	bool save();
	const QString &serverOrigin() const noexcept;
	bool autoConnect() const noexcept;
	bool reconnect() const noexcept;
	int sseRejectionRetries() const noexcept;
	bool unsaved() const noexcept;
	const QString &error() const noexcept;
	const QString &errorKey() const noexcept;
	const QString &profileUuid() const noexcept;
	const QMap<QString, ProfileRecord> &profiles() const noexcept;
	// Ищет точную пару без дополнительной нормализации; пустая строка означает отсутствие привязки.
	QString selectedStream(const QString &origin, const QString &profileUuid) const;
	// bool у методов изменения означает принятие допустимого значения в RAM, а не успешный save().
	// Повтор прежнего значения не сохраняет файл; состояние записи проверяется через unsaved/error.
	bool setServerOrigin(const QString &origin);
	void setAutoConnect(bool enabled);
	void setReconnect(bool enabled);
	void setSseRejectionRetries(int retries);
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
	void changed();

private:
	void persist();
	// Удаляет профили, чьих имён нет в достоверном перечне inventory, вместе с их привязками; текущий UUID сохраняет.
	void retireAbsent(const QStringList &inventory);
	QString filePath_;
	QString origin_;
	bool autoConnect_ = true;
	bool reconnect_ = true;
	int sseRejectionRetries_ = DefaultSseRejectionRetries;
	bool unsaved_ = false;
	bool writeAllowed_ = true;
	QString error_;
	QString errorKey_;
	QMap<QString, ProfileRecord> profiles_;
	QMap<QString, QMap<QString, QString>> bindings_;
	QString currentUuid_;
	// changing_ отмечает явное переключение профиля. После неполного или позднего снимка
	// transitionUncertain_ запрещает перенос прежнего UUID на неизвестный путь до распознавания известного профиля.
	bool changing_ = false;
	bool transitionUncertain_ = false;
};

} // namespace gost
