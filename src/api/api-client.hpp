#pragma once
#include "api/models.hpp"
#include "api/configuration-requests.hpp"
#include <QObject>
#include <QUrl>
#include <QStringList>
#include <memory>

namespace gost::api {
// Закрытый набор методов диагностики исключает произвольные строки и payload.
enum class HttpMethod { Get, Post, Put, Patch, Delete };
// Тип маршрута не содержит origin, идентификаторов, пути или query.
enum class RouteType {
	Health,
	Capabilities,
	Snapshot,
	Stream,
	Destination,
	Profile,
	Group,
	Session,
	Command,
	Asset,
	Other
};
// Безопасные метаданные завершённого REST-запроса.
struct HttpDiagnostic {
	// Использованный HTTP-метод.
	HttpMethod method = HttpMethod::Get;
	// Категория типизированного маршрута.
	RouteType route = RouteType::Other;
	// Полученный HTTP-статус либо ноль при отсутствии ответа.
	int status = 0;
	// Время транспорта в миллисекундах; ожидание очереди исключено.
	qint64 durationMs = 0;
	// Число принятых байтов без содержимого ответа.
	qsizetype receivedBytes = 0;
	// Классификация ошибки без сообщений сервера.
	std::optional<ErrorKind> error;
	// Число активных запросов после завершения этого запроса.
	qsizetype activeRequests = 0;
	// Число ожидающих запросов в момент завершения.
	qsizetype queuedRequests = 0;
};
// Обработчик диагностики выполняется в потоке клиента и не должен уничтожать клиент.
using DiagnosticCallback = std::function<void(const HttpDiagnostic &)>;
// Сообщает переданные байты текущего upload; totalBytes — захваченный размер, а не готовность Generation.
// Обработчик выполняется в потоке клиента и может отменить запрос, сменить контекст или удалить клиент.
using UploadProgressCallback = std::function<void(RequestContext, qint64 bytesSent, qint64 totalBytes)>;
/**
 * Асинхронно читает REST API, изменяет конфигурацию и отправляет типизированные команды выбранного сервера.
 * Проверяет ответы перед выдачей DTO.
 * Клиент владеет своим QNetworkAccessManager, очередью запросов и активными ответами. Все методы,
 * сетевые сигналы и обработчики результата выполняются в потоке клиента с работающим циклом Qt.
 * Перенос вызовов в поток receiver не выполняется; receiver служит ограничителем времени жизни.
 * Передаваемый callback должен быть вызываемым и не должен удерживать незащищённые чужие объекты.
 *
 * Результат доставляется только живому receiver и только для действующего RequestContext и поколения
 * отмены. Обработчик получает контекст, захваченный при создании запроса, и может повторно войти
 * в клиент, перенастроить или уничтожить его. Отмена и уничтожение получателя подавляют результат,
 * поэтому ожидание вызывающей стороны должно завершаться и по её собственному жизненному циклу.
 *
 * Очередь, число активных операций, размеры тел и обход списков ограничиваются Limits. Ошибки
 * транспорта, HTTP, таймаута, размера и контракта различаются в Result. Запросы не повторяются:
 * восстановлением подключения и ручным повтором команд управляет вызывающий код.
 * Редиректы автоматически не выполняются; Location команды проверяется без перехода по ссылке.
 */
class APIClient : public QObject {
public:
	// Создаёт клиент без выбранного сервера; limits задают бюджеты, сроки новых запросов можно изменить отдельно.
	explicit APIClient(Limits limits = {}, QObject *parent = nullptr);
	// Отменяет собственные запросы до уничтожения менеджера и захваченных обработчиков.
	~APIClient() override;
	/**
	 * Отменяет все прежние задания, затем устанавливает origin и контекст новых запросов.
	 * Допустим только HTTP(S) origin без учётных данных, параметров запроса, фрагмента и пути кроме корня.
	 * Недопустимый origin сохраняется как пустой: последующие запросы получают ошибку Contract.
	 * Перенастройка с тем же контекстом также запрещает доставку старых результатов.
	 * После shutdown() вызов не действует.
	 */
	void configure(QUrl origin, RequestContext context);
	/**
	 * Очищает очередь, отключает обработчики активных ответов и прерывает их через abort().
	 * Сначала меняет внутреннее поколение: уже запланированные продолжения теряют право выдавать
	 * результат, даже если RequestContext не изменился. Ответы освобождаются через deleteLater().
	 * Отмена не вызывает callback с ErrorKind::Cancelled. origin и контекст сохраняются,
	 * поэтому клиент может принимать новые запросы. Вызывать требуется в потоке клиента.
	 */
	void cancelAll();
	/** Отменяет только задания receiver, включая отложенные продолжения списков.
	 * Возвращает true, если хотя бы одно отменённое задание уже передано транспорту.
	 * Callback отменённых заданий подавляется; ответ backend после отправки остаётся неизвестным.
	 * null receiver ничего не отменяет. Вызывать требуется в потоке клиента.
	 */
	bool cancelRequests(QObject *receiver);
	// Необратимо закрывает клиент и отменяет задания. Повторный вызов безопасен; новые запросы игнорируются.
	void shutdown();
	// Меняет сроки только новых обычных REST-заданий: absolute=timeoutMs, idle=min(10000, timeoutMs).
	// Диапазон пользовательской настройки проверяет SettingsStore; положительные короткие сроки допустимы тестам.
	void setHttpTimeoutMs(int timeoutMs);
	// Устанавливает безопасный sink без зависимости API от OBS и без глобального Qt-handler.
	void setDiagnosticCallback(DiagnosticCallback callback);
	// Создаёт Stream; callback получает проверенный DTO ответа 201.
	void createStream(StreamCreate request, QObject *receiver, Callback<Stream> callback);
	// Изменяет Stream по ревизии; callback получает DTO ответа 200.
	void patchStream(QString id, StreamPatch request, QObject *receiver, Callback<Stream> callback);
	// Удаляет Stream по ревизии; успехом является пустой ответ 204.
	void deleteStream(QString id, quint64 expectedRevision, QObject *receiver, Callback<Empty> callback);
	// Создаёт Destination; секретные строки сохраняются точно.
	void createDestination(DestinationCreate request, QObject *receiver, Callback<Destination> callback);
	// Изменяет явно указанные поля Destination по ревизии.
	void patchDestination(QString id, DestinationPatch request, QObject *receiver, Callback<Destination> callback);
	// Удаляет Destination по ревизии без автоматического каскада.
	void deleteDestination(QString id, quint64 expectedRevision, QObject *receiver, Callback<Empty> callback);
	// Создаёт связь Group с потоком и профилем.
	void createGroup(GroupCreate request, QObject *receiver, Callback<Group> callback);
	// Изменяет явно указанные связи/имя Group по ревизии.
	void patchGroup(QString id, GroupPatch request, QObject *receiver, Callback<Group> callback);
	// Удаляет Group по ревизии; сервер решает допустимость удаления.
	void deleteGroup(QString id, quint64 expectedRevision, QObject *receiver, Callback<Empty> callback);
	// Создаёт именованный профиль с полным рецептом.
	void createProfile(ProfileCreate request, QObject *receiver, Callback<Profile> callback);
	// Заменяет явно переданные имя/полный рецепт Profile по ревизии.
	void patchProfile(QString id, ProfilePatch request, QObject *receiver, Callback<Profile> callback);
	// Удаляет Profile по ревизии без скрытого изменения связанных групп.
	void deleteProfile(QString id, quint64 expectedRevision, QObject *receiver, Callback<Empty> callback);
	// Создаёт именованный Asset; callback получает проверенный DTO ответа 201.
	void createAsset(AssetCreate request, QObject *receiver, Callback<Asset> callback);
	// Удаляет точный Asset по положительной ревизии; успешен только пустой 204.
	void deleteAsset(QString id, quint64 expectedRevision, QObject *receiver, Callback<Empty> callback);
	/** Открывает обычный читаемый файл filePath и передаёт его один раз как image либо video.
	 * Размер фиксируется до постановки в очередь; положительный файл ограничен uploadMaxBytes.
	 * Job удерживает файл до завершения транспорта. Загрузка имеет собственные сроки Limits:
	 * общий срок включает очередь, простой продлевается только реальным обменом байтами.
	 * Проверенный 202/Location даёт только принятую пару Asset/Generation, не готовность медиа.
	 */
	void uploadAsset(QString assetId, quint64 expectedRevision, QString kind, QString filePath, QObject *receiver,
			 Callback<Preparation> callback, UploadProgressCallback progress = {});
	// Просит подготовить сохранённый оригинал; числовая ревизия отправляется JSON, ответ — Preparation202.
	void rebuildAsset(QString assetId, quint64 expectedRevision, QObject *receiver, Callback<Preparation> callback);
	// Полностью заменяет политику точного Stream либо отправляет явный null; revision результата относится к Stream.
	void setFallbackPolicy(QString streamId, PolicyPut request, QObject *receiver, Callback<PolicyResult> callback);
	// Прикрепляет destinationId к точной Session; ответ 202 означает лишь принятие команды.
	void addOutput(QString sessionId, QString destinationId, QString requestId, QObject *receiver,
		       Callback<Receipt> callback);
	// Проверки live/ready описывают HTTP-сервис; их успех не подтверждает готовность медиатракта.
	void getReady(QObject *receiver, Callback<Health> callback);
	// Читает live-проверку процесса сервера; receiver ограничивает доставку результата callback.
	void getLive(QObject *receiver, Callback<Health> callback);
	// Читает объявленные сервером возможности без вывода о версии фактически установленного бинарника.
	void getCapabilities(QObject *receiver, Callback<Capabilities> callback);
	// Получает оперативный снимок с отдельным увеличенным лимитом тела. Установку снимка и SSE выполняет вызывающий код.
	void getSnapshot(QObject *receiver, Callback<Anchor> callback);
	// Детальные GET читают ровно указанный непрозрачный ID. Каждый ID кодируется как один сегмент пути.
	// Конфигурация и исторические снимки в этих DTO не заменяют авторитетные оперативные наблюдения SSE.
	void getStream(QString id, QObject *receiver, Callback<Stream> callback);
	// Читает Destination id; возвращаемые секретные поля предназначены карточке, а не журналу.
	void getDestination(QString id, QObject *receiver, Callback<Destination> callback);
	// Читает точный Profile id без изменения серверной конфигурации.
	void getProfile(QString id, QObject *receiver, Callback<Profile> callback);
	// Читает точную Group id вместе с её параметрами профиля.
	void getGroup(QString id, QObject *receiver, Callback<Group> callback);
	// Читает сохранённую Session id; её состояние проверяется отдельно от квитанции команды.
	void getSession(QString id, QObject *receiver, Callback<Session> callback);
	// Читает квитанцию по command id; само наличие квитанции не подтверждает желаемое состояние.
	void getCommand(QString id, QObject *receiver, Callback<Receipt> callback);
	// Читает Asset id с указателями на текущую и подготавливаемую генерации.
	void getAsset(QString id, QObject *receiver, Callback<Asset> callback);
	// Читает точное поколение указанного Asset; новое pending не подменяет выбранное current.
	void getGeneration(QString assetId, QString generationId, QObject *receiver, Callback<Generation> callback);
	// Отсутствие назначенной политики представляется PolicyResult, а не выдуманным Asset.
	void getFallbackPolicy(QString streamId, QObject *receiver, Callback<PolicyResult> callback);
	// Детальные наблюдения возвращаются отдельно; решение об их свежести и применении принимает вызывающий код.
	void getSessionMetrics(QString sessionId, QObject *receiver, Callback<SessionMetrics> callback);
	// Читает outputId внутри точной sessionId; callback получает независимый детальный REST-ответ.
	void getOutput(QString sessionId, QString outputId, QObject *receiver, Callback<OutputDetail> callback);
	/**
	 * Отправляет один start для streamId с неизменяемыми requestId и непустым фиксированным destinationIds.
	 * Пустые ID отклоняются локально; callback живого receiver получает только проверенную квитанцию 202.
	 * Это принятие команды, а не подтверждение состояния. Повторы и генерация ID принадлежат вызывающему коду.
	 */
	void startSession(QString streamId, QString requestId, QStringList destinationIds, QObject *receiver,
			  Callback<Receipt> callback);
	// Отправляет pause точной sessionId с готовым requestId; receiver ограничивает жизнь callback квитанции.
	void pauseSession(QString sessionId, QString requestId, QObject *receiver, Callback<Receipt> callback);
	// Отправляет resume точной sessionId с готовым requestId; receiver ограничивает жизнь callback квитанции.
	void resumeSession(QString sessionId, QString requestId, QObject *receiver, Callback<Receipt> callback);
	// Отправляет stop точной sessionId с готовым requestId; receiver ограничивает жизнь callback квитанции.
	void stopSession(QString sessionId, QString requestId, QObject *receiver, Callback<Receipt> callback);
	// Запускает точный outputId внутри sessionId; requestId сохраняется, receiver ограничивает жизнь callback.
	void startOutput(QString sessionId, QString outputId, QString requestId, QObject *receiver,
			 Callback<Receipt> callback);
	// Останавливает точный outputId внутри sessionId; requestId сохраняется, receiver ограничивает жизнь callback.
	void stopOutput(QString sessionId, QString outputId, QString requestId, QObject *receiver,
			Callback<Receipt> callback);
	// Отсоединяет точный outputId внутри sessionId; requestId сохраняется, receiver ограничивает жизнь callback.
	void detachOutput(QString sessionId, QString outputId, QString requestId, QObject *receiver,
			  Callback<Receipt> callback);
	/**
	 * Семейства get<Name> читают одну страницу, getAll<Name> последовательно обходят страницы
	 * с исходными фильтрами query. У вложенных списков действуют те же правила; ID задаёт родительскую сущность.
	 * Курсоры передаются без интерпретации и сравнения порядка; повтор курсора считается Contract.
	 *
	 * Collection.complete=true означает, что сервер вернул конец списка. Общий предел страниц,
	 * элементов или байтов возвращает успешную известную часть с complete=false. Ошибка отдельной
	 * страницы остаётся ошибкой всей операции. nextCursor не обещает продолжение с точного элемента,
	 * если предел элементов отсёк середину страницы. Независимого атомарного снимка всех страниц нет.
	 *
	 * Если общий бюджет исчерпан до первой страницы (например, maxPages, maxItems или pageWalkBytes
	 * равен нулю), getAll<Name> вызывает callback до своего возврата, без отправки GET. Результат
	 * при этом успешный, но неполный. Вызывающая сторона должна подготовить состояние обработчика
	 * до вызова метода; всегда отложенная доставка для этого семейства не гарантируется.
	 */
	// Читает страницу Streams с query; receiver ограничивает время жизни callback.
	void getStreams(QObject *receiver, Callback<Page<Stream>> callback, ListQuery query = {});
	// Обходит Streams с query до конца или лимита; callback получает явную полноту.
	void getAllStreams(QObject *receiver, Callback<Collection<Stream>> callback, ListQuery query = {});
	// Читает страницу Destinations с query, сохраняя точные значения серверных полей.
	void getDestinations(QObject *receiver, Callback<Page<Destination>> callback, ListQuery query = {});
	// Обходит Destinations с query; callback получает полноту, receiver ограничивает его жизнь.
	void getAllDestinations(QObject *receiver, Callback<Collection<Destination>> callback, ListQuery query = {});
	// Читает страницу Profiles с query без изменения конфигурации.
	void getProfiles(QObject *receiver, Callback<Page<Profile>> callback, ListQuery query = {});
	// Обходит Profiles с query до конца или лимита, возвращая явную полноту.
	void getAllProfiles(QObject *receiver, Callback<Collection<Profile>> callback, ListQuery query = {});
	// Читает страницу Groups с query вместе с привязками профилей.
	void getGroups(QObject *receiver, Callback<Page<Group>> callback, ListQuery query = {});
	// Обходит Groups с query до конца или лимита, возвращая явную полноту.
	void getAllGroups(QObject *receiver, Callback<Collection<Group>> callback, ListQuery query = {});
	// Читает страницу сохранённых Session с фильтрами query.
	void getSessions(QObject *receiver, Callback<Page<Session>> callback, ListQuery query = {});
	// Обходит Session с query до конца или лимита, возвращая явную полноту.
	void getAllSessions(QObject *receiver, Callback<Collection<Session>> callback, ListQuery query = {});
	// Читает страницу Assets с query и указателями на их поколения.
	void getAssets(QObject *receiver, Callback<Page<Asset>> callback, ListQuery query = {});
	// Обходит Assets с query до конца или лимита, возвращая явную полноту.
	void getAllAssets(QObject *receiver, Callback<Collection<Asset>> callback, ListQuery query = {});
	// Читает страницу назначений Stream id с фильтрами query и ограничением жизни receiver/callback.
	void getStreamDestinations(QString id, QObject *receiver, Callback<Page<Destination>> callback,
				   ListQuery query = {});
	// Обходит назначения Stream id с query; callback получает полноту, receiver ограничивает его жизнь.
	void getAllStreamDestinations(QString id, QObject *receiver, Callback<Collection<Destination>> callback,
				      ListQuery query = {});
	// Читает страницу Session для Stream id, сохраняя фильтры query.
	void getStreamSessions(QString id, QObject *receiver, Callback<Page<Session>> callback, ListQuery query = {});
	// Обходит Session Stream id до конца или лимита, возвращая явную полноту.
	void getAllStreamSessions(QString id, QObject *receiver, Callback<Collection<Session>> callback,
				  ListQuery query = {});
	// Читает страницу выходов точной sessionId с фильтрами query.
	void getOutputs(QString sessionId, QObject *receiver, Callback<Page<Output>> callback, ListQuery query = {});
	// Обходит выходы точной sessionId до конца или лимита, возвращая явную полноту.
	void getAllOutputs(QString sessionId, QObject *receiver, Callback<Collection<Output>> callback,
			   ListQuery query = {});
	// Читает страницу вариантов Asset assetId; query может фиксировать generation_id.
	void getVariants(QString assetId, QObject *receiver, Callback<Page<Variant>> callback, ListQuery query = {});
	// Обходит варианты Asset assetId с исходным query; callback получает явную полноту чтения.
	void getAllVariants(QString assetId, QObject *receiver, Callback<Collection<Variant>> callback,
			    ListQuery query = {});

private:
	// Скрывает очередь, менеджер и поколение отмены от потребителей типизированного API.
	class Impl;
	// Единственный владелец транспортного состояния; освобождается после shutdown в деструкторе.
	std::unique_ptr<Impl> impl_;
};
} // namespace gost::api
