#include "api/event-client.hpp"
#include "api/json-decoder.hpp"
#include "api/network-origin.hpp"
#include "api/sse-parser.hpp"
#include <QNetworkAccessManager>
#include <QNetworkProxy>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QPointer>
#include <QTimer>
#include <QUrlQuery>
#include <algorithm>
namespace gost::api {
class EventClient::Impl {
public:
	EventClient *owner;
	Limits limits;
	QNetworkAccessManager network;
	SSEParser parser;
	QPointer<QNetworkReply> reply;
	QPointer<QObject> receiver;
	EventHandler eventHandler;
	ErrorHandler errorHandler;
	QMetaObject::Connection receiverConnection;
	RequestContext context;
	quint64 serial = 0;
	bool stopped = false;
	bool validated = false;
	bool finished = false;
	bool scheduled = false;
	QTimer handshake;
	QTimer idle;
	QString confirmed;
	Impl(EventClient *owner, Limits limits) : owner(owner), limits(limits), network(owner), parser(limits)
	{
		network.setProxy(QNetworkProxy(QNetworkProxy::NoProxy));
		handshake.setSingleShot(true);
		idle.setSingleShot(true);
		QObject::connect(&handshake, &QTimer::timeout, owner,
				 [this] { fail(makeError(ErrorKind::Timeout), false); });
		QObject::connect(&idle, &QTimer::timeout, owner,
				 [this] { fail(makeError(ErrorKind::Timeout), false); });
	}
	void close()
	{
		++serial;
		handshake.stop();
		idle.stop();
		QObject::disconnect(receiverConnection);
		auto *previous = reply.data();
		reply = nullptr;
		if (previous) {
			QObject::disconnect(previous, nullptr, owner, nullptr);
			previous->abort();
			previous->deleteLater();
		}
		parser.reset();
		receiver = nullptr;
		eventHandler = {};
		errorHandler = {};
		validated = false;
		finished = false;
		scheduled = false;
	}
	void fail(Error error, bool resync)
	{
		auto callback = errorHandler;
		auto target = receiver;
		const auto ctx = context;
		close();
		if (!stopped && target && callback)
			callback(ctx, std::move(error), resync);
	}
	bool headers()
	{
		if (!reply)
			return false;
		const auto attribute = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute);
		if (!attribute.isValid())
			return false;
		const int status = attribute.toInt();
		const auto media = reply->header(QNetworkRequest::ContentTypeHeader)
					   .toString()
					   .section(QLatin1Char(';'), 0, 0)
					   .trimmed()
					   .toLower();
		if (status != 200) {
			fail(makeError(ErrorKind::Http, status), status == 400 || status == 409);
			return false;
		}
		if (media != QStringLiteral("text/event-stream")) {
			fail(makeError(ErrorKind::Contract, status), true);
			return false;
		}
		validated = true;
		handshake.stop();
		idle.start(std::max(1, limits.sseIdleMs));
		return true;
	}
	void schedule()
	{
		if (scheduled)
			return;
		scheduled = true;
		const auto expected = serial;
		QTimer::singleShot(0, owner, [this, expected] {
			if (expected != serial || stopped)
				return;
			scheduled = false;
			drain();
		});
	}
	bool frame(SSEFrame frame)
	{
		if (frame.event == QStringLiteral("resync_required")) {
			const auto json = QJsonDocument::fromJson(frame.data);
			if (!json.isObject() || !json.object().value(QStringLiteral("reason")).isString() ||
			    !frame.id.isEmpty()) {
				fail(makeError(ErrorKind::Contract), true);
				return false;
			}
			fail(makeError(ErrorKind::Contract), true);
			return false;
		}
		const auto result = decode<RuntimeEvent>(frame.data);
		if (!result || frame.event != result.value->kind || frame.id.isEmpty() ||
		    frame.id != result.value->cursor || !validateRuntimeEvent(*result.value)) {
			fail(makeError(ErrorKind::Contract), true);
			return false;
		}
		const auto expected = serial;
		const auto ctx = context;
		const auto target = receiver;
		const auto handler = eventHandler;
		const QPointer<EventClient> guard = owner;
		const bool applied = target && handler && handler(ctx, *result.value);
		if (!guard)
			return false;
		if (expected != serial || stopped || !receiver)
			return false;
		if (!applied) {
			fail(makeError(ErrorKind::Contract), true);
			return false;
		}
		confirmed = result.value->cursor;
		return true;
	}
	void drain()
	{
		if (scheduled)
			return;
		if (!reply || stopped || !receiver)
			return;
		if (!validated) {
			const auto expected = serial;
			const QPointer<EventClient> guard = owner;
			if (!headers()) {
				if (guard && expected == serial && finished)
					fail(makeError(ErrorKind::Transport), false);
				return;
			}
		}
		qsizetype processedBytes = 0;
		int processedEvents = 0;
		auto deliverFrames = [&] {
			while (parser.hasFrame()) {
				if (processedEvents >= limits.eventsPerTurn ||
				    (processedEvents > 0 &&
				     parser.nextFrameBytes() > limits.bytesPerTurn - processedBytes)) {
					schedule();
					return false;
				}
				auto next = parser.takeFrame();
				processedBytes += next.bytes;
				++processedEvents;
				if (!frame(std::move(next)))
					return false;
				if (processedBytes >= limits.bytesPerTurn) {
					schedule();
					return false;
				}
			}
			return true;
		};
		if (!deliverFrames())
			return;
		if (!reply)
			return;
		const auto bytes = reply->read(std::min(limits.readBytes, limits.bytesPerTurn - processedBytes));
		if (!bytes.isEmpty()) {
			idle.start(std::max(1, limits.sseIdleMs));
			if (!parser.feed(bytes)) {
				fail(parser.error(), true);
				return;
			}
			if (!deliverFrames())
				return;
		}
		if (!reply)
			return;
		if (reply->bytesAvailable() > 0 || parser.hasFrame()) {
			schedule();
			return;
		}
		if (finished) {
			if (!parser.finish()) {
				fail(parser.error(), true);
				return;
			}
			fail(makeError(ErrorKind::Transport), false);
		}
	}
	void subscribe(QUrl origin, QString cursor, RequestContext ctx, QObject *target, EventHandler events,
		       ErrorHandler errors)
	{
		if (stopped)
			return;
		close();
		context = ctx;
		receiver = target;
		eventHandler = std::move(events);
		errorHandler = std::move(errors);
		confirmed = cursor;
		if (!receiver)
			return;
		if (!validServerOrigin(origin) || cursor.isEmpty() || cursor.contains(QChar::Null) ||
		    limits.readBytes <= 0 || limits.bytesPerTurn <= 0 || limits.eventsPerTurn <= 0) {
			fail(makeError(ErrorKind::Contract), true);
			return;
		}
		origin.setPath(QStringLiteral("/api/v1/events"));
		QUrlQuery query;
		query.addQueryItem(QStringLiteral("cursor"), QString::fromLatin1(QUrl::toPercentEncoding(cursor)));
		origin.setQuery(query);
		QNetworkRequest request(origin);
		request.setRawHeader("Accept", "text/event-stream");
		request.setAttribute(QNetworkRequest::RedirectPolicyAttribute, QNetworkRequest::ManualRedirectPolicy);
		reply = network.get(request);
		reply->setReadBufferSize(limits.readBytes);
		const auto expected = serial;
		receiverConnection = QObject::connect(target, &QObject::destroyed, owner, [this, expected] {
			if (expected == serial)
				close();
		});
		QObject::connect(reply, &QNetworkReply::metaDataChanged, owner, [this, expected] {
			if (expected == serial && !validated)
				headers();
		});
		QObject::connect(reply, &QIODevice::readyRead, owner, [this, expected] {
			if (expected == serial)
				drain();
		});
		QObject::connect(reply, &QNetworkReply::finished, owner, [this, expected] {
			if (expected == serial) {
				finished = true;
				drain();
			}
		});
		handshake.start(std::max(1, limits.sseHandshakeMs));
	}
};
EventClient::EventClient(Limits limits, QObject *parent) : QObject(parent), impl_(std::make_unique<Impl>(this, limits))
{
}
EventClient::~EventClient()
{
	shutdown();
}
void EventClient::subscribe(QUrl origin, QString cursor, RequestContext context, QObject *receiver, EventHandler events,
			    ErrorHandler errors)
{
	impl_->subscribe(std::move(origin), std::move(cursor), context, receiver, std::move(events), std::move(errors));
}
void EventClient::close()
{
	impl_->close();
}
void EventClient::shutdown()
{
	if (impl_->stopped)
		return;
	impl_->stopped = true;
	impl_->close();
}
QString EventClient::confirmedCursor() const
{
	return impl_->confirmed;
}
} // namespace gost::api
