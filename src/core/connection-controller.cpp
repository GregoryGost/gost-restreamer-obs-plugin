#include "connection-controller.hpp"

#include "api/api-client.hpp"
#include "api/event-client.hpp"
#include "core/settings-store.hpp"
#include "core/state-store.hpp"

#include <QElapsedTimer>
#include <QPointer>
#include <QRandomGenerator>
#include <QSet>
#include <QThread>
#include <QTimer>
#include <QUrl>

#include <algorithm>
#include <array>
#include <cstddef>
#include <deque>
#include <limits>
#include <utility>

namespace gost {
namespace {

// Двух записей достаточно для обнаружения неоднозначности; продолжение страницы не позволяет подтвердить Session.
constexpr int SessionPageSize = 2;
const std::array ActiveSessionStates{QStringLiteral("STARTING"), QStringLiteral("RUNNING"), QStringLiteral("STOPPING")};

bool terminalSession(const QString &state)
{
	return state == QStringLiteral("STOPPED") || state == QStringLiteral("FAILED") ||
	       state == QStringLiteral("INTERRUPTED");
}

bool activeSession(const QString &state)
{
	return std::find(ActiveSessionStates.cbegin(), ActiveSessionStates.cend(), state) != ActiveSessionStates.cend();
}

bool validDockId(const QString &id)
{
	return !id.isEmpty() && id.size() <= 4096 && !id.contains(QChar::Null);
}

void markReadUnconfirmed(ReadStatus &status)
{
	if (status.availability == RuntimeAvailability::Fresh)
		status.availability = RuntimeAvailability::Stale;
	status.complete = false;
}

} // namespace

// В одном цикле GUI ведёт подключение и отдельно читает REST-метаданные для выбранного Dock.
// Ответ прежнего подключения или выбора отсекается по context. Новый снимок при том же выборе
// отличается по anchorVersion, а конкретный набор запросов метаданных — по dockRead. Сигнал StateStore
// может сразу вызвать код, меняющий выбор, поэтому после уведомлений эти проверки выполняются заново.
class ConnectionController::Impl {
public:
	// Заимствует owner/state/settings/api/events и связывает таймер с временем жизни owner.
	// Обработчик retry начинает сохранённую фазу только для прежнего контекста;
	// обработчик changed переносит текущие предпочтения SettingsStore в StateStore.
	Impl(ConnectionController &owner, StateStore &state, SettingsStore &settings, api::APIClient &api,
	     api::EventClient &events)
		: owner(owner),
		  state(state),
		  settings(settings),
		  api(api),
		  events(events)
	{
		clock.start();
		retry.setSingleShot(true);
		QObject::connect(&retry, &QTimer::timeout, &owner, [this] {
			if (active && !stopped && scheduledContext == context)
				start(nextPhase);
		});
		QObject::connect(&settings, &SettingsStore::changed, &owner, [this] { syncPreferences(); });
		syncPreferences();
		state.setConnection(settings.serverOrigin(), ConnectionStatus::Disconnected);
	}

