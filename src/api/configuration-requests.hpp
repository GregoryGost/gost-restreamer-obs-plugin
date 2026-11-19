#pragma once
#include "api/models.hpp"

namespace gost::api {
// Успешный DELETE не содержит JSON-документа.
struct Empty {};
// Имя нового Asset; загрузка оригинала выполняется отдельным намерением.
struct AssetCreate {
	// Отображаемое имя без нормализации.
	QString name;
};
// Полная замена политики Stream с контролем ревизии самой записи Stream.
struct PolicyPut {
	// Положительная ревизия Stream, полученная из подтверждённой серверной базы.
	quint64 expectedRevision = 0;
	// Отсутствие означает явную очистку fallback:null; значение содержит все четыре интервала watchdog.
	std::optional<Policy> fallback;
};
// Поля нового потока; ключ задаётся только при создании.
struct StreamCreate {
	// Отображаемое имя потока.
	QString name;
	// Точный ключ публикации без нормализации.
	QString publicationKey;
};
// Изменение потока не предоставляет доступа к ключу и fallback-политике.
struct StreamPatch {
	// Ревизия исходного серверного документа.
	quint64 expectedRevision = 0;
	// Отсутствующее значение сохраняет прежнее имя.
	std::optional<QString> name;
};
// Полная конфигурация нового назначения.
struct DestinationCreate {
	// Поток, которому принадлежит назначение.
	QString streamId;
	// Группа профиля в выбранном потоке.
	QString groupId;
	// Отображаемое имя назначения.
	QString name;
	// Серверное обозначение площадки.
	QString platform;
	// Точная строка адреса, которая может содержать секреты.
	QString endpoint;
	// Точный ключ; пустая строка допустима.
	QString streamKey;
	// Участие назначения в последующих запусках.
	bool enabled = true;
};
// Каждое optional-поле означает явное намерение заменить значение, включая false и пустую строку.
struct DestinationPatch {
	// Ревизия исходного серверного документа.
	quint64 expectedRevision = 0;
	// Новая принадлежность потоку либо отсутствие изменения.
	std::optional<QString> streamId;
	// Новая группа либо отсутствие изменения.
	std::optional<QString> groupId;
	// Новое имя либо отсутствие изменения.
	std::optional<QString> name;
	// Новая площадка либо отсутствие изменения.
	std::optional<QString> platform;
	// Явно введённый точный адрес; маска не является значением.
	std::optional<QString> endpoint;
	// Явно введённый ключ; пустая строка очищает прежний ключ.
	std::optional<QString> streamKey;
	// Явная смена участия в будущих запусках.
	std::optional<bool> enabled;
};
// Связь нового Group с потоком и именованным профилем.
struct GroupCreate {
	// Поток-владелец группы.
	QString streamId;
	// Выбранный профиль кодирования.
	QString profileId;
	// Отображаемое имя группы.
	QString name;
};
// Частичное изменение связи группы с контролем ревизии.
struct GroupPatch {
	// Ревизия исходного серверного документа.
	quint64 expectedRevision = 0;
	// Новый поток либо отсутствие изменения.
	std::optional<QString> streamId;
	// Новый профиль либо отсутствие изменения.
	std::optional<QString> profileId;
	// Новое имя либо отсутствие изменения.
	std::optional<QString> name;
};
// Полная конфигурация нового именованного профиля.
struct ProfileCreate {
	// Отображаемое имя профиля.
	QString name;
	// Полный рецепт; новый редактор явно выбирает liveAudioMode=copy.
	GroupProfile profile;
};
// Частичное изменение имени или полная замена рецепта.
struct ProfilePatch {
	// Ревизия исходного серверного документа.
	quint64 expectedRevision = 0;
	// Новое имя; отсутствие сохраняет прежнее.
	std::optional<QString> name;
	// Полный рецепт; отсутствие не отправляет поле profile.
	std::optional<GroupProfile> profile;
};
} // namespace gost::api
