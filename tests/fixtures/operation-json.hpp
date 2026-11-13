#pragma once
#include "fixtures/api-json.hpp"

namespace fixture {
// Создаёт серверную квитанцию независимо от клиента: requestId связывает POST, kind/targetId задают адресата,
// sessionId/outputId сохраняют родителя и выход; id/status допускают отрицательные сценарии чтения квитанции.
inline QJsonObject receipt(const QString &requestId, const QString &kind, const QString &targetId,
			   const QString &sessionId = QStringLiteral("session"), const QString &outputId = {},
			   const QString &id = QStringLiteral("command"),
			   const QString &status = QStringLiteral("ISSUED"))
{
	// result содержит обязательные поля Record и точные связанные идентификаторы backend.
	auto result = record(id);
	result.insert(QStringLiteral("request_id"), requestId);
	result.insert(QStringLiteral("kind"), kind);
	result.insert(QStringLiteral("target_id"), targetId);
	result.insert(QStringLiteral("session_id"), sessionId);
	result.insert(QStringLiteral("output_id"), outputId);
	result.insert(QStringLiteral("status"), status);
	result.insert(QStringLiteral("failure"), QJsonValue::Null);
	return result;
}
} // namespace fixture