	// Handshake читает ready, capabilities и Streams перед снимком. Snapshot запрашивает новый снимок.
	// Events возобновляет подписку с установленного cursor; без курсора повторяет начальные чтения Handshake.
	enum class Phase { Handshake, Snapshot, Events };
	// Заимствованный QObject владеет реализацией и ограничивает время жизни callbacks.
	ConnectionController &owner;
	// Общее GUI-состояние для публикации проверенных наблюдений.
	StateStore &state;
	// Хранилище предпочтений и устойчивой идентичности OBS Profile.
	SettingsStore &settings;
	// Заимствованный REST-клиент наблюдения; команды используют отдельный клиент.
	api::APIClient &api;
	// Заимствованный клиент единственной оперативной SSE-подписки.
	api::EventClient &events;
	// Поколения текущего подключения и выбора, проверяемые каждым ответом.
	api::RequestContext context;
	// Контекст, которому принадлежит запланированное восстановление.
	api::RequestContext scheduledContext;
	// Одноразовый таймер паузы перед следующим циклом подключения.
	QTimer retry;
	// Монотонные часы скользящего окна ограничения частоты стартов.
	QElapsedTimer clock;
	// Время фактических стартов в миллисекундах clock за последние 10 секунд.
	std::deque<qint64> starts;
	// Фаза, которую должен начать следующий timeout таймера retry.
	Phase nextPhase = Phase::Handshake;
	// Число последовательных отказов для роста паузы; ограничено значением 5.
	int failures = 0;
	// Последний HTTP 400/409 подписки, требующий нового snapshot.
	int sseRejectionStatus = 0;
	// Число начатых восстановлений snapshot после отказа подписки.
	int sseRejectionAttempts = 0;
	// Необратимый запрет работы после shutdown.
	bool stopped = false;
	// Разрешение текущего цикла подключения и его автоматического восстановления.
	bool active = false;
	// Подтверждён ли UUID текущего OBS Profile через SettingsStore.
	bool profileResolved = false;
	// Автоподключение при FinishedLoading уже было рассмотрено в этом процессе.
	bool autoStarted = false;
	// Две независимо завершаемые части чтения метаданных одного выбора и snapshot.
	struct DockRead {
		// Один набор запросов читает Session с Outputs и отдельно цепочку fallback. Готовая часть
		// сразу передаётся в StateStore: медленный Asset не задерживает показ Session. complete показывает,
		// удалось ли полностью проверить поиск единственной активной Session. Если во время чтения
		// изменились состав или состояние исполнений, dirty запрещает передавать состав Session/Outputs
		// в StateStore; после завершения обеих частей выполняется новое чтение. При смене выбора или
		// снимка результаты всего набора больше не передаются в StateStore.
		// Тройка сервер/UUID профиля/Stream, зафиксированная до первого запроса.
		DockContext selection;
		// Версия snapshot, на котором началось это чтение.
		quint64 anchorVersion = 0;
		// Собираемые метаданные с сохранением прежних неподтверждённых значений.
		DockServerState next;
		// Уникальные ID активных Session, найденные по трём фильтрам состояния.
		QSet<QString> candidates;
		// Число ещё не завершившихся страниц STARTING/RUNNING/STOPPING.
		std::size_t remainingPages = ActiveSessionStates.size();
		// Все страницы поиска полны и согласованы с фильтром.
		bool complete = true;
		// Цепочка Session/Outputs завершилась независимо от результата.
		bool sessionDone = false;
		// Цепочка policy/Asset/Generation завершилась независимо от результата.
		bool fallbackDone = false;
		// Готовая часть Session/Outputs уже передана в StateStore.
		bool sessionPublished = false;
		// Готовая часть fallback уже передана в StateStore.
		bool fallbackPublished = false;
		// Топология изменилась во время чтения: после завершения требуется новое чтение.
		bool dirty = false;
	};
	// Общая ссылка удерживает пакет до завершения всех его асинхронных ответов.
	using DockBatch = std::shared_ptr<DockRead>;
	// Единственный текущий пакет; несовпадение ссылки запрещает публикацию старого ответа.
	DockBatch dockRead;
	// Увеличивается после каждой установки нового оперативного snapshot.
	quint64 anchorVersion = 0;
	// Выбор, которому принадлежит подсказка последней прочитанной Session.
	DockContext dockSessionHintContext;
	// ID последней подтверждённой Session; очищается при смене контекста выбора.
	QString dockSessionHint;

	// Проверяет полученный контекст received и разрешение текущего подключения.
	bool current(api::RequestContext received) const { return !stopped && active && received == context; }

	// Публикует предпочтения и безопасную ошибку сохранения из SettingsStore.
	void syncPreferences()
	{
		state.setPreferences(settings.autoConnect(), settings.reconnect(), settings.sseRejectionRetries(),
				     settings.unsaved(), settings.error(), settings.errorKey());
	}

	// Сбрасывает бюджет отказов подписки после явного сброса или успешного открытия SSE.
	void clearSseRejection()
	{
		sseRejectionStatus = 0;
		sseRejectionAttempts = 0;
	}

	// При connection=true меняет поколения подключения и выбора; при false — только выбора.
	// Останавливает повтор и сбрасывает бюджет SSE-отказов до отмены запросов. Нужные данные StateStore
	// очищает вызывающий сценарий: смена профиля и смена сервера требуют разного объёма очистки.
	void invalidate(bool connection)
	{
		// Поколения меняются раньше отмены, чтобы уже поставленные в очередь обработчики стали устаревшими.
		if (connection)
			++context.connection;
		++context.selection;
		retry.stop();
		clearSseRejection();
		dockRead.reset();
		api.cancelAll();
		events.close();
		if (!connection && active)
			state.setConnection(state.serverOrigin(), ConnectionStatus::Connecting);
	}

	// Отменяет прежние REST-чтения и задаёт клиенту текущий origin/context.
	void configure()
	{
		dockRead.reset();
		api.configure(QUrl(state.serverOrigin()), context);
	}

	// Читает сохранённый Stream по каноническому origin и UUID текущего профиля OBS. Первый доступный
	// Stream автоматически не выбирается. Подсказка dockSessionHint сохраняется только при той же
	// тройке «сервер + UUID профиля + Stream», чтобы завершённая Session не перешла в другой выбор.
	void resolveSelection(QString notice = {}, QString noticeKey = {})
	{
		// profileUuid — подтверждённая идентичность профиля; streamId — сохранённый для неё выбор.
		const auto profileUuid = profileResolved ? settings.profileUuid() : QString{};
		const auto streamId = profileResolved ? settings.selectedStream(state.serverOrigin(), profileUuid)
						      : QString{};
		// selection связывает подсказку Session с точным сервером, профилем и Stream.
		const DockContext selection{state.serverOrigin(), profileUuid, streamId};
		if (dockSessionHintContext != selection) {
			dockSessionHintContext = selection;
			dockSessionHint.clear();
		}
		state.setSelection(streamId, profileResolved, std::move(notice), std::move(noticeKey), profileUuid);
	}

