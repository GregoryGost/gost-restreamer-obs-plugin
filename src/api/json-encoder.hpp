#pragma once
#include "api/configuration-requests.hpp"
#include <QJsonObject>

namespace gost::api {
// Проверяет документированный рецепт и кодирует полный профиль, сохраняя байты и Missing/null.
Result<QJsonObject> encodeProfile(const GroupProfile &profile);
// Кодирует обязательные поля создания Stream; неверные параметры дают локальную Contract-ошибку.
Result<QJsonObject> encodeRequest(const StreamCreate &request);
// Кодирует только явно изменённое имя Stream и точную десятичную ревизию.
Result<QJsonObject> encodeRequest(const StreamPatch &request);
// Кодирует создание назначения без преобразования endpoint или streamKey.
Result<QJsonObject> encodeRequest(const DestinationCreate &request);
// Кодирует только изменённые поля назначения, сохраняя false и пустые строки.
Result<QJsonObject> encodeRequest(const DestinationPatch &request);
// Кодирует создание связи Group с Stream и Profile.
Result<QJsonObject> encodeRequest(const GroupCreate &request);
// Кодирует изменённые связи/имя Group и точную ревизию.
Result<QJsonObject> encodeRequest(const GroupPatch &request);
// Кодирует новый именованный профиль после проверки полного рецепта.
Result<QJsonObject> encodeRequest(const ProfileCreate &request);
// Кодирует имя и/или полную замену профиля; отсутствие profile сохраняется.
Result<QJsonObject> encodeRequest(const ProfilePatch &request);
} // namespace gost::api
