#pragma once

#include "core/fallback-controller.hpp"
#include "fixtures/configuration-rig.hpp"
#include <QUrlQuery>

namespace fallbackFixture {
// Создаёт корректный Asset с независимыми current/pending, не приписывая готовность Session.
inline QJsonObject asset(QString current = QStringLiteral("old"), QString pending = {})
{
	// value содержит только серверную копию метаданных выбранного Asset.
	auto value = fixture::asset();
	value.insert(QStringLiteral("name"), QStringLiteral("Заставка"));
	value.insert(QStringLiteral("kind"), QStringLiteral("image"));
	value.insert(QStringLiteral("state"),
		     pending.isEmpty() ? QStringLiteral("READY") : QStringLiteral("PREPARING"));
	value.insert(QStringLiteral("current_generation_id"), current);
	value.insert(QStringLiteral("pending_generation_id"), pending);
	return value;
}
// Возвращает точное поколение id/status, используя строчные значения контракта GenerationDTO.
inline QJsonObject generation(const QString &id, const QString &status)
{
	// value задаёт независимый документ, чей родитель всегда asset.
	auto value = fixture::record(id);
	value.insert(QStringLiteral("asset_id"), QStringLiteral("asset"));
	value.insert(QStringLiteral("kind"), QStringLiteral("image"));
	value.insert(QStringLiteral("status"), status);
	value.insert(QStringLiteral("size_bytes"), 1024);
	value.insert(QStringLiteral("failure"), QJsonValue::Null);
	return value;
}
// Создаёт вариант точной Generation; profile/format происходят из проверенного независимого набора данных.
inline QJsonObject variant(const QString &generationId, const QString &id = QStringLiteral("variant"))
{
	// value сохраняет независимый desired-рецепт и измеренный формат.
	auto value = fixture::variant();
	value.insert(QStringLiteral("id"), id);
	value.insert(QStringLiteral("generation_id"), generationId);
	return value;
}
// Полная политика содержит четыре исходных интервала; revision относится к Stream.
inline QJsonObject policy(const QString &assetId = QStringLiteral("asset"), qint64 revision = 1)
{
	return {{QStringLiteral("revision"), revision},
		{QStringLiteral("fallback"),
		 assetId.isEmpty()
			 ? QJsonValue(QJsonValue::Null)
			 : QJsonValue(QJsonObject{
				   {QStringLiteral("asset_id"), assetId},
				   {QStringLiteral("watchdog"),
				    QJsonObject{{QStringLiteral("video_timeout_ns"), qint64(3000000000LL)},
						{QStringLiteral("audio_timeout_ns"), qint64(3000000000LL)},
						{QStringLiteral("recovery_window_ns"), qint64(1000000000LL)},
						{QStringLiteral("keyframe_timeout_ns"), qint64(5000000000LL)}}}})}};
}
} // namespace fallbackFixture

// Соединяет настоящие configuration/fallback/runtime контроллеры с одним loopback сервером.
// Серверные документы не ссылаются на RAM-модель; intercept позволяет удерживать отдельный HTTP-шаг.
class FallbackRig final {
public:
	// Создаёт заимствования в порядке runtime; limits задаёт polling, apiLimits — пределы общего конфигурационного API.
	explicit FallbackRig(gost::FallbackController::Limits limits = {1000, 5}, gost::api::Limits apiLimits = {})
		: rig(apiLimits),
		  fallback(rig.state, rig.configApi, rig.configuration, rig.operations, limits)
	{
		rig.intercept = [this](const auto &request) {
			if (intercept && intercept(request))
				return true;
			return respond(request);
		};
	}
	// Закрывает fallback до единственного владельца configurationApi.
	~FallbackRig() { fallback.shutdown(); }
	// Подтверждает только принадлежащий тесту origin и рабочий Stream.
	bool initialize() { return rig.initialize(); }
	// Обслуживает только fallback-маршруты; остальные остаются у ConfigurationRig.
	bool respond(const HttpServer::Request &request)
	{
		// url/path/query адресуют независимые серверные сущности и точный generation_id.
		const auto url = QUrl::fromEncoded("http://local" + request.target);
		const auto path = url.path();
		const QUrlQuery query(url);
		if (path.endsWith(QStringLiteral("/fallback-policy"))) {
			if (request.method == "PUT") {
				// body — полное намерение, реально полученное через HTTP.
				const auto body = fixture::object(request.body);
				policy.insert(QStringLiteral("fallback"), body.value(QStringLiteral("fallback")));
				policy.insert(QStringLiteral("revision"),
					      policy.value(QStringLiteral("revision")).toInteger() + 1);
				rig.stream.insert(QStringLiteral("fallback"), policy.value(QStringLiteral("fallback")));
				rig.stream.insert(QStringLiteral("revision"), policy.value(QStringLiteral("revision")));
			}
			HttpServer::reply(request, fixture::bytes(policy));
			return true;
		}
		if (!path.startsWith(QStringLiteral("/api/v1/fallback-assets")))
			return false;
		if (request.method == "DELETE") {
			HttpServer::reply(request, {}, 204, {});
			return true;
		}
		if (path.endsWith(QStringLiteral("/rebuild")) || path.endsWith(QStringLiteral("/content"))) {
			HttpServer::reply(request, R"({"asset_id":"asset","generation_id":"new"})", 202,
					  "application/json",
					  "Location: /api/v1/fallback-assets/asset/generations/new\r\n");
			return true;
		}
		if (request.method == "POST") {
			HttpServer::reply(request, fixture::bytes(asset), 201);
			return true;
		}
		if (path == QStringLiteral("/api/v1/fallback-assets")) {
			HttpServer::reply(request, fixture::page({asset}));
			return true;
		}
		if (path.endsWith(QStringLiteral("/variants"))) {
			HttpServer::reply(request, fixture::page({fallbackFixture::variant(
							   query.queryItemValue(QStringLiteral("generation_id")))}));
			return true;
		}
		if (path.contains(QStringLiteral("/generations/"))) {
			// id извлекается из адреса GET, не из выбранного current/pending в StateStore.
			const auto id = path.section(QLatin1Char('/'), -1);
			HttpServer::reply(request,
					  fixture::bytes(generations.value(
						  id, fallbackFixture::generation(id, QStringLiteral("ready")))));
			return true;
		}
		HttpServer::reply(request, fixture::bytes(asset));
		return true;
	}
	// Общие каналы, двойник OBS и файлы находятся под единственным владельцем rig.
	ConfigurationRig rig;
	// Проверяемый координатор заимствует существующий configurationApi.
	gost::FallbackController fallback;
	// Независимая текущая серверная запись Asset.
	QJsonObject asset = fallbackFixture::asset();
	// Текущая серверная политика; её изменения не подменяют форму клиента.
	QJsonObject policy = fallbackFixture::policy();
	// Набор точных серверных Generation по ID; отсутствующие записи по умолчанию ready.
	QMap<QString, QJsonObject> generations;
	// Перехватчик принимает ответственность за ответ, возвращая true.
	std::function<bool(const HttpServer::Request &)> intercept;
};