	// Проверяет принадлежность batch текущим received, snapshot, выбору и открытой подписке.
	bool currentDockRead(const DockBatch &batch, api::RequestContext received) const
	{
		// Одного совпадения HTTP-контекста недостаточно: новый снимок мог заменить оперативное дерево.
		return current(received) && dockRead == batch && batch->anchorVersion == anchorVersion &&
		       state.connectionStatus() == ConnectionStatus::Connected &&
		       state.dockContext() == batch->selection;
	}

	// Передаёт каждую готовую часть в StateStore один раз. Флаг выставляется до setDockServer,
	// иначе синхронный сигнал мог бы повторно опубликовать ту же часть. После уведомления проверяет,
	// что owner ещё существует и набор запросов всё ещё относится к текущему выбору и снимку.
	// При dirty новое чтение начинается после завершения обеих частей; параллельные наборы не накапливаются.
	void finishDockRead(const DockBatch &batch, api::RequestContext received)
	{
		if (!currentDockRead(batch, received))
			return;
		// Метаданные Session и Output читаются независимо от заглушки; сомнительный пакет Session не публикуется.
		if (batch->sessionDone && !batch->sessionPublished && !batch->dirty) {
			// published заменяет только готовую часть Session/Outputs, сохраняя независимый fallback.
			auto published = state.dockServer();
			published.session = batch->next.session;
			published.sessionStatus = batch->next.sessionStatus;
			published.outputs = batch->next.outputs;
			published.outputsStatus = batch->next.outputsStatus;
			batch->sessionPublished = true;
			// guard проверяет существование owner после синхронного уведомления StateStore.
			const QPointer<ConnectionController> guard = &owner;
			state.setDockServer(std::move(published));
			if (!guard || !currentDockRead(batch, received))
				return;
		}
		if (batch->fallbackDone && !batch->fallbackPublished) {
			// published заменяет только fallback, не возвращая старые Session/Outputs.
			auto published = state.dockServer();
			published.fallback = batch->next.fallback;
			batch->fallbackPublished = true;
			// guard запрещает продолжение после удаления owner обработчиком сигнала.
			const QPointer<ConnectionController> guard = &owner;
			state.setDockServer(std::move(published));
			if (!guard || !currentDockRead(batch, received))
				return;
		}
		if (batch->sessionDone && batch->fallbackDone) {
			dockRead.reset();
			if (batch->dirty)
				refreshDock();
		}
	}

	// Помечает часть Session/Outputs завершённой и передаёт её при действующем received.
	void finishDockSession(const DockBatch &batch, api::RequestContext received)
	{
		batch->sessionDone = true;
		finishDockRead(batch, received);
	}

	// Помечает цепочку fallback завершённой и передаёт её при действующем received.
	void finishDockFallback(const DockBatch &batch, api::RequestContext received)
	{
		batch->fallbackDone = true;
		finishDockRead(batch, received);
	}

	// Каждый Output должен принадлежать точной Session и выбранному Stream; дубликат ID отвергает
	// весь результат. При complete=false отсутствие ID в полученной части не подтверждает detach:
	// обновляем найденные записи, а остальные сохраняем до полного чтения.
	void readDockOutputs(const DockBatch &batch, const QString &sessionId)
	{
		// Обработчик принимает result только для текущего received и проверяет родителей каждого выхода.
		api.getAllOutputs(
			sessionId, &owner,
			[this, batch, sessionId](auto received, api::Result<api::Collection<api::Output>> result) {
				if (!currentDockRead(batch, received))
					return;
				if (!batch->dirty && result) {
					// outputs собирает проверенные записи; неполное чтение сохраняет прежние отсутствующие ID.
					QMap<QString, DockOutput> outputs;
					if (!result.value->complete)
						outputs = batch->next.outputs;
					// receivedIds выявляет дубликаты; valid запрещает публикацию всего противоречивого ответа.
					QSet<QString> receivedIds;
					bool valid = true;
					// output — очередная запись с проверяемыми ID Session и Stream.
					for (const auto &output : result.value->items) {
						if (!validDockId(output.id) || receivedIds.contains(output.id) ||
						    output.sessionId != sessionId ||
						    output.destination.streamId != batch->selection.streamId) {
							valid = false;
							break;
						}
						receivedIds.insert(output.id);
						outputs.insert(output.id, {output.destination.name, output.attached,
									   output.desiredRunning});
					}
					if (valid) {
						batch->next.outputs = std::move(outputs);
						batch->next.outputsStatus = {RuntimeAvailability::Fresh,
									     result.value->complete,
									     {}};
					}
				}
				finishDockSession(batch, received);
			});
	}

