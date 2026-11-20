#include "fallback-controller.hpp"
#include "api/api-client.hpp"
#include "core/configuration-controller.hpp"
#include "core/restream-controller.hpp"
#include "core/state-store.hpp"

#include <QElapsedTimer>
#include <QPointer>
#include <QSet>
#include <QTimer>
#include <algorithm>
#include <map>
#include <tuple>

namespace gost {
namespace {
// Отвергает пустые и недопустимые непрозрачные идентичности до адресного GET.
bool validId(const QString &id)
{
	return !id.isEmpty() && id.size() <= 4096 && !id.contains(QChar::Null);
}
// Эти серверные состояния обозначают активную Session, включая ручную паузу RUNNING.
bool activeSession(const QString &status)
{
	return status == QStringLiteral("STARTING") || status == QStringLiteral("RUNNING") ||
	       status == QStringLiteral("STOPPING");
}
// Проверяет терминальный результат подготовки, не выводя его из состояния Asset.
bool terminalGeneration(const QString &status)
{
	return status == QStringLiteral("ready") || status == QStringLiteral("error");
}
} // namespace

/** Отделяет передачу mutation от последующего наблюдения точного поколения.
 * Все поля принадлежат GUI event loop. Каждый receiver ограничивает лишь свою цепочку
 * GET или одну запись; ConfigurationController остаётся единственным владельцем канала.
 * Unknown хранится по origin независимо от нынешней карточки и снимается только свежей сверкой.
 */
class FallbackController::Impl {
public:
	// Подключает lifecycle раньше начальной синхронизации, чтобы не пропустить закрытие канала.
	Impl(FallbackController &owner, StateStore &state, api::APIClient &api, ConfigurationController &configuration,
	     RestreamController &restream, Limits limits)
		: owner(owner),
		  state(state),
		  api(api),
		  configuration(configuration),
		  restream(restream),
		  limits(limits)
	{
		poll.setSingleShot(true);
		deadline.setSingleShot(true);
		reconcilePoll.setSingleShot(true);
		// Закрытие сохраняет потенциально отправленную запись до глобальной отмены API.
		QObject::connect(&configuration, &ConfigurationController::channelClosing, &owner,
				 [this] { closeChannel(); });
		// Готовность публикуется только после configure нового логического контекста.
		QObject::connect(&configuration, &ConfigurationController::channelReady, &owner,
				 [this] { openChannel(); });
		// Изменённые рецепты отзывают свежесть и старые GET, сохраняя собственную принятую пару.
		QObject::connect(&state, &StateStore::configurationInvalidated, &owner,
				 [this](quint64) { invalidateReads(); });
		// Таймер продолжает только точное собственное наблюдение, а не повтор mutation.
		QObject::connect(&poll, &QTimer::timeout, &owner, [this] { observeGeneration(); });
		// Исчерпание бюджета не превращает серверное поколение в error.
		QObject::connect(&deadline, &QTimer::timeout, &owner, [this] {
			// guard проверяет жизнь фасада; token и expected фиксируют истёкший цикл чтения и его контекст.
			const QPointer<FallbackController> guard = &this->owner;
			const auto token = readToken;
			const auto expected = context;
			poll.stop();
			cancelScope(observationReceiver);
			// Отмена может запустить чужой обработчик, который начнёт Refresh или закроет канал.
			if (!guard || stopped || closing || readToken != token || context != expected ||
			    deadline.isActive())
				return;
			model.observation = FallbackObservation::Unknown;
			model.messageKey = QStringLiteral("Fallback.Incomplete");
			publish();
		});
		// Чтение неизвестного pending продолжается в пределах одного бюджета сверки.
		QObject::connect(&reconcilePoll, &QTimer::timeout, &owner, [this] { reconcileUnknown(); });
		// Восстановление Connected разрешает свежую сверку, но не повтор отправленного запроса.
		QObject::connect(&state, &StateStore::serverStateChanged, &owner, [this] {
			// connected фиксирует переход статуса, а не каждую публикацию кеша.
			const bool connected = this->state.connectionStatus() == ConnectionStatus::Connected;
			if (connected && !wasConnected && ready)
				queueRefresh();
			wasConnected = connected;
		});
		if (configuration.channelIsReady())
			openChannel();
	}

	// Фасад ограничивает обработчики и является QObject-родителем собственных receivers.
	FallbackController &owner;
	// Общее состояние принимает только очищенные value-модели.
	StateStore &state;
	// Заимствованный клиент; глобальные configure/cancelAll/shutdown здесь запрещены.
	api::APIClient &api;
	// Единственный владелец жизненного цикла конфигурационного канала.
	ConfigurationController &configuration;
	// Прежний исполнитель предоставляет единую предварительную проверку и matcher.
	RestreamController &restream;
	// Внутренние конечные сроки наблюдения и сверки.
	Limits limits;
	// Публикуемая модель нынешнего логического контекста.
	FallbackState model;
	// Копия контекста не позволяет обработчикам прежнего канала применить ответ к новой форме.
	ConfigurationContext context;
	// Точное поколение API сверяется вместе с собственными токенами.
	api::RequestContext requestContext;
	// Канал доступен только между channelReady и channelClosing.
	bool ready = false;
	// Shutdown необратимо запрещает новые действия.
	bool stopped = false;
	// Закрытие подавляет побочные обновления из синхронных сигналов отмены.
	bool closing = false;
	// Последняя связность отличает переход Connected от обновлений коллекций.
	bool wasConnected = false;
	// Свежесть policy относится к точному target и последнему завершённому GET.
	bool policyFresh = false;
	// Монотонный номер отличает мутации даже при возврате к прежнему origin.
	quint64 mutationToken = 0;
	// Монотонное поколение собственных чтений отзывается перед отменой receivers.
	quint64 readToken = 0;
	// Поколение обязательств не позволяет GET до Unknown снять новый барьер.
	quint64 reconciliationToken = 0;
	// Пауза между GET точного принятого поколения.
	QTimer poll;
	// Общий срок цикла: очередь GET, Generation, все страницы Variants и итоговый GET Asset.
	QTimer deadline;
	// Пауза только для сверки ещё выполняющегося неизвестного задания.
	QTimer reconcilePoll;
	// Начало ограниченного цикла сверки pending без принятого ID.
	QElapsedTimer reconciliationBudget;
	// Получатель ограничивает цепочку страниц каталога Asset.
	QPointer<QObject> catalogReceiver;
	// Получатель ограничивает GET карточки и точных current/pending Generation/Variants.
	QPointer<QObject> detailReceiver;
	// Получатель относится лишь к нынешней адресной policy форме.
	QPointer<QObject> policyReceiver;
	// Получатель относится к точной собственной принятой паре и её последующему GET Asset.
	QPointer<QObject> observationReceiver;
	// Получатель ограничивает один цикл сверки сохранённого Unknown.
	QPointer<QObject> reconciliationReceiver;
	// Вид mutation выбирает обязательный способ сверки.
	enum class WriteKind {
		// Неизвестный create сверяется полным каталогом без угадывания ID по имени.
		Create,
		// Неизвестный DELETE требует GET точного Asset.
		Delete,
		// Передача оригинала сверяется через Asset и точное серверное поколение.
		Upload,
		// Переподготовка сохранённого оригинала сверяется без повторного POST.
		Rebuild,
		// Полный PUT сверяется адресными GET policy и Stream.
		Policy
	};
	// Содержит неизменяемую адресацию единственной записи до получения ответа.
	struct Write {
		// Исходный логический контекст не изменяется навигацией пользователя.
		ConfigurationContext context;
		// Вид операции определяет значение успешного ответа и сверку Unknown.
		WriteKind kind = WriteKind::Create;
		// Точный Asset исходного намерения; пуст только у create.
		QString assetId;
		// Точный Stream исходного PUT policy, не текущий выбор Dock.
		QString streamId;
		// Поколение владения общим барьером.
		quint64 token = 0;
		// Получатель отменяет лишь HTTP этого намерения.
		QPointer<QObject> receiver;
	};
	// Единственная mutation существует до достоверного ответа либо Unknown/NotSent.
	std::optional<Write> write;
	// Точное обязательство не содержит payload для повтора.
	struct Unknown {
		// Origin сохраняет запрет при смене OBS Profile и Stream.
		QString origin;
		// Вид задаёт GET Asset, policy либо полный каталог.
		WriteKind kind = WriteKind::Create;
		// Адрес Asset для delete/upload/rebuild.
		QString assetId;
		// Известная принятая пара, если она была подтверждена отдельно.
		QString generationId;
		// Адрес политики исходного Stream.
		QString streamId;
	};
	// Не более 64 обязательств; новые намерения отклоняются до роста сверх предела.
	QVector<Unknown> unknown;
	// Поколение собственного принятия отличает уведомления о подготовке от последующих mutation.
	quint64 acceptedToken = 0;
	// Ключ паркует ввод по полному логическому контексту без неоднозначной склейки строк.
	using ContextKey = std::tuple<QString, QString, QString>;
	// Старые формы остаются только в RAM; не являются журналом повторяемых операций.
	std::map<ContextKey, FallbackState> parked;
	// Ключ policy дополнительно фиксирует явно просматриваемый Stream.
	using PolicyKey = std::tuple<QString, QString, QString, QString>;
	// Изменённая форма policy сохраняется при переходе между карточками Stream.
	std::map<PolicyKey, FallbackPolicyState> policies;

