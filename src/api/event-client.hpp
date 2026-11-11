#pragma once
#include "api/models.hpp"
#include <QObject>
#include <QUrl>
#include <memory>
namespace gost::api {
/**
 * Ведёт одну подписку SSE от установленного оперативного снимка или подтверждённого курсора.
 * Владеет отдельным сетевым менеджером, ответом, парсером и таймерами. Вызовы и обработчики
 * выполняются в потоке клиента; receiver заимствован и служит только защитой времени жизни.
 * Нужен работающий цикл Qt; автоматического переноса вызовов в поток получателя нет.
 *
 * Подписка имеет две стадии: ожидание допустимых заголовков и чтение кадров. OpenHandler
 * подтверждает транспорт, EventHandler отдельно подтверждает применение каждого наблюдения.
 * После закрытия подтверждённый курсор доступен вызывающей стороне для восстановления.
 * Выбор нового снимка, пауз и числа повторов принадлежит ConnectionController.
 */
class EventClient : public QObject {
public:
	// true разрешает подтвердить cursor события; false означает недостоверное продолжение и требует повторной синхронизации.
	// Ссылка на RuntimeEvent действительна только внутри вызова. Обработчики могут закрыть/заменить подписку.
	using EventHandler = std::function<bool(RequestContext, const RuntimeEvent &)>;
	// Последний bool означает необходимость нового снимка. false передаёт классификацию отказа контроллеру.
	using ErrorHandler = std::function<void(RequestContext, Error, bool)>;
	// Вызывается один раз после 200 text/event-stream, даже если сервер ещё не отправил ни одного события.
	using OpenHandler = std::function<void(RequestContext)>;
	explicit EventClient(Limits limits = {}, QObject *parent = nullptr);
	~EventClient() override;
	/**
	 * Закрывает прежнюю подписку и открывает /api/v1/events с непрозрачным параметром cursor.
	 * origin должен быть допустимым HTTP(S) адресом сервера, cursor — непустым без NUL. Начальный cursor
	 * считается точкой восстановления снимка, но сам по себе не подтверждает получение заголовков.
	 * Ошибка аргументов может вызвать errorHandler непосредственно до возврата subscribe().
	 *
	 * Заголовки ограничены sseHandshakeMs, отсутствие любых байтов — sseIdleMs. Общего срока жизни
	 * у подписки нет; heartbeat продлевает простой. HTTP-статусы, включая 204/400/409, передаются
	 * контроллеру без самостоятельного повтора. После shutdown() вызов игнорируется.
	 */
	void subscribe(QUrl origin, QString cursor, RequestContext context, QObject *receiver,
		       EventHandler eventHandler, ErrorHandler errorHandler, OpenHandler openHandler = {});
	// Прерывает ответ, очищает кадры/обработчики и запрещает старые продолжения. Ошибку не сообщает.
	// Курсор сохраняется; следующая subscribe() может открыть новую подписку.
	void close();
	// Идемпотентно и необратимо закрывает клиент; новые подписки больше не создаются.
	void shutdown();
	// Возвращает начальный курсор подписки либо курсор последнего успешно применённого события.
	QString confirmedCursor() const;

private:
	class Impl;
	std::unique_ptr<Impl> impl_;
};
} // namespace gost::api
