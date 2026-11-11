#include "api/api-client.hpp"
#include "api/json-decoder.hpp"
#include "api/network-origin.hpp"
#include <QNetworkAccessManager>
#include <QNetworkProxy>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QPointer>
#include <QHash>
#include <QQueue>
#include <QSet>
#include <QTimer>
#include <QUrlQuery>
#include <algorithm>
#include <utility>

namespace gost::api {
namespace {
QString segment(const QString &id)
{
	// Непрозрачный ID целиком кодируется как один сегмент, включая возможные разделители пути.
	return QString::fromLatin1(QUrl::toPercentEncoding(id));
}
QUrlQuery queryParameters(const ListQuery &query)
{
	// Все фильтры кодируются как значения query, включая '+', '&' и '%' в непрозрачном курсоре.
	// Пустая строка означает отсутствие фильтра; limit передаётся всегда, его валидность решает сервер.
	QUrlQuery result;
	result.addQueryItem(QStringLiteral("limit"), QString::number(query.limit));
	for (auto pair :
	     {std::pair{"cursor", &query.cursor}, std::pair{"from", &query.from}, std::pair{"to", &query.to},
	      std::pair{"stream_id", &query.streamId}, std::pair{"group_id", &query.groupId},
	      std::pair{"profile_id", &query.profileId}, std::pair{"asset_id", &query.assetId},
	      std::pair{"generation_id", &query.generationId}, std::pair{"state", &query.state}})
		if (!pair.second->isEmpty())
			result.addQueryItem(QLatin1String(pair.first),
					    QString::fromLatin1(QUrl::toPercentEncoding(*pair.second)));
	return result;
}
} // namespace
// Управляет очередью и активными REST-запросами в потоке owner. Задание сначала ожидает в queue,
// затем занимает слот в active и завершается через complete. Отмена или удаление receiver подавляет
// выдачу результата. Несколько сетевых сигналов могут ссылаться на один Job; завершить его и
// освободить слот можно только пока он находится в active. Все переходы выполняются в одном цикле Qt.
class APIClient::Impl {
public:
	using RawCallback = Callback<QByteArray>;
	// Состояние одного GET разделяется между очередью и захватами обработчиков через shared_ptr.
	// Job не владеет receiver и reply. reply принадлежит network; оба таймера являются его детьми.
	// finished отмечает конец транспорта, а не готовность результата: в reply ещё могут оставаться байты.
	// scheduled означает единственное уже поставленное продолжение drain и предотвращает двойное чтение.
	// context защищает смену выбора, serial дополнительно защищает явную отмену при том же выборе.
	struct Job {
		QPointer<QObject> receiver;
		RequestContext context;
		quint64 serial = 0;
		QString path;
		QUrlQuery query;
		RawCallback callback;
		qsizetype cap = 0;
		QByteArray body;
		QPointer<QNetworkReply> reply;
		QTimer *idle = nullptr;
		QTimer *deadline = nullptr;
		bool finished = false;
		bool scheduled = false;
		QMetaObject::Connection receiverConnection;
		~Job() { QObject::disconnect(receiverConnection); }
	};
	APIClient *owner;
	Limits limits;
	QNetworkAccessManager network;
	QUrl origin;
	RequestContext context;
	quint64 serial = 0;
	bool stopped = false;
	QQueue<std::shared_ptr<Job>> queue;
	QHash<QNetworkReply *, std::shared_ptr<Job>> active;
	Impl(APIClient *owner, Limits limits) : owner(owner), limits(limits), network(owner)
	{
		// Для явно выбранного локального сервера обходим системное определение прокси: WinHTTP может
		// заблокировать GUI ещё внутри get(), до запуска таймеров запроса. Настройки прокси OBS не меняем.
		network.setProxy(QNetworkProxy(QNetworkProxy::NoProxy));
	}
	// Проверяет право выдавать результат, а не только наличие сетевого ответа. Проверка нужна также
	// для ошибок допуска, у которых reply вообще не создавался, и для продолжений обхода списка.
	bool current(const std::shared_ptr<Job> &job) const
	{
		// serial запрещает доставку и отложенных заданий того же RequestContext после явной отмены.
		return !stopped && job->serial == serial && job->context == context && job->receiver;
	}
	// Вызывает обработчик получателя прямо в текущем стеке. Тот может удалить owner, поэтому после
	// возврата из deliver обращаться к Impl можно лишь после проверки, что клиент ещё существует.
	void deliver(const std::shared_ptr<Job> &job, Result<QByteArray> result)
	{
		if (current(job))
			job->callback(job->context, std::move(result));
	}
	// Завершает активный GET не более одного раза. До callback удаляет запись из active и отключает
	// сигналы, чтобы abort, timeout и finished не завершили его повторно. abort нужен при досрочном
	// отказе; нормальный EOF уже завершил транспорт. Освободившийся слот заполняется после callback,
	// с учётом возможной отмены, перенастройки или уничтожения клиента внутри него.
	void complete(const std::shared_ptr<Job> &job, Result<QByteArray> result, bool abort = false)
	{
		auto *reply = job->reply.data();
		if (!reply || !active.contains(reply))
			return;
		active.remove(reply);
		job->reply = nullptr;
		QObject::disconnect(reply, nullptr, owner, nullptr);
		if (abort)
			reply->abort();
		reply->deleteLater();
		// Пользовательский обработчик может удалить клиент; очередь продолжается только при живом владельце.
		const QPointer<APIClient> guard = owner;
		deliver(job, std::move(result));
		if (guard)
			pump();
	}
	// Отменяет активные и ожидающие задания. Новое значение serial запрещает выдачу результатов
	// также из уже запланированных singleShot. Очередь очищается без уведомлений; active очищается
	// до abort, поскольку прерывание может породить сетевые сигналы. Новые задания допустимы до shutdown().
	void cancel()
	{
		// Сначала запрещаем доставку, затем прерываем ответы: их завершение не должно вызвать старые обработчики.
		++serial;
		queue.clear();
		const auto jobs = active;
		active.clear();
		for (const auto &job : jobs)
			if (auto *reply = job->reply.data()) {
				job->reply = nullptr;
				QObject::disconnect(reply, nullptr, owner, nullptr);
				reply->abort();
				reply->deleteLater();
			}
	}
	// Принимает относительный путь внутри /api/v1 и предел полного тела. Ошибки origin/лимитов/очереди
	// выдаются отложенно с теми же проверками актуальности, что и сетевой результат. При остановленном
	// клиенте или null receiver операция не создаётся. Исчезновение активного receiver сразу освобождает
	// слот; исчезнувшее задание в очереди будет пропущено pump. Слабый захват не удерживает Job сам собой.
	void raw(QString path, QUrlQuery query, QObject *receiver, RawCallback callback, qsizetype cap)
	{
		if (stopped || !receiver)
			return;
		auto job = std::make_shared<Job>();
		job->receiver = receiver;
		job->context = context;
		job->serial = serial;
		job->path = std::move(path);
		job->query = std::move(query);
		job->callback = std::move(callback);
		job->cap = cap;
		if (origin.isEmpty() || limits.maxActiveRequests <= 0 || limits.readBytes <= 0 || cap <= 0) {
			QTimer::singleShot(0, owner, [this, job] {
				deliver(job, Result<QByteArray>::failure(makeError(ErrorKind::Contract)));
			});
			return;
		}
		if (active.size() >= limits.maxActiveRequests && queue.size() >= limits.maxQueuedRequests) {
			QTimer::singleShot(0, owner, [this, job] {
				deliver(job, Result<QByteArray>::failure(makeError(ErrorKind::Limit)));
			});
			return;
		}
		const std::weak_ptr<Job> weak = job;
		job->receiverConnection = QObject::connect(receiver, &QObject::destroyed, owner, [this, weak] {
			if (auto job = weak.lock())
				if (auto *reply = job->reply.data()) {
					active.remove(reply);
					job->reply = nullptr;
					QObject::disconnect(reply, nullptr, owner, nullptr);
					reply->abort();
					reply->deleteLater();
					pump();
				}
		});
		queue.enqueue(std::move(job));
		pump();
	}
	// Запускает ожидающие актуальные задания до maxActiveRequests. Порядок очереди сохраняется,
	// устаревшие записи пропускаются. Каждый ответ получает собственные idle/deadline таймеры:
	// получение очередных байтов продлевает только idle, абсолютный срок отправленного GET неизменен.
	void pump()
	{
		while (!stopped && active.size() < limits.maxActiveRequests && !queue.isEmpty()) {
			auto job = queue.dequeue();
			if (!current(job))
				continue;
			QUrl url = origin;
			url.setPath(QStringLiteral("/api/v1") + job->path, QUrl::StrictMode);
			url.setQuery(job->query);
			QNetworkRequest request(url);
			request.setRawHeader("Accept", "application/json");
			request.setAttribute(QNetworkRequest::RedirectPolicyAttribute,
					     QNetworkRequest::ManualRedirectPolicy);
			auto *reply = network.get(request);
			job->reply = reply;
			active.insert(reply, job);
			reply->setReadBufferSize(limits.readBytes);
			job->deadline = new QTimer(reply);
			job->deadline->setSingleShot(true);
			job->idle = new QTimer(reply);
			job->idle->setSingleShot(true);
			auto timeout = [this, job] {
				complete(job, Result<QByteArray>::failure(makeError(ErrorKind::Timeout)), true);
			};
			QObject::connect(job->deadline, &QTimer::timeout, owner, timeout);
			QObject::connect(job->idle, &QTimer::timeout, owner, timeout);
			// Оба бюджета начинаются при отправке, а не во время ожидания места в очереди.
			job->deadline->start(std::max(1, limits.httpDeadlineMs));
			job->idle->start(std::max(1, limits.httpIdleMs));
			QObject::connect(reply, &QIODevice::readyRead, owner, [this, job] { drain(job); });
			QObject::connect(reply, &QNetworkReply::finished, owner, [this, job] {
				job->finished = true;
				drain(job);
			});
		}
	}
	// За один вызов считывает ограниченную порцию. Сначала проверяет актуальность и размер, затем
	// дожидается и EOF транспорта, и опустошения буфера. Только после этого классифицирует ответ.
	// Это сохраняет последние байты после finished и не позволяет принять усечённый JSON как полный.
	// Продолжение привязано к owner; отмена делает его безвредным через reply/active/serial.
	void drain(const std::shared_ptr<Job> &job)
	{
		if (job->scheduled)
			return;
		auto *reply = job->reply.data();
		if (!reply || !active.contains(reply))
			return;
		if (!current(job)) {
			complete(job, Result<QByteArray>::failure(makeError(ErrorKind::Cancelled)), true);
			return;
		}
		const auto room = job->cap - job->body.size();
		// Дополнительный байт позволяет обнаружить превышение, не выдавая усечённый ответ как полный.
		const auto bytes = reply->read(std::min(limits.readBytes, room + 1));
		if (!bytes.isEmpty()) {
			job->body.append(bytes);
			job->idle->start(std::max(1, limits.httpIdleMs));
		}
		if (job->body.size() > job->cap) {
			complete(job, Result<QByteArray>::failure(makeError(ErrorKind::Limit)), true);
			return;
		}
		if (reply->bytesAvailable() > 0) {
			// Большое тело читается несколькими оборотами цикла событий, сохраняя доступность интерфейса.
			if (!job->scheduled) {
				job->scheduled = true;
				QTimer::singleShot(0, owner, [this, job] {
					job->scheduled = false;
					drain(job);
				});
			}
			return;
		}
		if (!job->finished)
			return;
		const int status = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
		const auto media = reply->header(QNetworkRequest::ContentTypeHeader)
					   .toString()
					   .section(QLatin1Char(';'), 0, 0)
					   .trimmed()
					   .toLower();
		if (status >= 400) {
			// HTTP-ошибка обрабатывается до общей ошибки транспорта, которую Qt также выставляет для таких ответов.
			complete(job, Result<QByteArray>::failure(
					      decodeProblem(status, media == QStringLiteral("application/problem+json")
									    ? job->body
									    : QByteArray{})));
			return;
		}
		if (reply->error() != QNetworkReply::NoError) {
			complete(job, Result<QByteArray>::failure(makeError(ErrorKind::Transport, status)));
			return;
		}
		if (status != 200 || media != QStringLiteral("application/json")) {
			complete(job, Result<QByteArray>::failure(makeError(ErrorKind::Contract, status)));
			return;
		}
		complete(job, Result<QByteArray>::success(std::move(job->body)));
	}
	// Преобразование DTO выполняется только для полного успешного тела. decode создаёт новый объект,
	// поэтому ошибка в середине документа не публикует частично заполненную сущность.
	template<class T>
	void get(QString path, QObject *receiver, Callback<T> callback, QUrlQuery query = {}, qsizetype cap = 0)
	{
		raw(
			std::move(path), std::move(query), receiver,
			[callback = std::move(callback)](RequestContext context, Result<QByteArray> result) {
				if (!result) {
					callback(context, Result<T>::failure(*result.error));
					return;
				}
				callback(context, decode<T>(*result.value));
			},
			cap ? cap : limits.restBodyBytes);
	}
	template<class T> void page(QString path, QObject *receiver, Callback<Page<T>> callback, ListQuery query)
	{
		get(std::move(path), receiver, std::move(callback), queryParameters(query));
	}
	// Состояние одного последовательного обхода, живущее между GET и отложенными walkNext.
	// Фильтры query сохраняются; меняется только cursor. result хранит лишь принятые страницы,
	// bytes считает их исходные JSON-байты, pages — принятые документы, cursors обнаруживает циклы.
	// complete остаётся false до явного конца списка. receiver заимствован, serial/context фиксируются
	// на весь обход, поэтому его нельзя незаметно продолжить после смены подключения или выбора.
	template<class T> struct Walk {
		QString path;
		ListQuery query;
		QPointer<QObject> receiver;
		Callback<Collection<T>> callback;
		Collection<T> result;
		QSet<QString> cursors;
		int pages = 0;
		quint64 serial = 0;
		RequestContext context;
	};
	// Выполняет ровно один шаг обхода. Общий бюджет проверяется до GET и перед принятием страницы;
	// размер отдельного ответа независимо ограничивает raw. Предел элементов может принять часть
	// страницы, но это заведомо неполный результат. Следующая страница ставится через singleShot,
	// чтобы не наращивать стек и дать обработать GUI/отмену между страницами.
	template<class T> void walkNext(std::shared_ptr<Walk<T>> walk)
	{
		if (walk->serial != serial || walk->context != context || !walk->receiver || stopped)
			return;
		if (walk->pages >= limits.maxPages || walk->result.items.size() >= limits.maxItems ||
		    walk->result.bytes >= limits.pageWalkBytes) {
			// Общий бюджет возвращает известную часть с complete=false; ошибка отдельного ответа остаётся ошибкой.
			walk->callback(walk->context, Result<Collection<T>>::success(std::move(walk->result)));
			return;
		}
		raw(
			walk->path, queryParameters(walk->query), walk->receiver,
			[this, walk](RequestContext ctx, Result<QByteArray> rawResult) {
				if (!rawResult) {
					walk->callback(ctx, Result<Collection<T>>::failure(*rawResult.error));
					return;
				}
				const auto remaining = limits.pageWalkBytes - walk->result.bytes;
				if (rawResult.value->size() > remaining) {
					// Страница либо принимается целиком по объёму, либо сохраняются только предыдущие страницы.
					walk->callback(ctx, Result<Collection<T>>::success(std::move(walk->result)));
					return;
				}
				auto result = decode<Page<T>>(*rawResult.value);
				if (!result) {
					walk->callback(ctx, Result<Collection<T>>::failure(*result.error));
					return;
				}
				++walk->pages;
				walk->result.bytes += rawResult.value->size();
				const auto remainingItems =
					std::max<qsizetype>(0, limits.maxItems - walk->result.items.size());
				const auto count = std::min<qsizetype>(remainingItems, result.value->items.size());
				for (qsizetype i = 0; i < count; ++i)
					walk->result.items.push_back(std::move(result.value->items[i]));
				walk->result.nextCursor = result.value->nextCursor;
				if (count < result.value->items.size()) {
					walk->callback(ctx, Result<Collection<T>>::success(std::move(walk->result)));
					return;
				}
				if (result.value->nextCursor.isEmpty()) {
					walk->result.complete = true;
					walk->callback(ctx, Result<Collection<T>>::success(std::move(walk->result)));
					return;
				}
				if (walk->cursors.contains(result.value->nextCursor)) {
					walk->callback(ctx,
						       Result<Collection<T>>::failure(makeError(ErrorKind::Contract)));
					return;
				}
				walk->query.cursor = result.value->nextCursor;
				walk->cursors.insert(walk->query.cursor);
				QTimer::singleShot(0, owner, [this, walk] { walkNext(walk); });
			},
			limits.restBodyBytes);
	}
	// Начальный cursor тоже участвует в обнаружении циклов. walkNext вызывается непосредственно:
	// если бюджет уже исчерпан, callback выполнится в этом стеке, как описано у публичного getAll<Name>.
	template<class T> void all(QString path, QObject *receiver, Callback<Collection<T>> callback, ListQuery query)
	{
		auto walk = std::make_shared<Walk<T>>();
		walk->path = std::move(path);
		walk->query = std::move(query);
		walk->receiver = receiver;
		walk->callback = std::move(callback);
		walk->serial = serial;
		walk->context = context;
		walk->result.nextCursor = walk->query.cursor;
		if (!walk->query.cursor.isEmpty())
			walk->cursors.insert(walk->query.cursor);
		walkNext(walk);
	}
};
APIClient::APIClient(Limits limits, QObject *parent) : QObject(parent), impl_(std::make_unique<Impl>(this, limits)) {}
APIClient::~APIClient()
{
	shutdown();
}
void APIClient::configure(QUrl origin, RequestContext context)
{
	if (impl_->stopped)
		return;
	impl_->cancel();
	impl_->origin = validServerOrigin(origin) ? std::move(origin) : QUrl{};
	impl_->context = context;
}
void APIClient::cancelAll()
{
	impl_->cancel();
}
void APIClient::shutdown()
{
	if (impl_->stopped)
		return;
	impl_->stopped = true;
	impl_->cancel();
}
void APIClient::getReady(QObject *receiver, Callback<Health> callback)
{
	impl_->get(QStringLiteral("/health/ready"), receiver, std::move(callback));
}
void APIClient::getLive(QObject *receiver, Callback<Health> callback)
{
	impl_->get(QStringLiteral("/health/live"), receiver, std::move(callback));
}
void APIClient::getCapabilities(QObject *receiver, Callback<Capabilities> callback)
{
	impl_->get(QStringLiteral("/system/capabilities"), receiver, std::move(callback));
}
void APIClient::getSnapshot(QObject *receiver, Callback<Anchor> callback)
{
	impl_->get(QStringLiteral("/events/snapshot"), receiver, std::move(callback), {},
		   impl_->limits.snapshotBodyBytes);
}
#define GOST_DETAIL(Method, Type, Path) \
void APIClient::Method(QString id,QObject *receiver,Callback<Type> callback) { impl_->get(QStringLiteral(Path)+segment(id),receiver,std::move(callback)); }
GOST_DETAIL(getStream, Stream, "/streams/")
GOST_DETAIL(getDestination, Destination, "/destinations/")
GOST_DETAIL(getProfile, Profile, "/transcode-profiles/")
GOST_DETAIL(getGroup, Group, "/transcode-groups/")
GOST_DETAIL(getSession, Session, "/sessions/")
GOST_DETAIL(getCommand, Receipt, "/commands/")
GOST_DETAIL(getAsset, Asset, "/fallback-assets/")
#undef GOST_DETAIL
#define GOST_LIST(Name, Type, Path) \
void APIClient::get##Name(QObject *receiver,Callback<Page<Type>> callback,ListQuery query) { impl_->page(QStringLiteral(Path),receiver,std::move(callback),std::move(query)); } \
void APIClient::getAll##Name(QObject *receiver,Callback<Collection<Type>> callback,ListQuery query) { impl_->all(QStringLiteral(Path),receiver,std::move(callback),std::move(query)); }
GOST_LIST(Streams, Stream, "/streams")
GOST_LIST(Destinations, Destination, "/destinations")
GOST_LIST(Profiles, Profile, "/transcode-profiles")
GOST_LIST(Groups, Group, "/transcode-groups")
GOST_LIST(Sessions, Session, "/sessions")
GOST_LIST(Assets, Asset, "/fallback-assets")
#undef GOST_LIST
#define GOST_NESTED(Name, Type, Prefix, Suffix) \
void APIClient::get##Name(QString id,QObject *receiver,Callback<Page<Type>> callback,ListQuery query) { impl_->page(QStringLiteral(Prefix)+segment(id)+QStringLiteral(Suffix),receiver,std::move(callback),std::move(query)); } \
void APIClient::getAll##Name(QString id,QObject *receiver,Callback<Collection<Type>> callback,ListQuery query) { impl_->all(QStringLiteral(Prefix)+segment(id)+QStringLiteral(Suffix),receiver,std::move(callback),std::move(query)); }
GOST_NESTED(StreamDestinations, Destination, "/streams/", "/destinations")
GOST_NESTED(StreamSessions, Session, "/streams/", "/sessions")
GOST_NESTED(Outputs, Output, "/sessions/", "/outputs")
GOST_NESTED(Variants, Variant, "/fallback-assets/", "/variants")
#undef GOST_NESTED
void APIClient::getGeneration(QString assetId, QString generationId, QObject *receiver, Callback<Generation> callback)
{
	impl_->get(QStringLiteral("/fallback-assets/") + segment(assetId) + QStringLiteral("/generations/") +
			   segment(generationId),
		   receiver, std::move(callback));
}
void APIClient::getFallbackPolicy(QString streamId, QObject *receiver, Callback<PolicyResult> callback)
{
	impl_->get(QStringLiteral("/streams/") + segment(streamId) + QStringLiteral("/fallback-policy"), receiver,
		   std::move(callback));
}
void APIClient::getSessionMetrics(QString sessionId, QObject *receiver, Callback<SessionMetrics> callback)
{
	impl_->get(QStringLiteral("/sessions/") + segment(sessionId) + QStringLiteral("/metrics"), receiver,
		   std::move(callback));
}
void APIClient::getOutput(QString sessionId, QString outputId, QObject *receiver, Callback<OutputDetail> callback)
{
	impl_->get(QStringLiteral("/sessions/") + segment(sessionId) + QStringLiteral("/outputs/") + segment(outputId),
		   receiver, std::move(callback));
}
} // namespace gost::api
