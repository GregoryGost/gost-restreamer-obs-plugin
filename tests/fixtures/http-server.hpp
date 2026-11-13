#pragma once

#include <QHash>
#include <QHostAddress>
#include <QPointer>
#include <QSet>
#include <QTcpServer>
#include <QTcpSocket>
#include <QUrl>
#include <functional>

// Принимает один ограниченный HTTP-запрос на соединение в цикле событий теста.
// Полное тело Content-Length сохраняется до handler; фикстура не поддерживает chunked/pipelining.
class HttpServer final : public QObject {
public:
	// Содержит исходные байты запроса и заимствованный сокет для управляемого ответа.
	struct Request {
		// Метод запроса без нормализации.
		QByteArray method;
		// Закодированный путь с query для проверки непрозрачных идентификаторов.
		QByteArray target;
		// Первая строка и заголовки без завершающего CRLFCRLF.
		QByteArray headers;
		// Заимствованный сокет становится null после закрытия и удаления соединения.
		QPointer<QTcpSocket> socket;
		// Полное тело, ограниченное одним MiB; GET без Content-Length имеет пустое тело.
		QByteArray body;
	};

	// Создаёт сервер с владельцем parent; callbacks принимают сокеты и читают их только в потоке Qt.
	explicit HttpServer(QObject *parent = nullptr) : QObject(parent)
	{
		connect(&server_, &QTcpServer::newConnection, this, [this] {
			// socket — очередное соединение с автоматически выбранным локальным портом.
			while (auto *socket = server_.nextPendingConnection()) {
				socket->setParent(this);
				connect(socket, &QTcpSocket::disconnected, socket, &QObject::deleteLater);
				// Удаление сокета освобождает также накопленные байты и отметку обработки.
				connect(socket, &QObject::destroyed, this, [this, socket] {
					buffers_.remove(socket);
					handled_.remove(socket);
				});
				// readyRead накапливает фрагменты; handler получает ровно один полный запрос.
				connect(socket, &QTcpSocket::readyRead, this, [this, socket] { readRequest(socket); });
			}
		});
	}

	// Слушает address на свободном порту, исключая конфликт параллельных процессов.
	bool listen(const QHostAddress &address = QHostAddress::LocalHost) { return server_.listen(address, 0); }
	// Возвращает origin петлевого IPv4-интерфейса этой фикстуры.
	QUrl origin() const { return QUrl(QStringLiteral("http://127.0.0.1:%1").arg(server_.serverPort())); }

	// Отправляет body/status/contentType с дополнительными extraHeaders и закрывает живой request.socket.
	// extraHeaders позволяет проверить Location, не меняя MIME или тело квитанции.
	static void reply(const Request &request, QByteArray body, int status = 200,
			  QByteArray contentType = "application/json", QByteArray extraHeaders = {})
	{
		if (!request.socket)
			return;
		request.socket->write("HTTP/1.1 " + QByteArray::number(status) + " Response\r\nContent-Type: " +
				      contentType + "\r\nContent-Length: " + QByteArray::number(body.size()) +
				      "\r\nConnection: close\r\n" + extraHeaders + "\r\n" + body);
		request.socket->disconnectFromHost();
	}

	// Открывает ответ request с body/status/contentType без Content-Length для последующих SSE-фрагментов.
	static void stream(const Request &request, QByteArray body = {}, int status = 200,
			   QByteArray contentType = "text/event-stream")
	{
		if (request.socket)
			request.socket->write("HTTP/1.1 " + QByteArray::number(status) + " Response\r\nContent-Type: " +
					      contentType + "\r\nConnection: close\r\n\r\n" + body);
	}

	// Полные запросы в порядке приёма; запись появляется до вызова handler.
	QList<Request> requests;
	// Синхронный обработчик полного запроса; захваченные им объекты должны переживать сервер.
	std::function<void(const Request &)> handler;

private:
	// Читает socket до полного заголовка и тела; неверная длина и превышение лимита закрывают соединение.
	void readRequest(QTcpSocket *socket)
	{
		if (handled_.contains(socket))
			return;
		// buffer содержит ещё не переданный обработчику запрос этого соединения.
		auto &buffer = buffers_[socket];
		buffer += socket->readAll();
		// end указывает начало разделителя между заголовками и телом.
		const auto end = buffer.indexOf("\r\n\r\n");
		if (buffer.size() > 1024 * 1024 + 65536 || (end < 0 && buffer.size() > 65536)) {
			socket->abort();
			return;
		}
		if (end < 0)
			return;
		// firstLine сохраняет method и target; length задаёт ожидаемый размер тела в байтах.
		const auto firstLine = buffer.first(buffer.indexOf("\r\n")).split(' ');
		qsizetype length = 0;
		if (firstLine.size() < 2 || end > 65536) {
			socket->abort();
			return;
		}
		// line — очередной исходный заголовок, без влияния регистра имени на Content-Length.
		for (const auto &line : buffer.first(end).split('\n')) {
			if (line.trimmed().toLower().startsWith("content-length:")) {
				// valid отличает корректную десятичную длину от ошибочного нуля преобразования.
				bool valid = false;
				length = line.mid(line.indexOf(':') + 1).trimmed().toLongLong(&valid);
				if (!valid || length < 0 || length > 1024 * 1024) {
					socket->abort();
					return;
				}
			}
		}
		if (buffer.size() < end + 4 + length)
			return;
		// request копирует полное тело, чтобы последующее удаление buffer не меняло проверяемые байты.
		Request request{firstLine[0], firstLine[1], buffer.first(end), socket, buffer.mid(end + 4, length)};
		handled_.insert(socket);
		buffers_.remove(socket);
		requests.append(request);
		if (handler)
			handler(request);
	}

	// Локальный слушатель принадлежит фикстуре.
	QTcpServer server_;
	// Незавершённые запросы по заимствованному адресу сокета.
	QHash<QTcpSocket *, QByteArray> buffers_;
	// Сокеты, чей единственный запрос уже передан обработчику.
	QSet<QTcpSocket *> handled_;
};
