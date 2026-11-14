#pragma once

#include "api/api-client.hpp"
#include "api/event-client.hpp"
#include "api/json-decoder.hpp"
#include "core/configuration-controller.hpp"
#include "core/connection-controller.hpp"
#include "core/restream-controller.hpp"
#include "core/settings-store.hpp"
#include "core/state-store.hpp"
#include "fixtures/api-json.hpp"
#include "fixtures/fake-frontend.hpp"
#include "fixtures/http-server.hpp"
#include "fixtures/workspace-temp.hpp"
#include "obs/obs-controller.hpp"

// Предоставляет настоящим контроллерам отдельные observation/command/configuration API.
// Все HTTP-запросы замыкаются на петлевую фикстуру; нужен QApplication для FakeFrontend.
class ConfigurationRig final {
public:
	// Создаёт независимые каналы с коротким ограниченным сроком сетевых ожиданий.
	ConfigurationRig()
		: settings(temp.filePath(QStringLiteral("settings.json"))),
		  connection(state, settings, observer, events),
		  obs(frontend, state),
		  operations(state, command, connection, obs, {250, 5, 30}),
		  configuration(state, configApi, connection, operations)
	{
		configApi.setHttpTimeoutMs(120);
		// Обработчик сначала даёт intercept возможность удержать запрос или внедрить отказ.
		server.handler = [this](const HttpServer::Request &request) {
			if (!intercept || !intercept(request))
				respond(request);
		};
	}
	// Завершает асинхронные сервисы, пока фикстуры и заимствованные зависимости ещё существуют.
	~ConfigurationRig()
	{
		configuration.shutdown();
		operations.shutdown();
		connection.shutdown();
		obs.shutdown();
	}
	// Подтверждает origin/UUID; withStream=false моделирует создание первого Stream.
	bool initialize(bool withStream = true)
	{
		if (!temp.isValid() || !server.listen())
			return false;
		settings.setAutoConnect(false);
		state.setConnection(server.origin().toString(), gost::ConnectionStatus::Connected);
		// stream — независимо декодированная конфигурация рабочего выбора.
		const auto stream = gost::api::decode<gost::api::Stream>(fixture::bytes(fixture::stream()));
		if (!stream)
			return false;
		state.setStreams(withStream ? QVector<gost::api::Stream>{*stream.value} : QVector<gost::api::Stream>{},
				 true);
		state.setSelection(withStream ? QStringLiteral("stream") : QString{}, true, {}, {},
				   QStringLiteral("profile-uuid"));
		return true;
	}
	// Возвращает только записи HTTP, исключая GET наблюдения и сверки после успеха.
	QList<HttpServer::Request> mutations() const
	{
		// result аккумулирует записи в реальном порядке поступления; request адресует очередной запрос.
		QList<HttpServer::Request> result;
		for (const auto &request : server.requests)
			if (request.method != "GET")
				result.append(request);
		return result;
	}
	// Обслуживает публичные коллекции и карточки синтетическими серверными DTO.
	void respond(const HttpServer::Request &request)
	{
		// path адресует ресурс без фильтров query; value — возвращаемый типизированный документ.
		const auto path = QUrl::fromEncoded("http://local" + request.target).path();
		QJsonObject value;
		if (path.startsWith(QStringLiteral("/api/v1/streams")))
			value = stream;
		if (path.startsWith(QStringLiteral("/api/v1/destinations")))
			value = destination;
		if (path.startsWith(QStringLiteral("/api/v1/transcode-groups")))
			value = group;
		if (path.startsWith(QStringLiteral("/api/v1/transcode-profiles")))
			value = profile;
		if (request.method == "DELETE") {
			HttpServer::reply(request, {}, 204, {});
			return;
		}
		if (request.method == "POST" || request.method == "PATCH") {
			// body содержит независимое намерение, которое сервер отражает в новом DTO с ревизией.
			const auto body = QJsonDocument::fromJson(request.body).object();
			// key перебирает изменённые поля, исключая служебную expected_revision.
			for (const auto &key : body.keys())
				if (key != QStringLiteral("expected_revision"))
					value.insert(key, body.value(key));
			value.insert(QStringLiteral("revision"), QStringLiteral("2"));
			if (path.startsWith(QStringLiteral("/api/v1/streams")))
				stream = value;
			if (path.startsWith(QStringLiteral("/api/v1/destinations")))
				destination = value;
			if (path.startsWith(QStringLiteral("/api/v1/transcode-groups")))
				group = value;
			if (path.startsWith(QStringLiteral("/api/v1/transcode-profiles")))
				profile = value;
			HttpServer::reply(request, fixture::bytes(value), request.method == "POST" ? 201 : 200);
			return;
		}
		if (path.endsWith(QStringLiteral("/fallback-policy"))) {
			HttpServer::reply(request, R"({"revision":"1","fallback":null})");
			return;
		}
		if (path.endsWith(QStringLiteral("/sessions")) || path.endsWith(QStringLiteral("/outputs"))) {
			HttpServer::reply(request, fixture::page({}));
			return;
		}
		if (path.endsWith(QStringLiteral("/destinations"))) {
			HttpServer::reply(request, fixture::page({destination}));
			return;
		}
		if (path == QStringLiteral("/api/v1/streams") || path == QStringLiteral("/api/v1/destinations") ||
		    path == QStringLiteral("/api/v1/transcode-groups") ||
		    path == QStringLiteral("/api/v1/transcode-profiles")) {
			HttpServer::reply(request, fixture::page({value}));
			return;
		}
		if (!value.isEmpty()) {
			HttpServer::reply(request, fixture::bytes(value));
			return;
		}
		HttpServer::reply(request, "{}", 404);
	}
	// Все файлы принадлежат одному сценарию workspace.
	WorkspaceTemp temp{QStringLiteral("configuration")};
	// Настоящий локальный HTTP-сервер владеет своими сокетами.
	HttpServer server;
	// Локальные preferences не содержат серверных секретов.
	gost::SettingsStore settings;
	// Публичное общее состояние контроллеров и редактора.
	gost::StateStore state;
	// observer читает наблюдения; command выполняет workflow; configApi передаёт независимые CRUD-запросы.
	gost::api::APIClient observer;
	gost::api::APIClient command;
	gost::api::APIClient configApi;
	// Отдельная подписка не создаётся без явного подключения наблюдателя.
	gost::api::EventClient events;
	// Настоящий наблюдатель рабочего выбора.
	gost::ConnectionController connection;
	// Изолированная граница OBS с окном, принадлежащим тесту.
	FakeFrontend frontend;
	// Единственный контроллер штатной остановки OBS.
	gost::OBSController obs;
	// Исполнитель команд не разделяет очередь с configuration.
	gost::RestreamController operations;
	// Проверяемый координатор RAM-формы и типизированного CRUD.
	gost::ConfigurationController configuration;
	// Сценарный перехватчик возвращает true после самостоятельного обслуживания запроса.
	std::function<bool(const HttpServer::Request &)> intercept;
	// stream содержит конфигурацию потока, destination — назначение, group — связь, profile — полный профиль.
	// Эти серверные документы не связаны с draft или StateStore по ссылке.
	QJsonObject stream = fixture::stream();
	QJsonObject destination = fixture::destination();
	QJsonObject group = fixture::group();
	QJsonObject profile = fixture::profile();
};