	// Полный поиск без кандидатов подтверждает отсутствие активной Session. Единственный кандидат
	// дополнительно проверяется GET по его точному ID; неполный поиск и конфликт не подтверждают отсутствие/выбор.
	void readExactDockSession(const DockBatch &batch, api::RequestContext received)
	{
		// Полный поиск без кандидатов подтверждает отсутствие активной Session. Историческая
		// завершённая запись больше не блокирует запуск и не подменяет отсутствие текущей Session.
		if (!batch->dirty && batch->complete && batch->candidates.isEmpty()) {
			batch->next.session.reset();
			batch->next.sessionStatus = {RuntimeAvailability::Fresh, true, {}};
			batch->next.outputs.clear();
			batch->next.outputsStatus = {RuntimeAvailability::Fresh, true, {}};
			finishDockSession(batch, received);
			return;
		}
		if (batch->dirty || !batch->complete || batch->candidates.size() != 1) {
			finishDockSession(batch, received);
			return;
		}
		// sessionId фиксирует кандидата до асинхронного ответа, callback проверяет обе связи Stream.
		const auto sessionId = *batch->candidates.cbegin();
		// callback повторно проверяет ID/Stream и принимает только актуального активного кандидата.
		api.getSession(sessionId, &owner, [this, batch, sessionId](auto ctx, api::Result<api::Session> result) {
			if (!currentDockRead(batch, ctx))
				return;
			if (!result || result.value->id != sessionId ||
			    result.value->streamId != batch->selection.streamId ||
			    result.value->stream.id != batch->selection.streamId ||
			    !activeSession(result.value->state)) {
				finishDockSession(batch, ctx);
				return;
			}
			dockSessionHint = sessionId;
			if (batch->dirty) {
				finishDockSession(batch, ctx);
				return;
			}
			if (!batch->next.session || batch->next.session->id != sessionId) {
				batch->next.outputs.clear();
				batch->next.outputsStatus = {};
			}
			batch->next.session = DockSession{sessionId, result.value->state, result.value->desiredPaused};
			batch->next.sessionStatus = {RuntimeAvailability::Fresh, true, {}};
			readDockOutputs(batch, sessionId);
		});
	}

	// Три небольших чтения STARTING/RUNNING/STOPPING не являются атомарным серверным снимком.
	// Любое продолжение, несоответствие фильтру или несколько кандидатов оставляют Session
	// неподтверждённой; порядок прихода страниц не используется как выбор «самой новой».
	void readDockSessionPage(const DockBatch &batch, const QString &status)
	{
		// query ограничивает размер страницы и фиксирует проверяемое активное состояние status.
		api::ListQuery query;
		query.limit = SessionPageSize;
		query.state = status;
		// Обработчик проверяет result/ctx, уникальность Session и точное соответствие фильтру status.
		api.getStreamSessions(
			batch->selection.streamId, &owner,
			[this, batch, status](auto ctx, api::Result<api::Page<api::Session>> result) {
				if (!currentDockRead(batch, ctx))
					return;
				if (!result || !result.value->nextCursor.isEmpty() ||
				    result.value->items.size() > SessionPageSize) {
					batch->complete = false;
				} else {
					// ids содержит уникальные ID этой страницы, затем объединяется с кандидатами других состояний.
					QSet<QString> ids;
					// session — очередной кандидат, чьи родители и состояние сверяются с запросом.
					for (const auto &session : result.value->items) {
						if (!validDockId(session.id) || ids.contains(session.id) ||
						    session.streamId != batch->selection.streamId ||
						    session.stream.id != batch->selection.streamId ||
						    session.state != status) {
							batch->complete = false;
							break;
						}
						ids.insert(session.id);
					}
					batch->candidates.unite(ids);
				}
				if (--batch->remainingPages == 0)
					readExactDockSession(batch, ctx);
			},
			std::move(query));
	}

	// Завершение цепочки policy -> Asset -> точное current Generation. Совпадение обоих ID обязательно;
	// status описывает только метаданные подготовки, не пригодность всех вариантов и не готовность Session.
	void readDockGeneration(const DockBatch &batch, const QString &assetId, const QString &generationId)
	{
		// Обработчик принимает status лишь при совпадении ctx и обоих запрошенных ID.
		api.getGeneration(assetId, generationId, &owner,
				  [this, batch, assetId, generationId](auto ctx, api::Result<api::Generation> result) {
					  if (!currentDockRead(batch, ctx))
						  return;
					  if (result && result.value->id == generationId &&
					      result.value->assetId == assetId)
						  batch->next.fallback.currentStatus = result.value->status;
					  finishDockFallback(batch, ctx);
				  });
	}

	// Читает точный Asset и продолжает цепочку только по его currentGenerationId.
	void readDockAsset(const DockBatch &batch, const QString &assetId)
	{
		// Обработчик result/ctx отличает найденный Asset от отсутствующего текущего поколения.
		api.getAsset(assetId, &owner, [this, batch, assetId](auto ctx, api::Result<api::Asset> result) {
			if (!currentDockRead(batch, ctx))
				return;
			// fallback — собираемая часть метаданных заглушки именно этого пакета.
			auto &fallback = batch->next.fallback;
			if (result && result.value->id == assetId) {
				fallback.assetKnown = true;
				fallback.pendingGenerationId = result.value->pendingGenerationId;
				fallback.currentGenerationId = result.value->currentGenerationId;
				if (validDockId(fallback.currentGenerationId)) {
					readDockGeneration(batch, assetId, fallback.currentGenerationId);
					return;
				}
			}
			finishDockFallback(batch, ctx);
		});
	}

