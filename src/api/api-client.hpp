#pragma once
#include "api/models.hpp"
#include <QObject>
#include <QUrl>
#include <memory>

namespace gost::api {
/**
 * Асинхронно читает REST API одного выбранного сервера и проверяет ответы перед выдачей DTO.
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
 * транспорта, HTTP, таймаута, размера и контракта различаются в Result. Отдельные GET не повторяются:
 * восстановлением подключения управляет ConnectionController. Редиректы автоматически не выполняются.
 */
class APIClient : public QObject {
public:
	// Создаёт клиент без выбранного сервера; limits копируются на всё время его жизни.
	explicit APIClient(Limits limits = {}, QObject *parent = nullptr);
	~APIClient() override;
	/**
	 * Отменяет все прежние задания, затем устанавливает origin и контекст новых запросов.
	 * Допустим только HTTP(S) origin без учётных данных, параметров запроса, фрагмента и пути кроме корня.
	 * Недопустимый origin сохраняется как пустой: последующие GET получают ошибку Contract.
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
	// Необратимо закрывает клиент и отменяет задания. Повторный вызов безопасен; новые GET игнорируются.
	void shutdown();
	// Проверки live/ready описывают HTTP-сервис; их успех не подтверждает готовность медиатракта.
	void getReady(QObject *receiver, Callback<Health> callback);
	void getLive(QObject *receiver, Callback<Health> callback);
	// Читает объявленные сервером возможности без вывода о версии фактически установленного бинарника.
	void getCapabilities(QObject *receiver, Callback<Capabilities> callback);
	// Получает оперативный снимок с отдельным увеличенным лимитом тела. Установку снимка и SSE выполняет вызывающий код.
	void getSnapshot(QObject *receiver, Callback<Anchor> callback);
	// Детальные GET читают ровно указанный непрозрачный ID. Каждый ID кодируется как один сегмент пути.
	// Конфигурация и исторические снимки в этих DTO не заменяют авторитетные оперативные наблюдения SSE.
	void getStream(QString id, QObject *receiver, Callback<Stream> callback);
	void getDestination(QString id, QObject *receiver, Callback<Destination> callback);
	void getProfile(QString id, QObject *receiver, Callback<Profile> callback);
	void getGroup(QString id, QObject *receiver, Callback<Group> callback);
	void getSession(QString id, QObject *receiver, Callback<Session> callback);
	void getCommand(QString id, QObject *receiver, Callback<Receipt> callback);
	void getAsset(QString id, QObject *receiver, Callback<Asset> callback);
	// Читает точное поколение указанного Asset; новое pending не подменяет выбранное current.
	void getGeneration(QString assetId, QString generationId, QObject *receiver, Callback<Generation> callback);
	// Отсутствие назначенной политики представляется PolicyResult, а не выдуманным Asset.
	void getFallbackPolicy(QString streamId, QObject *receiver, Callback<PolicyResult> callback);
	// Детальные наблюдения возвращаются отдельно; решение об их свежести и применении принимает вызывающий код.
	void getSessionMetrics(QString sessionId, QObject *receiver, Callback<SessionMetrics> callback);
	void getOutput(QString sessionId, QString outputId, QObject *receiver, Callback<OutputDetail> callback);
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
#define GOST_DECLARE_LIST(Name, Type) \
 void get##Name(QObject *receiver, Callback<Page<Type>> callback, ListQuery query = {}); \
 void getAll##Name(QObject *receiver, Callback<Collection<Type>> callback, ListQuery query = {});
	GOST_DECLARE_LIST(Streams, Stream)
	GOST_DECLARE_LIST(Destinations, Destination)
	GOST_DECLARE_LIST(Profiles, Profile)
	GOST_DECLARE_LIST(Groups, Group)
	GOST_DECLARE_LIST(Sessions, Session)
	GOST_DECLARE_LIST(Assets, Asset)
#undef GOST_DECLARE_LIST
	void getStreamDestinations(QString id, QObject *receiver, Callback<Page<Destination>> callback,
				   ListQuery query = {});
	void getAllStreamDestinations(QString id, QObject *receiver, Callback<Collection<Destination>> callback,
				      ListQuery query = {});
	void getStreamSessions(QString id, QObject *receiver, Callback<Page<Session>> callback, ListQuery query = {});
	void getAllStreamSessions(QString id, QObject *receiver, Callback<Collection<Session>> callback,
				  ListQuery query = {});
	void getOutputs(QString sessionId, QObject *receiver, Callback<Page<Output>> callback, ListQuery query = {});
	void getAllOutputs(QString sessionId, QObject *receiver, Callback<Collection<Output>> callback,
			   ListQuery query = {});
	void getVariants(QString assetId, QObject *receiver, Callback<Page<Variant>> callback, ListQuery query = {});
	void getAllVariants(QString assetId, QObject *receiver, Callback<Collection<Variant>> callback,
			    ListQuery query = {});

private:
	class Impl;
	std::unique_ptr<Impl> impl_;
};
} // namespace gost::api
