#pragma once
#include <QUrl>
namespace gost::api {
// Предусловие построения API URL: адрес должен обозначать только HTTP(S) origin, чтобы setPath/setQuery
// не затёрли скрытую часть пользовательского адреса. Нормализация ввода и удаление стандартного порта —
// ответственность SettingsStore; эта проверка ничего не исправляет и не объединяет псевдонимы DNS с IP.
inline bool validServerOrigin(const QUrl &origin)
{
	return origin.isValid() &&
	       (origin.scheme() == QStringLiteral("http") || origin.scheme() == QStringLiteral("https")) &&
	       !origin.host().isEmpty() && origin.userInfo().isEmpty() && !origin.hasQuery() && !origin.hasFragment() &&
	       (origin.path().isEmpty() || origin.path() == QStringLiteral("/"));
}
} // namespace gost::api