	// Null-политика подтверждает отсутствие назначения. Missing/ошибка сохраняет неизвестность;
	// наличие назначенного Asset ещё не доказывает, что его current удалось прочитать.
	void readDockFallback(const DockBatch &batch)
	{
		// Обработчик result/ctx различает null-политику и назначенный Asset без подмены неизвестности.
		api.getFallbackPolicy(batch->selection.streamId, &owner,
				      [this, batch](auto ctx, api::Result<api::PolicyResult> result) {
					      if (!currentDockRead(batch, ctx))
						      return;
					      if (result && result.value->fallback.isNull()) {
						      batch->next.fallback = {};
						      batch->next.fallback.availability = RuntimeAvailability::Fresh;
						      batch->next.fallback.assigned = false;
					      } else if (result && result.value->fallback &&
							 validDockId(result.value->fallback->assetId)) {
						      batch->next.fallback = {};
						      batch->next.fallback.availability = RuntimeAvailability::Fresh;
						      batch->next.fallback.assigned = true;
						      readDockAsset(batch, result.value->fallback->assetId);
						      return;
					      }
					      finishDockFallback(batch, ctx);
				      });
	}

	// Запускается только после подтверждения SSE и разрешённого выбора. Предыдущие метаданные
	// копируются как неподтверждённые, чтобы отказ чтения не выдавал фиктивное отсутствие Session/выходов.
	void refreshDock()
	{
		if (!active || stopped || state.connectionStatus() != ConnectionStatus::Connected ||
		    !state.dockContext().valid())
			return;
		if (dockRead) {
			// Изменение топологии во время чтения не порождает параллельный пакет: результат перечитывается после него.
			dockRead->dirty = true;
			return;
		}
		// batch удерживается ответами, но применять их разрешает только совпадение с dockRead.
		auto batch = std::make_shared<DockRead>();
		batch->selection = state.dockContext();
		batch->anchorVersion = anchorVersion;
		batch->next = state.dockServer();
		markReadUnconfirmed(batch->next.sessionStatus);
		markReadUnconfirmed(batch->next.outputsStatus);
		if (batch->next.fallback.availability == RuntimeAvailability::Fresh)
			batch->next.fallback.availability = RuntimeAvailability::Stale;
		dockRead = batch;
		// status задаёт один из трёх фильтров поиска активной Session.
		for (const auto &status : ActiveSessionStates)
			readDockSessionPage(batch, status);
		readDockFallback(batch);
	}

	// Определяет, требует ли изменение топологии event перечитать метаданные выбранного Dock.
	bool relevantDockEvent(const api::RuntimeEvent &event) const
	{
		// Изменение известной Session требует перечитать её состав. Событие другой Session
		// запускает поиск лишь при отсутствии/завершении/неподтверждённости текущей, чтобы обычная
		// телеметрия постороннего эфира не порождала постоянные REST-пакеты выбранного Dock.
		// session — последняя выбранная серверная запись, независимо от доступности её runtime.
		const auto &session = state.dockServer().session;
		if (session && session->id == event.sessionId)
			return true;
		return event.owner == QStringLiteral("session") &&
		       (!session || terminalSession(session->state) ||
			state.dockServer().sessionStatus.availability != RuntimeAvailability::Fresh);
	}

	// Терминальный отказ подключения: выключает active до уведомления StateStore, прекращает REST
	// и повторы, сохраняет последние наблюдения. EventClient уже закрыл отказавшую подписку до callback.
	void stopWithReason(ConnectionStatus status, QString message, QString key)
	{
		// expected фиксирует контекст до уведомления; guard проверяет жизнь owner после сигнала.
		const auto expected = context;
		const QPointer<ConnectionController> guard = &owner;
		active = false;
		retry.stop();
		dockRead.reset();
		api.cancelAll();
		state.markRuntimeStale();
		if (guard && !stopped && !active && context == expected)
			state.setConnection(state.serverOrigin(), status, std::move(message), std::move(key));
	}

	// Проверяет, израсходован ли конечный бюджет восстановления после SSE 400/409.
	bool rejectionLimitReached() const
	{
		// limit — выбранное число повторных циклов либо специальное значение без ограничения.
		const int limit = settings.sseRejectionRetries();
		return limit != SettingsStore::UnlimitedSseRejectionRetries && sseRejectionAttempts >= limit;
	}

	// Останавливает восстановление с безопасной локализуемой причиной последнего отказа SSE.
	void stopAfterRejection()
	{
		// badRequest выбирает причину HTTP 400 отдельно от конфликта HTTP 409.
		const bool badRequest = sseRejectionStatus == 400;
		stopWithReason(
			ConnectionStatus::Error,
			badRequest ? QStringLiteral("Сервер отклонил запрос подписки на события (HTTP 400).")
				   : QStringLiteral("Конфликт состояния сервера при подписке на события (HTTP 409)."),
			badRequest ? QStringLiteral("Error.SseBadRequest") : QStringLiteral("Error.SseConflict"));
	}