	// Возвращает точный ключ логического контекста.
	ContextKey key() const { return {context.origin, context.profileUuid, context.workingStreamId}; }
	// Возвращает ключ адресной формы policy нынешнего контекста.
	PolicyKey policyKey(const QString &streamId) const
	{
		return {context.origin, context.profileUuid, context.workingStreamId, streamId};
	}
	// Проверяет наличие обязательств именно нынешнего сервера.
	bool hasUnknown() const
	{
		return std::any_of(unknown.cbegin(), unknown.cend(),
				   [this](const auto &item) { return item.origin == context.origin; });
	}
	// Публикует целое значение и сообщает, пережил ли контроллер синхронный сигнал.
	bool publish()
	{
		if (stopped)
			return false;
		model.reconciliationRequired = hasUnknown();
		++model.generation;
		// guard защищает продолжение от удаления владельца обработчиком UI.
		const QPointer<FallbackController> guard = &owner;
		state.setFallback(model);
		return guard && !stopped;
	}
	// Отменяет один scope, не затрагивая соседние запросы общего API.
	bool cancelScope(QPointer<QObject> &receiver)
	{
		if (!receiver)
			return false;
		// scope отсоединяется до отмены, чтобы повторный вход не отменил его дважды.
		const QPointer<QObject> scope = receiver;
		receiver = nullptr;
		// sent сообщает только фактическую передачу этого scope транспорту.
		const bool sent = api.cancelRequests(scope);
		delete scope.data();
		return sent;
	}
	// Создаёт новый scope после безопасной адресной отмены предыдущего.
	QObject *newScope(QPointer<QObject> &receiver)
	{
		// guard обнаруживает удаление владельца при запуске чужого отложенного обработчика из API отмены.
		const QPointer<FallbackController> guard = &owner;
		cancelScope(receiver);
		if (!guard)
			return nullptr;
		receiver = new QObject(&owner);
		return receiver;
	}
	// Отменяет все собственные цепочки чтения с проверкой жизни между вызовами API.
	bool cancelReadScopes()
	{
		// guard обнаруживает удаление фасада из чужого обработчика, запущенного сетевой очередью.
		const QPointer<FallbackController> guard = &owner;
		cancelScope(catalogReceiver);
		if (!guard)
			return false;
		cancelScope(detailReceiver);
		if (!guard)
			return false;
		cancelScope(policyReceiver);
		if (!guard)
			return false;
		cancelScope(observationReceiver);
		if (!guard)
			return false;
		cancelScope(reconciliationReceiver);
		return !guard.isNull();
	}
	// Допускает новые HTTP только в настроенном канале и подтверждённом профиле.
	bool available() const
	{
		return !stopped && !closing && ready && context.valid() && configuration.channelIsReady() &&
		       configuration.channelContext() == context &&
		       configuration.channelRequestContext() == requestContext && state.profileResolved() &&
		       context == ConfigurationContext{state.serverOrigin(), state.dockContext().profileUuid,
						       state.selectedStreamId()} &&
		       state.connectionStatus() == ConnectionStatus::Connected;
	}
	// Разрешает mutation только при свободном общем барьере и конечном наборе обязательств.
	bool canWrite() const
	{
		return available() && !write && unknown.size() < 64 && state.configurationSettled(context.origin);
	}
	// Проверяет свежую revision, отсутствие pending и локально наблюдаемой активной Session.
	bool canPrepare() const
	{
		if (!canWrite() || !model.assetFresh || !model.asset || model.asset->id != model.selectedAssetId ||
		    model.asset->revision == 0 || !model.asset->pendingGenerationId.isEmpty())
			return false;
		if (state.dockServer().session && activeSession(state.dockServer().session->state))
			return false;
		// session относится к известному runtime; пауза не меняет RUNNING на terminal.
		for (const auto &session : state.runtime().sessions)
			if (session.observation && activeSession(session.observation->state))
				return false;
		return true;
	}
	// Требует именно полную свежую базу target Stream и явное изменение пользователя.
	bool canSavePolicy() const
	{
		return canWrite() && validId(model.policy.streamId) && policyFresh && model.policy.base &&
		       model.policy.base->revision > 0 && !model.policy.base->fallback.isMissing() &&
		       !model.policy.loading && !model.policy.serverConflict && model.policy.dirty;
	}
	// Проверяет контекст и эпоху цепочки GET; старое чтение не освежает новый каталог.
	bool current(quint64 token, quint64 epoch, const ConfigurationContext &expected,
		     api::RequestContext received) const
	{
		return available() && token == readToken && epoch == state.configurationGeneration() &&
		       context == expected && received == requestContext;
	}
	// Объединяет обновления в следующий оборот GUI без создания очереди пользовательских mutation.
	void queueRefresh()
	{
		QTimer::singleShot(0, &owner, [this] {
			if (available())
				refresh();
		});
	}
	// Сохраняет Unknown до снятия активного владения, не позволяя новым записям обойти сверку.
	bool rememberUnknown(const Write &intent)
	{
		unknown.append({intent.context.origin, intent.kind, intent.assetId, {}, intent.streamId});
		++reconciliationToken;
		// guard защищает публикацию общего запрета от удаления контроллера.
		const QPointer<FallbackController> guard = &owner;
		state.setConfigurationUnknown(intent.context.origin, QStringLiteral("Fallback"), true);
		return guard && !stopped && context == intent.context && write && write->token == intent.token;
	}
	// Завершает владение mutation; после 202 наблюдение уже не держит блокировку CRUD.
	bool releaseWrite()
	{
		if (!write)
			return true;
		// intent сохраняет исходный адрес до синхронного уведомления освобождения.
		const auto intent = *write;
		write.reset();
		model.writing = false;
		// guard защищает удаление receiver и последующее уведомление барьера.
		const QPointer<FallbackController> guard = &owner;
		// receiver уничтожается после удаления write, поэтому вложенная отмена не повторяет завершение.
		delete intent.receiver.data();
		if (!guard)
			return false;
		state.endConfigurationWrite(intent.context.origin, QStringLiteral("Fallback"), intent.token);
		return guard && !stopped && context == intent.context;
	}
	// Отзывает токены и собственные ресурсы, пока прежний канал ещё сконфигурирован.
	void closeChannel()
	{
		if (closing || stopped)
			return;
		closing = true;
		ready = false;
		++readToken;
		poll.stop();
		deadline.stop();
		reconcilePoll.stop();
		// guard защищает закрытие от удаления из уведомления Unknown/освобождения.
		const QPointer<FallbackController> guard = &owner;
		if (write) {
			// intent фиксирует прежнюю mutation, sent показывает фактический dispatch.
			const auto intent = *write;
			const bool sent = api.cancelRequests(intent.receiver);
			if (!guard)
				return;
			model.outcome = sent ? FallbackWriteOutcome::Unknown : FallbackWriteOutcome::NotSent;
			if (intent.kind == WriteKind::Policy)
				model.policy.outcome = model.outcome;
			if (sent && !rememberUnknown(intent))
				return;
			if (!releaseWrite() || !guard)
				return;
		}
		if (!cancelReadScopes())
			return;
		model.assetFresh = false;
		policyFresh = false;
		model.loading = false;
		model.policy.loading = false;
		if (model.observation == FallbackObservation::Observing)
			model.observation = FallbackObservation::Unknown;
		parked.insert_or_assign(key(), model);
		closing = false;
		publish();
	}
	// Принимает готовый канал и восстанавливает только форму его полного логического контекста.
	void openChannel()
	{
		if (stopped || !configuration.channelIsReady())
			return;
		context = configuration.channelContext();
		requestContext = configuration.channelRequestContext();
		// found относится только к точному origin/UUID/рабочему Stream.
		const auto found = parked.find(key());
		model = found == parked.end() ? FallbackState{} : found->second;
		model.context = context;
		model.assetFresh = false;
		model.loading = false;
		model.policy.loading = false;
		policyFresh = false;
		ready = true;
		if (!publish())
			return;
		queueRefresh();
	}
	// Снимает свежесть локальных доказательств, сохраняя accepted идентичность при изменении рецепта.
	void invalidateReads()
	{
		if (stopped || closing)
			return;
		++readToken;
		if (!cancelReadScopes())
			return;
		model.assetFresh = false;
		policyFresh = false;
		model.loading = false;
		model.policy.loading = false;
		model.assetsStatus.availability = RuntimeAvailability::Stale;
		model.current.variantsStatus.availability = RuntimeAvailability::Stale;
		model.pending.variantsStatus.availability = RuntimeAvailability::Stale;
		model.accepted.variantsStatus.availability = RuntimeAvailability::Stale;
		if (model.observation == FallbackObservation::Ready)
			model.observation = FallbackObservation::Unknown;
		if (model.observation == FallbackObservation::Observing && deadline.isActive())
			poll.start(std::max(1, limits.pollIntervalMs));
		publish();
	}
	// Занимает общий барьер до публикации намерения; непосредственно перед HTTP проверяется тот же token.
	bool beginWrite(WriteKind kind, const QString &assetId = {}, const QString &streamId = {})
	{
		if (!canWrite())
			return false;
		write = Write{context, kind, assetId, streamId, ++mutationToken, new QObject(&owner)};
		model.writing = true;
		model.outcome = FallbackWriteOutcome::Pending;
		model.messageKey.clear();
		model.bytesSent = 0;
		model.totalBytes = 0;
		if (kind == WriteKind::Policy)
			model.policy.outcome = FallbackWriteOutcome::Pending;
		// token и guard ограждают продолжение после уведомления общей активности.
		const auto token = write->token;
		const QPointer<FallbackController> guard = &owner;
		if (!state.beginConfigurationWrite(context.origin, QStringLiteral("Fallback"), token)) {
			delete write->receiver.data();
			write.reset();
			model.writing = false;
			return false;
		}
		if (!guard || !write || write->token != token)
			return false;
		state.invalidateConfiguration();
		if (!guard || !write || write->token != token)
			return false;
		if (!publish())
			return false;
		return writeCurrent(token);
	}
	// Проверяет исходную mutation прямо перед HTTP и при его результате.
	bool writeCurrent(quint64 token) const
	{
		return available() && write && write->token == token && write->context == context &&
		       state.ownsConfigurationWrite(context.origin, QStringLiteral("Fallback"), token);
	}
	// Доставляет проверенный отказ своего намерения; тело не попадает в общую модель.
	bool notify(OperationFailure failure, quint64 token, const QString &step)
	{
		failure.source = OperationFailureSource::Fallback;
		failure.context = {context.origin, context.profileUuid, context.workingStreamId};
		failure.generation = token;
		failure.stepIdentity = step;
		// guard и expected защищают продолжение после асинхронного presenter.
		const QPointer<FallbackController> guard = &owner;
		const auto expected = context;
		emit owner.operationFailed(failure);
		return guard && !stopped && context == expected && !closing;
	}
	// Завершает отказ без повторного запроса; только revision_conflict открывает сравнение policy.
	void failed(const api::Error &error)
	{
		if (!write)
			return;
		// intent сохраняет исходный вид и адрес до освобождения владения.
		const auto intent = *write;
		// uncertain различает неоднозначную потерю ответа и известный HTTP/локальный отказ.
		const bool uncertain = error.requestSent && !mutationRejected(error);
		model.outcome = uncertain ? FallbackWriteOutcome::Unknown : FallbackWriteOutcome::Rejected;
		if (intent.kind == WriteKind::Policy)
			model.policy.outcome = revisionConflict(error) ? FallbackWriteOutcome::Conflict : model.outcome;
		model.messageKey = uncertain ? QStringLiteral("Configuration.Unknown")
					     : QStringLiteral("Configuration.Rejected");
		if (uncertain && !rememberUnknown(intent))
			return;
		if (!releaseWrite())
			return;
		// guard ограждает дальнейшее чтение и уведомление после публикации новой эпохи.
		const QPointer<FallbackController> guard = &owner;
		state.invalidateConfiguration();
		if (!guard || !publish())
			return;
		if (mutationRejected(error) || error.requestSent) {
			// failure содержит полученный отказ либо локальное пояснение потерянного подтверждения.
			auto failure = operationFailure(error);
			if (uncertain) {
				failure.reasonKey = error.kind == api::ErrorKind::Timeout
							    ? QStringLiteral("Configuration.ConfirmationTimeout")
							    : QStringLiteral("Configuration.ConfirmationUnavailable");
				failure.actionKey = QStringLiteral("Configuration.RefreshBeforeSaving");
			}
			if (!notify(std::move(failure), intent.token,
				    QString::number(static_cast<int>(intent.kind)) + QLatin1Char('/') + intent.assetId +
					    intent.streamId))
				return;
		}
		if (intent.kind == WriteKind::Policy && revisionConflict(error))
			readPolicy(true);
		else
			queueRefresh();
	}
	// Увеличивает эпоху после достоверного завершения и планирует свежую предварительную проверку.
	bool writeFinished()
	{
		if (!releaseWrite())
			return false;
		// guard защищает продолжение после синхронной инвалидации readiness.
		const QPointer<FallbackController> guard = &owner;
		state.invalidateConfiguration();
		if (!guard || !publish())
			return false;
		restream.refreshFallbackAfterConfiguration();
		return guard && !stopped;
	}

