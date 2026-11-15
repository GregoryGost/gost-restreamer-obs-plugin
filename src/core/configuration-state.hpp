#pragma once

#include "api/models.hpp"
#include <QSet>
#include <variant>

namespace gost {

// Вид текущей конфигурационной карточки; не меняет рабочий Stream.
enum class ConfigurationKind { Stream, Destination, Group, Profile };
// Терминальный результат записи отделён от текущего содержимого серверного кеша.
enum class ConfigurationOutcome { None, Saved, Deleted, Rejected, Conflict, Unknown };
// Полная конфигурационная запись сохраняет исходные байтовые и отсутствующие поля DTO.
using ConfigurationEntity = std::variant<api::Stream, api::Destination, api::Group, api::Profile>;

// Адрес конфигурации допускает отсутствие рабочего Stream для создания первого потока.
struct ConfigurationContext {
	// Канонический адрес сервера.
	QString origin;
	// Подтверждённая устойчивая идентичность профиля OBS.
	QString profileUuid;
	// Рабочий выбор участвует в поколении, но не обязателен для CRUD.
	QString workingStreamId;
	// Возвращает наличие обязательных частей адреса конфигурации.
	bool valid() const noexcept { return !origin.isEmpty() && !profileUuid.isEmpty(); }
	// Сравнивает все части логического выбора.
	bool operator==(const ConfigurationContext &) const = default;
};

// Содержит причину ошибки поля для обычного текста интерфейса, но не для журнала.
struct ConfigurationFieldIssue {
	// Ключ перевода, установленный только доверенным локальным валидатором.
	QString messageKey;
	// Структурированное пояснение сервера без сырого тела ответа и отдельного value.
	QString detail;
};

// Содержит одну RAM-форму: серверную базу, пользовательский ввод и результат записи.
struct ConfigurationEditorState {
	// Контекст запрещает отправить старый ввод в другой сервер или профиль.
	ConfigurationContext context;
	// Вид сущности определяет поля формы и тип альтернативы draft.
	ConfigurationKind kind = ConfigurationKind::Stream;
	// ID открытой серверной записи; пуст до открытия карточки и в форме создания.
	QString id;
	// Исходный подтверждённый DTO и его точная revision остаются неизменны при конфликте.
	std::optional<ConfigurationEntity> base;
	// Полный ввод пользователя; dirtyFields определяет частичный PATCH.
	std::optional<ConfigurationEntity> draft;
	// Имена изменённых полей соответствуют JSON API, включая единое поле profile.
	QSet<QString> dirtyFields;
	// Новая серверная версия для явного сравнения после 409.
	std::optional<ConfigurationEntity> serverConflict;
	// Безопасные пояснения ошибок по известным полям; значения запроса не хранятся здесь.
	QMap<QString, ConfigurationFieldIssue> fieldIssues;
	// Явные изменения участия собственных назначений Stream; успешные записи удаляются из этого набора.
	QMap<QString, bool> destinationChanges;
	// Исходная ревизия каждого изменённого назначения; изменяется только после адресной сверки.
	QMap<QString, quint64> destinationRevisions;
	// Безопасные имена сохраняются для результата даже при удалении назначения из последующего списка.
	QMap<QString, QString> destinationNames;
	// Отдельный итог для каждого отправленного назначения сохраняет видимость частичного результата.
	QMap<QString, ConfigurationOutcome> destinationOutcomes;
	// Неоднозначные ответы и конфликты требуют адресного GET перед новым явным сохранением.
	QSet<QString> destinationReconciliation;
	// Итог выбора назначений выводится отдельно от результата сохранения имени самого Stream.
	QString destinationMessageKey;
	// Форма создания не требует серверной базы.
	bool creating = false;
	// Выполняется адресное GET текущей карточки.
	bool loading = false;
	// Уже принято локальное намерение записи, которое переживает скрытие окна.
	bool writing = false;
	// После Unknown требуется явное перечитывание перед новым намерением.
	bool reconciliationRequired = false;
	// Результат предыдущей записи не выводится из одного лишь последующего GET.
	ConfigurationOutcome outcome = ConfigurationOutcome::None;
	// Ключ локализованного безопасного сообщения, без серверных значений и секретов.
	QString messageKey;
};

} // namespace gost
