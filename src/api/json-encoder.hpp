#pragma once
#include "api/configuration-requests.hpp"
#include "api/json-value.hpp"

namespace gost::api {
// Проверяет документированный рецепт и кодирует полный профиль, сохраняя точные числа и явные настройки.
Result<JsonValue> encodeProfile(const GroupProfile &profile);
// Кодирует обязательные поля создания Stream; неверные параметры дают локальную Contract-ошибку.
Result<JsonValue> encodeRequest(const StreamCreate &request);
// Кодирует только явно изменённое имя Stream и точную десятичную ревизию.
Result<JsonValue> encodeRequest(const StreamPatch &request);
// Кодирует создание назначения без преобразования endpoint или streamKey.
Result<JsonValue> encodeRequest(const DestinationCreate &request);
// Кодирует только изменённые поля назначения, сохраняя false и пустые строки.
Result<JsonValue> encodeRequest(const DestinationPatch &request);
// Кодирует создание связи Group с Stream и Profile.
Result<JsonValue> encodeRequest(const GroupCreate &request);
// Кодирует изменённые связи/имя Group и точную ревизию.
Result<JsonValue> encodeRequest(const GroupPatch &request);
// Кодирует новый именованный профиль после проверки полного рецепта.
Result<JsonValue> encodeRequest(const ProfileCreate &request);
// Кодирует имя и/или полную замену профиля; отсутствие profile сохраняется.
Result<JsonValue> encodeRequest(const ProfilePatch &request);
} // namespace gost::api