	// Читает полный ограниченный каталог; отсутствие в частичном ответе не доказывает удаления.
	void readCatalog()
	{
		if (!available())
			return;
		// receiver ограничивает обход каталога, token фиксирует поколение чтения, epoch — эпоху конфигурации.
		// expected сохраняет логический контекст до возможного синхронного вызова обработчика.
		// alive проверяет жизнь фасада после адресной отмены прежнего scope каталога.
		const QPointer<FallbackController> alive = &owner;
		const QPointer<QObject> receiver = newScope(catalogReceiver);
		if (!alive || !receiver)
			return;
		const auto token = readToken;
		const auto epoch = state.configurationGeneration();
		const auto expected = context;
		model.loading = true;
		if (!publish() || !receiver || !current(token, epoch, expected, requestContext))
			return;
		// Callback проверяет дубли ID и не выводит собственное create из совпадения имени.
		api.getAllAssets(receiver, [this, receiver, token, epoch,
					    expected](auto received, api::Result<api::Collection<api::Asset>> result) {
			if (!receiver || !current(token, epoch, expected, received))
				return;
			model.loading = false;
			if (!result) {
				model.assetsStatus.availability = RuntimeAvailability::Stale;
				model.messageKey = QStringLiteral("Configuration.ReadFailed");
				publish();
				return;
			}
			// values собирает весь полученный каталог до атомарной публикации.
			QMap<QString, api::Asset> values;
			// asset проверяется независимо от полноты ответа.
			for (const auto &asset : result.value->items) {
				if (!validId(asset.id) || values.contains(asset.id)) {
					model.assetsStatus = {RuntimeAvailability::Stale, false,
							      QStringLiteral("Configuration.ReadFailed")};
					publish();
					return;
				}
				values.insert(asset.id, asset);
			}
			if (result.value->complete)
				model.assets = values;
			else {
				// asset дополняет известную часть, не стирая пропущенные ID.
				for (const auto &asset : values)
					model.assets.insert(asset.id, asset);
			}
			model.assetsStatus = {RuntimeAvailability::Fresh, result.value->complete, {}};
			publish();
		});
	}
	// Читает точное current/pending; один generation_id сохраняется на всех страницах вариантов.
	void readGeneration(FallbackGenerationState target, bool pending)
	{
		if (!detailReceiver || !validId(target.generationId))
			return;
		// receiver/token/epoch/expected фиксируют исходную карточку и её HTTP scope.
		const QPointer<QObject> receiver = detailReceiver;
		const auto token = readToken;
		const auto epoch = state.configurationGeneration();
		const auto expected = context;
		// Callback сохраняет только точное поколение и очищает временную сырую диагностику.
		api.getGeneration(
			target.assetId, target.generationId, receiver,
			[this, receiver, token, epoch, expected, target,
			 pending](auto received, api::Result<api::Generation> result) mutable {
				if (!receiver || !current(token, epoch, expected, received) ||
				    model.selectedAssetId != target.assetId)
					return;
				if (!result || result.value->id != target.generationId ||
				    result.value->assetId != target.assetId) {
					target.variantsStatus = {RuntimeAvailability::Stale, false,
								 QStringLiteral("Configuration.ReadFailed")};
					if (pending)
						model.pending = target;
					else
						model.current = target;
					publish();
					return;
				}
				result.value->failure.value.diagnosticBody.clear();
				target.generation = *result.value;
				// query неизменно адресует ровно одно поколение во всём getAllVariants.
				api::ListQuery query;
				query.generationId = target.generationId;
				// Callback требует полноту, точных родителей и уникальность ID вариантов.
				api.getAllVariants(
					target.assetId, receiver,
					[this, receiver, token, epoch, expected, target,
					 pending](auto variantContext,
						  api::Result<api::Collection<api::Variant>> variants) mutable {
						if (!receiver || !current(token, epoch, expected, variantContext) ||
						    model.selectedAssetId != target.assetId)
							return;
						// ids обнаруживает дубли между страницами неатомарного списка.
						QSet<QString> ids;
						// valid не допускает Ready по неполному или чужому набору.
						bool valid = variants && variants.value->complete;
						if (variants) {
							// variant должен принадлежать зафиксированной паре Asset/Generation.
							for (const auto &variant : variants.value->items) {
								if (!validId(variant.id) || ids.contains(variant.id) ||
								    variant.assetId != target.assetId ||
								    variant.generationId != target.generationId)
									valid = false;
								ids.insert(variant.id);
							}
							target.variants = variants.value->items;
						}
						target.variantsStatus = {
							valid ? RuntimeAvailability::Fresh : RuntimeAvailability::Stale,
							valid,
							valid ? QString{} : QStringLiteral("Fallback.Incomplete")};
						if (pending)
							model.pending = target;
						else
							model.current = target;
						publish();
					},
					query);
			});
	}
	// Получает Asset адресно; только его указатели определяют опубликованный current и серверный pending.
	void readAsset()
	{
		if (!available() || !validId(model.selectedAssetId))
			return;
		// receiver/token/epoch/expected/id фиксируют полную принадлежность карточки до HTTP.
		// alive проверяет жизнь фасада после адресной отмены прежнего scope карточки.
		const QPointer<FallbackController> alive = &owner;
		const QPointer<QObject> receiver = newScope(detailReceiver);
		if (!alive || !receiver)
			return;
		const auto token = readToken;
		const auto epoch = state.configurationGeneration();
		const auto expected = context;
		const auto id = model.selectedAssetId;
		model.assetFresh = false;
		// UI должен отозвать подготовку до HTTP. Синхронный обработчик публикации может сменить
		// карточку, эпоху или удалить владельца; в таком случае прежнее чтение больше не отправляется.
		if (!publish() || !alive || !receiver || !current(token, epoch, expected, requestContext) ||
		    model.selectedAssetId != id)
			return;
		// Callback не принимает другое ID и не позволяет старому current доказать новую публикацию.
		api.getAsset(
			id, receiver,
			[this, receiver, token, epoch, expected, id](auto received, api::Result<api::Asset> result) {
				if (!receiver || !current(token, epoch, expected, received) ||
				    model.selectedAssetId != id)
					return;
				if (!result || result.value->id != id) {
					model.messageKey = QStringLiteral("Configuration.ReadFailed");
					model.assetFresh = false;
					publish();
					return;
				}
				// previousCurrent фиксирует опубликованную идентичность до обновления.
				const auto previousCurrent = model.asset ? model.asset->currentGenerationId : QString{};
				model.asset = *result.value;
				model.assetFresh = true;
				model.assets.insert(id, *result.value);
				model.current = {};
				model.pending = {};
				model.current.assetId = id;
				model.current.generationId = result.value->currentGenerationId;
				model.pending.assetId = id;
				model.pending.generationId = result.value->pendingGenerationId;
				// guard защищает повторное чтение после публикации обнаруженного current.
				const QPointer<FallbackController> guard = &owner;
				if (previousCurrent != result.value->currentGenerationId) {
					// Инвалидация отзывает все прежние GET; новое адресное чтение подтвердит текущую эпоху.
					state.invalidateConfiguration();
					if (!guard || !available())
						return;
					model.assetFresh = true;
					newScope(detailReceiver);
					restream.refreshFallbackAfterConfiguration();
					if (!guard)
						return;
				}
				if (!publish() || !guard || model.selectedAssetId != id)
					return;
				readGeneration(model.current, false);
				if (guard)
					readGeneration(model.pending, true);
				if (guard && previousCurrent != result.value->currentGenerationId)
					queueRefresh();
			});
	}
	// Читает одну точную policy базу; dirty ввод и его revision не перезаписываются.
	// conflict отмечает чтение после подтверждённого revision_conflict; Refresh сохраняет эту причину сравнения.
	void readPolicy(bool conflict = false)
	{
		if (!available() || !validId(model.policy.streamId))
			return;
		// receiver/token/epoch/expected/id защищают target от навигации и отмены.
		// alive проверяет жизнь фасада после адресной отмены прежнего scope policy.
		const QPointer<FallbackController> alive = &owner;
		const QPointer<QObject> receiver = newScope(policyReceiver);
		if (!alive || !receiver)
			return;
		const auto token = readToken;
		const auto epoch = state.configurationGeneration();
		const auto expected = context;
		const auto id = model.policy.streamId;
		// compareConflict сохраняет подтверждённый revision_conflict, если Refresh заменил ещё не завершённое CAS-чтение.
		// Для остальных HTTP 409 сравнение ревизий не включается.
		const bool compareConflict = conflict || model.policy.outcome == FallbackWriteOutcome::Conflict;
		model.policy.loading = true;
		policyFresh = false;
		if (!publish() || !receiver || !current(token, epoch, expected, requestContext))
			return;
		// Callback сохраняет missing как ошибку контракта, а null как подтверждённое отсутствие политики.
		api.getFallbackPolicy(
			id, receiver,
			[this, receiver, token, epoch, expected, id,
			 compareConflict](auto received, api::Result<api::PolicyResult> result) {
				if (!receiver || !current(token, epoch, expected, received) ||
				    model.policy.streamId != id)
					return;
				model.policy.loading = false;
				if (!result || result.value->revision == 0 || result.value->fallback.isMissing()) {
					model.messageKey = QStringLiteral("Configuration.ReadFailed");
					publish();
					return;
				}
				if (compareConflict) {
					model.policy.serverConflict = *result.value;
					model.policy.outcome = FallbackWriteOutcome::Conflict;
				} else if (!model.policy.dirty) {
					model.policy.base = *result.value;
					model.policy.desiredAssetId =
						result.value->fallback ? result.value->fallback->assetId : QString{};
				} else if (!model.policy.base)
					model.policy.base = *result.value;
				policyFresh = true;
				++model.policy.generation;
				publish();
			});
	}
	// Обновляет только адресный Stream кеша, не затрагивая draft редактора имени и его base revision.
	void readPolicyStream(const QString &id)
	{
		if (!available() || !policyReceiver)
			return;
		// receiver/token/epoch/expected фиксируют область чтения успешного PUT.
		const QPointer<QObject> receiver = policyReceiver;
		const auto token = readToken;
		const auto epoch = state.configurationGeneration();
		const auto expected = context;
		// Callback сливает один Stream с последним aggregate после проверки исходной эпохи.
		api.getStream(id, receiver,
			      [this, receiver, token, epoch, expected, id](auto received,
									   api::Result<api::Stream> result) {
				      if (!receiver || !current(token, epoch, expected, received) || !result ||
					  result.value->id != id)
					      return;
				      // next сохраняет изменения других конфигурационных коллекций.
				      auto next = state.configuration();
				      next.streams.insert(id, *result.value);
				      state.setConfiguration(std::move(next));
			      });
	}
	// Начинает новый ограниченный цикл GET для сохранённой собственной пары без повторного upload/rebuild.
	void startObservation()
	{
		if (!available() || !validId(model.acceptedAssetId) || !validId(model.acceptedGenerationId))
			return;
		// guard проверяет жизнь фасада; token сохраняет поколение чтения, expected — контекст до отмены.
		const QPointer<FallbackController> guard = &owner;
		const auto token = readToken;
		const auto expected = context;
		poll.stop();
		deadline.stop();
		cancelScope(observationReceiver);
		if (!guard || !available() || readToken != token || context != expected || deadline.isActive())
			return;
		model.observation = FallbackObservation::Observing;
		deadline.start(std::max(1, limits.observationTimeoutMs));
		observeGeneration();
	}
	// Наблюдает принятую Generation; terminal, Variants и итоговый GET Asset делят один общий срок.
	void observeGeneration()
	{
		if (!available() || !deadline.isActive() || !validId(model.acceptedGenerationId))
			return;
		// receiver ограничивает цепочку GET; token фиксирует поколение чтения, epoch — эпоху конфигурации.
		// expected сохраняет контекст, assetId и generationId — собственную принятую пару.
		// alive проверяет жизнь фасада после адресной отмены прежнего scope наблюдения.
		const QPointer<FallbackController> alive = &owner;
		const QPointer<QObject> receiver = newScope(observationReceiver);
		if (!alive || !receiver)
			return;
		const auto token = readToken;
		const auto epoch = state.configurationGeneration();
		const auto expected = context;
		const auto assetId = model.acceptedAssetId;
		const auto generationId = model.acceptedGenerationId;
		// Callback извлекает failure до общей модели и уведомляет только собственную terminal ошибку.
		api.getGeneration(
			assetId, generationId, receiver,
			[this, receiver, token, epoch, expected, assetId,
			 generationId](auto received, api::Result<api::Generation> result) {
				if (!receiver || !deadline.isActive() || !current(token, epoch, expected, received) ||
				    model.acceptedAssetId != assetId || model.acceptedGenerationId != generationId)
					return;
				if (!result || result.value->id != generationId || result.value->assetId != assetId) {
					deadline.stop();
					model.observation = FallbackObservation::Unknown;
					model.messageKey = QStringLiteral("Fallback.Incomplete");
					publish();
					return;
				}
				// failure копируется до удаления диагностического тела из полученного DTO.
				OperationFailure failure;
				if (result.value->failure) {
					failure.code = result.value->failure->code;
					failure.reason = result.value->failure->message;
					failure.action = result.value->failure->action;
					failure.field = result.value->failure->field;
					failure.ids = result.value->failure->context;
					failure.diagnosticBody = result.value->failure->diagnosticBody;
				}
				result.value->failure.value.diagnosticBody.clear();
				model.accepted.assetId = assetId;
				model.accepted.generationId = generationId;
				model.accepted.generation = *result.value;
				if (!terminalGeneration(result.value->status)) {
					if (result.value->status != QStringLiteral("queued") &&
					    result.value->status != QStringLiteral("preparing")) {
						deadline.stop();
						model.observation = FallbackObservation::Unknown;
						publish();
						return;
					}
					model.observation = FallbackObservation::Observing;
					if (publish() && receiver && deadline.isActive() &&
					    current(token, epoch, expected, received))
						poll.start(std::max(1, limits.pollIntervalMs));
					return;
				}
				// Терминальный статус Generation ещё не завершает проверку вариантов и нынешнего Asset.
				model.observation = result.value->status == QStringLiteral("error")
							    ? FallbackObservation::Error
							    : FallbackObservation::Observing;
				if (!publish() || !receiver || !deadline.isActive() ||
				    !current(token, epoch, expected, received))
					return;
				if (result.value->status == QStringLiteral("error") && !model.acceptedFailureNotified) {
					model.acceptedFailureNotified = true;
					if (!notify(std::move(failure), acceptedToken,
						    QStringLiteral("generation/") + generationId))
						return;
				}
				if (!receiver || !deadline.isActive() || !current(token, epoch, expected, received))
					return;
				readAcceptedVariants(assetId, generationId);
			});
	}
	// Читает Variants точной принятой пары и итоговый Asset, не продлевая общий срок наблюдения.
	void readAcceptedVariants(const QString &assetId, const QString &generationId)
	{
		if (!available() || !deadline.isActive() || !observationReceiver || model.acceptedAssetId != assetId ||
		    model.acceptedGenerationId != generationId)
			return;
		// receiver сохраняет scope наблюдения, token — поколение чтения, epoch — эпоху конфигурации.
		// expected фиксирует логический контекст до обхода всех страниц принятой пары.
		const QPointer<QObject> receiver = observationReceiver;
		const auto token = readToken;
		const auto epoch = state.configurationGeneration();
		const auto expected = context;
		// query фиксирует generation_id для каждой страницы getAllVariants.
		api::ListQuery query;
		query.generationId = generationId;
		// Callback не допускает Ready при неполноте, дублях или чужой принадлежности вариантов.
		api.getAllVariants(
			assetId, receiver,
			[this, receiver, token, epoch, expected, assetId,
			 generationId](auto received, api::Result<api::Collection<api::Variant>> result) {
				if (!receiver || !deadline.isActive() || !current(token, epoch, expected, received) ||
				    model.acceptedAssetId != assetId || model.acceptedGenerationId != generationId)
					return;
				// valid/ids проверяют всю полученную коллекцию до публикации.
				bool valid = result && result.value->complete;
				QSet<QString> ids;
				if (result) {
					// variant проверяется на неизменную родительскую пару.
					for (const auto &variant : result.value->items) {
						if (!validId(variant.id) || ids.contains(variant.id) ||
						    variant.assetId != assetId || variant.generationId != generationId)
							valid = false;
						ids.insert(variant.id);
					}
					model.accepted.variants = result.value->items;
				}
				model.accepted.variantsStatus = {valid ? RuntimeAvailability::Fresh
								       : RuntimeAvailability::Stale,
								 valid,
								 {}};
				if (!valid && model.observation != FallbackObservation::Error)
					model.observation = FallbackObservation::Unknown;
				if (!publish() || !receiver || !deadline.isActive() ||
				    !current(token, epoch, expected, received))
					return;
				// Asset читается даже при ошибке вариантов; только сервер определяет нынешнюю публикацию.
				api.getAsset(assetId, receiver,
					     [this, receiver, token, epoch, expected, assetId, generationId,
					      valid](auto assetContext, api::Result<api::Asset> asset) {
						     if (!receiver || !deadline.isActive() ||
							 !current(token, epoch, expected, assetContext) ||
							 model.acceptedAssetId != assetId ||
							 model.acceptedGenerationId != generationId)
							     return;
						     // Итоговый GET завершает цикл и при отказе; его результат больше не ждёт таймера.
						     deadline.stop();
						     if (!asset || asset.value->id != assetId) {
							     model.observation = FallbackObservation::Unknown;
							     publish();
							     return;
						     }
						     model.assets.insert(assetId, *asset.value);
						     if (model.accepted.generation &&
							 model.accepted.generation->status == QStringLiteral("ready"))
							     model.observation = valid ? FallbackObservation::Ready
										       : FallbackObservation::Unknown;
						     // guard защищает последующее чтение выбранной карточки после публикации accepted Asset.
						     const QPointer<FallbackController> guard = &owner;
						     if (!publish() || !receiver ||
							 !current(token, epoch, expected, assetContext))
							     return;
						     if (model.selectedAssetId == assetId)
							     readAsset();
						     if (guard && available() && context == expected)
							     restream.refreshFallbackAfterConfiguration();
					     });
			},
			query);
	}

