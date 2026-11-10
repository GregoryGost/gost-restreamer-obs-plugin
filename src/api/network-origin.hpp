#pragma once
#include <QUrl>
namespace gost::api {
inline bool validServerOrigin(const QUrl &origin)
{
	return origin.isValid() &&
	       (origin.scheme() == QStringLiteral("http") || origin.scheme() == QStringLiteral("https")) &&
	       !origin.host().isEmpty() && origin.userInfo().isEmpty() && !origin.hasQuery() && !origin.hasFragment() &&
	       (origin.path().isEmpty() || origin.path() == QStringLiteral("/"));
}
} // namespace gost::api
