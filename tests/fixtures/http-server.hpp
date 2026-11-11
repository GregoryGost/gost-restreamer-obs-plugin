#pragma once

#include <QHash>
#include <QHostAddress>
#include <QPointer>
#include <QTcpServer>
#include <QTcpSocket>
#include <QUrl>
#include <functional>

// Минимальный управляемый HTTP-сервер для GET без тела, работающий в цикле событий Qt теста.
// Слушает свободный локальный порт; накапливает заголовки до CRLFCRLF и сохраняет исходный target
// для проверки кодирования пути/query. Не является универсальным HTTP-парсером или сервером продукта.
// Сокеты принадлежат фикстуре, Request заимствует их через QPointer. handler вызывается синхронно
// при полном заголовке и может удержать копию Request для управляемого позднего ответа/отмены.
class HttpServer final : public QObject {
public:
	struct Request {
		QByteArray method;
		QByteArray target;
		QByteArray headers;
		QPointer<QTcpSocket> socket;
	};

	explicit HttpServer(QObject *parent = nullptr) : QObject(parent)
	{
		connect(&server_, &QTcpServer::newConnection, this, [this] {
			while (auto *socket = server_.nextPendingConnection()) {
				socket->setParent(this);
				connect(socket, &QTcpSocket::disconnected, socket, &QObject::deleteLater);
				connect(socket, &QObject::destroyed, this, [this, socket] { buffers_.remove(socket); });
				connect(socket, &QTcpSocket::readyRead, this, [this, socket] {
					auto &buffer = buffers_[socket];
					buffer += socket->readAll();
					const auto end = buffer.indexOf("\r\n\r\n");
					if (end < 0)
						return;
					const auto firstLine = buffer.first(buffer.indexOf("\r\n")).split(' ');
					if (firstLine.size() < 2)
						return;
					Request request{firstLine[0], firstLine[1], buffer.first(end), socket};
					buffers_.remove(socket);
					requests.append(request);
					if (handler)
						handler(request);
				});
			}
		});
	}

	// Порт 0 исключает конфликт между тестами. origin предназначен для сценариев на петлевом IPv4-интерфейсе фикстуры.
	bool listen(const QHostAddress &address = QHostAddress::LocalHost) { return server_.listen(address, 0); }
	QUrl origin() const { return QUrl(QStringLiteral("http://127.0.0.1:%1").arg(server_.serverPort())); }

	// Полный ответ с точным Content-Length и закрытием соединения. После удаления сокета ответ не отправляется,
	// что позволяет проверять запоздалый ответ сервера после явной отмены/уничтожения получателя.
	static void reply(const Request &request, QByteArray body, int status = 200,
			  QByteArray contentType = "application/json")
	{
		if (!request.socket)
			return;
		request.socket->write("HTTP/1.1 " + QByteArray::number(status) + " Response\r\nContent-Type: " +
				      contentType + "\r\nContent-Length: " + QByteArray::number(body.size()) +
				      "\r\nConnection: close\r\n\r\n" + body);
		request.socket->disconnectFromHost();
	}

	// Открытый ответ без Content-Length: тест самостоятельно пишет фрагменты/heartbeat и задаёт EOF.
	// Это позволяет отделить подтверждение заголовков от доставки прикладного события.
	static void stream(const Request &request, QByteArray body = {}, int status = 200,
			   QByteArray contentType = "text/event-stream")
	{
		// Соединение остаётся открытым, позволяя отдельно отправлять заголовки, heartbeat и фрагменты кадров.
		if (request.socket)
			request.socket->write("HTTP/1.1 " + QByteArray::number(status) + " Response\r\nContent-Type: " +
					      contentType + "\r\nConnection: close\r\n\r\n" + body);
	}

	// Запись в requests подтверждает, что запрос дошёл до сервера. Объекты, захваченные в handler
	// по ссылке, должны существовать, пока возможна обработка запросов. Их время жизни обеспечивает тест;
	// сама фикстура этими объектами не владеет и не задаёт порядок их уничтожения.
	QList<Request> requests;
	std::function<void(const Request &)> handler;

private:
	QTcpServer server_;
	QHash<QTcpSocket *, QByteArray> buffers_;
};
