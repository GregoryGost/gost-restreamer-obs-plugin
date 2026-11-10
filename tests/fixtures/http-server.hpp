#pragma once

#include <QHash>
#include <QHostAddress>
#include <QPointer>
#include <QTcpServer>
#include <QTcpSocket>
#include <QUrl>
#include <functional>

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

	bool listen(const QHostAddress &address = QHostAddress::LocalHost) { return server_.listen(address, 0); }
	QUrl origin() const { return QUrl(QStringLiteral("http://127.0.0.1:%1").arg(server_.serverPort())); }

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

	static void stream(const Request &request, QByteArray body = {}, int status = 200,
			   QByteArray contentType = "text/event-stream")
	{
		if (request.socket)
			request.socket->write("HTTP/1.1 " + QByteArray::number(status) + " Response\r\nContent-Type: " +
					      contentType + "\r\nConnection: close\r\n\r\n" + body);
	}

	QList<Request> requests;
	std::function<void(const Request &)> handler;

private:
	QTcpServer server_;
	QHash<QTcpSocket *, QByteArray> buffers_;
};