	// Завершает только одно доказанно свежее обязательство, сохраняя прежний Unknown в форме.
	void reconciled(const Unknown &item, quint64 token, quint64 epoch, const ConfigurationContext &expected)
	{
		if (!available() || context != expected || token != reconciliationToken ||
		    epoch != state.configurationGeneration())
			return;
		unknown.erase(std::remove_if(unknown.begin(), unknown.end(),
					     [&item](const auto &other) {
						     return other.origin == item.origin && other.kind == item.kind &&
							    other.assetId == item.assetId &&
							    other.streamId == item.streamId;
					     }),
			      unknown.end());
		++reconciliationToken;
		// remaining сохраняет запрет при другом обязательстве того же origin.
		const bool remaining = hasUnknown();
		// guard защищает продолжение после общей активности и инвалидации старых GET.
		const QPointer<FallbackController> guard = &owner;
		state.setConfigurationUnknown(item.origin, QStringLiteral("Fallback"), remaining);
		if (!guard)
			return;
		state.invalidateConfiguration();
		if (!guard || !publish())
			return;
		restream.refreshFallbackAfterConfiguration();
		if (!guard)
			return;
		queueRefresh();
	}
	// Сверяет Unknown новыми адресными GET без воспроизведения mutation или угадывания принятого ID.
	void reconcileUnknown()
	{
		if (!available() || write || !hasUnknown())
			return;
		// found выбирает только нынешний origin, не очищая обязательства других серверов.
		const auto found = std::find_if(unknown.cbegin(), unknown.cend(),
						[this](const auto &item) { return item.origin == context.origin; });
		if (found == unknown.cend())
			return;
		// item/receiver/token/epoch/expected фиксируют конкретную сверку после Unknown.
		const auto item = *found;
		// alive проверяет жизнь фасада после адресной отмены прежнего scope сверки.
		const QPointer<FallbackController> alive = &owner;
		const QPointer<QObject> receiver = newScope(reconciliationReceiver);
		if (!alive || !receiver)
			return;
		const auto token = reconciliationToken;
		const auto epoch = state.configurationGeneration();
		const auto expected = context;
		// fresh не принимает поздние результаты прежнего origin или чтения до нового Unknown.
		const auto fresh = [this, receiver, token, epoch, expected](api::RequestContext received) {
			return receiver && available() && token == reconciliationToken &&
			       epoch == state.configurationGeneration() && context == expected &&
			       received == requestContext;
		};
		if (item.kind == WriteKind::Create) {
			// Callback требует полный свежий каталог с уникальными ID; имя не доказывает собственность create.
			api.getAllAssets(receiver,
					 [this, item, token, epoch, expected,
					  fresh](auto received, api::Result<api::Collection<api::Asset>> result) {
						 if (!fresh(received) || !result || !result.value->complete)
							 return;
						 // ids проверяет полноту идентичностей всех страниц перед снятием запрета.
						 QSet<QString> ids;
						 // asset должен иметь отдельную непрозрачную идентичность.
						 for (const auto &asset : result.value->items) {
							 if (!validId(asset.id) || ids.contains(asset.id))
								 return;
							 ids.insert(asset.id);
						 }
						 reconciled(item, token, epoch, expected);
					 });
			return;
		}
		if (item.kind == WriteKind::Policy) {
			// Callback требует адресную policy с явным fallback и ревизией Stream.
			api.getFallbackPolicy(item.streamId, receiver,
					      [this, receiver, item, token, epoch, expected,
					       fresh](auto received, api::Result<api::PolicyResult> policy) {
						      if (!fresh(received) || !policy || policy.value->revision == 0 ||
							  policy.value->fallback.isMissing())
							      return;
						      // revision связывает два свежих чтения policy/Stream, не подменяя dirty base редактора.
						      const auto revision = policy.value->revision;
						      // Callback подтверждает тот же Stream и его ревизию; гонка требует нового Refresh.
						      api.getStream(item.streamId, receiver,
								    [this, item, token, epoch, expected, fresh,
								     revision](auto streamContext,
									       api::Result<api::Stream> stream) {
									    if (!fresh(streamContext) || !stream ||
										stream.value->id != item.streamId ||
										stream.value->revision != revision)
										    return;
									    reconciled(item, token, epoch, expected);
								    });
					      });
			return;
		}
		// Callback сверяет delete адресно; upload/rebuild дополнительно требуют точное известное поколение.
		api.getAsset(
			item.assetId, receiver,
			[this, receiver, item, token, epoch, expected, fresh](auto received,
									      api::Result<api::Asset> asset) {
				if (!fresh(received))
					return;
				if (!asset) {
					if (asset.error && asset.error->httpStatus == 404)
						reconciled(item, token, epoch, expected);
					return;
				}
				if (asset.value->id != item.assetId)
					return;
				if (item.kind == WriteKind::Delete) {
					reconciled(item, token, epoch, expected);
					return;
				}
				// generationId не объявляется собственным: это наблюдаемое серверное задание неизвестной принадлежности.
				const auto generationId = !item.generationId.isEmpty()
								  ? item.generationId
								  : (!asset.value->pendingGenerationId.isEmpty()
									     ? asset.value->pendingGenerationId
									     : asset.value->currentGenerationId);
				if (generationId.isEmpty()) {
					if (asset.value->pendingGenerationId.isEmpty())
						reconciled(item, token, epoch, expected);
					return;
				}
				// Callback требует проверенную родительскую пару и terminal; queued/preparing сохраняют барьер.
				api.getGeneration(
					item.assetId, generationId, receiver,
					[this, receiver, item, token, epoch, expected, fresh, generationId](
						auto generationContext, api::Result<api::Generation> generation) {
						if (!fresh(generationContext) || !generation ||
						    generation.value->id != generationId ||
						    generation.value->assetId != item.assetId)
							return;
						if (!terminalGeneration(generation.value->status)) {
							if ((generation.value->status == QStringLiteral("queued") ||
							     generation.value->status == QStringLiteral("preparing")) &&
							    reconciliationBudget.isValid() &&
							    reconciliationBudget.elapsed() <
								    std::max(1, limits.observationTimeoutMs))
								reconcilePoll.start(std::max(1, limits.pollIntervalMs));
							return;
						}
						// Callback заново проверяет публикацию после terminal; первое чтение могло предшествовать завершению.
						api.getAsset(
							item.assetId, receiver,
							[this, item, token, epoch, expected,
							 fresh](auto finalContext, api::Result<api::Asset> finalAsset) {
								if (!fresh(finalContext) || !finalAsset ||
								    finalAsset.value->id != item.assetId ||
								    !finalAsset.value->pendingGenerationId.isEmpty())
									return;
								reconciled(item, token, epoch, expected);
							});
					});
			});
	}
	// Обновляет все активные адресные формы и даёт новым GET собственную границу свежести.
	void refresh()
	{
		if (!available() || write)
			return;
		++readToken;
		if (!cancelReadScopes())
			return;
		// guard защищает последовательность от удаления контроллера из публикаций чтения.
		const QPointer<FallbackController> guard = &owner;
		readCatalog();
		if (!guard || !available())
			return;
		readAsset();
		if (!guard || !available())
			return;
		readPolicy();
		if (!guard || !available())
			return;
		if (hasUnknown()) {
			reconciliationBudget.restart();
			reconcileUnknown();
		}
		if (!model.acceptedGenerationId.isEmpty())
			startObservation();
	}
	// Выбирает Asset для просмотра; изменение карточки не уничтожает принятую пару.
	bool selectAsset(QString id)
	{
		if (!available() || write || (!id.isEmpty() && !validId(id)))
			return false;
		cancelScope(detailReceiver);
		model.selectedAssetId = std::move(id);
		model.asset.reset();
		model.assetFresh = false;
		model.current = {};
		model.pending = {};
		if (!publish())
			return false;
		readAsset();
		return true;
	}
	// Сохраняет ввод без файлового I/O; kind имеет только две публичные альтернативы.
	bool setDraft(QString name, QString filePath, QString kind)
	{
		if (stopped || write || (kind != QStringLiteral("image") && kind != QStringLiteral("video")))
			return false;
		model.draftName = std::move(name);
		model.draftFilePath = std::move(filePath);
		model.draftKind = std::move(kind);
		return publish();
	}
	// Создаёт пустую сущность одним POST; неоднозначный ответ не угадывается по имени.
	bool createAsset()
	{
		if (model.draftName.trimmed().isEmpty())
			return false;
		// request фиксирует ввод до публикации занятого состояния.
		const api::AssetCreate request{model.draftName};
		if (!beginWrite(WriteKind::Create))
			return false;
		// token/receiver принадлежат ровно этому create.
		const auto token = write->token;
		const QPointer<QObject> receiver = write->receiver;
		// Callback принимает только проверенный create201 нынешнего намерения.
		api.createAsset(request, receiver, [this, token](auto received, api::Result<api::Asset> result) {
			if (!writeCurrent(token) || received != requestContext)
				return;
			if (!result) {
				failed(*result.error);
				return;
			}
			if (!validId(result.value->id) || result.value->revision == 0) {
				// error обозначает недостоверный успешный документ после возможной записи.
				api::Error error;
				error.kind = api::ErrorKind::Contract;
				error.requestSent = true;
				failed(error);
				return;
			}
			model.selectedAssetId = result.value->id;
			model.asset = *result.value;
			model.assets.insert(result.value->id, *result.value);
			model.outcome = FallbackWriteOutcome::Saved;
			if (writeFinished())
				queueRefresh();
		});
		return true;
	}
	// Удаляет только выбранный свежий Asset, не меняя назначение Stream автоматически.
	bool deleteAsset()
	{
		if (!canWrite() || !model.assetFresh || !model.asset || model.asset->id != model.selectedAssetId ||
		    model.asset->revision == 0)
			return false;
		// id/revision фиксируют подтверждённую карточку до вложенных событий UI.
		const auto id = model.asset->id;
		const auto revision = model.asset->revision;
		if (!beginWrite(WriteKind::Delete, id))
			return false;
		// token исключает результат другого удаления того же ID.
		const auto token = write->token;
		// Callback считает только 204 подтверждением DELETE.
		api.deleteAsset(id, revision, write->receiver,
				[this, token, id](auto received, api::Result<api::Empty> result) {
					if (!writeCurrent(token) || received != requestContext)
						return;
					if (!result) {
						failed(*result.error);
						return;
					}
					model.assets.remove(id);
					model.asset.reset();
					model.selectedAssetId.clear();
					model.current = {};
					model.pending = {};
					model.outcome = FallbackWriteOutcome::Deleted;
					if (writeFinished())
						queueRefresh();
				});
		return true;
	}
	// Принимает проверенный Preparation202 и освобождает mutation до долгого наблюдения.
	void accepted(api::Preparation preparation)
	{
		if (!write)
			return;
		if (preparation.assetId != write->assetId || !validId(preparation.generationId)) {
			// error сохраняет неопределённость успешного ответа с неверной парой.
			api::Error error;
			error.kind = api::ErrorKind::Contract;
			error.requestSent = true;
			failed(error);
			return;
		}
		model.acceptedAssetId = preparation.assetId;
		model.acceptedGenerationId = preparation.generationId;
		model.accepted = {};
		model.accepted.assetId = preparation.assetId;
		model.accepted.generationId = preparation.generationId;
		acceptedToken = write->token;
		model.acceptedFailureNotified = false;
		model.outcome = FallbackWriteOutcome::Accepted;
		if (!writeFinished())
			return;
		// guard защищает последующее наблюдение от удаления во время каталога.
		const QPointer<FallbackController> guard = &owner;
		readAsset();
		if (guard)
			startObservation();
	}
	// Отправляет upload либо rebuild один раз; байты прогресса не означают принятия 202.
	bool prepare(bool rebuild)
	{
		if (!canPrepare() || (!rebuild && model.draftFilePath.isEmpty()))
			return false;
		// id/revision/kind/path фиксируют исходную сущность и пользовательский файл до публикаций.
		const auto id = model.asset->id;
		const auto revision = model.asset->revision;
		const auto kind = model.draftKind;
		const auto path = model.draftFilePath;
		if (!beginWrite(rebuild ? WriteKind::Rebuild : WriteKind::Upload, id))
			return false;
		// token/receiver идентифицируют лишь эту передачу.
		const auto token = write->token;
		const QPointer<QObject> receiver = write->receiver;
		// обработчик требует проверенный Preparation, отдельный от runtime Receipt.
		auto callback = [this, token](auto received, api::Result<api::Preparation> result) {
			if (!writeCurrent(token) || received != requestContext)
				return;
			if (!result) {
				failed(*result.error);
				return;
			}
			accepted(std::move(*result.value));
		};
		if (rebuild)
			api.rebuildAsset(id, revision, receiver, callback);
		else {
			// progress обновляет только живое исходное намерение и его принятые транспортом байты.
			api.uploadAsset(id, revision, kind, path, receiver, callback,
					[this, token](auto received, qint64 sent, qint64 total) {
						if (!writeCurrent(token) || received != requestContext)
							return;
						model.bytesSent = sent;
						model.totalBytes = total;
						publish();
					});
		}
		return true;
	}
	// Меняет активный policy target без выбора Dock; предыдущий dirty ввод паркуется в RAM.
	bool setPolicyTarget(QString id)
	{
		if (!available() || (write && write->kind == WriteKind::Policy) || (!id.isEmpty() && !validId(id)))
			return false;
		if (model.policy.streamId == id)
			return true;
		if (!model.policy.streamId.isEmpty())
			policies.insert_or_assign(policyKey(model.policy.streamId), model.policy);
		cancelScope(policyReceiver);
		policyFresh = false;
		// found ищет только явно адресованную форму нынешнего контекста.
		const auto found = policies.find(policyKey(id));
		model.policy = found == policies.end() ? FallbackPolicyState{} : found->second;
		model.policy.streamId = std::move(id);
		model.policy.loading = false;
		++model.policy.generation;
		if (!publish())
			return false;
		readPolicy();
		return true;
	}
	// Обновляет лишь желаемый Asset, сохраняя подтверждённую base для CAS.
	bool setPolicyAsset(QString id)
	{
		if (!available() || write || !validId(model.policy.streamId) || (!id.isEmpty() && !validId(id)))
			return false;
		model.policy.desiredAssetId = std::move(id);
		model.policy.dirty = true;
		++model.policy.generation;
		return publish();
	}
	// Отправляет полный policy/null с revision Stream и точной копией существующих watchdog интервалов.
	bool savePolicy()
	{
		if (!canSavePolicy())
			return false;
		// target/request фиксируют адрес и полный документ до синхронных сигналов общего барьера.
		const auto target = model.policy.streamId;
		api::PolicyPut request;
		request.expectedRevision = model.policy.base->revision;
		if (!model.policy.desiredAssetId.isEmpty()) {
			// policy сохраняет все четыре серверных интервала; первое назначение использует 3/3/1/5 секунд.
			api::Policy policy;
			policy.assetId = model.policy.desiredAssetId;
			policy.watchdog = model.policy.base->fallback ? model.policy.base->fallback->watchdog
								      : api::Watchdog{3000000000ULL, 3000000000ULL,
										      1000000000ULL, 5000000000ULL};
			request.fallback = policy;
		}
		if (!beginWrite(WriteKind::Policy, {}, target))
			return false;
		// token идентифицирует только эту полную замену политики.
		const auto token = write->token;
		// Callback не принимает missing/null вместо явного полного результата 200.
		api.setFallbackPolicy(
			target, request, write->receiver,
			[this, token, target, request](auto received, api::Result<api::PolicyResult> result) {
				if (!writeCurrent(token) || received != requestContext ||
				    model.policy.streamId != target)
					return;
				if (!result) {
					failed(*result.error);
					return;
				}
				// matches подтверждает именно запрошенный полный policy/null, а не произвольный успешный DTO.
				const bool matches =
					request.fallback
						? result.value->fallback &&
							  result.value->fallback->assetId ==
								  request.fallback->assetId &&
							  result.value->fallback->watchdog.videoTimeoutNs ==
								  request.fallback->watchdog.videoTimeoutNs &&
							  result.value->fallback->watchdog.audioTimeoutNs ==
								  request.fallback->watchdog.audioTimeoutNs &&
							  result.value->fallback->watchdog.recoveryWindowNs ==
								  request.fallback->watchdog.recoveryWindowNs &&
							  result.value->fallback->watchdog.keyframeTimeoutNs ==
								  request.fallback->watchdog.keyframeTimeoutNs
						: result.value->fallback.isNull();
				if (result.value->revision == 0 || !matches) {
					// error представляет недостоверный успешный ответ после возможного PUT.
					api::Error error;
					error.kind = api::ErrorKind::Contract;
					error.requestSent = true;
					failed(error);
					return;
				}
				model.policy.base = *result.value;
				model.policy.serverConflict.reset();
				model.policy.dirty = false;
				model.policy.desiredAssetId = result.value->fallback ? result.value->fallback->assetId
										     : QString{};
				model.policy.outcome = FallbackWriteOutcome::Saved;
				model.outcome = FallbackWriteOutcome::Saved;
				++model.policy.generation;
				if (!writeFinished())
					return;
				// guard защищает адресное чтение Stream после публикаций policy.
				const QPointer<FallbackController> guard = &owner;
				readPolicy();
				if (guard)
					readPolicyStream(target);
			});
		return true;
	}
	// Принимает свежую серверную версию только явным действием; PUT автоматически не отправляет.
	bool confirmPolicyConflict()
	{
		if (!available() || write || !model.policy.serverConflict)
			return false;
		model.policy.base = model.policy.serverConflict;
		model.policy.serverConflict.reset();
		model.policy.outcome = FallbackWriteOutcome::None;
		policyFresh = true;
		++model.policy.generation;
		return publish();
	}
	// Отменяет собственную mutation либо местное наблюдение принятого поколения.
	// Потенциально отправленная запись остаётся Unknown; свежие GET сверяют сервер без повторной записи.
	bool cancel()
	{
		if (stopped)
			return false;
		// expected сохраняет адрес отменённой записи; reconcileCancelledWrite запрашивает только свежие GET.
		const auto expected = context;
		bool reconcileCancelledWrite = false;
		// guard защищает дальнейшую отмену от удаления из публикации общего барьера.
		const QPointer<FallbackController> guard = &owner;
		if (write) {
			// intent фиксирует адрес отменяемого намерения, sent — факт его передачи транспорту.
			const auto intent = *write;
			const bool sent = api.cancelRequests(intent.receiver);
			if (!guard)
				return false;
			model.outcome = sent ? FallbackWriteOutcome::Unknown : FallbackWriteOutcome::NotSent;
			if (intent.kind == WriteKind::Policy)
				model.policy.outcome = model.outcome;
			if (sent && !rememberUnknown(intent))
				return false;
			reconcileCancelledWrite = sent;
			if (!releaseWrite())
				return false;
			state.invalidateConfiguration();
			if (!guard)
				return false;
		} else {
			if (model.observation != FallbackObservation::Observing)
				return false;
			cancelScope(observationReceiver);
			if (!guard)
				return false;
			poll.stop();
			deadline.stop();
			model.observation = FallbackObservation::Cancelled;
		}
		if (!publish())
			return false;
		if (reconcileCancelledWrite && context == expected && available())
			queueRefresh();
		return true;
	}
	// Прекращает только собственные ресурсы раньше закрытия заимствованного API.
	void shutdown()
	{
		if (stopped)
			return;
		// guard замечает удаление владельца из публикации закрытия.
		const QPointer<FallbackController> guard = &owner;
		closeChannel();
		if (!guard)
			return;
		stopped = true;
		ready = false;
		++readToken;
		poll.stop();
		deadline.stop();
		reconcilePoll.stop();
		if (!cancelReadScopes())
			return;
	}
};

// Создаёт неактивный до готовности канала контроллер с обычными пределами наблюдения.
FallbackController::FallbackController(StateStore &state, api::APIClient &api, ConfigurationController &configuration,
				       RestreamController &restream, QObject *parent)
	: FallbackController(state, api, configuration, restream, Limits{}, parent)
{
}
// Создаёт реализацию с явно заданными внутренними сроками изолированных проверок.
FallbackController::FallbackController(StateStore &state, api::APIClient &api, ConfigurationController &configuration,
				       RestreamController &restream, Limits limits, QObject *parent)
	: QObject(parent),
	  impl_(std::make_unique<Impl>(*this, state, api, configuration, restream, limits))
{
}
// Завершает свои запросы до освобождения реализации и заимствованных сервисов.
FallbackController::~FallbackController()
{
	shutdown();
}
// Возвращает допуск новой mutation через общий барьер.
bool FallbackController::canWrite() const
{
	return impl_->canWrite();
}
// Возвращает допуск подготовки свежего Asset без pending и наблюдаемой активной Session.
bool FallbackController::canPrepare() const
{
	return impl_->canPrepare();
}
// Возвращает допуск полного policy PUT относительно свежей адресной базы.
bool FallbackController::canSavePolicy() const
{
	return impl_->canSavePolicy();
}
// Обновляет только чтения; потерянные mutation автоматически не повторяются.
void FallbackController::refresh()
{
	impl_->refresh();
}
// Адресует карточку Asset без изменения принятой пары.
bool FallbackController::selectAsset(QString id)
{
	return impl_->selectAsset(std::move(id));
}
// Сохраняет RAM-ввод без открытия выбранного файла.
bool FallbackController::setDraft(QString name, QString path, QString kind)
{
	return impl_->setDraft(std::move(name), std::move(path), std::move(kind));
}
// Создаёт пустой Asset одним намерением.
bool FallbackController::createAsset()
{
	return impl_->createAsset();
}
// Удаляет выбранный Asset по свежей revision.
bool FallbackController::deleteAsset()
{
	return impl_->deleteAsset();
}
// Загружает один файл и наблюдает точное принятое поколение.
bool FallbackController::upload()
{
	return impl_->prepare(false);
}
// Запускает подготовку сохранённого оригинала без повторной передачи файла.
bool FallbackController::rebuild()
{
	return impl_->prepare(true);
}
// Прекращает лишь местное ожидание или собственную HTTP передачу.
bool FallbackController::cancel()
{
	return impl_->cancel();
}
// Задаёт отдельную цель политики без выбора Dock.
bool FallbackController::setPolicyTarget(QString id)
{
	return impl_->setPolicyTarget(std::move(id));
}
// Сохраняет намерение полного назначения Asset либо явного null.
bool FallbackController::setPolicyAsset(QString id)
{
	return impl_->setPolicyAsset(std::move(id));
}
// Читает только активную адресную policy форму.
void FallbackController::refreshPolicy()
{
	impl_->readPolicy();
}
// Отправляет один полный PUT политики.
bool FallbackController::savePolicy()
{
	return impl_->savePolicy();
}
// Принимает серверный CAS документ новой базой без HTTP записи.
bool FallbackController::confirmPolicyConflict()
{
	return impl_->confirmPolicyConflict();
}
// Использует прежнюю общую проверку и matcher без запуска Session/OBS.
bool FallbackController::checkReadiness()
{
	return impl_->available() && impl_->restream.checkFallback(impl_->state.dockContext());
}
// Закрывает только принадлежащие этому контроллеру scopes.
void FallbackController::shutdown()
{
	if (impl_)
		impl_->shutdown();
}
} // namespace gost