	// Единственное место обычного планирования повторов. Паузы и лимит частоты стартов независимы:
	// даже неограниченный режим для 400/409 не отменяет паузы. Поколение захватывается для timeout; отключённый
	// reconnect или исчерпанный бюджет прекращают восстановление без нового запроса.
	void schedule(Phase phase)
	{
		if (!active || stopped)
			return;
		if (phase == Phase::Snapshot && sseRejectionStatus &&
		    (!settings.reconnect() || rejectionLimitReached())) {
			stopAfterRejection();
			return;
		}
		if (!settings.reconnect()) {
			stopWithReason(ConnectionStatus::Error,
				       QStringLiteral("Нет связи с сервером. Повторите подключение."),
				       QStringLiteral("Error.ConnectionLost"));
			return;
		}
		nextPhase = phase;
		scheduledContext = context;
		// exponent — прежнее число отказов для текущего экспоненциального увеличения паузы.
		const int exponent = failures;
		failures = std::min(failures + 1, 5);
		// base — базовая пауза до 30 с; jitter — случайное смещение; delay — итоговые 1000..30000 мс.
		const int base = std::min(30000, 1000 * (1 << exponent));
		const int jitter = QRandomGenerator::global()->bounded(std::max(1, base / 5));
		const int delay = std::clamp(base - base / 10 + jitter, 1000, 30000);
		// expected фиксирует контекст; guard проверяет жизнь owner после уведомления об устаревших данных.
		const auto expected = context;
		const QPointer<ConnectionController> guard = &owner;
		retry.start(delay);
		dockRead.reset();
		api.cancelAll();
		state.markRuntimeStale();
		if (guard && current(expected))
			state.setConnection(state.serverOrigin(), ConnectionStatus::Reconnecting,
					    QStringLiteral("Нет связи с сервером. Ожидается повторное подключение."),
					    QStringLiteral("Error.Reconnecting"));
	}

	// Учитывает фактический старт в скользящем монотонном окне 10 с, не число вызовов schedule.
	// Во время вынужденной паузы сохраняет фазу без расходования бюджета 400/409. Новый Snapshot расходует один
	// такой цикл уже до запроса: неудачный snapshot тоже был попыткой восстановления после отказа.
	void start(Phase phase)
	{
		if (!active || stopped)
			return;
		if (phase == Phase::Snapshot && sseRejectionStatus && rejectionLimitReached()) {
			stopAfterRejection();
			return;
		}
		// now — монотонное время текущего старта в миллисекундах для скользящего окна starts.
		const auto now = clock.elapsed();
		while (!starts.empty() && now - starts.front() >= 10000)
			starts.pop_front();
		if (starts.size() >= 3) {
			nextPhase = phase;
			scheduledContext = context;
			retry.start(static_cast<int>(std::max<qint64>(1, 10000 - (now - starts.front()))));
			return;
		}
		starts.push_back(now);
		configure();
		if (phase == Phase::Events && !state.runtime().cursor.isEmpty()) {
			subscribe();
			return;
		}
		if (phase == Phase::Snapshot) {
			// Бюджет отказов подписки расходуется при начале цикла, даже если следующий снимок завершится ошибкой.
			if (sseRejectionStatus && sseRejectionAttempts < std::numeric_limits<int>::max())
				++sseRejectionAttempts;
			snapshot();
			return;
		}
		// Обработчики ready/capabilities/Streams последовательно проверяют свои ctx и результаты;
		// любой отказ повторяет Handshake, а успешный полный цикл переходит к snapshot.
		api.getReady(&owner, [this](auto ctx, api::Result<api::Health> result) {
			if (!current(ctx))
				return;
			if (!result) {
				schedule(Phase::Handshake);
				return;
			}
			state.setReady(std::move(*result.value));
			if (!current(ctx))
				return;
			api.getCapabilities(&owner, [this](auto capsContext, api::Result<api::Capabilities> caps) {
				if (!current(capsContext))
					return;
				if (!caps) {
					schedule(Phase::Handshake);
					return;
				}
				state.setCapabilities(std::move(*caps.value));
				if (!current(capsContext))
					return;
				api.getAllStreams(&owner, [this](auto listContext,
								 api::Result<api::Collection<api::Stream>> streams) {
					if (!current(listContext))
						return;
					if (!streams) {
						schedule(Phase::Handshake);
						return;
					}
					// complete сохраняет полноту до переноса элементов ответа в StateStore.
					const bool complete = streams.value->complete;
					state.setStreams(std::move(streams.value->items), complete);
					if (!current(listContext))
						return;
					resolveSelection(state.profileNotice(), state.profileNoticeKey());
					if (!current(listContext))
						return;
					snapshot();
				});
			});
		});
	}

