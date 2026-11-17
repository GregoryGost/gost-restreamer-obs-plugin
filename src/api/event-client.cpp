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
// Вся работа подписки выполняется в одном цикле Qt. Обработчик события или ошибки может сразу
// открыть новую подписку с тем же контекстом; serial не даёт оставшимся обработчикам старой подписки
// изменить новую. validated отмечает проверенные заголовки, finished — конец транспорта, scheduled —
// запланированный вызов drain. Курсор подтверждается отдельно, только после успешного EventHandler.
class EventClient::Impl {
public:
	// owner задаёт Qt-контекст сигналов и отложенных обработчиков.
	EventClient *owner;
	// limits фиксирует бюджеты одной подписки.
	Limits limits;
	// network создаёт ответы только этого клиента с локальной политикой NoProxy.
	QNetworkAccessManager network;
	// parser собирает кадры только после подтверждения SSE-заголовков.
	SSEParser parser;
	// reply заимствует текущий ответ менеджера; close отключает его до abort.
	QPointer<QNetworkReply> reply;
	// receiver ограничивает время жизни пользовательских обработчиков.
	QPointer<QObject> receiver;
	// eventHandler применяет наблюдение и разрешает подтверждение курсора.
	EventHandler eventHandler;
	// errorHandler получает один окончательный отказ попытки.
	ErrorHandler errorHandler;
	// openHandler подтверждает транспорт один раз по заголовкам.
	OpenHandler openHandler;
	// receiverConnection закрывает подписку при уничтожении получателя.
	QMetaObject::Connection receiverConnection;
	// context связывает результаты с выбранным подключением и Stream.
	RequestContext context;
	// serial запрещает прежним продолжениям менять новую подписку.
	quint64 serial = 0;
	// stopped необратимо запрещает новые подписки после shutdown.
	bool stopped = false;
	// validated отмечает подтверждённый 200 text/event-stream.
	bool validated = false;
	// finished отмечает EOF транспорта, но не опустошение буфера reply.
	bool finished = false;
	// scheduled объединяет несколько сигналов в одно продолжение чтения.
	bool scheduled = false;
	// rejectionStatus отличает чтение Problem до открытия от подтверждённого SSE-потока.
	int rejectionStatus = 0;
	// rejectionProblem разрешает структурированный разбор только для правильного MIME.
	bool rejectionProblem = false;
	// rejectionBody накапливает только ограниченное тело отказа текущей попытки.
	QByteArray rejectionBody;
	// handshake ограничивает всю стадию до открытия, включая тело Problem.
	QTimer handshake;
	// idle ограничивает простой только подтверждённого SSE-потока.
	QTimer idle;
	// confirmed сохраняет начальный либо последний применённый cursor.
	QString confirmed;
	/** Создаёт отдельный Qt-транспорт и таймеры; callbacks завершают только текущую попытку. */
	Impl(EventClient *owner, Limits limits) : owner(owner), limits(limits), network(owner), parser(limits)
	{
		// Как и в APIClient, исключаем системное определение прокси до запуска таймера заголовков.
		// Это настройка только нашего менеджера для соединения с выбранным локальным сервером.
		network.setProxy(QNetworkProxy(QNetworkProxy::NoProxy));
		handshake.setSingleShot(true);
		idle.setSingleShot(true);
		// Обработчик handshake завершает также чтение Problem по неизменному сроку попытки.
		QObject::connect(&handshake, &QTimer::timeout, owner, [this] {
			if (rejectionStatus)
				reject(false);
			else
				fail(makeError(ErrorKind::Timeout), false);
		});
		// Обработчик idle сообщает отсутствие байтов после открытия SSE, не повторяя запрос.
		QObject::connect(&idle, &QTimer::timeout, owner,
				 [this] { fail(makeError(ErrorKind::Timeout), false); });
	}
	// Сначала меняет поколение и убирает reply, затем вызывает abort. Это исключает продолжение
	// старого чтения при сигналах прерывания. confirmed намеренно не очищается: вызывающая сторона может
	// прочитать его после отказа, когда обработчики и недоставленные кадры уничтожены.
	void close()
	{
		// Отложенная обработка прежнего ответа не должна продолжиться после закрытия или новой подписки.
		++serial;
		handshake.stop();
		idle.stop();
		QObject::disconnect(receiverConnection);
		// previous сохраняет ответ до отключения и отложенного удаления.
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
		openHandler = {};
		validated = false;
		finished = false;
		scheduled = false;
		rejectionStatus = 0;
		rejectionProblem = false;
		rejectionBody.clear();
	}
	// Захватывает обработчик и получателя до close, но вызывает его уже после полной очистки.
	// Так новый subscribe из errorHandler не будет стёрт последующим завершением старого отказа.
	void fail(Error error, bool resync)
	{
		// callback и target переживают очистку полей клиента; ctx сохраняет исходное поколение контекста.
		auto callback = errorHandler;
		auto target = receiver;
		const auto ctx = context;
		close();
		if (!stopped && target && callback)
			callback(ctx, std::move(error), resync);
	}
	/** Завершает HTTP-отказ один раз; неполное тело остаётся диагностикой без структурированного Problem. */
	void reject(bool complete)
	{
		// error сохраняет исходный HTTP-статус, в том числе для политики восстановления 400/409.
		auto error = rejectionStatus == 200 ? makeError(ErrorKind::Contract, rejectionStatus)
						    : decodeProblem(rejectionStatus, rejectionBody);
		if (!complete || !rejectionProblem)
			error.problem.reset();
		fail(std::move(error), false);
	}
	/** Читает Problem по порциям до EOF, лимита REST или первоначального handshake deadline. */
	void readRejection()
	{
		if (!reply)
			return;
		// room ограничивает накопление тела; bytes включает один контрольный байт превышения.
		const auto room = limits.restBodyBytes - rejectionBody.size();
		const auto bytes = reply->read(std::min(limits.readBytes, room + 1));
		if (bytes.size() > room) {
			rejectionBody.append(bytes.first(room));
			reject(false);
			return;
		}
		rejectionBody.append(bytes);
		if (reply->bytesAvailable() > 0) {
			schedule();
			return;
		}
		if (finished)
			reject(reply->error() == QNetworkReply::NoError || rejectionStatus >= 400);
	}
	// false означает либо «заголовков ещё нет», либо уже закрытый отказом/обработчиком поток.
	// После OpenHandler повторно проверяются owner, serial и receiver: подтверждение транспорта
	// не даёт права продолжить чтение подписки, заменённой из пользовательского обработчика.
	bool headers()
	{
		if (!reply)
			return false;
		if (rejectionStatus)
			return false;
		// attribute содержит полученный HTTP-статус, status — его числовое значение, media — MIME без параметров.
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
			if (status == 204) {
				fail(makeError(ErrorKind::Http, status), false);
				return false;
			}
			rejectionStatus = status;
			rejectionProblem = media == QStringLiteral("application/problem+json");
			return false;
		}
		if (media != QStringLiteral("text/event-stream")) {
			rejectionStatus = status;
			return false;
		}
		validated = true;
		// Подключение подтверждают заголовки: ждать первого события с данными не требуется.
		handshake.stop();
		idle.start(std::max(1, limits.sseIdleMs));
		// expected фиксирует попытку для проверки после пользовательского callback или отложенного продолжения.
		const auto expected = serial;
		// guard обнаруживает удаление клиента внутри пользовательского обработчика.
		const QPointer<EventClient> guard = owner;
		// callback забирает одноразовое подтверждение открытия.
		auto callback = std::move(openHandler);
		if (receiver && callback)
			callback(context);
		return guard && expected == serial && receiver;
	}
	// Объединяет readyRead/finished и собственное продолжение в один следующий оборот Qt.
	// Лямбда проверяет поколение до обращения к состоянию новой подписки.
	void schedule()
	{
		if (scheduled)
			return;
		scheduled = true;
		// expected запрещает отложенному чтению продолжить заменённую попытку.
		const auto expected = serial;
		QTimer::singleShot(0, owner, [this, expected] {
			if (expected != serial || stopped)
				return;
			scheduled = false;
			drain();
		});
	}
	// Сопоставляет транспортный тип и id с JSON kind/cursor, затем передаёт событие потребителю.
	// resync_required не является RuntimeEvent и не имеет id; содержащиеся в нём данные не задают URL.
	// Курсор продвигается после true от потребителя и повторной проверки поколения. Отказ применения,
	// неверный JSON или противоречие оболочки требуют нового снимка, а не пропуска «плохого» события.
	bool frame(SSEFrame frame)
	{
		if (frame.event == QStringLiteral("resync_required")) {
			// json использует тот же ограниченный parser, что REST и обычные SSE-события.
			JsonValue json;
			if (!parseJson(frame.data, json) || !json.isObject() ||
			    !json.toObject().value(QStringLiteral("reason")).isString() || !frame.id.isEmpty()) {
				fail(makeError(ErrorKind::Contract), true);
				return false;
			}
			fail(makeError(ErrorKind::Contract), true);
			return false;
		}
		// result содержит только целиком проверенное событие с точными числами.
		const auto result = decode<RuntimeEvent>(frame.data);
		if (!result || frame.event != result.value->kind || frame.id.isEmpty() ||
		    frame.id != result.value->cursor || !validateRuntimeEvent(*result.value)) {
			fail(makeError(ErrorKind::Contract), true);
			return false;
		}
		// expected обнаруживает замену подписки внутри обработчика события.
		const auto expected = serial;
		// ctx сохраняет контекст; target и handler удерживают текущего получателя и копию callback на время вызова.
		const auto ctx = context;
		const auto target = receiver;
		const auto handler = eventHandler;
		// guard обнаруживает уничтожение клиента обработчиком события.
		const QPointer<EventClient> guard = owner;
		// applied подтверждает только успешное применение события живым потребителем.
		const bool applied = target && handler && handler(ctx, *result.value);
		if (!guard)
			return false;
		if (expected != serial || stopped || !receiver)
			return false;
		if (!applied) {
			fail(makeError(ErrorKind::Contract), true);
			return false;
		}
		// Полученный, но отвергнутый потребителем кадр не продвигает точку восстановления.
		confirmed = result.value->cursor;
		return true;
	}
	// Сначала доставляет готовые кадры, затем читает ограниченную порцию новых байтов. Бюджеты
	// считаются по доставленным кадрам; один допустимый крупный кадр разрешён даже сверх bytesPerTurn,
	// но перед следующим управление возвращается Qt. Так размер кадра и отзывчивость GUI не смешиваются.
	// При EOF сначала выдаются полные кадры и отбрасывается незаконченный хвост, затем сообщается
	// Transport. Нет ни «успешного конца» бесконечной подписки, ни подтверждения частичного события.
	void drain()
	{
		if (scheduled)
			return;
		if (!reply || stopped || !receiver)
			return;
		if (!validated) {
			// expected и guard защищают продолжение после проверки заголовков и OpenHandler.
			const auto expected = serial;
			const QPointer<EventClient> guard = owner;
			if (!headers()) {
				if (guard && expected == serial && reply && rejectionStatus) {
					readRejection();
					return;
				}
				if (guard && expected == serial && finished) {
					// kind сохраняет отличие TLS-отказа от обычного разрыва транспорта.
					const auto kind = reply->error() == QNetworkReply::SslHandshakeFailedError
								  ? ErrorKind::Tls
								  : ErrorKind::Transport;
					fail(makeError(kind), false);
				}
				return;
			}
		}
		// processedBytes и processedEvents ограничивают доставку за оборот Qt.
		qsizetype processedBytes = 0;
		int processedEvents = 0;
		// deliverFrames выдаёт готовые кадры до бюджета и планирует продолжение без блокировки Qt.
		auto deliverFrames = [&] {
			while (parser.hasFrame()) {
				// Один крупный допустимый кадр обрабатывается целиком; перед следующим возвращаем управление Qt.
				if (processedEvents >= limits.eventsPerTurn ||
				    (processedEvents > 0 &&
				     parser.nextFrameBytes() > limits.bytesPerTurn - processedBytes)) {
					schedule();
					return false;
				}
				// next является одним полным кадром, удалённым из ограниченной очереди parser.
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
		// bytes содержит следующую порцию SSE в оставшемся бюджете текущего оборота.
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
			// Остаток незавершённого кадра отбрасывается; переподключение использует только подтверждённый курсор.
			if (!parser.finish()) {
				fail(parser.error(), true);
				return;
			}
			fail(makeError(ErrorKind::Transport), false);
		}
	}
	// Каждая новая попытка начинает чистый парсер и собственное ожидание заголовков, но использует переданный
	// подтверждённый cursor. Сигналы захватывают serial; уничтожение target закрывает только эту попытку.
	void subscribe(QUrl origin, QString cursor, RequestContext ctx, QObject *target, EventHandler events,
		       ErrorHandler errors, OpenHandler opened)
	{
		if (stopped)
			return;
		close();
		context = ctx;
		receiver = target;
		eventHandler = std::move(events);
		errorHandler = std::move(errors);
		openHandler = std::move(opened);
		confirmed = cursor;
		if (!receiver)
			return;
		if (!validServerOrigin(origin) || cursor.isEmpty() || cursor.contains(QChar::Null) ||
		    limits.readBytes <= 0 || limits.bytesPerTurn <= 0 || limits.eventsPerTurn <= 0 ||
		    limits.restBodyBytes <= 0) {
			fail(makeError(ErrorKind::Contract), true);
			return;
		}
		origin.setPath(QStringLiteral("/api/v1/events"));
		// Серверный контракт использует параметр cursor в URL; второй курсор в заголовке не добавляется.
		// query передаёт непрозрачный cursor одним значением, request сохраняет ручные редиректы.
		QUrlQuery query;
		query.addQueryItem(QStringLiteral("cursor"), QString::fromLatin1(QUrl::toPercentEncoding(cursor)));
		origin.setQuery(query);
		QNetworkRequest request(origin);
		request.setRawHeader("Accept", "text/event-stream");
		request.setAttribute(QNetworkRequest::RedirectPolicyAttribute, QNetworkRequest::ManualRedirectPolicy);
		reply = network.get(request);
		reply->setReadBufferSize(limits.readBytes);
		// expected закрепляет поколение для всех сетевых сигналов и уничтожения получателя.
		const auto expected = serial;
		// Обработчик уничтожения target закрывает только текущую serial-попытку.
		receiverConnection = QObject::connect(target, &QObject::destroyed, owner, [this, expected] {
			if (expected == serial)
				close();
		});
		// Обработчик metadata подтверждает 200 SSE либо начинает отдельное чтение HTTP-отказа.
		QObject::connect(reply, &QNetworkReply::metaDataChanged, owner, [this, expected] {
			if (expected == serial && !validated)
				headers();
		});
		// Обработчик readyRead читает только актуальную попытку с ограничением порции.
		QObject::connect(reply, &QIODevice::readyRead, owner, [this, expected] {
			if (expected == serial)
				drain();
		});
		// Обработчик finished фиксирует EOF и дочитывает оставшиеся байты перед одним результатом.
		QObject::connect(reply, &QNetworkReply::finished, owner, [this, expected] {
			if (expected == serial) {
				finished = true;
				drain();
			}
		});
		handshake.start(std::max(1, limits.sseHandshakeMs));
	}
};
/** Создаёт клиента с отдельным состоянием подписки и Qt-владением. */
EventClient::EventClient(Limits limits, QObject *parent) : QObject(parent), impl_(std::make_unique<Impl>(this, limits))
{
}
/** Закрывает подписку и запрещает отложенные сетевые продолжения. */
EventClient::~EventClient()
{
	shutdown();
}
/** Передаёт параметры новой попытки единственному состоянию подписки. */
void EventClient::subscribe(QUrl origin, QString cursor, RequestContext context, QObject *receiver, EventHandler events,
			    ErrorHandler errors, OpenHandler opened)
{
	impl_->subscribe(std::move(origin), std::move(cursor), context, receiver, std::move(events), std::move(errors),
			 std::move(opened));
}
/** Отменяет текущую попытку без уведомления об ошибке, сохраняя подтверждённый cursor. */
void EventClient::close()
{
	impl_->close();
}
/** Идемпотентно закрывает клиента и запрещает последующие subscribe. */
void EventClient::shutdown()
{
	if (impl_->stopped)
		return;
	impl_->stopped = true;
	impl_->close();
}
/** Возвращает начальную либо последнюю применённую точку продолжения. */
QString EventClient::confirmedCursor() const
{
	return impl_->confirmed;
}
} // namespace gost::api
