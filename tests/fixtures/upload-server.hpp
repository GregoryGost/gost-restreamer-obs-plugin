#pragma once

#include <QHostAddress>
#include <QPointer>
#include <QTcpServer>
#include <QTcpSocket>
#include <QUrl>
#include <functional>
#include <memory>

// Принимает заголовки до полного тела и считает переданные байты без накопления файла.
// Каждый сокет ограничен буфером 16 KiB; тест может остановить чтение для отмены посреди upload.
class UploadServer final : public QObject {
public:
	// Содержит наблюдаемую часть одного HTTP-запроса и заимствованный сокет.
	struct Request {
		// Первая строка и заголовки; размер ограничен 64 KiB.
		QByteArray headers;
		// Ещё не разобранный префикс заголовков; после разбора очищается.
		QByteArray prefix;
		// Точный Content-Length, объявленный клиентом.
		qint64 length = -1;
		// Число реально прочитанных байтов тела, без сохранения их содержимого.
		qint64 received = 0;
		// Заголовки полностью разобраны, поэтому тело уже можно адресно остановить.
		bool ready = false;
		// Полное тело уже передано onComplete; повторный readyRead не повторяет принятие.
		bool completed = false;
		// Закрытие наблюдалось независимо от результата клиентского callback.
		bool disconnected = false;
		// Разрешает следующие порции тела; false оставляет TCP backpressure.
		bool drain = true;
		// Живой сокет принадлежит фикстуре; QPointer защищает запоздалые проверки.
		QPointer<QTcpSocket> socket;
	};
	// Создаёт локальный слушатель; callbacks читают сокеты только в Qt event loop теста.
	explicit UploadServer(QObject *parent = nullptr) : QObject(parent)
	{
		connect(&server_, &QTcpServer::newConnection, this, [this] {
			// socket — очередное соединение, принадлежащее только этой фикстуре.
			while (auto *socket = server_.nextPendingConnection()) {
				socket->setParent(this);
				socket->setReadBufferSize(16384);
				// request хранит счётчики независимо от удаления сокета.
				auto request = std::make_shared<Request>();
				request->socket = socket;
				requests.append(request);
				connect(socket, &QTcpSocket::readyRead, this, [this, request] { read(request); });
				connect(socket, &QTcpSocket::disconnected, this, [request, socket] {
					request->disconnected = true;
					socket->deleteLater();
				});
			}
		});
	}
	// Освобождает только свои сокеты до уничтожения захваченных запросов.
	~UploadServer() override
	{
		server_.close();
		// request адресует собственное соединение, а не общий сервис.
		for (const auto &request : requests)
			if (request->socket) {
				request->socket->disconnect(this);
				request->socket->abort();
				delete request->socket.data();
			}
	}
	// Выбирает свободный порт IPv4 loopback без доступа к настоящему backend.
	bool listen() { return server_.listen(QHostAddress::LocalHost, 0); }
	// Возвращает origin принадлежащего тесту слушателя.
	QUrl origin() const { return QUrl(QStringLiteral("http://127.0.0.1:%1").arg(server_.serverPort())); }
	// Продолжает чтение ранее остановленного тела request без дополнительных соединений.
	void resume(const std::shared_ptr<Request> &request)
	{
		request->drain = true;
		read(request);
	}
	// Отправляет ограниченный ответ request и закрывает соединение штатно.
	static void reply(const std::shared_ptr<Request> &request, const QByteArray &body, int status = 200,
			  const QByteArray &extra = {})
	{
		if (!request->socket)
			return;
		request->socket->write("HTTP/1.1 " + QByteArray::number(status) +
				       " Response\r\nContent-Type: application/json\r\nContent-Length: " +
				       QByteArray::number(body.size()) + "\r\nConnection: close\r\n" + extra + "\r\n" +
				       body);
		request->socket->disconnectFromHost();
	}
	// Запросы сохраняют только заголовки и счётчики даже для многомегабайтного файла.
	QList<std::shared_ptr<Request>> requests;
	// Вызывается один раз при заголовках; может установить drain=false или ответить досрочно.
	std::function<void(const std::shared_ptr<Request> &)> onHeaders;
	// Вызывается при достижении Content-Length; пригоден для управляемого принятия 202.
	std::function<void(const std::shared_ptr<Request> &)> onComplete;

private:
	// Считывает request ограниченными порциями; лишние байты и неверная длина закрывают сокет.
	void read(const std::shared_ptr<Request> &request)
	{
		if (!request->socket)
			return;
		if (!request->ready) {
			request->prefix += request->socket->read(16384);
			// end отделяет ограниченные заголовки от уже полученной части тела.
			const auto end = request->prefix.indexOf("\r\n\r\n");
			if (end > 65536) {
				request->socket->abort();
				return;
			}
			if (end < 0) {
				if (request->prefix.size() > 65536)
					request->socket->abort();
				return;
			}
			request->headers = request->prefix.first(end);
			// line — один заголовок исходного запроса; имена нечувствительны к регистру.
			for (const auto &line : request->headers.split('\n'))
				if (line.trimmed().toLower().startsWith("content-length:"))
					request->length = line.mid(line.indexOf(':') + 1).trimmed().toLongLong();
			if (request->length < 0 || request->length > 1024LL * 1024 * 1024) {
				request->socket->abort();
				return;
			}
			request->received = request->prefix.size() - end - 4;
			request->prefix.clear();
			request->ready = true;
			if (onHeaders)
				onHeaders(request);
		}
		while (request->socket && request->drain && request->socket->bytesAvailable() > 0) {
			// bytes существует только одну итерацию; полный upload в памяти не сохраняется.
			const auto bytes = request->socket->read(16384);
			request->received += bytes.size();
		}
		if (request->received > request->length) {
			request->socket->abort();
			return;
		}
		if (request->received == request->length && !request->completed) {
			request->completed = true;
			if (onComplete)
				onComplete(request);
		}
	}
	// Слушатель принадлежит этому объекту и не управляет чужими портами.
	QTcpServer server_;
};