	// StateStore сначала проверяет и устанавливает снимок целиком. Только после успеха увеличивается
	// anchorVersion и открывается подписка. До её допустимых заголовков StateStore хранит снимок как
	// ожидающий подтверждения: UI ещё не получает свежие наблюдения, а Connected не устанавливается по одному JSON.
	void snapshot()
	{
		// Обработчик result/ctx не открывает SSE после отклонённого snapshot или смены контекста.
		api.getSnapshot(&owner, [this](auto ctx, api::Result<api::Anchor> result) {
			if (!current(ctx))
				return;
			// guard проверяет жизнь owner после уведомлений установки snapshot.
			const QPointer<ConnectionController> guard = &owner;
			if (!result || !state.installAnchor(*result.value, true)) {
				schedule(Phase::Snapshot);
				return;
			}
			if (!guard || !current(ctx))
				return;
			++anchorVersion;
			subscribe();
		});
	}

	// Разделяет терминальные отказы и восстановление. 400/409 начинают новый snapshot с отдельным
	// бюджетом; 204 завершает подписку штатно, 404/постоянный HTTP/TLS/неверный MIME останавливают повторы.
	// Для временного отказа Events использует последний принятый StateStore cursor, не хвост парсера.
	void handleSseFailure(const api::Error &error, bool requiresSnapshot)
	{
		// status — HTTP-код отказа; 400/409 требуют нового snapshot вместо возобновления курсора.
		const int status = error.httpStatus;
		if (error.kind == api::ErrorKind::Http) {
			if (status == 400 || status == 409) {
				sseRejectionStatus = status;
				schedule(Phase::Snapshot);
				return;
			}
			if (status == 204) {
				stopWithReason(ConnectionStatus::Disconnected,
					       QStringLiteral("Сервер завершил подписку на события."),
					       QStringLiteral("Error.SseEnded"));
				return;
			}
			if (status == 404) {
				stopWithReason(ConnectionStatus::Error,
					       QStringLiteral("На сервере не найден маршрут событий (HTTP 404)."),
					       QStringLiteral("Error.SseNotFound"));
				return;
			}
			if (status != 408 && status != 429 && (status < 500 || status > 599)) {
				stopWithReason(ConnectionStatus::Error,
					       QStringLiteral("Сервер отклонил подписку на события."),
					       QStringLiteral("Error.SseRejected"));
				return;
			}
		}
		if (error.kind == api::ErrorKind::Tls) {
			stopWithReason(ConnectionStatus::Error, error.message, QStringLiteral("Error.SseTls"));
			return;
		}
		if (error.kind == api::ErrorKind::Contract && status == 200) {
			stopWithReason(ConnectionStatus::Error,
				       QStringLiteral("Ответ сервера не содержит поток событий SSE."),
				       QStringLiteral("Error.SseInvalidContentType"));
			return;
		}
		schedule(requiresSnapshot ? Phase::Snapshot : Phase::Events);
	}

	// EventHandler возвращает true лишь после применения и проверки актуальности контекста.
	// OpenHandler отдельно сбрасывает бюджет отказов. Успех нового snapshot и временный обрыв
	// до заголовков его не сбрасывают, иначе последовательность 409/snapshot/503 обходила бы лимит.
	void subscribe()
	{
		events.subscribe(
			QUrl(state.serverOrigin()), state.runtime().cursor, context, &owner,
			[this](auto ctx, const api::RuntimeEvent &event) {
				if (!current(ctx))
					return false;
				// guard проверяет жизнь owner после reducer; topologyChanged требует обновить REST-состав.
				const QPointer<ConnectionController> guard = &owner;
				bool topologyChanged = false;
				if (!state.applyEvent(event, &topologyChanged))
					return false;
				if (!guard || !current(ctx))
					return false;
				failures = 0;
				if (topologyChanged && relevantDockEvent(event))
					refreshDock();
				return true;
			},
			[this](auto ctx, api::Error error, bool requiresSnapshot) {
				if (!current(ctx))
					return;
				handleSseFailure(error, requiresSnapshot);
			},
			[this](auto ctx) {
				if (!current(ctx))
					return;
				// Счётчик отказов сбрасывается по заголовкам принятой подписки, а не по получению снимка.
				clearSseRejection();
				// guard запрещает чтение Dock после удаления owner при уведомлении Connected.
				const QPointer<ConnectionController> guard = &owner;
				state.setConnection(state.serverOrigin(), ConnectionStatus::Connected);
				if (guard && current(ctx))
					refreshDock();
			});
	}

