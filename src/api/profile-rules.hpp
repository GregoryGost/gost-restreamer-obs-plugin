#pragma once

#include "models.hpp"
#include <QStringList>

namespace gost::api {

// Проверяет полный явный desired-профиль; при отказе error содержит доверенный ключ и wire-путь поля.
bool validateProfile(const GroupProfile &profile, Error &error);
// Возвращает конечные допустимые значения fieldPath с учётом определяющих полей profile.
// Остальные ошибки черновика не мешают исправлению поля; неизвестные значения в каталог не добавляются.
QStringList profileOptions(const GroupProfile &profile, const QString &fieldPath);
// Возвращает применимость wire-поля fieldPath к выбранному рецепту profile без проверки всего черновика.
bool profileFieldApplicable(const GroupProfile &profile, const QString &fieldPath);
// Сравнивает все поля двух валидных профилей, включая присутствие качества; FPS сравнивается как дробь.
bool sameProfile(const GroupProfile &left, const GroupProfile &right);
// Сверяет измеренный actual с валидным desired по серверным правилам псевдонимов и необязательных level/tier.
bool compatibleMeasuredFormat(const Format &actual, const GroupProfile &desired);

} // namespace gost::api
