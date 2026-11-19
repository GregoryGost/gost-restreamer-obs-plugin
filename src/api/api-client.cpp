#include "api/api-client.hpp"
#include "api/json-decoder.hpp"
#include "api/json-encoder.hpp"
#include "api/network-origin.hpp"
#include <QNetworkAccessManager>
#include <QFile>
#include <QFileInfo>
#include <QNetworkProxy>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QPointer>
#include <QHash>
#include <QQueue>
#include <QSet>
#include <QTimer>
#include <QUrlQuery>
#include "api/json-value.hpp"

#include <QElapsedTimer>
#include <algorithm>
#include <type_traits>
#include <utility>

namespace gost::api {
namespace {
/** Передаёт захваченное число байтов обычного файла через последовательный QIODevice.
 * Файл открыт до допуска в очередь. Буферизация всего тела и перемотка запрещены;
 * рост файла не расширяет тело, ранний EOF или ошибка чтения не дают успешной загрузки.
 */
class UploadSource final : public QIODevice {
public:
	/** Открывает path только для чтения и фиксирует размер в пределах maxBytes.
	 * Неверный вид/размер файла возвращает локальный Contract либо Limit через error.
	 */
	bool openFile(const QString &path, qint64 maxBytes, Error &error)
	{
		file_.setFileName(path);
		if (!QFileInfo(file_).isFile() || !file_.open(QIODevice::ReadOnly)) {
			error = makeError(ErrorKind::Contract);
			return false;
		}
		size_ = file_.size();
		remaining_ = size_;
		if (size_ <= 0 || maxBytes <= 0 || size_ > maxBytes) {
			error = makeError(size_ > 0 ? ErrorKind::Limit : ErrorKind::Contract);
			file_.close();
			return false;
		}
		return QIODevice::open(QIODevice::ReadOnly | QIODevice::Unbuffered);
	}
	/** Возвращает последовательный режим: Qt не должен буферизовать файл ради перемотки. */
	bool isSequential() const override { return true; }
	/** Возвращает захваченный размер, который используется в точном Content-Length. */
	qint64 size() const override { return size_; }
	/** Возвращает ещё не переданное число байтов; дополнительного чтения файла не выполняет. */
	qint64 bytesAvailable() const override { return remaining_ + QIODevice::bytesAvailable(); }
	/** Отличает настоящий конец захваченного тела от текущего состояния буфера QIODevice. */
	bool atEnd() const override { return remaining_ == 0 || failed_; }
	/** Запрещает произвольную позицию, включая ноль; повторное тело не воспроизводится. */
	bool seek(qint64) override
	{
		failed_ = true;
		return false;
	}
	/** Запрещает перемотку при перенаправлении, повторе или повторной аутентификации Qt. */
	bool reset() override
	{
		failed_ = true;
		return false;
	}
	/** Закрывает исходный файл после завершения/прерывания транспорта; счётчики сохраняются для проверки. */
	void close() override
	{
		file_.close();
		QIODevice::close();
	}
	/** Подтверждает полное однократное чтение захваченного тела без ошибки. */
	bool fullyRead() const { return !failed_ && remaining_ == 0; }

protected:
	/** Читает не более maxSize и оставшегося размера; ошибка или ранний EOF прекращают выдачу. */
	qint64 readData(char *data, qint64 maxSize) override
	{
		if (failed_)
			return -1;
		if (remaining_ == 0 || maxSize <= 0)
			return 0;
		// count отражает реально прочитанную ограниченную порцию, без чтения выросшего хвоста.
		const auto count = file_.read(data, std::min(maxSize, remaining_));
		if (count <= 0) {
			failed_ = true;
			setErrorString(QStringLiteral("Не удалось прочитать файл полностью."));
			return -1;
		}
		remaining_ -= count;
		return count;
	}
	/** Отвергает запись: адаптер служит только источником бинарного HTTP-тела. */
	qint64 writeData(const char *, qint64) override { return -1; }

private:
	// Открытый исходный файл; закрывается при завершении транспорта или уничтожении задания очереди.
	QFile file_;
	// Захваченная длина тела в байтах, неизменная после успешного openFile.
	qint64 size_ = 0;
	// Ещё не выданная транспорту часть захваченного размера, в байтах.
	qint64 remaining_ = 0;
	// Ошибка чтения либо попытка перемотки запрещает принять ответ как успешную загрузку.
	bool failed_ = false;
};
// Кодирует непрозрачный id одним сегментом пути, сохраняя разделители внутри значения.
QString segment(const QString &id)
{
	// Непрозрачный ID целиком кодируется как один сегмент, включая возможные разделители пути.
	return QString::fromLatin1(QUrl::toPercentEncoding(id));
}
// Возвращает категорию заранее известного маршрута, не передавая его текст диагностике.
RouteType routeType(const QString &path)
{
	if (path.startsWith(QStringLiteral("/health/")))
		return RouteType::Health;
	if (path == QStringLiteral("/system/capabilities"))
		return RouteType::Capabilities;
	if (path == QStringLiteral("/events/snapshot"))
		return RouteType::Snapshot;
	if (path.startsWith(QStringLiteral("/streams")))
		return RouteType::Stream;
	if (path.startsWith(QStringLiteral("/destinations")))
		return RouteType::Destination;
	if (path.startsWith(QStringLiteral("/transcode-profiles")))
		return RouteType::Profile;
	if (path.startsWith(QStringLiteral("/transcode-groups")))
		return RouteType::Group;
	if (path.startsWith(QStringLiteral("/sessions")))
		return RouteType::Session;
	if (path.startsWith(QStringLiteral("/commands")))
		return RouteType::Command;
	if (path.startsWith(QStringLiteral("/fallback-assets")))
		return RouteType::Asset;
	return RouteType::Other;
}
// Преобразует внутреннее обозначение Qt в закрытый безопасный набор методов.
HttpMethod diagnosticMethod(QNetworkAccessManager::Operation method)
{
	if (method == QNetworkAccessManager::PostOperation)
		return HttpMethod::Post;
	if (method == QNetworkAccessManager::PutOperation)
		return HttpMethod::Put;
	if (method == QNetworkAccessManager::DeleteOperation)
		return HttpMethod::Delete;
	if (method == QNetworkAccessManager::CustomOperation)
		return HttpMethod::Patch;
	return HttpMethod::Get;
}
// Преобразует фильтры query в параметры URL, сохраняя точные значения непрозрачных курсоров и ID.
QUrlQuery queryParameters(const ListQuery &query)
{
	// Все фильтры кодируются как значения query, включая '+', '&' и '%' в непрозрачном курсоре.
	// Пустая строка означает отсутствие фильтра; limit передаётся всегда, его валидность решает сервер.
	// result содержит закодированные значения фильтров и обязательный limit.
	QUrlQuery result;
	result.addQueryItem(QStringLiteral("limit"), QString::number(query.limit));
	// pair связывает имя параметра с соответствующей строкой query; пустые значения пропускаются.
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
	// Объявляет транспортное задание до типа callback, который проверяет его актуальность после диагностики.
	struct Job;
	// Обработчик полного тела получает само задание для повторной проверки scope после внешних вызовов.
	using RawCallback =
		std::function<void(RequestContext, Result<QByteArray>, HttpDiagnostic, const std::shared_ptr<Job> &)>;
	/** Ограничивает задания одного receiver, включая промежутки между страницами.
	 * После адресной отмены новый вызов того же receiver получает отдельный scope.
	 */
	struct ReceiverScope {
		// Отзывается до прерывания reply, чтобы сетевые сигналы не вызвали старый callback.
		bool cancelled = false;
		// Число завершённых транспортов, чей результат сейчас проходит обработчики диагностики и результата.
		int sentDeliveries = 0;
		// Подписка действует до отмены/уничтожения receiver либо освобождения последнего удерживаемого scope.
		QMetaObject::Connection destruction;
		/** Снимает подписку, когда карта и задания больше не удерживают scope. */
		~ReceiverScope() { QObject::disconnect(destruction); }
	};
	// Состояние одного запроса разделяется между очередью и захватами обработчиков через shared_ptr.
	// Job не владеет receiver и reply. reply принадлежит network; оба таймера являются его детьми.
	// finished отмечает конец транспорта, а не готовность результата: в reply ещё могут оставаться байты.
	// scheduled означает единственное уже поставленное продолжение drain и предотвращает двойное чтение.
	// context защищает смену выбора, serial дополнительно защищает явную отмену при том же выборе.
	struct Job {
		// Получатель ограничивает время жизни результата, но не удерживается заданием.
		QPointer<QObject> receiver;
		// Общий токен адресной отмены получателя; его удерживают также продолжения списков.
		std::shared_ptr<ReceiverScope> scope;
		// Необязательный однопроходный источник; после отправки удерживается также самим reply.
		std::shared_ptr<UploadSource> upload;
		// Необязательный обработчик реального прогресса отправки байтов upload.
		UploadProgressCallback progress;
		// Последнее сообщённое число отправленных байтов; одинаковые сигналы не продлевают простой.
		qint64 sentBytes = 0;
		// Возраст upload от постановки в очередь; обычные JSON-запросы его не используют.
		QElapsedTimer queuedElapsed;
		// Таймер общего срока, пока upload ожидает допуска; после отправки заменяется deadline reply.
		std::unique_ptr<QTimer> queueDeadline;
		// Непустой Asset отличает Preparation202 от независимого контракта Receipt202.
		QString preparationAssetId;
		// Исходный контекст выбранного подключения и Stream.
		RequestContext context;
		// Поколение отмены данного экземпляра клиента.
		quint64 serial = 0;
		// Относительный маршрут после /api/v1 с уже закодированными ID.
		QString path;
		// Параметры только этого запроса, в том числе фильтры страниц.
		QUrlQuery query;
		// Метод выбирается только типизированными методами; CustomOperation обозначает PATCH.
		QNetworkAccessManager::Operation method = QNetworkAccessManager::GetOperation;
		// Неизменяемый JSON исходной команды/конфигурации; не смешивается с накапливаемым ответом body.
		QByteArray requestBody;
		// Единственный успешный статус: чтение/PATCH/policy200, create201, принятие202 либо DELETE204.
		int expectedStatus = 200;
		// Зафиксированный общий срок в миллисекундах; изменение настройки не затрагивает очередь.
		int deadlineMs = 15000;
		// Зафиксированный срок простоя этого запроса в миллисекундах.
		int idleMs = 10000;
		// Измеряет транспорт после отправки, отдельно от очереди.
		QElapsedTimer elapsed;
		// Последний HTTP-статус, включая досрочный отказ по размеру или сроку.
		int status = 0;
		// Локальная ошибка кодирования передаётся без потери пути поля.
		std::optional<Error> inputError;
		// Локальная проверка параметров позволяет отклонить запрос до сетевого допуска.
		bool inputValid = true;
		// Обработчик полного тела после транспортной проверки.
		RawCallback callback;
		// Максимальный размер полного ответа в байтах.
		qsizetype cap = 0;
		// Накопленный ограниченный ответ, который никогда не пишется в журнал.
		QByteArray body;
		// Сетевой ответ принадлежит network; слабый указатель не продлевает его жизнь.
		QPointer<QNetworkReply> reply;
		// Таймер простоя, принадлежащий reply.
		QTimer *idle = nullptr;
		// Абсолютный срок отправленного запроса, принадлежащий reply.
		QTimer *deadline = nullptr;
		// finished отмечает EOF транспорта, но не опустошение буфера reply.
		bool finished = false;
		// Одно отложенное продолжение чтения уже стоит в GUI-очереди.
		bool scheduled = false;
	};
	// Заимствованный QObject клиента задаёт контекст сетевых и отложенных обработчиков.
	APIClient *owner;
	// Ограничения копируются при создании и не меняются для уже принятых заданий.
	Limits limits;
	// Получает только безопасные типизированные метаданные; не владеет клиентом.
	DiagnosticCallback diagnosticCallback;
	// Собственный менеджер с NoProxy владеет сетевыми ответами этого экземпляра клиента.
	QNetworkAccessManager network;
	// Проверенный HTTP(S) origin, не содержащий пути, параметров или учётных данных.
	QUrl origin;
	// Контекст, который должны иметь новые запросы и доставляемые результаты.
	RequestContext context;
	// Поколение отмены меняется даже при configure с тем же контекстом.
	quint64 serial = 0;
	// Необратимый запрет новых заданий после shutdown.
	bool stopped = false;
	// Ожидающие задания в порядке допуска, ограниченные maxQueuedRequests.
	QQueue<std::shared_ptr<Job>> queue;
	// Отправленные задания, занимающие не более maxActiveRequests слотов.
	QHash<QNetworkReply *, std::shared_ptr<Job>> active;
	// Живой receiver сохраняет общий scope; уничтожение и отмена удаляют запись без накопления старых токенов.
	QHash<QObject *, std::shared_ptr<ReceiverScope>> scopes;
	// owner задаёт Qt-владение менеджером, limits — начальные бюджеты; глобальная proxy-политика не меняется.
	Impl(APIClient *owner, Limits limits) : owner(owner), limits(limits), network(owner)
	{
		// Для явно выбранного локального сервера обходим системное определение прокси: WinHTTP может
		// заблокировать GUI ещё внутри get(), до запуска таймеров запроса. Настройки прокси OBS не меняем.
		network.setProxy(QNetworkProxy(QNetworkProxy::NoProxy));
	}
	/** Создаёт либо возвращает общий scope receiver; null или shutdown не принимают работу.
	 * Подписка уничтожения удаляет также очередные задания и отложенные обходы этого receiver.
	 */
	std::shared_ptr<ReceiverScope> receiverScope(QObject *receiver)
	{
		if (stopped || !receiver)
			return {};
		if (scopes.contains(receiver))
			return scopes.value(receiver);
		// scope живёт до отмены или уничтожения receiver, а weak не создаёт цикла с подпиской Qt.
		auto scope = std::make_shared<ReceiverScope>();
		const std::weak_ptr<ReceiverScope> weak = scope;
		scope->destruction = QObject::connect(receiver, &QObject::destroyed, owner, [this, receiver, weak] {
			// currentScope удерживает отзываемые задания во время abort; receiver используется только как ключ.
			if (auto currentScope = weak.lock()) {
				cancelScope(currentScope);
				if (scopes.value(receiver) == currentScope)
					scopes.remove(receiver);
				pump();
			}
		});
		scopes.insert(receiver, scope);
		return scope;
	}
	/** Отзывает scope, удаляет его очередь и прерывает только принадлежащие ему сетевые ответы.
	 * Возвращает факт передачи активного либо ещё доставляемого задания транспорту; обработчики результата подавляются.
	 * pump вызывается владельцем после завершения всей очистки, чтобы не отправить отменённый хвост.
	 */
	bool cancelScope(const std::shared_ptr<ReceiverScope> &scope)
	{
		scope->cancelled = true;
		// Предикат удаляет только задания данного scope, сохраняя порядок чужой очереди.
		queue.removeIf([&scope](const std::shared_ptr<Job> &job) { return job->scope == scope; });
		// jobs удерживает активные задания, пока active меняется перед abort.
		const auto jobs = active;
		// sent учитывает и завершённый транспорт, чей результат может быть отозван из внешнего обработчика диагностики.
		bool sent = scope->sentDeliveries > 0;
		// job обозначает один активный запрос; чужие scopes остаются нетронутыми.
		for (const auto &job : jobs) {
			if (job->scope != scope)
				continue;
			// reply заимствуется до deleteLater; обработчики клиента отключаются до abort.
			if (auto *reply = job->reply.data()) {
				sent = true;
				active.remove(reply);
				job->reply = nullptr;
				job->idle->stop();
				job->deadline->stop();
				QObject::disconnect(reply, nullptr, owner, nullptr);
				reply->abort();
				reply->deleteLater();
			}
		}
		return sent;
	}
	/** Отменяет задания текущего scope receiver, разрешая последующим вызовам создать новый scope. */
	bool cancelReceiver(QObject *receiver)
	{
		// scope копируется до удаления карты; запросы и продолжения удерживают тот же отозванный объект.
		const auto scope = scopes.take(receiver);
		if (!scope)
			return false;
		// sent сохраняет факт передачи до заполнения освободившихся слотов чужими запросами.
		const bool sent = cancelScope(scope);
		pump();
		return sent;
	}
	// Проверяет право выдавать результат, а не только наличие сетевого ответа. Проверка нужна также
	// для ошибок допуска, у которых reply вообще не создавался, и для продолжений обхода списка.
	bool current(const std::shared_ptr<Job> &job) const
	{
		// serial запрещает доставку и отложенных заданий того же RequestContext после явной отмены.
		return !stopped && job->serial == serial && job->context == context && job->receiver && job->scope &&
		       !job->scope->cancelled;
	}
	// Вызывает обработчик получателя прямо в текущем стеке. Тот может удалить owner, поэтому после
	// возврата из deliver обращаться к Impl можно лишь после проверки, что клиент ещё существует.
	void deliver(const std::shared_ptr<Job> &job, Result<QByteArray> result)
	{
		if (!current(job))
			return;
		// diagnostic не содержит строк из запроса или ответа; очередь измеряется до callback.
		HttpDiagnostic diagnostic;
		diagnostic.method = diagnosticMethod(job->method);
		diagnostic.route = routeType(job->path);
		diagnostic.status = job->status;
		diagnostic.durationMs = job->elapsed.isValid() ? job->elapsed.elapsed() : 0;
		diagnostic.receivedBytes = result.value ? result.value->size() : job->body.size();
		diagnostic.activeRequests = active.size();
		diagnostic.queuedRequests = queue.size();
		job->callback(job->context, std::move(result), diagnostic, job);
	}
	// Выдаёт финальную классификацию после DTO-декодирования; обработчик диагностики не получает серверные тексты.
	void report(HttpDiagnostic diagnostic, const std::optional<Error> &error)
	{
		if (error)
			diagnostic.error = error->kind;
		// callback удерживает текущий обработчик диагностики во время вызова, в том числе при его явной замене.
		const auto callback = diagnosticCallback;
		if (callback)
			callback(diagnostic);
	}
	// Декодирует успешный JSON до диагностики и передаёт ровно один окончательный результат callback.
	// Ошибка DTO после ответа имеет requestSent=true; callback может уничтожить owner.
	// requireIdentity дополнительно требует непустой Record ID; expectedId фиксирует адресуемую PATCH-сущность.
	// job сохраняет scope/receiver для повторной проверки после внешнего обработчика диагностики.
	template<class T>
	void decoded(const std::shared_ptr<Job> &job, RequestContext context, Result<QByteArray> rawResult,
		     HttpDiagnostic diagnostic, const Callback<T> &callback, const QString &expectedId = {},
		     bool requireIdentity = false)
	{
		// result содержит только полностью проверенный DTO либо исходную классифицированную ошибку.
		auto result = rawResult ? decode<T>(*rawResult.value) : Result<T>::failure(*rawResult.error);
		if constexpr (std::is_base_of_v<Record, T>) {
			if (result && requireIdentity &&
			    (result.value->id.isEmpty() || (!expectedId.isEmpty() && result.value->id != expectedId)))
				result = Result<T>::failure(makeError(ErrorKind::Contract, diagnostic.status));
		}
		if (rawResult && result.error)
			result.error->requestSent = true;
		// guard защищает повторную проверку после обработчика диагностики: он может отменить receiver или сменить контекст.
		const QPointer<APIClient> guard = owner;
		report(diagnostic, result.error);
		if (guard && current(job))
			callback(context, std::move(result));
	}
	// Завершает активный job результатом result не более одного раза; abort прерывает незавершённый транспорт.
	// До callback удаляет запись из active и отключает
	// сигналы, чтобы abort, timeout и finished не завершили его повторно. abort нужен при досрочном
	// отказе; нормальный EOF уже завершил транспорт. Освободившийся слот заполняется после callback,
	// с учётом возможной отмены, перенастройки или уничтожения клиента внутри него.
	void complete(const std::shared_ptr<Job> &job, Result<QByteArray> result, bool abort = false)
	{
		// reply — ответ завершаемого активного задания; отсутствие записи означает повторный сигнал.
		auto *reply = job->reply.data();
		if (!reply || !active.contains(reply))
			return;
		active.remove(reply);
		job->status = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
		if (result.error) {
			result.error->requestSent = true;
			// При досрочном отказе сохраняется только ограниченная диагностика полученного HTTP-отклонения.
			// Успешные HTTP-тела и исходящие payload в этот канал не копируются.
			if (job->status >= 400 && result.error->diagnosticBody.isEmpty())
				result.error->diagnosticBody = job->body.first(std::min(job->cap, job->body.size()));
		}
		job->reply = nullptr;
		job->idle->stop();
		job->deadline->stop();
		QObject::disconnect(reply, nullptr, owner, nullptr);
		if (abort)
			reply->abort();
		reply->deleteLater();
		// Пользовательский обработчик может удалить клиент; очередь продолжается только при живом владельце.
		const QPointer<APIClient> guard = owner;
		// deliveryJob удерживает callback и источник, даже если пользователь удалит клиент и его reply.
		const auto deliveryJob = job;
		// deliveryScope остаётся жив после удаления клиента: счётчик снимается без обращения к Impl.
		const auto deliveryScope = deliveryJob->scope;
		++deliveryScope->sentDeliveries;
		deliver(deliveryJob, std::move(result));
		--deliveryScope->sentDeliveries;
		if (guard)
			pump();
	}
	// Отменяет активные и ожидающие задания. Новое значение serial запрещает выдачу результатов
	// также из уже запланированных singleShot. Очередь очищается без уведомлений; active очищается
	// до abort, поскольку прерывание может породить сетевые сигналы. Новые задания допустимы до shutdown().
	void cancel()
	{
		++serial;
		// scope отзывается до очистки заданий, включая уже отложенные продолжения страниц.
		for (const auto &scope : scopes)
			scope->cancelled = true;
		scopes.clear();
		queue.clear();
		// jobs удерживает задания во время очистки active и вызовов abort.
		const auto jobs = active;
		active.clear();
		// job — одно отменяемое задание; reply заимствуется только до deleteLater.
		for (const auto &job : jobs)
			if (auto *reply = job->reply.data()) {
				job->reply = nullptr;
				job->idle->stop();
				job->deadline->stop();
				QObject::disconnect(reply, nullptr, owner, nullptr);
				reply->abort();
				reply->deleteLater();
			}
	}

	// Принимает относительный путь внутри /api/v1 и предел полного тела. Ошибки origin/лимитов/очереди
	// выдаются отложенно с теми же проверками актуальности, что и сетевой результат. При остановленном
	// клиенте или null receiver операция не создаётся. Уничтожение receiver сразу освобождает его
	// активные слоты, удаляет очередь и отзывает продолжения страниц через общий scope.
	// path/query задают маршрут, receiver ограничивает callback, cap — байты ответа; method/requestBody
	// и expectedStatus задают транспортный контракт. inputValid запрещает локально неверные параметры;
	// inputError сохраняет причину отказа. upload — открытый последовательный файл, preparationAssetId
	// отличает Preparation202 от Receipt202, progress получает только рост отправленных байтов.
	void raw(QString path, QUrlQuery query, QObject *receiver, RawCallback callback, qsizetype cap,
		 QNetworkAccessManager::Operation method = QNetworkAccessManager::GetOperation,
		 QByteArray requestBody = {}, int expectedStatus = 200, bool inputValid = true,
		 std::optional<Error> inputError = std::nullopt, std::shared_ptr<UploadSource> upload = {},
		 QString preparationAssetId = {}, UploadProgressCallback progress = {})
	{
		if (stopped || !receiver)
			return;
		// job хранит неизменные параметры, контекст и обработчик до завершения либо отмены.
		auto job = std::make_shared<Job>();
		job->receiver = receiver;
		job->scope = receiverScope(receiver);
		job->upload = std::move(upload);
		job->preparationAssetId = std::move(preparationAssetId);
		job->progress = std::move(progress);
		job->context = context;
		job->serial = serial;
		job->path = std::move(path);
		job->query = std::move(query);
		job->method = method;
		job->requestBody = std::move(requestBody);
		job->expectedStatus = expectedStatus;
		job->inputValid = inputValid;
		job->inputError = std::move(inputError);
		job->deadlineMs = std::max(1, limits.httpDeadlineMs);
		job->idleMs = std::max(1, limits.httpIdleMs);
		if (job->upload) {
			job->deadlineMs = std::max(1, limits.uploadDeadlineMs);
			job->idleMs = std::max(1, limits.uploadIdleMs);
			job->queuedElapsed.start();
		}
		job->callback = std::move(callback);
		job->cap = cap;
		if (!job->inputValid || origin.isEmpty() || limits.maxActiveRequests <= 0 || limits.readBytes <= 0 ||
		    cap <= 0) {
			job->upload.reset();
			// Отложенный отказ использует тот же context/serial/receiver, что и настоящий ответ.
			QTimer::singleShot(0, owner, [this, job] {
				deliver(job, Result<QByteArray>::failure(
						     job->inputError.value_or(makeError(ErrorKind::Contract))));
			});
			return;
		}
		if (job->requestBody.size() > limits.restBodyBytes) {
			job->upload.reset();
			// Превышение исходящего документа отсекается до передачи транспорту; результат записи известен локально.
			QTimer::singleShot(0, owner, [this, job] {
				deliver(job, Result<QByteArray>::failure(makeError(ErrorKind::Limit)));
			});
			return;
		}
		if (active.size() >= limits.maxActiveRequests && queue.size() >= limits.maxQueuedRequests) {
			job->upload.reset();
			// Обработчик отказа очереди не получает дополнительных прав на доставку после отмены.
			QTimer::singleShot(0, owner, [this, job] {
				deliver(job, Result<QByteArray>::failure(makeError(ErrorKind::Limit)));
			});
			return;
		}
		if (job->upload) {
			job->queueDeadline = std::make_unique<QTimer>();
			job->queueDeadline->setSingleShot(true);
			// weak не удерживает Job через принадлежащий ему таймер.
			const std::weak_ptr<Job> weak = job;
			// Таймер очереди завершает только ещё не отправленное задание; requestSent остаётся false.
			QObject::connect(job->queueDeadline.get(), &QTimer::timeout, owner, [this, weak] {
				// queued удерживает просроченное задание на время удаления очереди и callback.
				const auto queued = weak.lock();
				if (!queued || queued->reply || !queue.removeOne(queued))
					return;
				queued->upload.reset();
				// guard не позволяет продолжить pump после удаления клиента из обработчика результата.
				const QPointer<APIClient> guard = owner;
				deliver(queued, Result<QByteArray>::failure(makeError(ErrorKind::Timeout)));
				if (guard)
					pump();
			});
			job->queueDeadline->start(job->deadlineMs);
		}
		queue.enqueue(std::move(job));
		pump();
	}
	// Запускает ожидающие актуальные задания до maxActiveRequests. Порядок очереди сохраняется,
	// устаревшие записи пропускаются. Каждый ответ получает собственные idle/deadline таймеры:
	// реальный обмен байтами продлевает только idle; общий срок upload включает ожидание в queue.
	void pump()
	{
		while (!stopped && active.size() < limits.maxActiveRequests && !queue.isEmpty()) {
			// job — старейший кандидат; отозванные либо лишившиеся receiver задания не отправляются.
			auto job = queue.dequeue();
			if (!current(job))
				continue;
			// remainingMs учитывает всю очередь upload; JSON сохраняет прежний срок после отправки.
			const qint64 remainingMs = job->upload ? job->deadlineMs - job->queuedElapsed.elapsed()
							       : job->deadlineMs;
			if (job->queueDeadline)
				job->queueDeadline->stop();
			if (remainingMs <= 0) {
				job->upload.reset();
				// guard защищает цикл допуска от удаления клиента внутри локального callback.
				const QPointer<APIClient> guard = owner;
				deliver(job, Result<QByteArray>::failure(makeError(ErrorKind::Timeout)));
				if (!guard)
					return;
				continue;
			}
			// url строится только из проверенного origin и типизированного локального маршрута.
			QUrl url = origin;
			url.setPath(QStringLiteral("/api/v1") + job->path, QUrl::StrictMode);
			url.setQuery(job->query);
			// request сохраняет ручные редиректы и штатную проверку TLS Qt.
			QNetworkRequest request(url);
			request.setRawHeader("Accept", "application/json");
			request.setAttribute(QNetworkRequest::RedirectPolicyAttribute,
					     QNetworkRequest::ManualRedirectPolicy);
			if (job->upload) {
				request.setHeader(QNetworkRequest::ContentTypeHeader,
						  QStringLiteral("application/octet-stream"));
				request.setHeader(QNetworkRequest::ContentLengthHeader, job->upload->size());
				request.setAttribute(QNetworkRequest::DoNotBufferUploadDataAttribute, true);
				request.setAttribute(QNetworkRequest::EmitAllUploadProgressSignalsAttribute, true);
			} else if (job->method == QNetworkAccessManager::PostOperation ||
				   job->method == QNetworkAccessManager::CustomOperation ||
				   job->method == QNetworkAccessManager::PutOperation) {
				request.setHeader(QNetworkRequest::ContentTypeHeader,
						  QStringLiteral("application/json"));
			}
			// reply принадлежит network; последовательный источник не допускает перемотку для повторной передачи.
			job->elapsed.start();
			auto *reply = job->upload ? network.put(request, job->upload.get())
				      : job->method == QNetworkAccessManager::PutOperation
					      ? network.put(request, job->requestBody)
				      : job->method == QNetworkAccessManager::PostOperation
					      ? network.post(request, job->requestBody)
				      : job->method == QNetworkAccessManager::CustomOperation
					      ? network.sendCustomRequest(request, QByteArrayLiteral("PATCH"),
									  job->requestBody)
				      : job->method == QNetworkAccessManager::DeleteOperation
					      ? network.deleteResource(request)
					      : network.get(request);
			job->reply = reply;
			active.insert(reply, job);
			reply->setReadBufferSize(limits.readBytes);
			job->deadline = new QTimer(reply);
			job->deadline->setSingleShot(true);
			job->idle = new QTimer(reply);
			job->idle->setSingleShot(true);
			// timeout обслуживает оба срока и завершает активный job не более одного раза.
			auto timeout = [this, job] {
				complete(job, Result<QByteArray>::failure(makeError(ErrorKind::Timeout)), true);
			};
			QObject::connect(job->deadline, &QTimer::timeout, owner, timeout);
			QObject::connect(job->idle, &QTimer::timeout, owner, timeout);
			job->deadline->start(int(remainingMs));
			job->idle->start(job->idleMs);
			if (job->upload) {
				// source остаётся жив до удаления reply, даже если Job уже снят из active.
				const auto source = job->upload;
				// Завершение транспорта закрывает файл; context reply сохраняет эту очистку при cancelAll.
				QObject::connect(reply, &QNetworkReply::finished, reply, [source] { source->close(); });
				// Реальный рост sent продлевает простой. Внешний callback может удалить клиент/receiver:
				// после него обработчик не обращается ни к Impl, ни к reply.
				QObject::connect(
					reply, &QNetworkReply::uploadProgress, owner, [this, job](qint64 sent, qint64) {
						if (!current(job) || !job->reply || !active.contains(job->reply))
							return;
						// boundedSent ограничивает сообщение захваченным размером тела, без служебных байтов HTTP.
						const auto boundedSent =
							std::clamp(sent, qint64(0), job->upload->size());
						if (boundedSent <= job->sentBytes)
							return;
						job->sentBytes = boundedSent;
						job->idle->start(job->idleMs);
						// progressJob удерживает источник при удалении клиента внутри progress.
						const auto progressJob = job;
						// callback удерживает собственную копию обработчика на время повторного входа.
						const auto callback = progressJob->progress;
						if (callback)
							callback(progressJob->context, boundedSent,
								 progressJob->upload->size());
					});
			}
			// readyRead запускает ограниченное чтение; finished дополнительно фиксирует EOF транспорта.
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
		// reply действителен только пока job занимает активный слот.
		auto *reply = job->reply.data();
		if (!reply || !active.contains(reply))
			return;
		if (!current(job)) {
			complete(job, Result<QByteArray>::failure(makeError(ErrorKind::Cancelled)), true);
			return;
		}
		// room — остаток разрешённого полного тела; bytes содержит следующую ограниченную порцию.
		const auto room = job->cap - job->body.size();
		// Дополнительный байт позволяет обнаружить превышение, не выдавая усечённый ответ как полный.
		const auto bytes = reply->read(std::min(limits.readBytes, room + 1));
		if (!bytes.isEmpty()) {
			job->body.append(bytes);
			job->idle->start(job->idleMs);
		}
		if (job->body.size() > job->cap) {
			complete(job, Result<QByteArray>::failure(makeError(ErrorKind::Limit)), true);
			return;
		}
		if (reply->bytesAvailable() > 0) {
			// Большое тело читается несколькими оборотами цикла событий, сохраняя доступность интерфейса.
			if (!job->scheduled) {
				job->scheduled = true;
				// Продолжение снимает scheduled до новой порции; active/serial защищают его после отмены.
				QTimer::singleShot(0, owner, [this, job] {
					job->scheduled = false;
					drain(job);
				});
			}
			return;
		}
		if (!job->finished)
			return;
		// status — HTTP-результат; media — MIME без параметров, проверяемый независимо от ошибки Qt.
		const int status = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
		const auto media = reply->header(QNetworkRequest::ContentTypeHeader)
					   .toString()
					   .section(QLatin1Char(';'), 0, 0)
					   .trimmed()
					   .toLower();
		if (status >= 400) {
			// HTTP-ошибка обрабатывается до общей ошибки транспорта, которую Qt также выставляет для таких ответов.
			// error хранит ограниченное исходное тело даже при неверном MIME или повреждённом Problem.
			auto error = decodeProblem(status, job->body);
			if (media != QStringLiteral("application/problem+json"))
				error.problem.reset();
			complete(job, Result<QByteArray>::failure(std::move(error)));
			return;
		}
		if (reply->error() != QNetworkReply::NoError) {
			complete(job, Result<QByteArray>::failure(
					      makeError(reply->error() == QNetworkReply::SslHandshakeFailedError
								? ErrorKind::Tls
								: ErrorKind::Transport,
							status)));
			return;
		}
		if (status != job->expectedStatus || (status != 204 && media != QStringLiteral("application/json")) ||
		    (status == 204 && !job->body.isEmpty())) {
			complete(job, Result<QByteArray>::failure(makeError(ErrorKind::Contract, status)));
			return;
		}
		if (job->upload && !job->upload->fullyRead()) {
			complete(job, Result<QByteArray>::failure(makeError(ErrorKind::Transport, status)));
			return;
		}
		if (job->expectedStatus == 202 && !job->preparationAssetId.isEmpty()) {
			// preparation связывает принятые IDs с намерением; обязательная ссылка не используется для перехода.
			const auto preparation = decode<Preparation>(job->body);
			if (!preparation || preparation.value->assetId != job->preparationAssetId ||
			    !reply->hasRawHeader("Location") ||
			    !validPreparationLocation(reply->url(), reply->rawHeader("Location"), *preparation.value)) {
				complete(job, Result<QByteArray>::failure(makeError(ErrorKind::Contract, status)));
				return;
			}
		} else if (job->expectedStatus == 202) {
			// receipt нужен здесь только для связи Location с id; типизированный callback декодирует DTO.
			const auto receipt = decode<Receipt>(job->body);
			if (!receipt) {
				complete(job, Result<QByteArray>::failure(*receipt.error));
				return;
			}
			// location может отсутствовать: тогда источником command ID остаётся квитанция.
			// Любая переданная ссылка проверяется, но никогда не становится адресом сетевого запроса.
			const auto location = reply->rawHeader("Location");
			if (receipt.value->id.isEmpty() ||
			    (reply->hasRawHeader("Location") &&
			     !validCommandLocation(reply->url(), location, receipt.value->id))) {
				complete(job, Result<QByteArray>::failure(makeError(ErrorKind::Contract, status)));
				return;
			}
		}
		complete(job, Result<QByteArray>::success(std::move(job->body)));
	}
	/** Проверяет обязательный Location точного принятого поколения в исходном origin.
	 * Путь строится только из проверенных IDs; query, fragment и userinfo отвергаются.
	 */
	bool validPreparationLocation(const QUrl &requestUrl, const QByteArray &location,
				      const Preparation &preparation) const
	{
		// reference сохраняет строгую проверку escape-последовательностей относительной либо абсолютной ссылки.
		const auto reference = QUrl::fromEncoded(location, QUrl::StrictMode);
		// target разрешается только для сравнения; последующий GET строится типизированным wrapper.
		const auto target = requestUrl.resolved(reference);
		// expected содержит единственный допустимый путь точного поколения.
		QUrl expected = origin;
		expected.setPath(QStringLiteral("/api/v1/fallback-assets/") + segment(preparation.assetId) +
					 QStringLiteral("/generations/") + segment(preparation.generationId),
				 QUrl::StrictMode);
		return !location.isEmpty() && reference.isValid() && target.isValid() && target.userInfo().isEmpty() &&
		       !target.authority(QUrl::FullyEncoded).contains(QLatin1Char('@')) && !target.hasQuery() &&
		       !target.hasFragment() && target.scheme() == origin.scheme() && target.host() == origin.host() &&
		       target.port(target.scheme() == QStringLiteral("https") ? 443 : 80) ==
			       origin.port(origin.scheme() == QStringLiteral("https") ? 443 : 80) &&
		       target.path(QUrl::FullyEncoded) == expected.path(QUrl::FullyEncoded);
	}
	/**
	 * Проверяет location относительно исходного requestUrl: тот же origin и точный commandId.
	 * Параметры, fragment, userinfo и другой путь отвергаются; разрешения на переход метод не даёт.
	 */
	bool validCommandLocation(const QUrl &requestUrl, const QByteArray &location, const QString &commandId) const
	{
		// reference разбирается строго, чтобы неверные escape-последовательности не исправлялись молча.
		const auto reference = QUrl::fromEncoded(location, QUrl::StrictMode);
		// target допускает абсолютную либо относительную ссылку, разрешённую относительно исходного POST.
		const auto target = requestUrl.resolved(reference);
		// expected содержит единственный допустимый путь квитанции с непрозрачным commandId.
		QUrl expected = origin;
		expected.setPath(QStringLiteral("/api/v1/commands/") + segment(commandId), QUrl::StrictMode);
		return reference.isValid() && target.isValid() && target.userInfo().isEmpty() && !target.hasQuery() &&
		       !target.hasFragment() && target.scheme() == origin.scheme() && target.host() == origin.host() &&
		       target.port(target.scheme() == QStringLiteral("https") ? 443 : 80) ==
			       origin.port(origin.scheme() == QStringLiteral("https") ? 443 : 80) &&
		       target.path(QUrl::FullyEncoded) == expected.path(QUrl::FullyEncoded);
	}
	/**
	 * Сериализует payload одной команды path; valid отражает проверенные вызывающим wrapper ID.
	 * receiver ограничивает callback квитанции; valid не допускает локально неверные параметры в сеть.
	 */
	void post(QString path, JsonValue::Object payload, bool valid, QObject *receiver, Callback<Receipt> callback)
	{
		// bytes содержит полный типизированный JSON; пустой результат означает неверную строку или предел writer.
		const auto bytes = serializeJson(JsonValue(std::move(payload)));
		raw(
			std::move(path), {}, receiver,
			// context и result — исходный контекст и полное тело; ошибки передаются без журналирования payload.
			[this, callback = std::move(callback)](RequestContext context, Result<QByteArray> result,
							       HttpDiagnostic diagnostic, const auto &job) {
				decoded<Receipt>(job, context, std::move(result), diagnostic, callback);
			},
			limits.restBodyBytes, QNetworkAccessManager::PostOperation, bytes, 202,
			valid && !bytes.isEmpty());
	}
	// Преобразование DTO выполняется только для полного успешного тела. decode создаёт новый объект,
	// поэтому ошибка в середине документа не публикует частично заполненную сущность.
	// path/query задают GET, receiver ограничивает callback; cap=0 выбирает стандартный предел REST.
	template<class T>
	void get(QString path, QObject *receiver, Callback<T> callback, QUrlQuery query = {}, qsizetype cap = 0)
	{
		raw(
			std::move(path), std::move(query), receiver,
			// context и result — контекст исходного GET и его полное тело либо классифицированная ошибка.
			[this, callback = std::move(callback)](RequestContext context, Result<QByteArray> result,
							       HttpDiagnostic diagnostic, const auto &job) {
				decoded<T>(job, context, std::move(result), diagnostic, callback);
			},
			cap ? cap : limits.restBodyBytes);
	}
	// Отправляет типизированный create/PATCH без повтора; локальная ошибка кодирования не выходит в сеть.
	// path — фиксированный маршрут, request — DTO намерения, valid проверяет непустой ID типизированного метода.
	// method/status различают create201 и PATCH200; expectedId связывает результат PATCH с исходным ID.
	template<class T, class Request>
	void mutate(QString path, Request request, QNetworkAccessManager::Operation method, int status, bool valid,
		    QObject *receiver, Callback<T> callback, QString expectedId = {})
	{
		// encoded содержит весь исходящий JSON либо безопасную ошибку известного поля.
		auto encoded = encodeRequest(request);
		// bytes не позволяет отправить пустое тело, если writer отверг Unicode или структурный предел.
		const auto bytes = encoded ? serializeJson(*encoded.value) : QByteArray{};
		raw(
			std::move(path), {}, receiver,
			// Обработчик проверяет непустой ID создания либо expectedId изменения до выдачи DTO callback.
			[this, callback = std::move(callback),
			 expectedId = std::move(expectedId)](RequestContext context, Result<QByteArray> result,
							     HttpDiagnostic diagnostic, const auto &job) {
				decoded<T>(job, context, std::move(result), diagnostic, callback, expectedId, true);
			},
			limits.restBodyBytes, method, bytes, status, valid && bool(encoded) && !bytes.isEmpty(),
			encoded.error);
	}
	// Передаёт expectedRevision только в DELETE query и принимает 204 без JSON/MIME.
	void remove(QString path, quint64 expectedRevision, bool valid, QObject *receiver, Callback<Empty> callback)
	{
		// query содержит точную десятичную строку, безопасную для обычного кодирования URL.
		QUrlQuery query;
		query.addQueryItem(QStringLiteral("expected_revision"), QString::number(expectedRevision));
		raw(
			std::move(path), std::move(query), receiver,
			// Обработчик создаёт пустой успешный результат; тело никогда не разбирается как Receipt.
			[this, callback = std::move(callback)](RequestContext context, Result<QByteArray> rawResult,
							       HttpDiagnostic diagnostic, const auto &job) {
				// result различает подтверждённый 204 и классифицированный сетевой/серверный отказ.
				auto result = rawResult ? Result<Empty>::success({})
							: Result<Empty>::failure(*rawResult.error);
				// guard не позволяет выдать результат после адресной отмены/удаления receiver из обработчика диагностики.
				const QPointer<APIClient> guard = owner;
				report(diagnostic, result.error);
				if (guard && current(job))
					callback(context, std::move(result));
			},
			limits.restBodyBytes, QNetworkAccessManager::DeleteOperation, {}, 204,
			valid && expectedRevision > 0);
	}
	// Читает одну страницу path с фильтрами query; результат callback ограничен жизнью receiver.
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
		// Маршрут списка, неизменный между страницами.
		QString path;
		// Исходные фильтры и cursor следующей страницы.
		ListQuery query;
		// Получатель ограничивает время жизни всего обхода.
		QPointer<QObject> receiver;
		// Тот же scope, что у страниц: адресная отмена действует и между последовательными GET.
		std::shared_ptr<ReceiverScope> scope;
		// Получает накопленную коллекцию либо ошибку одной из страниц.
		Callback<Collection<T>> callback;
		// Принятые элементы, объём страниц и признак полноты.
		Collection<T> result;
		// Уже встреченные cursor для обнаружения циклической пагинации.
		QSet<QString> cursors;
		// Число целиком принятых страниц.
		int pages = 0;
		// Поколение отмены в момент начала обхода.
		quint64 serial = 0;
		// Контекст выбора в момент начала обхода.
		RequestContext context;
	};
	// Выполняет ровно один шаг обхода. Общий бюджет проверяется до GET и перед принятием страницы;
	// размер отдельного ответа независимо ограничивает raw. Предел элементов может принять часть
	// страницы, но это заведомо неполный результат. Следующая страница ставится через singleShot,
	// чтобы не наращивать стек и дать обработать GUI/отмену между страницами.
	template<class T> void walkNext(std::shared_ptr<Walk<T>> walk)
	{
		if (walk->serial != serial || walk->context != context || !walk->receiver || stopped || !walk->scope ||
		    walk->scope->cancelled)
			return;
		if (walk->pages >= limits.maxPages || walk->result.items.size() >= limits.maxItems ||
		    walk->result.bytes >= limits.pageWalkBytes) {
			// Общий бюджет возвращает известную часть с complete=false; ошибка отдельного ответа остаётся ошибкой.
			walk->callback(walk->context, Result<Collection<T>>::success(std::move(walk->result)));
			return;
		}
		raw(
			walk->path, queryParameters(walk->query), walk->receiver,
			// ctx и rawResult относятся к одной странице; обработчик принимает её целиком по бюджету байтов.
			[this, walk](RequestContext ctx, Result<QByteArray> rawResult, HttpDiagnostic diagnostic,
				     const auto &job) {
				// guard защищает обход после внешнего обработчика диагностики, который может отменить receiver либо удалить клиент.
				const QPointer<APIClient> guard = owner;
				if (!rawResult) {
					report(diagnostic, rawResult.error);
					if (!guard || !current(job))
						return;
					walk->callback(ctx, Result<Collection<T>>::failure(*rawResult.error));
					return;
				}
				// remaining — остаток общего бюджета исходных JSON-байтов всех страниц.
				const auto remaining = limits.pageWalkBytes - walk->result.bytes;
				if (rawResult.value->size() > remaining) {
					report(diagnostic, {});
					if (!guard || !current(job))
						return;
					// Страница либо принимается целиком по объёму, либо сохраняются только предыдущие страницы.
					walk->callback(ctx, Result<Collection<T>>::success(std::move(walk->result)));
					return;
				}
				// result содержит проверенную страницу без частично декодированных DTO.
				auto result = decode<Page<T>>(*rawResult.value);
				if (result.error)
					result.error->requestSent = true;
				report(diagnostic, result.error);
				if (!guard || !current(job))
					return;
				if (!result) {
					walk->callback(ctx, Result<Collection<T>>::failure(*result.error));
					return;
				}
				++walk->pages;
				walk->result.bytes += rawResult.value->size();
				// remainingItems — остаток числа элементов, count — допустимая часть принятой страницы.
				const auto remainingItems =
					std::max<qsizetype>(0, limits.maxItems - walk->result.items.size());
				const auto count = std::min<qsizetype>(remainingItems, result.value->items.size());
				// i указывает переносимый элемент страницы; остальные элементы не объявляются полученными.
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
				// Продолжение выполняет следующий шаг только при актуальных поколении и receiver.
				QTimer::singleShot(0, owner, [this, walk] { walkNext(walk); });
			},
			limits.restBodyBytes);
	}
	// Начальный cursor тоже участвует в обнаружении циклов. walkNext вызывается непосредственно:
	// если бюджет уже исчерпан, callback выполнится в этом стеке, как описано у публичного getAll<Name>.
	// path/query задают список и исходные фильтры; receiver ограничивает callback всего обхода.
	template<class T> void all(QString path, QObject *receiver, Callback<Collection<T>> callback, ListQuery query)
	{
		// walk сохраняет параметры и накопленную коллекцию между асинхронными страницами.
		auto walk = std::make_shared<Walk<T>>();
		walk->path = std::move(path);
		walk->query = std::move(query);
		walk->receiver = receiver;
		walk->scope = receiverScope(receiver);
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
// Адресная отмена отзывает и ответы, и продолжения страниц получателя, сохраняя чужую работу.
bool APIClient::cancelRequests(QObject *receiver)
{
	return receiver && impl_->cancelReceiver(receiver);
}
void APIClient::shutdown()
{
	if (impl_->stopped)
		return;
	impl_->stopped = true;
	impl_->cancel();
}
// Изменяет только сроки новых Job; уже сохранённые deadlineMs/idleMs не переписываются.
void APIClient::setHttpTimeoutMs(int timeoutMs)
{
	impl_->limits.httpDeadlineMs = std::max(1, timeoutMs);
	impl_->limits.httpIdleMs = std::min(10000, impl_->limits.httpDeadlineMs);
}
// Заменяет sink в потоке клиента; метаданные не включают строки запроса/ответа.
void APIClient::setDiagnosticCallback(DiagnosticCallback callback)
{
	impl_->diagnosticCallback = std::move(callback);
}

// Создаёт Stream; request проверяется до передачи транспорту, receiver ограничивает callback.
void APIClient::createStream(StreamCreate request, QObject *receiver, Callback<Stream> callback)
{
	impl_->mutate<Stream>(QStringLiteral("/streams"), std::move(request), QNetworkAccessManager::PostOperation, 201,
			      true, receiver, std::move(callback));
}
// Изменяет точный id Stream по ревизии request; callback получает проверенный полный DTO.
void APIClient::patchStream(QString id, StreamPatch request, QObject *receiver, Callback<Stream> callback)
{
	impl_->mutate<Stream>(QStringLiteral("/streams/") + segment(id), std::move(request),
			      QNetworkAccessManager::CustomOperation, 200, !id.isEmpty(), receiver, std::move(callback),
			      id);
}
// Удаляет точный id Stream по expectedRevision; receiver ограничивает пустой результат 204.
void APIClient::deleteStream(QString id, quint64 expectedRevision, QObject *receiver, Callback<Empty> callback)
{
	impl_->remove(QStringLiteral("/streams/") + segment(id), expectedRevision, !id.isEmpty(), receiver,
		      std::move(callback));
}

// Создаёт Destination; request проверяется до передачи транспорту, receiver ограничивает callback.
void APIClient::createDestination(DestinationCreate request, QObject *receiver, Callback<Destination> callback)
{
	impl_->mutate<Destination>(QStringLiteral("/destinations"), std::move(request),
				   QNetworkAccessManager::PostOperation, 201, true, receiver, std::move(callback));
}
// Изменяет точный id Destination по ревизии request; callback получает проверенный полный DTO.
void APIClient::patchDestination(QString id, DestinationPatch request, QObject *receiver,
				 Callback<Destination> callback)
{
	impl_->mutate<Destination>(QStringLiteral("/destinations/") + segment(id), std::move(request),
				   QNetworkAccessManager::CustomOperation, 200, !id.isEmpty(), receiver,
				   std::move(callback), id);
}
// Удаляет точный id Destination по expectedRevision; receiver ограничивает пустой результат 204.
void APIClient::deleteDestination(QString id, quint64 expectedRevision, QObject *receiver, Callback<Empty> callback)
{
	impl_->remove(QStringLiteral("/destinations/") + segment(id), expectedRevision, !id.isEmpty(), receiver,
		      std::move(callback));
}

// Создаёт Group; request проверяется до передачи транспорту, receiver ограничивает callback.
void APIClient::createGroup(GroupCreate request, QObject *receiver, Callback<Group> callback)
{
	impl_->mutate<Group>(QStringLiteral("/transcode-groups"), std::move(request),
			     QNetworkAccessManager::PostOperation, 201, true, receiver, std::move(callback));
}
// Изменяет точный id Group по ревизии request; callback получает проверенный полный DTO.
void APIClient::patchGroup(QString id, GroupPatch request, QObject *receiver, Callback<Group> callback)
{
	impl_->mutate<Group>(QStringLiteral("/transcode-groups/") + segment(id), std::move(request),
			     QNetworkAccessManager::CustomOperation, 200, !id.isEmpty(), receiver, std::move(callback),
			     id);
}
// Удаляет точный id Group по expectedRevision; receiver ограничивает пустой результат 204.
void APIClient::deleteGroup(QString id, quint64 expectedRevision, QObject *receiver, Callback<Empty> callback)
{
	impl_->remove(QStringLiteral("/transcode-groups/") + segment(id), expectedRevision, !id.isEmpty(), receiver,
		      std::move(callback));
}

// Создаёт Profile; request проверяется до передачи транспорту, receiver ограничивает callback.
void APIClient::createProfile(ProfileCreate request, QObject *receiver, Callback<Profile> callback)
{
	impl_->mutate<Profile>(QStringLiteral("/transcode-profiles"), std::move(request),
			       QNetworkAccessManager::PostOperation, 201, true, receiver, std::move(callback));
}
// Изменяет точный id Profile по ревизии request; callback получает проверенный полный DTO.
void APIClient::patchProfile(QString id, ProfilePatch request, QObject *receiver, Callback<Profile> callback)
{
	impl_->mutate<Profile>(QStringLiteral("/transcode-profiles/") + segment(id), std::move(request),
			       QNetworkAccessManager::CustomOperation, 200, !id.isEmpty(), receiver,
			       std::move(callback), id);
}
// Удаляет точный id Profile по expectedRevision; receiver ограничивает пустой результат 204.
void APIClient::deleteProfile(QString id, quint64 expectedRevision, QObject *receiver, Callback<Empty> callback)
{
	impl_->remove(QStringLiteral("/transcode-profiles/") + segment(id), expectedRevision, !id.isEmpty(), receiver,
		      std::move(callback));
}

// Создаёт именованный Asset без загрузки файла; callback получает проверенный DTO201.
void APIClient::createAsset(AssetCreate request, QObject *receiver, Callback<Asset> callback)
{
	impl_->mutate<Asset>(QStringLiteral("/fallback-assets"), std::move(request),
			     QNetworkAccessManager::PostOperation, 201, true, receiver, std::move(callback));
}
// Удаляет точный Asset по положительной ревизии через query, без скрытой каскадной операции.
void APIClient::deleteAsset(QString id, quint64 expectedRevision, QObject *receiver, Callback<Empty> callback)
{
	impl_->remove(QStringLiteral("/fallback-assets/") + segment(id), expectedRevision, !id.isEmpty(), receiver,
		      std::move(callback));
}
/** Открывает filePath и передаёт захваченный размер один раз как бинарный PUT.
 * assetId/revision/kind проверяются до HTTP; ошибку открытия получает только живой receiver.
 * progress сообщает передачу байтов, callback — проверенное принятие пары Asset/Generation.
 */
void APIClient::uploadAsset(QString assetId, quint64 expectedRevision, QString kind, QString filePath,
			    QObject *receiver, Callback<Preparation> callback, UploadProgressCallback progress)
{
	if (impl_->stopped || !receiver)
		return;
	// valid не допускает неизвестный kind, пустой ID или нулевую ревизию к транспорту.
	const bool valid = !assetId.isEmpty() && expectedRevision > 0 &&
			   (kind == QStringLiteral("image") || kind == QStringLiteral("video"));
	// source держит открытый файл до конца транспорта; ошибка не раскрывает локальный путь.
	auto source = std::make_shared<UploadSource>();
	// error описывает локальный отказ открытия либо ограничение размера, без факта отправки.
	Error error = makeError(ErrorKind::Contract);
	// opened сохраняет локальный результат до постановки raw в очередь.
	const bool opened = valid && source->openFile(filePath, impl_->limits.uploadMaxBytes, error);
	// query содержит точные kind и десятичную ревизию, без промежуточного JSON double.
	QUrlQuery query;
	query.addQueryItem(QStringLiteral("kind"), kind);
	query.addQueryItem(QStringLiteral("expected_revision"), QString::number(expectedRevision));
	impl_->raw(
		QStringLiteral("/fallback-assets/") + segment(assetId) + QStringLiteral("/content"), std::move(query),
		receiver,
		// Ответ уже прошёл независимую проверку Preparation/Location; decoded выдаёт только полный DTO.
		[this, callback = std::move(callback)](RequestContext context, Result<QByteArray> result,
						       HttpDiagnostic diagnostic, const auto &job) {
			impl_->decoded<Preparation>(job, context, std::move(result), diagnostic, callback);
		},
		impl_->limits.restBodyBytes, QNetworkAccessManager::PutOperation, {}, 202, opened,
		opened ? std::nullopt : std::optional<Error>(std::move(error)), opened ? std::move(source) : nullptr,
		assetId, std::move(progress));
}
/** Принимает rebuild сохранённого оригинала без локального файла и без повторного POST.
 * Ревизия — положительное точное число JSON, а 202 проверяется как Preparation, отдельно от Receipt.
 */
void APIClient::rebuildAsset(QString assetId, quint64 expectedRevision, QObject *receiver,
			     Callback<Preparation> callback)
{
	// body содержит единственное поле CAS без преобразования U64 в double.
	const auto body = serializeJson(JsonValue::Object{{QStringLiteral("expected_revision"), expectedRevision}});
	impl_->raw(
		QStringLiteral("/fallback-assets/") + segment(assetId) + QStringLiteral("/rebuild"), {}, receiver,
		// Обработчик получает только полную проверенную пару либо исходный классифицированный отказ.
		[this, callback = std::move(callback)](RequestContext context, Result<QByteArray> result,
						       HttpDiagnostic diagnostic, const auto &job) {
			impl_->decoded<Preparation>(job, context, std::move(result), diagnostic, callback);
		},
		impl_->limits.restBodyBytes, QNetworkAccessManager::PostOperation, body, 202,
		!assetId.isEmpty() && expectedRevision > 0 && !body.isEmpty(), std::nullopt, {}, assetId);
}
// Полностью заменяет fallback точного Stream; null является явным намерением очистить назначение.
void APIClient::setFallbackPolicy(QString streamId, PolicyPut request, QObject *receiver,
				  Callback<PolicyResult> callback)
{
	impl_->mutate<PolicyResult>(QStringLiteral("/streams/") + segment(streamId) +
					    QStringLiteral("/fallback-policy"),
				    std::move(request), QNetworkAccessManager::PutOperation, 200, !streamId.isEmpty(),
				    receiver, std::move(callback));
}

// Отправляет одну команду прикрепления destinationId; requestId и точная Session сохраняются вызывающим кодом.
void APIClient::addOutput(QString sessionId, QString destinationId, QString requestId, QObject *receiver,
			  Callback<Receipt> callback)
{
	impl_->post(QStringLiteral("/sessions/") + segment(sessionId) + QStringLiteral("/outputs"),
		    {{QStringLiteral("request_id"), requestId}, {QStringLiteral("destination_id"), destinationId}},
		    !sessionId.isEmpty() && !destinationId.isEmpty() && !requestId.isEmpty(), receiver,
		    std::move(callback));
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

// streamId задаёт точную цель, requestId сохраняется в payload; destinationIds фиксирует непустой состав.
// receiver ограничивает callback: ошибка параметров доставляется асинхронно без отправки POST.
void APIClient::startSession(QString streamId, QString requestId, QStringList destinationIds, QObject *receiver,
			     Callback<Receipt> callback)
{
	// destinations сохраняет порядок и значения исходного списка без динамического выбора Enabled на сервере.
	JsonValue::Array destinations;
	// valid не позволяет серверу трактовать пустой список как выбор всех текущих Enabled.
	bool valid = !streamId.isEmpty() && !requestId.isEmpty() && !destinationIds.isEmpty();
	// id — одно явно выбранное назначение; пустое значение делает весь запрос недопустимым.
	for (const auto &id : destinationIds) {
		valid = valid && !id.isEmpty();
		destinations.append(id);
	}
	impl_->post(QStringLiteral("/streams/") + segment(streamId) + QStringLiteral("/start"),
		    {{QStringLiteral("request_id"), requestId}, {QStringLiteral("destination_ids"), destinations}},
		    valid, receiver, std::move(callback));
}

// sessionId и requestId передаются без подмены; receiver ограничивает callback принятия pause.
void APIClient::pauseSession(QString sessionId, QString requestId, QObject *receiver, Callback<Receipt> callback)
{
	impl_->post(QStringLiteral("/sessions/") + segment(sessionId) + QStringLiteral("/pause"),
		    {{QStringLiteral("request_id"), requestId}}, !sessionId.isEmpty() && !requestId.isEmpty(), receiver,
		    std::move(callback));
}

// sessionId и requestId передаются без подмены; receiver ограничивает callback принятия resume.
void APIClient::resumeSession(QString sessionId, QString requestId, QObject *receiver, Callback<Receipt> callback)
{
	impl_->post(QStringLiteral("/sessions/") + segment(sessionId) + QStringLiteral("/resume"),
		    {{QStringLiteral("request_id"), requestId}}, !sessionId.isEmpty() && !requestId.isEmpty(), receiver,
		    std::move(callback));
}

// sessionId и requestId передаются без подмены; receiver ограничивает callback принятия stop.
void APIClient::stopSession(QString sessionId, QString requestId, QObject *receiver, Callback<Receipt> callback)
{
	impl_->post(QStringLiteral("/sessions/") + segment(sessionId) + QStringLiteral("/stop"),
		    {{QStringLiteral("request_id"), requestId}}, !sessionId.isEmpty() && !requestId.isEmpty(), receiver,
		    std::move(callback));
}

// sessionId и outputId задают единственный выход; requestId сохраняется, receiver ограничивает callback start.
void APIClient::startOutput(QString sessionId, QString outputId, QString requestId, QObject *receiver,
			    Callback<Receipt> callback)
{
	impl_->post(QStringLiteral("/sessions/") + segment(sessionId) + QStringLiteral("/outputs/") +
			    segment(outputId) + QStringLiteral("/start"),
		    {{QStringLiteral("request_id"), requestId}},
		    !sessionId.isEmpty() && !outputId.isEmpty() && !requestId.isEmpty(), receiver, std::move(callback));
}

// sessionId и outputId задают единственный выход; requestId сохраняется, receiver ограничивает callback stop.
void APIClient::stopOutput(QString sessionId, QString outputId, QString requestId, QObject *receiver,
			   Callback<Receipt> callback)
{
	impl_->post(QStringLiteral("/sessions/") + segment(sessionId) + QStringLiteral("/outputs/") +
			    segment(outputId) + QStringLiteral("/stop"),
		    {{QStringLiteral("request_id"), requestId}},
		    !sessionId.isEmpty() && !outputId.isEmpty() && !requestId.isEmpty(), receiver, std::move(callback));
}

// sessionId и outputId задают единственный выход; requestId сохраняется, receiver ограничивает callback detach.
void APIClient::detachOutput(QString sessionId, QString outputId, QString requestId, QObject *receiver,
			     Callback<Receipt> callback)
{
	impl_->post(QStringLiteral("/sessions/") + segment(sessionId) + QStringLiteral("/outputs/") +
			    segment(outputId) + QStringLiteral("/detach"),
		    {{QStringLiteral("request_id"), requestId}},
		    !sessionId.isEmpty() && !outputId.isEmpty() && !requestId.isEmpty(), receiver, std::move(callback));
}
} // namespace gost::api