	// Проверяет origin, сохраняет адрес и начинает новое согласование без изменения OBS.
	void connectToServer(const QString &origin)
	{
		if (stopped)
			return;
		// canonical содержит допустимый канонический HTTP(S) origin либо отсутствие при неверном вводе.
		const auto canonical = SettingsStore::canonicalOrigin(origin);
		if (!canonical) {
			state.setConnection(
				state.serverOrigin(), state.connectionStatus(),
				QStringLiteral("Укажите адрес http/https без пути, учётных данных, query и fragment."),
				QStringLiteral("Error.InvalidServerUrl"));
			return;
		}
		active = false;
		invalidate(true);
		settings.setServerOrigin(*canonical);
		// sameOrigin разрешает сохранить прежние метаданные Dock как устаревшие.
		const bool sameOrigin = state.serverOrigin() == *canonical;
		if (sameOrigin)
			state.markRuntimeStale();
		state.clearServerState(sameOrigin);
		state.setConnection(*canonical, ConnectionStatus::Connecting);
		resolveSelection();
		failures = 0;
		active = true;
		start(Phase::Handshake);
	}
};

ConnectionController::ConnectionController(StateStore &state, SettingsStore &settings, api::APIClient &api,
					   api::EventClient &events, QObject *parent)
	: QObject(parent),
	  impl_(std::make_unique<Impl>(*this, state, settings, api, events))
{
}

ConnectionController::~ConnectionController()
{
	shutdown();
}

void ConnectionController::observeProfile(FrontendEvent event, const ProfileDescriptor &descriptor)
{
	Q_ASSERT(QThread::currentThread() == thread());
	auto &self = *impl_;
	if (self.stopped)
		return;
	const bool wasResolved = self.profileResolved;
	const auto resolution = self.settings.observeProfile(event, descriptor);
	self.profileResolved = resolution.resolved;
	const bool selectionChanged = resolution.changed || wasResolved != resolution.resolved;
	// Подтверждённое переименование сохраняет UUID и подписку; настоящий переход меняет поколение выбора.
	if (selectionChanged && !resolution.renamed) {
		self.invalidate(false);
		self.state.clearRuntime();
		self.configure();
	}
	self.resolveSelection(
		selectionChanged && !resolution.renamed
			? QStringLiteral("Профиль OBS изменён. Проверьте соответствие профиля выбранному Stream.")
			: self.state.profileNotice(),
		selectionChanged && !resolution.renamed ? QStringLiteral("Profile.BindingNotice")
							: self.state.profileNoticeKey());
	if (self.active && selectionChanged && !resolution.renamed)
		self.start(Impl::Phase::Handshake);
	if (event == FrontendEvent::FinishedLoading && !self.autoStarted) {
		self.autoStarted = true;
		if (self.settings.autoConnect() && !self.settings.serverOrigin().isEmpty() && !self.active)
			self.connectToServer(self.settings.serverOrigin());
	}
}

void ConnectionController::connectToServer(QString origin)
{
	Q_ASSERT(QThread::currentThread() == thread());
	impl_->connectToServer(origin);
}

// Отменяет наблюдение по явному запросу и отдельно уведомляет исполнителя команд до смены статуса.
void ConnectionController::disconnectFromServer()
{
	// self заимствует реализацию до первого сигнала; guard запрещает продолжение после удаления owner.
	auto &self = *impl_;
	if (self.stopped)
		return;
	self.active = false;
	self.invalidate(true);
	// guard контролирует удаление ConnectionController синхронным обработчиком сигнала.
	const QPointer<ConnectionController> guard = this;
	emit explicitlyDisconnected();
	if (!guard)
		return;
	self.state.markRuntimeStale();
	if (!guard)
		return;
	self.state.setConnection(self.state.serverOrigin(), ConnectionStatus::Disconnected);
}

// Объединяет обновление с уже запущенным DockRead; состояние соединения проверяется в refreshDock.
void ConnectionController::refreshState()
{
	Q_ASSERT(QThread::currentThread() == thread());
	impl_->refreshDock();
}

void ConnectionController::selectStream(QString streamId)
{
	auto &self = *impl_;
	if (self.stopped || !self.profileResolved || streamId == self.state.selectedStreamId())
		return;
	if (!streamId.isEmpty() && std::none_of(self.state.streams().cbegin(), self.state.streams().cend(),
						[&streamId](const auto &stream) { return stream.id == streamId; }))
		return;
	if (!self.settings.setSelectedStream(self.state.serverOrigin(), self.settings.profileUuid(), streamId))
		return;
	self.invalidate(false);
	self.state.clearRuntime();
	self.resolveSelection();
	if (self.active)
		self.start(Impl::Phase::Handshake);
}

void ConnectionController::setAutoConnect(bool enabled)
{
	if (!impl_->stopped)
		impl_->settings.setAutoConnect(enabled);
}

void ConnectionController::setReconnect(bool enabled)
{
	if (impl_->stopped)
		return;
	impl_->settings.setReconnect(enabled);
	if (!enabled && (impl_->retry.isActive() || impl_->state.connectionStatus() == ConnectionStatus::Reconnecting))
		disconnectFromServer();
}

void ConnectionController::setSseRejectionRetries(int retries)
{
	if (!impl_->stopped)
		impl_->settings.setSseRejectionRetries(retries);
}

void ConnectionController::shutdown()
{
	if (!impl_ || impl_->stopped)
		return;
	impl_->stopped = true;
	impl_->active = false;
	impl_->invalidate(true);
	impl_->api.shutdown();
	impl_->events.shutdown();
	impl_->state.markRuntimeStale();
	impl_->state.setConnection(impl_->state.serverOrigin(), ConnectionStatus::Disconnected);
}

} // namespace gost
