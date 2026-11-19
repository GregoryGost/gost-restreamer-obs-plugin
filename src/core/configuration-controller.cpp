#include "configuration-controller.hpp"
#include "api/api-client.hpp"
#include "api/configuration-requests.hpp"
#include "api/json-encoder.hpp"
#include "core/connection-controller.hpp"
#include "core/restream-controller.hpp"
#include "core/state-store.hpp"

#include <QPointer>
#include <QUrl>
#include <QUuid>
#include <QTimer>
#include <algorithm>
#include <array>
#include <map>
#include <tuple>
#include <type_traits>

namespace gost {
namespace {
// Возвращает общие метаданные одной альтернативы DTO, не преобразуя revision в double.
const api::Record &record(const ConfigurationEntity &entity)
{
	return std::visit([](const auto &value) -> const api::Record & { return value; }, entity);
}

// Проверяет соответствие альтернативы виду карточки до обращения к std::get.
bool matches(ConfigurationKind kind, const ConfigurationEntity &entity)
{
	return static_cast<size_t>(kind) == entity.index();
}

// Возвращает разрешённые намерения PATCH; секрет публикации существующего Stream сюда не входит.
QSet<QString> editableFields(ConfigurationKind kind)
{
	switch (kind) {
	case ConfigurationKind::Stream:
		return {QStringLiteral("name")};
	case ConfigurationKind::Destination:
		return {QStringLiteral("name"),      QStringLiteral("stream_id"), QStringLiteral("group_id"),
			QStringLiteral("platform"),  QStringLiteral("enabled"),   QStringLiteral("endpoint"),
			QStringLiteral("stream_key")};
	case ConfigurationKind::Group:
		return {QStringLiteral("name"), QStringLiteral("stream_id"), QStringLiteral("profile_id")};
	case ConfigurationKind::Profile:
		return {QStringLiteral("name"), QStringLiteral("profile")};
	}
	return {};
}

// Сохраняет только доверенные локальные ключи известных полей. Серверные причины получает popup,
// а форма сохраняет собственную валидацию и полный вложенный путь профиля.
void collectFieldIssues(ConfigurationEditorState &model, const api::Problem &problem)
{
	// allowed ограничивает корни видом карточки; неизменяемый ключ Stream доступен лишь при создании.
	auto allowed = editableFields(model.kind);
	if (model.creating && model.kind == ConfigurationKind::Stream)
		allowed.insert(QStringLiteral("publication_key"));
	// append проверяет путь issue и удаляет body-префикс перед сохранением локального ключа.
	const auto append = [&model, &allowed](const api::FieldError &issue) {
		// location сохраняет полный путь после единственного известного префикса тела.
		QString location = issue.location;
		if (location.startsWith(QStringLiteral("body.")))
			location.remove(0, 5);
		// root связывает вложенное поле с явным намерением полной замены профиля.
		const auto root = location.section(QLatin1Char('.'), 0, 0);
		if (!allowed.contains(root) || (!model.creating && !model.dirtyFields.contains(root)))
			return;
		// fieldIssue хранит доверенный локальный ключ полного пути без серверного пояснения.
		auto &fieldIssue = model.fieldIssues[location];
		fieldIssue.messageKey = issue.messageKey;
	};
	// issue содержит только структурированные location/message, без серверного value.
	for (const auto &issue : problem.errors)
		append(issue);
	if (problem.errors.isEmpty() && !problem.field.isEmpty()) {
		// fallback сохраняет прежний формат Problem.field/detail при отсутствии массива errors.
		const api::FieldError fallback{problem.field, problem.detail, {}};
		append(fallback);
	}
}

// Выбирает только свою коллекцию в свежей копии aggregate; остальные коллекции сохраняются.
template<class T> QMap<QString, T> &collection(ConfigurationState &state)
{
	if constexpr (std::is_same_v<T, api::Stream>)
		return state.streams;
	else if constexpr (std::is_same_v<T, api::Destination>)
		return state.destinations;
	else if constexpr (std::is_same_v<T, api::Group>)
		return state.groups;
	else
		return state.profiles;
}

// Выбирает статус полноты только нужной коллекции.
template<class T> ReadStatus &readStatus(ConfigurationState &state)
{
	if constexpr (std::is_same_v<T, api::Stream>)
		return state.streamsStatus;
	else if constexpr (std::is_same_v<T, api::Destination>)
		return state.destinationsStatus;
	else if constexpr (std::is_same_v<T, api::Group>)
		return state.groupsStatus;
	else
		return state.profilesStatus;
}
} // namespace

// Изолирует конфигурационные запросы от восстановления SSE и времени жизни виджетов.
class ConfigurationController::Impl {
public:
	// Подключает наблюдение контекста; сам конструктор не выполняет HTTP.
	Impl(ConfigurationController &owner, StateStore &state, api::APIClient &api, ConnectionController &connection,
	     RestreamController &restream)
		: owner(owner),
		  state(state),
		  api(api),
		  connection(connection),
		  restream(restream)
	{
		// Этот первый direct-обработчик снимает свою queued mutation до отмены чтений Fallback.
		// Иначе освобождённый ими слот pump мог бы отправить уже закрываемую запись старого контекста.
		QObject::connect(
			&owner, &ConfigurationController::channelClosing, &owner,
			[this] {
				// guard/sent защищают сохранение факта dispatch после возможного вложенного callback API.
				const QPointer<ConfigurationController> guard = &this->owner;
				const bool sent = mutationReceiver && this->api.cancelRequests(mutationReceiver);
				if (guard)
					closingMutationSent = sent;
			},
			Qt::DirectConnection);
		// Обработчик отзывает callbacks лишь при смене логического контекста.
		QObject::connect(&state, &StateStore::serverStateChanged, &owner, [this] { observe(); });
		// Явное отключение отменяет config API независимо от жизненного цикла SSE.
		QObject::connect(&connection, &ConnectionController::explicitlyDisconnected, &owner, [this] {
			disconnected = true;
			cancel(false);
		});
		observe();
	}
	// Публичный фасад ограничивает время жизни сетевых callbacks.
	ConfigurationController &owner;
	// Общее состояние принимает только проверенные результаты текущего контекста.
	StateStore &state;
	// Независимый клиент конфигурации с отдельной очередью.
	api::APIClient &api;
	// Контроллер сохраняет снятие рабочего выбора после удаления Stream.
	ConnectionController &connection;
	// Исполнитель отзывается и обновляет предварительную проверку без остановки workflow.
	RestreamController &restream;
	// Текущий адрес конфигурации, в том числе без выбранного Stream.
	ConfigurationContext context;
	// Транспортное поколение изменяется только при отмене логического контекста.
	api::RequestContext apiContext;
	// RAM-форма публикуется отдельно от конфигурационных коллекций.
	ConfigurationEditorState model;
	// Ключ RAM-карточки включает сервер, UUID OBS, рабочий выбор, вид и точный ID без неоднозначной склейки строк.
	using DraftKey = std::tuple<QString, QString, QString, ConfigurationKind, QString>;
	// Припаркованные карточки существуют только в памяти этого контроллера.
	std::map<DraftKey, ConfigurationEditorState> parked;
	// Очередь фиксирует назначения одного явного сохранения; текущий ID нужен при отмене отправленной записи.
	QStringList destinationQueue;
	QString destinationBusyId;
	// Признак отличает пакет enabled от обычного CRUD при отмене и завершении.
	bool destinationBatch = false;
	// editorGeneration меняется при каждой публикации формы, даже если ID нового черновика совпал.
	quint64 editorGeneration = 0;
	// Каждый вид списка имеет независимый номер последнего чтения.
	std::array<quint64, 4> reads{};
	// Флаги отмечают незавершённые чтения каталогов, которым скрытая сверка не должна отзывать эпоху.
	std::array<bool, 4> pendingReads{};
	// Поколение карточки отделяет навигацию и позднее GET.
	quint64 detailGeneration = 0;
	// Поколение записи запрещает завершение отменённого намерения.
	quint64 mutationGeneration = 0;
	// notifiedMutation и notifiedStep подавляют повтор события одного шага, не накапливая историю отказов.
	quint64 notifiedMutation = 0;
	QString notifiedStep;
	// Уведомление закрытия защищено от вложенной смены контекста.
	bool changingChannel = false;
	// Внешний observe удерживает переход до configure/channelReady и объединяет вложенные уведомления.
	bool contextTransition = false;
	// Отмечает необходимость заново прочитать последний logical context после вложенного сигнала.
	bool observeAgain = false;
	// Признак разрешает Fallback заимствовать уже сконфигурированный канал.
	bool channelReady = false;
	// Первый channelClosing-handler сохраняет факт dispatch до отмены чужих чтений общего канала.
	bool closingMutationSent = false;
	// Receiver относится только к нынешней mutation либо целому пакету enabled.
	QPointer<QObject> mutationReceiver;
	// Адрес и token владения переживают изменение model при синхронном уведомлении.
	QString writeOrigin;
	quint64 writeToken = 0;
	// Точное обязательство сверки сохраняется независимо от карточки и OBS Profile.
	struct UnknownWrite {
		// Сервер, на котором запись могла завершиться.
		QString origin;
		// Вид сущности выбирает адресный GET либо полный список неизвестного create.
		ConfigurationKind kind = ConfigurationKind::Stream;
		// Пустой ID означает create без достоверного ответа.
		QString id;
	};
	// Конечный набор обязательств не очищается при смене origin/карточки.
	QVector<UnknownWrite> unknownWrites;
	// Отдельное поколение сверки не допускает старых чтений после нового Unknown.
	quint64 reconciliationGeneration = 0;
	// Receiver объединяет скрытую сверку в одну цепочку чтения и позволяет уступить её видимой форме.
	QPointer<QObject> reconciliationReceiver;
	// Один отложенный запуск ждёт завершения чтений формы; ошибка GET сама повтор не планирует.
	bool reconciliationScheduled = false;
	// shutdown необратимо запрещает новые действия.
	bool stopped = false;
	// Явное отключение запрещает новые CRUD до нового подключения.
	bool disconnected = false;
	// Последний статус различает переход Connected от промежуточных сигналов.
	ConnectionStatus observedStatus = ConnectionStatus::Disconnected;

	// Публикует форму и отзывает подтверждения прежнего состояния; вызывающий код проверяет guard.
	void publish()
	{
		++editorGeneration;
		state.setConfigurationEditor(model);
	}

	// Разрешает новое намерение лишь для подтверждённого UUID и подключённого сервера.
	bool available() const
	{
		return !stopped && !disconnected && !changingChannel && context.valid() && state.profileResolved() &&
		       context == ConfigurationContext{state.serverOrigin(), state.dockContext().profileUuid,
						       state.selectedStreamId()} &&
		       state.connectionStatus() == ConnectionStatus::Connected;
	}

	// Возвращает однозначный адрес карточки; пустой ID относится к форме создания данного вида.
	DraftKey draftKey(const ConfigurationContext &address, ConfigurationKind kind, const QString &id) const
	{
		return {address.origin, address.profileUuid, address.workingStreamId, kind, id};
	}

	// Паркует свободную форму, включая ошибки и оставшееся намерение после частичного сохранения.
	bool parkDraft()
	{
		if (stopped || model.loading || model.writing)
			return false;
		if (model.context.valid())
			parked.insert_or_assign(draftKey(model.context, model.kind, model.id), model);
		return true;
	}

	// Восстанавливает только адрес нынешнего контекста, предварительно сохранив прежнюю свободную форму.
	bool restoreDraft(ConfigurationKind kind, const QString &id)
	{
		if (!available() ||
		    (model.context == context && model.requiresReconciliation() &&
		     (model.kind != kind || model.id != id)) ||
		    !parkDraft())
			return false;
		// found содержит RAM-карточку только для нынешнего адреса сервера и OBS Profile.
		const auto found = parked.find(draftKey(context, kind, id));
		if (found == parked.end())
			return false;
		++detailGeneration;
		model = found->second;
		publish();
		return true;
	}

	// Фиксирует только реальную разницу enabled; отношение Stream и секретные поля не изменяются.
	bool setDestinationEnabled(const QString &id, bool enabled)
	{
		if (!available() || model.context != context || model.kind != ConfigurationKind::Stream ||
		    model.creating || !model.base || model.loading || model.writing ||
		    model.destinationReconciliation.contains(id))
			return false;
		// config предоставляет подтверждённую полноту списка; found проверяет принадлежность точному Stream.
		const auto &config = state.configuration();
		const auto found = config.destinations.constFind(id);
		if (!config.destinationsStatus.complete ||
		    config.destinationsStatus.availability != RuntimeAvailability::Fresh ||
		    found == config.destinations.cend() || found->streamId != model.id)
			return false;
		if (enabled == found->enabled) {
			model.destinationChanges.remove(id);
			model.destinationRevisions.remove(id);
		} else {
			if (!model.destinationChanges.contains(id))
				model.destinationRevisions.insert(id, found->revision);
			model.destinationChanges.insert(id, enabled);
		}
		model.destinationOutcomes.remove(id);
		model.destinationNames.insert(id, found->name);
		publish();
		return true;
	}

	// Снимает только устаревшее намерение этого назначения и оставляет его результат явно отклонённым.
	void retireDestinationIntent(const QString &id)
	{
		model.destinationChanges.remove(id);
		model.destinationRevisions.remove(id);
		model.destinationReconciliation.remove(id);
		model.destinationOutcomes.insert(id, ConfigurationOutcome::Rejected);
	}

	// Возвращает true, если пакет содержит итог назначения, отличный от подтверждённого сохранения.
	bool hasDestinationFailure() const
	{
		// outcome обозначает отдельный итог, который пользователь должен увидеть как частичный результат.
		for (auto outcome = model.destinationOutcomes.cbegin(); outcome != model.destinationOutcomes.cend();
		     ++outcome)
			if (outcome.value() != ConfigurationOutcome::Saved)
				return true;
		return false;
	}

	// Регистрирует неопределённость до освобождения общего владения.
	void rememberUnknown(ConfigurationKind kind, const QString &id)
	{
		// found исключает повтор одного адреса; полнота нужна лишь для create без ID.
		const auto found =
			std::find_if(unknownWrites.cbegin(), unknownWrites.cend(), [this, kind, &id](const auto &item) {
				return item.origin == context.origin && item.kind == kind && item.id == id;
			});
		if (found == unknownWrites.cend())
			unknownWrites.append({context.origin, kind, id});
		++reconciliationGeneration;
		state.setConfigurationUnknown(context.origin, QStringLiteral("Configuration"), true);
	}

	// Атомарно занимает общую mutation; новый receiver изолирует адресную отмену записи.
	bool acquireWrite()
	{
		if (unknownWrites.size() >= 64 || !state.configurationSettled(context.origin))
			return false;
		writeOrigin = context.origin;
		writeToken = ++mutationGeneration;
		mutationReceiver = new QObject(&owner);
		model.writing = true;
		// guard обнаруживает удаление контроллера из уведомления общего барьера.
		const QPointer<ConfigurationController> guard = &owner;
		if (!state.beginConfigurationWrite(writeOrigin, QStringLiteral("Configuration"), writeToken)) {
			delete mutationReceiver.data();
			mutationReceiver = nullptr;
			model.writing = false;
			return false;
		}
		return guard && state.ownsConfigurationWrite(writeOrigin, QStringLiteral("Configuration"), writeToken);
	}

	// Освобождает намерение после регистрации Unknown; API не повторяет mutation.
	void releaseWrite()
	{
		// origin/token фиксируются до сигнала, способного начать следующее намерение.
		const auto origin = writeOrigin;
		const auto token = writeToken;
		// receiver и guard защищают освобождение от вложенного callback сетевой очереди.
		const QPointer<QObject> receiver = mutationReceiver;
		const QPointer<ConfigurationController> guard = &owner;
		mutationReceiver = nullptr;
		writeOrigin.clear();
		delete receiver.data();
		if (!guard)
			return;
		state.endConfigurationWrite(origin, QStringLiteral("Configuration"), token);
	}

	// Отменяет scopes до cancelAll; final необратимо запрещает новые действия, в том числе при вложенном вызове.
	// Прежний контекст остаётся доступен обработчикам channelClosing.
	void cancel(bool final)
	{
		// Вложенный shutdown отзывает канал немедленно, даже если внешнее закрытие ещё выполняется.
		stopped = stopped || final;
		channelReady = false;
		if (changingChannel)
			return;
		changingChannel = true;
		// guard защищает дальнейшую отмену от удаления из синхронного channelClosing.
		const QPointer<ConfigurationController> guard = &owner;
		closingMutationSent = false;
		emit owner.channelClosing();
		if (!guard)
			return;
		// sent отражает фактический dispatch, включая отмену ещё ожидавшей mutation.
		const bool sent = closingMutationSent;
		if (!guard)
			return;
		++apiContext.connection;
		++detailGeneration;
		++mutationGeneration;
		if (!stopReconciliation())
			return;
		pendingReads.fill(false);
		api.cancelAll();
		// wasWriting отличает активное намерение от длительного чтения.
		const bool wasWriting = model.writing;
		if (!destinationBusyId.isEmpty()) {
			model.destinationOutcomes.insert(destinationBusyId, sent ? ConfigurationOutcome::Unknown
										 : ConfigurationOutcome::Rejected);
			if (sent) {
				model.destinationReconciliation.insert(destinationBusyId);
				rememberUnknown(ConfigurationKind::Destination, destinationBusyId);
				if (!guard)
					return;
			}
			destinationBusyId.clear();
		}
		destinationQueue.clear();
		if (wasWriting && !destinationBatch) {
			model.outcome = sent ? ConfigurationOutcome::Unknown : ConfigurationOutcome::Rejected;
			model.reconciliationRequired = sent;
			model.messageKey = sent ? QStringLiteral("Configuration.Unknown")
						: QStringLiteral("Configuration.Rejected");
			if (sent) {
				rememberUnknown(model.kind, model.id);
				if (!guard)
					return;
			}
		}
		model.writing = false;
		if (destinationBatch) {
			model.destinationMessageKey = QStringLiteral("Configuration.DestinationsPartial");
			destinationBatch = false;
		}
		model.loading = false;
		if (model.context.valid())
			parked.insert_or_assign(draftKey(model.context, model.kind, model.id), model);
		releaseWrite();
		if (!guard)
			return;
		if (wasWriting)
			state.invalidateConfiguration();
		if (!guard)
			return;
		publish();
		if (!guard)
			return;
		changingChannel = false;
		// Глобальный shutdown ждёт всех обработчиков закрытия и их учёта отправленных записей.
		if (stopped) {
			api.shutdown();
			return;
		}
		// Отложенный обработчик перечитывает последний контекст после объединённых уведомлений закрытия.
		if (observeAgain && !contextTransition)
			QTimer::singleShot(0, &owner, [this] { observe(); });
	}

	// Закрывает старый канал до изменения context; обычное восстановление SSE configure не вызывает.
	void observe()
	{
		if (stopped)
			return;
		if (changingChannel || contextTransition) {
			observeAgain = true;
			return;
		}
		contextTransition = true;
		observeAgain = false;
		// next использует подтверждённый UUID, а не имя OBS Profile.
		const ConfigurationContext next{state.serverOrigin(),
						state.profileResolved() ? state.dockContext().profileUuid : QString{},
						state.selectedStreamId()};
		// reconnected отличает новое Connected от обычного сигнала кеша.
		const bool reconnected = observedStatus != ConnectionStatus::Connected &&
					 state.connectionStatus() == ConnectionStatus::Connected;
		observedStatus = state.connectionStatus();
		// guard защищает все продолжения после сигналов закрытия/готовности.
		const QPointer<ConfigurationController> guard = &owner;
		if (next != context || (reconnected && disconnected)) {
			cancel(false);
			if (!guard || stopped)
				return;
			context = {state.serverOrigin(),
				   state.profileResolved() ? state.dockContext().profileUuid : QString{},
				   state.selectedStreamId()};
			++apiContext.selection;
			if (reconnected)
				disconnected = false;
			api.configure(QUrl(context.origin), apiContext);
			if (!guard || stopped)
				return;
			channelReady = context.valid() && !disconnected;
			if (channelReady)
				emit owner.channelReady();
			if (!guard || stopped)
				return;
		}
		if (reconnected)
			disconnected = false;
		contextTransition = false;
		// Отложенный обработчик применит контекст, изменённый из синхронных уведомлений этого перехода.
		if (observeAgain)
			QTimer::singleShot(0, &owner, [this] { observe(); });
		if (reconnected && !unknownWrites.isEmpty()) {
			if (model.context == context && model.requiresReconciliation())
				reload();
			else
				deferReconciliation();
		}
	}

	// Уступает скрытое чтение явному обновлению формы; неизвестная запись при отмене GET сохраняется.
	bool stopReconciliation()
	{
		// receiver снимается до удаления, чтобы вложенный callback не завершил прежнюю сверку.
		const QPointer<QObject> receiver = reconciliationReceiver;
		// guard учитывает синхронные callbacks сетевой очереди при уничтожении receiver.
		const QPointer<ConfigurationController> guard = &owner;
		reconciliationReceiver = nullptr;
		delete receiver.data();
		return guard;
	}

	// Объединяет продолжения сверки; активные чтения формы завершатся до следующего скрытого GET.
	void deferReconciliation()
	{
		if (stopped || unknownWrites.isEmpty() || reconciliationScheduled)
			return;
		reconciliationScheduled = true;
		// Обработчик продолжает скрытую сверку после чтений формы; ошибка GET сама новый запуск не планирует.
		QTimer::singleShot(0, &owner, [this] {
			reconciliationScheduled = false;
			reconcileUnknown();
		});
	}

	// Снимает точное обязательство после применения свежего DTO либо полного списка создания.
	// Старый исход Unknown сохраняется; общий сигнал публикуется последним и требует проверки guard у вызывающего.
	void settleUnknown(ConfigurationKind kind, const QString &id)
	{
		// origin фиксирует сервер прочитанного адреса до внешнего уведомления общего барьера.
		const auto origin = context.origin;
		unknownWrites.erase(std::remove_if(unknownWrites.begin(), unknownWrites.end(),
						   [&origin, kind, &id](const auto &item) {
							   return item.origin == origin && item.kind == kind &&
								  item.id == id;
						   }),
				    unknownWrites.end());
		if (model.context.origin == origin) {
			if (model.kind == kind && model.id == id)
				model.reconciliationRequired = false;
			if (kind == ConfigurationKind::Destination)
				model.destinationReconciliation.remove(id);
		}
		// entry адресует припаркованную форму; draft сохраняет её прежний исход и пользовательский ввод.
		for (auto &entry : parked) {
			auto &draft = entry.second;
			if (draft.context.origin != origin)
				continue;
			if (draft.kind == kind && draft.id == id)
				draft.reconciliationRequired = false;
			if (kind == ConfigurationKind::Destination)
				draft.destinationReconciliation.remove(id);
		}
		// remaining сохраняет блокировку всех остальных обязательств данного сервера.
		const bool remaining = std::any_of(unknownWrites.cbegin(), unknownWrites.cend(),
						   [&origin](const auto &item) { return item.origin == origin; });
		state.setConfigurationUnknown(origin, QStringLiteral("Configuration"), remaining);
	}

	// Сверяет только скрытый адрес; видимая форма применяет DTO и снимает своё обязательство собственным GET.
	// Один receiver исключает дубли; ошибка и неполный список сохраняют барьер без автоматического повтора.
	void reconcileUnknown()
	{
		if (!available() || unknownWrites.isEmpty() || reconciliationReceiver || model.writing ||
		    model.loading ||
		    std::any_of(pendingReads.cbegin(), pendingReads.cend(), [](bool pending) { return pending; }) ||
		    (model.context == context && model.requiresReconciliation()))
			return;
		// found выбирает обязательство нынешнего origin; остальные серверы сохраняют собственный запрет.
		const auto found = std::find_if(unknownWrites.cbegin(), unknownWrites.cend(),
						[this](const auto &item) { return item.origin == context.origin; });
		if (found == unknownWrites.cend())
			return;
		// item, expected, token и epoch фиксируют адрес и свежесть единственного скрытого чтения.
		const auto item = *found;
		const auto expected = context;
		const auto token = reconciliationGeneration;
		const auto epoch = state.configurationGeneration();
		// receiver адресует только эту сверку и не отменяет запросы другого владельца общего API.
		const QPointer<QObject> receiver = new QObject(&owner);
		reconciliationReceiver = receiver;
		// valid проверяет receiver и поколение до применения результата или публикации сигналов.
		const auto valid = [this, receiver, expected, token, epoch] {
			return receiver && reconciliationReceiver == receiver && current(expected, epoch) &&
			       token == reconciliationGeneration;
		};
		// finish освобождает receiver, отзывает прежние наблюдения и продолжает лишь успешную свежую сверку.
		const auto finish = [this, item, expected, epoch, receiver, valid](bool complete) {
			if (!valid()) {
				if (reconciliationReceiver == receiver)
					reconciliationReceiver = nullptr;
				if (receiver)
					receiver->deleteLater();
				return;
			}
			reconciliationReceiver = nullptr;
			receiver->deleteLater();
			if (!complete)
				return;
			// guard защищает публикации кеша, барьера и эпохи от вложенного закрытия либо удаления.
			const QPointer<ConfigurationController> guard = &owner;
			settleUnknown(item.kind, item.id);
			if (!guard || !current(expected, epoch))
				return;
			state.invalidateConfiguration();
			if (!guard || !current(expected, epoch + 1))
				return;
			publish();
			if (!guard || !current(expected, epoch + 1))
				return;
			restream.refreshFallbackAfterConfiguration();
			if (guard && current(expected, epoch + 1))
				deferReconciliation();
		};
		// read применяет ровно тот проверенный DTO/список, который затем снимает обязательство.
		const auto read = [this, item, receiver, valid, finish]<class T>() {
			if (item.id.isEmpty()) {
				// callback обновляет полноту каталога; только полный список разрешает новое создание.
				const auto callback = [this, valid, finish](auto,
									    api::Result<api::Collection<T>> result) {
					if (!valid()) {
						finish(false);
						return;
					}
					if (!result) {
						finish(false);
						return;
					}
					// next сохраняет другие семейства и элементы, отсутствие которых неполный список не доказывает.
					auto next = state.configuration();
					if (result.value->complete)
						collection<T>(next).clear();
					// value содержит подтверждённую запись прочитанного каталога.
					for (const auto &value : result.value->items)
						collection<T>(next).insert(value.id, value);
					readStatus<T>(next) = {RuntimeAvailability::Fresh, result.value->complete, {}};
					// guard запрещает вызов finish после удаления из уведомления кеша.
					const QPointer<ConfigurationController> guard = &owner;
					state.setConfiguration(std::move(next));
					if (guard)
						finish(result.value->complete);
				};
				if constexpr (std::is_same_v<T, api::Stream>)
					api.getAllStreams(receiver, callback);
				else if constexpr (std::is_same_v<T, api::Destination>)
					api.getAllDestinations(receiver, callback);
				else if constexpr (std::is_same_v<T, api::Group>)
					api.getAllGroups(receiver, callback);
				else
					api.getAllProfiles(receiver, callback);
			} else {
				// callback применяет точный DTO либо подтверждённое отсутствие, не объявляя прежнюю запись успешной.
				const auto callback = [this, item, valid, finish](auto, api::Result<T> result) {
					if (!valid()) {
						finish(false);
						return;
					}
					// present/absent отличают адресное подтверждение от ошибки либо чужого ID.
					const bool present = result && result.value->id == item.id;
					const bool absent = !result && result.error && result.error->httpStatus == 404;
					if (!present && !absent) {
						finish(false);
						return;
					}
					// next изменяет только прочитанный адрес, сохраняя прочие коллекции и их полноту.
					auto next = state.configuration();
					if (present)
						collection<T>(next).insert(item.id, *result.value);
					else
						collection<T>(next).remove(item.id);
					// guard защищает продолжение после сигнала кеша.
					const QPointer<ConfigurationController> guard = &owner;
					state.setConfiguration(std::move(next));
					if (guard)
						finish(true);
				};
				if constexpr (std::is_same_v<T, api::Stream>)
					api.getStream(item.id, receiver, callback);
				else if constexpr (std::is_same_v<T, api::Destination>)
					api.getDestination(item.id, receiver, callback);
				else if constexpr (std::is_same_v<T, api::Group>)
					api.getGroup(item.id, receiver, callback);
				else
					api.getProfile(item.id, receiver, callback);
			}
		};
		switch (item.kind) {
		case ConfigurationKind::Stream:
			read.template operator()<api::Stream>();
			break;
		case ConfigurationKind::Destination:
			read.template operator()<api::Destination>();
			break;
		case ConfigurationKind::Group:
			read.template operator()<api::Group>();
			break;
		case ConfigurationKind::Profile:
			read.template operator()<api::Profile>();
			break;
		}
	}

	// Проверяет контекст и эпоху, захваченные перед GET.
	bool current(const ConfigurationContext &expected, quint64 epoch) const
	{
		return !stopped && !disconnected && context == expected && state.configurationGeneration() == epoch;
	}

	// Публикует DTO только в соответствующую коллекцию последнего aggregate.
	template<class T> void cache(const T &value)
	{
		// next копируется непосредственно перед изменением, чтобы не потерять другие завершённые GET.
		auto next = state.configuration();
		collection<T>(next).insert(value.id, value);
		state.setConfiguration(std::move(next));
	}

	// Удаляет из общего RAM-кеша только Destination, адресно подтверждённый ответом 404.
	void removeDestinationFromCache(const QString &id)
	{
		// next сохраняет Streams, Groups, Profiles и другие Destinations при удалении одного ID.
		auto next = state.configuration();
		next.destinations.remove(id);
		state.setConfiguration(std::move(next));
	}

	// Читает коллекцию выбранного kind с независимым поколением и явной неполнотой.
	// Полный список завершает сверку создания Unknown, отзывая прежнее чтение каталога Dock.
	template<class T> void refreshTyped(ConfigurationKind kind)
	{
		// index адресует семейство DTO; sequence отличает его чтения, epoch фиксирует конфигурацию,
		// expected сохраняет контекст, detailSequence отличает исходную карточку при сверке создания.
		const auto index = static_cast<size_t>(kind);
		const auto sequence = ++reads[index];
		pendingReads[index] = true;
		const auto epoch = state.configurationGeneration();
		const auto expected = context;
		const auto detailSequence = detailGeneration;
		// callback принимает result только в исходном чтении и сливает неполный список с кешем:
		// отсутствие элемента не является удалением, а полный список может завершить сверку создания.
		auto callback = [this, index, sequence, epoch, expected,
				 detailSequence](auto, api::Result<api::Collection<T>> result) {
			if (reads[index] != sequence)
				return;
			pendingReads[index] = false;
			deferReconciliation();
			if (!current(expected, epoch))
				return;
			// next всегда содержит последние результаты других коллекций.
			auto next = state.configuration();
			if (!result) {
				readStatus<T>(next).availability = RuntimeAvailability::Stale;
				readStatus<T>(next).safeError = QStringLiteral("Configuration.ReadFailed");
			} else {
				if (result.value->complete)
					collection<T>(next).clear();
				// item является подтверждённой серверной записью текущего списка.
				for (const auto &item : result.value->items)
					collection<T>(next).insert(item.id, item);
				readStatus<T>(next) = {RuntimeAvailability::Fresh, result.value->complete, {}};
			}
			// guard отслеживает удаление контроллера при уведомлении кеша.
			const QPointer<ConfigurationController> guard = &owner;
			state.setConfiguration(std::move(next));
			if (!guard || !current(expected, epoch) || reads[index] != sequence)
				return;
			if (result && result.value->complete && model.context == expected && model.creating &&
			    model.reconciliationRequired && static_cast<size_t>(model.kind) == index &&
			    detailGeneration == detailSequence) {
				settleUnknown(static_cast<ConfigurationKind>(index), {});
				if (!guard || !current(expected, epoch) || reads[index] != sequence ||
				    detailGeneration != detailSequence)
					return;
				// completedEpoch учитывает собственный отзыв каталогового GET до завершения сверки создания.
				const auto completedEpoch = epoch + 1;
				state.invalidateConfiguration();
				if (!guard || !current(expected, completedEpoch) || reads[index] != sequence ||
				    detailGeneration != detailSequence)
					return;
				publish();
				if (!guard || !current(expected, completedEpoch) || reads[index] != sequence ||
				    detailGeneration != detailSequence)
					return;
				connection.refreshState();
				if (guard && current(expected, completedEpoch) && reads[index] == sequence &&
				    detailGeneration == detailSequence)
					restream.refreshFallbackAfterConfiguration();
			}
		};
		if constexpr (std::is_same_v<T, api::Stream>)
			api.getAllStreams(&owner, callback);
		else if constexpr (std::is_same_v<T, api::Destination>)
			api.getAllDestinations(&owner, callback);
		else if constexpr (std::is_same_v<T, api::Group>)
			api.getAllGroups(&owner, callback);
		else
			api.getAllProfiles(&owner, callback);
	}

	// Выбирает типизированный GET списка без универсального HTTP в интерфейсе.
	void refresh(ConfigurationKind kind)
	{
		if (!available())
			return;
		if (!stopReconciliation() || !available())
			return;
		switch (kind) {
		case ConfigurationKind::Stream:
			refreshTyped<api::Stream>(kind);
			break;
		case ConfigurationKind::Destination:
			refreshTyped<api::Destination>(kind);
			break;
		case ConfigurationKind::Group:
			refreshTyped<api::Group>(kind);
			break;
		case ConfigurationKind::Profile:
			refreshTyped<api::Profile>(kind);
			break;
		}
	}

	// Читает точную сущность, сохраняя несохранённый ввод; conflict сравнивает версию после revision_conflict.
	// Сверка Unknown отзывает старые чтения конфигурации до публикации результата и обновляет Dock.
	// Успех GET и 404 подтверждают только нынешнее состояние, не успех потерянной записи.
	template<class T> void readTyped(bool conflict)
	{
		// sequence отличает адресные чтения; epoch фиксирует конфигурацию, expected — контекст,
		// id сохраняет точный адрес карточки до возможной синхронной доставки.
		const auto sequence = ++detailGeneration;
		const auto epoch = state.configurationGeneration();
		const auto expected = context;
		const auto id = model.id;
		model.loading = true;
		// guard обнаруживает удаление контроллера обработчиком сигнала.
		const QPointer<ConfigurationController> guard = &owner;
		publish();
		if (!guard)
			return;
		// callback проверяет адресный result, сохраняя несохранённый ввод и причину прежнего Unknown;
		// завершённая сверка не доказывает успех потерянной записи.
		auto callback = [this, sequence, epoch, expected, id, conflict](auto, api::Result<T> result) {
			if (sequence != detailGeneration || stopped)
				return;
			model.loading = false;
			deferReconciliation();
			if (!current(expected, epoch)) {
				publish();
				return;
			}
			if (!result && result.error && result.error->httpStatus == 404 &&
			    model.reconciliationRequired) {
				// Адресный GET подтверждает отсутствие сейчас, но не результат потерянной записи.
				model.reconciliationRequired = false;
				model.messageKey = QStringLiteral("Configuration.Unknown");
				// guard защищает продолжение после уведомлений эпохи, кеша и формы.
				const QPointer<ConfigurationController> guard = &owner;
				// completedEpoch допускает собственную инвалидацию, но не дополнительную смену эпохи из сигнала.
				const auto completedEpoch = epoch + 1;
				state.invalidateConfiguration();
				if (!guard || sequence != detailGeneration || !current(expected, completedEpoch))
					return;
				// next удаляет только точно отсутствующий ID из кеша после уведомлений смены эпохи.
				auto next = state.configuration();
				collection<T>(next).remove(id);
				state.setConfiguration(std::move(next));
				if (!guard || sequence != detailGeneration || !current(expected, completedEpoch))
					return;
				settleUnknown(model.kind, id);
				if (!guard || sequence != detailGeneration || !current(expected, completedEpoch))
					return;
				publish();
				if (!guard || sequence != detailGeneration || !current(expected, completedEpoch))
					return;
				connection.refreshState();
				if (guard && sequence == detailGeneration && current(expected, completedEpoch))
					restream.refreshFallbackAfterConfiguration();
				return;
			}
			if (!result || result.value->id != id) {
				model.messageKey = QStringLiteral("Configuration.ReadFailed");
				publish();
				return;
			}
			// value сохраняет DTO до уведомлений кеша.
			const auto value = *result.value;
			if (conflict) {
				// Причина уже установлена кодом revision_conflict исходного ответа, а не порядком ревизий GET.
				model.serverConflict = ConfigurationEntity(value);
				model.outcome = ConfigurationOutcome::Conflict;
				model.messageKey = QStringLiteral("Configuration.Conflict");
			} else if (model.reconciliationRequired) {
				// Свежая база задаёт ревизию следующего явного сохранения; dirty-ввод и исторический Unknown остаются.
				model.base = value;
				if (model.dirtyFields.isEmpty())
					model.draft = value;
			} else if (model.dirtyFields.isEmpty() && model.outcome != ConfigurationOutcome::Unknown) {
				model.base = value;
				model.draft = value;
			}
			// Успешное новое чтение снимает ошибку GET, сохраняя неопределённость прежней записи.
			if (!conflict && model.messageKey == QStringLiteral("Configuration.ReadFailed"))
				model.messageKey = model.outcome == ConfigurationOutcome::Unknown
							   ? QStringLiteral("Configuration.Unknown")
							   : QString{};
			// reconciled отмечает завершение явной сверки после неопределённого результата.
			const bool reconciled = model.reconciliationRequired;
			model.reconciliationRequired = false;
			// guard защищает дальнейшую публикацию после сигналов эпохи, кеша и формы.
			const QPointer<ConfigurationController> guard = &owner;
			// completedEpoch учитывает единственную собственную инвалидацию только после сверки Unknown.
			const auto completedEpoch = epoch + (reconciled ? 1 : 0);
			if (reconciled) {
				state.invalidateConfiguration();
				if (!guard || sequence != detailGeneration || !current(expected, completedEpoch))
					return;
			}
			cache(value);
			if (!guard || sequence != detailGeneration || !current(expected, completedEpoch))
				return;
			if (reconciled) {
				settleUnknown(model.kind, id);
				if (!guard || sequence != detailGeneration || !current(expected, completedEpoch))
					return;
			}
			publish();
			if (!guard || !reconciled || sequence != detailGeneration || !current(expected, completedEpoch))
				return;
			connection.refreshState();
			if (guard && sequence == detailGeneration && current(expected, completedEpoch))
				restream.refreshFallbackAfterConfiguration();
		};
		if constexpr (std::is_same_v<T, api::Stream>)
			api.getStream(id, &owner, callback);
		else if constexpr (std::is_same_v<T, api::Destination>)
			api.getDestination(id, &owner, callback);
		else if constexpr (std::is_same_v<T, api::Group>)
			api.getGroup(id, &owner, callback);
		else
			api.getProfile(id, &owner, callback);
	}

	// Выбирает адресное чтение текущей карточки; создавать фиктивную базу не требуется.
	bool reload(bool conflict = false)
	{
		if (!available() || model.writing || model.context != context)
			return false;
		if (!stopReconciliation() || !available() || model.context != context)
			return false;
		if (model.kind == ConfigurationKind::Stream && !model.destinationReconciliation.isEmpty())
			return reconcileDestinations();
		if (model.kind == ConfigurationKind::Stream) {
			// guard и generation запрещают продолжение после синхронного закрытия или смены карточки.
			const QPointer<ConfigurationController> guard = &owner;
			const auto generation = detailGeneration;
			refresh(ConfigurationKind::Destination);
			if (!guard || generation != detailGeneration)
				return false;
		}
		if (model.id.isEmpty()) {
			if (!model.creating || !model.reconciliationRequired)
				return false;
			refresh(model.kind);
			return true;
		}
		switch (model.kind) {
		case ConfigurationKind::Stream:
			readTyped<api::Stream>(conflict);
			break;
		case ConfigurationKind::Destination:
			readTyped<api::Destination>(conflict);
			break;
		case ConfigurationKind::Group:
			readTyped<api::Group>(conflict);
			break;
		case ConfigurationKind::Profile:
			readTyped<api::Profile>(conflict);
			break;
		}
		return true;
	}

	// Открывает новую карточку лишь после явного решения о несохранённом вводе.
	bool open(ConfigurationKind kind, QString id, bool discard)
	{
		if (!available() || model.writing || (model.context == context && model.requiresReconciliation()) ||
		    ((!model.dirtyFields.isEmpty() || !model.destinationChanges.isEmpty()) && !discard) || id.isEmpty())
			return false;
		++detailGeneration;
		model = {};
		model.context = context;
		model.kind = kind;
		model.id = std::move(id);
		return reload();
	}

	// Создаёт типизированный ввод; обязательные параметры пользователь заполняет до отправки.
	bool beginCreate(ConfigurationKind kind, bool discard)
	{
		if (!available() || model.writing || (model.context == context && model.requiresReconciliation()) ||
		    ((!model.dirtyFields.isEmpty() || !model.destinationChanges.isEmpty()) && !discard))
			return false;
		++detailGeneration;
		parked.erase(draftKey(context, kind, {}));
		model = {};
		model.context = context;
		model.kind = kind;
		model.creating = true;
		switch (kind) {
		case ConfigurationKind::Stream: {
			// value оставляет ключ недоступным до явного ввода или нажатия кнопки генерации в форме.
			api::Stream value;
			model.draft = value;
			break;
		}
		case ConfigurationKind::Destination: {
			// value использует рабочий Stream только как первоначальное значение поля.
			api::Destination value;
			value.streamId = context.workingStreamId;
			value.enabled = false;
			value.endpoint = {api::FieldState::Value, {}};
			value.streamKey = {api::FieldState::Value, {}};
			model.draft = value;
			break;
		}
		case ConfigurationKind::Group: {
			// value сохраняет первоначальную принадлежность рабочему потоку.
			api::Group value;
			value.streamId = context.workingStreamId;
			model.draft = value;
			break;
		}
		case ConfigurationKind::Profile: {
			// value явно задаёт новый LIVE AAC copy без изменения семантики старых профилей.
			api::Profile value;
			value.profile.mode = QStringLiteral("copy");
			value.profile.liveAudioMode = {api::FieldState::Value, QStringLiteral("copy")};
			model.draft = value;
			break;
		}
		}
		publish();
		return true;
	}

	// Принимает только правильную альтернативу текущей формы, сохраняя исходный ID и базу.
	bool setDraft(ConfigurationEntity value, QSet<QString> fields)
	{
		if (stopped || model.writing || model.loading || model.context != context ||
		    !matches(model.kind, value))
			return false;
		// allowed содержит публичные поля выбранного DTO; publication_key разрешён лишь в create.
		auto allowed = editableFields(model.kind);
		if (model.creating && model.kind == ConfigurationKind::Stream)
			allowed.insert(QStringLiteral("publication_key"));
		// field адресует одно явное намерение формы, а не произвольный путь JSON.
		for (const auto &field : fields)
			if (!allowed.contains(field))
				return false;
		model.draft = std::move(value);
		model.dirtyFields = std::move(fields);
		model.fieldIssues.clear();
		publish();
		return true;
	}

	// Сбрасывает подтверждённый RAM-ввод без права записи старой формы в новый контекст.
	// expectedGeneration фиксируется до диалога; Unknown текущего контекста требует GET перед уходом.
	bool discardDraft(quint64 expectedGeneration)
	{
		if (stopped || model.writing || model.loading || !model.draft ||
		    expectedGeneration != editorGeneration ||
		    (model.context == context && model.requiresReconciliation()))
			return false;
		model.draft = model.base.value_or(*model.draft);
		model.dirtyFields.clear();
		model.fieldIssues.clear();
		model.destinationChanges.clear();
		model.destinationRevisions.clear();
		model.destinationNames.clear();
		model.destinationOutcomes.clear();
		model.destinationReconciliation.clear();
		model.destinationMessageKey.clear();
		parked.erase(draftKey(model.context, model.kind, model.id));
		// guard замечает удаление контроллера, acceptedGeneration — замену формы из обработчика публикации.
		const QPointer<ConfigurationController> guard = &owner;
		const auto acceptedGeneration = editorGeneration + 1;
		publish();
		return guard && editorGeneration == acceptedGeneration;
	}

	// Передаёт отказ либо отсутствие подтверждения отправленного CRUD-шага один раз.
	// Сигнал не меняет модель формы и не хранит тело ответа; локальные причины переводит presenter.
	// Возвращает false, если обработчик завершил сервис либо сменил исходный контекст/намерение.
	bool notifyFailure(const api::Error &error, const QString &stepIdentity)
	{
		if (!mutationRejected(error) && !error.requestSent)
			return true;
		if (stopped || disconnected || model.context != context)
			return false;
		if (notifiedMutation == mutationGeneration && notifiedStep == stepIdentity)
			return true;
		notifiedMutation = mutationGeneration;
		notifiedStep = stepIdentity;
		// failure передаёт полученную диагностику либо доверенные ключи отсутствующего подтверждения.
		auto failure = operationFailure(error);
		if (!mutationRejected(error)) {
			failure.reasonKey = error.kind == api::ErrorKind::Timeout
						    ? QStringLiteral("Configuration.ConfirmationTimeout")
						    : QStringLiteral("Configuration.ConfirmationUnavailable");
			failure.actionKey = QStringLiteral("Configuration.RefreshBeforeSaving");
		}
		failure.source = OperationFailureSource::Configuration;
		failure.context = {context.origin, context.profileUuid, context.workingStreamId};
		failure.generation = mutationGeneration;
		failure.stepIdentity = stepIdentity;
		// guard отслеживает уничтожение фасада; expected/sequence запрещают продолжение чужой карточки.
		const QPointer<ConfigurationController> guard = &owner;
		const auto expected = context;
		const auto sequence = mutationGeneration;
		emit owner.operationFailed(failure);
		return guard && !stopped && !disconnected && context == expected && mutationGeneration == sequence;
	}

	// Сначала фиксирует Unknown и барьер, затем освобождает mutation и уведомляет presenter.
	// Только revision_conflict запускает сравнение; синхронный обработчик не может потерять обязательство.
	void failed(const api::Error &error)
	{
		// expected/sequence/guard защищают исходное намерение после каждого внешнего сигнала.
		// stepId сохраняет идентичность прежней карточки независимо от reentrant навигации.
		const auto stepId = model.id;
		const auto expected = context;
		const auto sequence = mutationGeneration;
		const QPointer<ConfigurationController> guard = &owner;
		model.writing = false;
		model.outcome = !error.requestSent || mutationRejected(error) ? ConfigurationOutcome::Rejected
									      : ConfigurationOutcome::Unknown;
		model.reconciliationRequired = model.outcome == ConfigurationOutcome::Unknown;
		model.messageKey = model.reconciliationRequired ? QStringLiteral("Configuration.Unknown")
								: QStringLiteral("Configuration.Rejected");
		model.fieldIssues.clear();
		if (model.reconciliationRequired)
			rememberUnknown(model.kind, model.id);
		if (!guard || context != expected || mutationGeneration != sequence)
			return;
		releaseWrite();
		if (!guard || context != expected || mutationGeneration != sequence)
			return;
		state.invalidateConfiguration();
		if (!guard || context != expected || mutationGeneration != sequence)
			return;
		publish();
		if (!guard || context != expected || mutationGeneration != sequence)
			return;
		if (!notifyFailure(error, stepId))
			return;
		if (revisionConflict(error))
			reload(true);
	}

	// Принимает value как подтверждённый DTO записи и обновляет лишь его коллекцию.
	// Смена контекста или намерения из синхронных сигналов прекращает дальнейшие публикации.
	template<class T> void saved(T value)
	{
		if (value.id.isEmpty() || (!model.creating && value.id != model.id)) {
			// error означает непроверенный успешный ответ после возможной записи.
			api::Error error;
			error.kind = api::ErrorKind::Contract;
			error.requestSent = true;
			failed(error);
			return;
		}
		parked.erase(draftKey(model.context, model.kind, model.id));
		model.writing = false;
		model.creating = false;
		model.id = value.id;
		model.base = value;
		model.draft = value;
		model.dirtyFields.clear();
		model.serverConflict.reset();
		model.outcome = ConfigurationOutcome::Saved;
		model.messageKey = QStringLiteral("Configuration.Saved");
		// expected/sequence фиксируют исходную запись до сигнала освобождения общего канала.
		const auto expected = context;
		const auto sequence = mutationGeneration;
		// writeGuard защищает продолжение после освобождения общей mutation.
		const QPointer<ConfigurationController> writeGuard = &owner;
		releaseWrite();
		if (!writeGuard || stopped || context != expected || mutationGeneration != sequence)
			return;
		// guard защищает последовательность синхронных публикаций.
		const QPointer<ConfigurationController> guard = &owner;
		state.invalidateConfiguration();
		if (!guard || stopped || context != expected || mutationGeneration != sequence)
			return;
		cache(value);
		if (!guard || stopped || context != expected || mutationGeneration != sequence)
			return;
		publish();
		if (!guard || stopped || context != expected || mutationGeneration != sequence)
			return;
		refresh(model.kind);
		if (!guard || stopped || context != expected || mutationGeneration != sequence)
			return;
		connection.refreshState();
		if (!guard || stopped || context != expected || mutationGeneration != sequence)
			return;
		restream.refreshFallbackAfterConfiguration();
	}

	// Валидирует payload до инвалидации и передачи в API, затем запускает один typed запрос.
	template<class T, class Request, class Send> bool submit(const Request &request, Send send)
	{
		// encoded проверяет обязательные поля до смены эпохи и передачи запроса сетевому клиенту.
		const auto encoded = api::encodeRequest(request);
		if (!encoded) {
			model.outcome = ConfigurationOutcome::Rejected;
			model.messageKey = QStringLiteral("Configuration.NotSaved");
			model.fieldIssues.clear();
			if (encoded.error && encoded.error->problem)
				collectFieldIssues(model, *encoded.error->problem);
			publish();
			return false;
		}
		if (!acquireWrite())
			return false;
		model.writing = true;
		model.outcome = ConfigurationOutcome::None;
		model.messageKey.clear();
		model.fieldIssues.clear();
		// sequence различает намерения, expected фиксирует сервер/UUID/рабочий Stream до HTTP.
		const auto sequence = mutationGeneration;
		const auto expected = context;
		// guard обнаруживает уничтожение контроллера при синхронных сигналах.
		const QPointer<ConfigurationController> guard = &owner;
		state.invalidateConfiguration();
		if (!guard || sequence != mutationGeneration)
			return false;
		publish();
		if (!guard || sequence != mutationGeneration)
			return false;
		if (!available() || context != expected ||
		    !state.ownsConfigurationWrite(writeOrigin, QStringLiteral("Configuration"), writeToken))
			return false;
		// callback принимает результат только пока существует исходное намерение.
		send([this, sequence, expected](auto, api::Result<T> result) {
			if (stopped || context != expected || sequence != mutationGeneration)
				return;
			if (!result) {
				failed(*result.error);
				return;
			}
			saved(std::move(*result.value));
		});
		return true;
	}

	// Запускает только явные различия enabled по сохранённым ревизиям полного свежего списка.
	bool saveDestinations()
	{
		if (!model.destinationReconciliation.isEmpty()) {
			model.destinationMessageKey = QStringLiteral("Configuration.DestinationsReconcile");
			publish();
			return false;
		}
		// config подтверждает полноту списка назначений перед запуском пакета.
		const auto &config = state.configuration();
		if (!config.destinationsStatus.complete ||
		    config.destinationsStatus.availability != RuntimeAvailability::Fresh) {
			model.destinationMessageKey = QStringLiteral("Configuration.DestinationsIncomplete");
			publish();
			return false;
		}
		// pendingIds фиксирует ключи до очистки устаревших намерений; id обозначает назначение текущего шага.
		const auto pendingIds = model.destinationChanges.keys();
		// id обозначает одно намерение из неизменяемого снимка pendingIds.
		for (const auto &id : pendingIds) {
			// found проверяет принадлежность непосредственно перед запуском пакета.
			const auto found = config.destinations.constFind(id);
			if (found == config.destinations.cend() || found->streamId != model.id) {
				retireDestinationIntent(id);
				continue;
			}
			if (!model.destinationRevisions.contains(id))
				return false;
		}
		if (!acquireWrite())
			return false;
		destinationQueue = model.destinationChanges.keys();
		destinationBatch = true;
		model.writing = true;
		model.destinationMessageKey.clear();
		// guard защищает продолжение при публикации занятой формы.
		const QPointer<ConfigurationController> guard = &owner;
		publish();
		if (guard)
			nextDestination();
		return true;
	}

	// Отправляет один PATCH enabled; после каждого результата продолжает остальные явные изменения.
	// Callback проверяет контекст и поколение; потерянный ответ не повторяется автоматически.
	void nextDestination()
	{
		if (!destinationBatch || model.context != context)
			return;
		// expected и sequence связывают весь шаг с прежним пакетом до любых внешних уведомлений.
		const auto expected = context;
		const auto sequence = mutationGeneration;
		if (!available()) {
			// guard исключает завершение прежнего пакета в контексте, изменённом при освобождении канала.
			const QPointer<ConfigurationController> guard = &owner;
			releaseWrite();
			if (!guard || stopped || context != expected || sequence != mutationGeneration)
				return;
			destinationBatch = false;
			destinationQueue.clear();
			model.writing = false;
			model.destinationMessageKey = QStringLiteral("Configuration.DestinationsPartial");
			publish();
			return;
		}
		// Каждый следующий PATCH должен иметь место для возможного Unknown. Неотправленный ввод остаётся в форме.
		// Предел совпадает с допуском новых намерений; уже сохранённые обязательства не удаляются ради продолжения.
		if (!destinationQueue.isEmpty() && unknownWrites.size() >= 64)
			destinationQueue.clear();
		if (destinationQueue.isEmpty()) {
			// writeGuard ограждает завершение пакета после синхронного освобождения канала.
			const QPointer<ConfigurationController> writeGuard = &owner;
			releaseWrite();
			if (!writeGuard || stopped || context != expected || sequence != mutationGeneration)
				return;
			destinationBatch = false;
			model.writing = false;
			// hasFailure не позволяет снять ошибку с общей сводки после удаления устаревших намерений.
			const bool hasFailure = hasDestinationFailure();
			model.destinationMessageKey = model.destinationChanges.isEmpty() && !hasFailure
							      ? QStringLiteral("Configuration.DestinationsSaved")
							      : QStringLiteral("Configuration.DestinationsPartial");
			model.outcome = model.destinationChanges.isEmpty() && !hasFailure
						? ConfigurationOutcome::Saved
						: ConfigurationOutcome::Rejected;
			// guard защищает последующее сохранение обычных полей и обновление кеша.
			const QPointer<ConfigurationController> guard = &owner;
			publish();
			if (!guard || stopped || context != expected || sequence != mutationGeneration)
				return;
			if (model.requiresReconciliation())
				return;
			if (!model.dirtyFields.isEmpty()) {
				save(true);
				return;
			}
			connection.refreshState();
			if (guard && !stopped && context == expected && sequence == mutationGeneration)
				restream.refreshFallbackAfterConfiguration();
			return;
		}
		// id и desired фиксируют адрес и значение одного шага; expected и sequence сохранены до освобождения канала.
		const auto id = destinationQueue.takeFirst();
		const bool desired = model.destinationChanges.value(id);
		// found проверяет, что обновлённый кеш всё ещё связывает назначение с этим Stream.
		const auto found = state.configuration().destinations.constFind(id);
		if (found == state.configuration().destinations.cend() || found->streamId != model.id) {
			// config позволяет считать отсутствие доказанным только по полному свежему списку.
			const auto &config = state.configuration();
			if (config.destinationsStatus.complete &&
			    config.destinationsStatus.availability == RuntimeAvailability::Fresh)
				retireDestinationIntent(id);
			else
				model.destinationOutcomes.insert(id, ConfigurationOutcome::Rejected);
			// Отложенный шаг принадлежит тому же пакету, даже если до него пользователь сменит контекст.
			QTimer::singleShot(0, &owner, [this, expected, sequence] {
				if (context == expected && sequence == mutationGeneration)
					nextDestination();
			});
			return;
		}
		if (found->enabled == desired) {
			model.destinationChanges.remove(id);
			model.destinationRevisions.remove(id);
			model.destinationOutcomes.insert(id, ConfigurationOutcome::Saved);
			// Полученный флаг уже соответствует выбору; отдельная запись не нужна.
			QTimer::singleShot(0, &owner, [this, expected, sequence] {
				if (context == expected && sequence == mutationGeneration)
					nextDestination();
			});
			return;
		}
		// request содержит только enabled и исходную точную revision, без секретов и иных отношений.
		api::DestinationPatch request;
		request.expectedRevision = model.destinationRevisions.value(id);
		request.enabled = desired;
		destinationBusyId = id;
		// guard запрещает отправку после синхронной смены контекста из уведомления эпохи.
		const QPointer<ConfigurationController> guard = &owner;
		state.invalidateConfiguration();
		if (!guard || sequence != mutationGeneration || context != expected || !destinationBatch ||
		    !state.ownsConfigurationWrite(writeOrigin, QStringLiteral("Configuration"), writeToken))
			return;
		// Callback сохраняет индивидуальный итог; даже после отказа следующий шаг требует того же контекста.
		api.patchDestination(
			id, request, mutationReceiver,
			[this, id, desired, expected, sequence](auto, api::Result<api::Destination> result) {
				if (stopped || disconnected || context != expected || sequence != mutationGeneration ||
				    !destinationBatch)
					return;
				destinationBusyId.clear();
				// validSuccess проверяет, что DTO действительно подтверждает адрес и требуемый флаг.
				const bool validSuccess = result && result.value->id == id &&
							  result.value->streamId == model.id &&
							  result.value->enabled == desired;
				// alive ограждает и обновление кеша, и последующую публикацию результата.
				const QPointer<ConfigurationController> alive = &owner;
				if (validSuccess) {
					model.destinationChanges.remove(id);
					model.destinationRevisions.remove(id);
					model.destinationOutcomes.insert(id, ConfigurationOutcome::Saved);
				} else {
					// rejected отделяет известный серверный отказ от возможной уже выполненной записи.
					const bool rejected = result.error && (!result.error->requestSent ||
									       mutationRejected(*result.error));
					// error сохраняет исходную причину либо отмечает недостоверный успешный DTO.
					api::Error error;
					if (result.error)
						error = *result.error;
					else {
						error.kind = api::ErrorKind::Contract;
						error.requestSent = true;
					}
					// outcome относится только к этому назначению, не к соседним шагам пакета.
					const auto outcome = result.error && revisionConflict(*result.error)
								     ? ConfigurationOutcome::Conflict
							     : rejected ? ConfigurationOutcome::Rejected
									: ConfigurationOutcome::Unknown;
					model.destinationOutcomes.insert(id, outcome);
					if (result.error && result.error->httpStatus == 404) {
						retireDestinationIntent(id);
						removeDestinationFromCache(id);
					} else if (outcome == ConfigurationOutcome::Unknown ||
						   outcome == ConfigurationOutcome::Conflict) {
						model.destinationReconciliation.insert(id);
						if (outcome == ConfigurationOutcome::Unknown) {
							rememberUnknown(ConfigurationKind::Destination, id);
							if (!alive)
								return;
							// Остальные шаги уже подтверждённого пакета сохраняют прежнее владение канала.
						}
						// GET, начатый до этого результата, ещё мог видеть прежнее enabled.
						state.invalidateConfiguration();
					}
					if (!alive || context != expected || sequence != mutationGeneration)
						return;
					if (!notifyFailure(error, id))
						return;
				}
				if (validSuccess)
					cache(*result.value);
				if (!alive || context != expected || sequence != mutationGeneration)
					return;
				state.invalidateConfiguration();
				if (!alive || context != expected || sequence != mutationGeneration)
					return;
				publish();
				if (alive && context == expected && sequence == mutationGeneration)
					QTimer::singleShot(0, &owner, [this, expected, sequence] {
						if (context == expected && sequence == mutationGeneration)
							nextDestination();
					});
			});
	}

	// Читает спорные назначения перед повтором, сохраняя желаемый enabled и индивидуальный прежний исход.
	bool reconcileDestinations()
	{
		model.loading = true;
		destinationQueue = model.destinationReconciliation.values();
		++detailGeneration;
		// guard защищает запуск первого GET после уведомления формы.
		const QPointer<ConfigurationController> guard = &owner;
		publish();
		if (guard)
			nextReconciliation();
		return true;
	}

	// Сверяет очередной точный ID только новым GET текущей эпохи конфигурации.
	// Ошибка чтения сохраняет запрет записи; успешная сверка не подтверждает прежний PATCH.
	void nextReconciliation()
	{
		if (model.context != context)
			return;
		if (!available()) {
			destinationQueue.clear();
			model.loading = false;
			publish();
			return;
		}
		if (destinationQueue.isEmpty()) {
			model.loading = false;
			model.destinationMessageKey = model.destinationReconciliation.isEmpty()
							      ? QStringLiteral("Configuration.DestinationsPartial")
							      : QStringLiteral("Configuration.DestinationsReconcile");
			// guard защищает уведомления; expected, sequence и epoch фиксируют завершённую сверку.
			const QPointer<ConfigurationController> guard = &owner;
			const auto expected = context;
			const auto sequence = detailGeneration;
			const auto epoch = state.configurationGeneration();
			deferReconciliation();
			if (!guard || sequence != detailGeneration || !current(expected, epoch))
				return;
			publish();
			if (!guard || model.requiresReconciliation() || sequence != detailGeneration ||
			    !current(expected, epoch))
				return;
			connection.refreshState();
			if (guard && sequence == detailGeneration && current(expected, epoch))
				restream.refreshFallbackAfterConfiguration();
			return;
		}
		// id адресует назначение, expected и sequence ограждают карточку, epoch исключает старые чтения.
		const auto id = destinationQueue.takeFirst();
		const auto expected = context;
		const auto sequence = detailGeneration;
		const auto epoch = state.configurationGeneration();
		// Callback обновляет ревизию и DTO доступного назначения, не меняя прежний исход PATCH.
		// Подтверждённое удаление либо перенос в другой Stream снимает намерение как Rejected.
		api.getDestination(
			id, &owner, [this, id, expected, sequence, epoch](auto, api::Result<api::Destination> result) {
				if (stopped || disconnected || context != expected || sequence != detailGeneration)
					return;
				if (!current(expected, epoch)) {
					destinationQueue.clear();
					model.loading = false;
					publish();
					return;
				}
				// alive защищает продолжение после уведомления кеша; результат не меняет исход PATCH на Saved.
				const QPointer<ConfigurationController> alive = &owner;
				// reconciled требует точный DTO либо адресное подтверждение отсутствия; ошибка не снимает барьер.
				const bool reconciled = (result && result.value->id == id) ||
							(!result && result.error && result.error->httpStatus == 404);
				if (!result && result.error && result.error->httpStatus == 404) {
					retireDestinationIntent(id);
					removeDestinationFromCache(id);
				} else if (result && result.value->id == id && result.value->streamId == model.id) {
					model.destinationReconciliation.remove(id);
					model.destinationRevisions.insert(id, result.value->revision);
					if (model.destinationChanges.contains(id) &&
					    model.destinationChanges.value(id) == result.value->enabled) {
						model.destinationChanges.remove(id);
						model.destinationRevisions.remove(id);
					}
					cache(*result.value);
				} else if (result && result.value->id == id) {
					retireDestinationIntent(id);
					cache(*result.value);
				}
				// Следующий GET не запускается после смены карточки, контекста либо эпохи из сигнала кеша.
				if (!alive || context != expected || sequence != detailGeneration)
					return;
				if (!current(expected, epoch)) {
					destinationQueue.clear();
					model.loading = false;
					publish();
					return;
				}
				if (reconciled) {
					settleUnknown(ConfigurationKind::Destination, id);
					if (!alive || sequence != detailGeneration || !current(expected, epoch))
						return;
					state.invalidateConfiguration();
					if (!alive || sequence != detailGeneration || !current(expected, epoch + 1))
						return;
				}
				// Отложенный шаг проверяет прежнюю карточку; собственная инвалидация уже завершена.
				QTimer::singleShot(0, &owner, [this, expected, sequence] {
					if (context == expected && sequence == detailGeneration)
						nextReconciliation();
				});
			});
	}

	// Формирует PATCH только по явным полям; skipDestinations продолжает обычные поля после завершения пакета.
	bool save(bool skipDestinations = false)
	{
		if (!available() || !state.configurationSettled(context.origin) || unknownWrites.size() >= 64 ||
		    model.context != context || model.writing || model.loading || !model.draft ||
		    model.requiresReconciliation() || model.serverConflict || (!model.creating && !model.base))
			return false;
		if (!skipDestinations && model.kind == ConfigurationKind::Stream && !model.creating &&
		    !model.destinationChanges.isEmpty())
			return saveDestinations();
		if (!model.creating && model.dirtyFields.isEmpty())
			return false;
		// revision берётся из исходного подтверждённого документа, а не из пользовательского draft.
		const auto revision = model.base ? record(*model.base).revision : 0;
		// fields содержит явные намерения частичного PATCH.
		const auto &fields = model.dirtyFields;
		switch (model.kind) {
		case ConfigurationKind::Stream: {
			// value является полной текущей формой Stream.
			const auto value = std::get<api::Stream>(*model.draft);
			if (model.creating) {
				if (!value.publicationKey)
					return false;
				// request содержит все обязательные поля создания, включая точный ключ.
				const api::StreamCreate request{value.name, *value.publicationKey};
				return submit<api::Stream>(request, [this, request](auto cb) {
					api.createStream(request, mutationReceiver, cb);
				});
			}
			// request не имеет publication_key: существующий ключ не может случайно попасть в PATCH.
			api::StreamPatch request;
			request.expectedRevision = revision;
			if (fields.contains(QStringLiteral("name")))
				request.name = value.name;
			return submit<api::Stream>(request, [this, request](auto cb) {
				api.patchStream(model.id, request, mutationReceiver, cb);
			});
		}
		case ConfigurationKind::Destination: {
			// value сохраняет полные исходные строки endpoint/streamKey.
			const auto value = std::get<api::Destination>(*model.draft);
			if (model.creating) {
				if (!value.endpoint || !value.streamKey)
					return false;
				// request фиксирует полное создание назначения.
				const api::DestinationCreate request{value.streamId, value.groupId,   value.name,
								     value.platform, *value.endpoint, *value.streamKey,
								     false};
				return submit<api::Destination>(request, [this, request](auto cb) {
					api.createDestination(request, mutationReceiver, cb);
				});
			}
			// request оставляет неизменённые секретные поля отсутствующими.
			api::DestinationPatch request;
			request.expectedRevision = revision;
			if (fields.contains(QStringLiteral("name")))
				request.name = value.name;
			if (fields.contains(QStringLiteral("stream_id")))
				request.streamId = value.streamId;
			if (fields.contains(QStringLiteral("group_id")))
				request.groupId = value.groupId;
			if (fields.contains(QStringLiteral("platform")))
				request.platform = value.platform;
			if (fields.contains(QStringLiteral("enabled")))
				request.enabled = value.enabled;
			if (fields.contains(QStringLiteral("endpoint"))) {
				if (!value.endpoint)
					return false;
				request.endpoint = *value.endpoint;
			}
			if (fields.contains(QStringLiteral("stream_key"))) {
				if (!value.streamKey)
					return false;
				request.streamKey = *value.streamKey;
			}
			return submit<api::Destination>(request, [this, request](auto cb) {
				api.patchDestination(model.id, request, mutationReceiver, cb);
			});
		}
		case ConfigurationKind::Group: {
			// value содержит актуальный ввод связей Group.
			const auto value = std::get<api::Group>(*model.draft);
			if (model.creating) {
				// request фиксирует обязательные связи нового Group.
				const api::GroupCreate request{value.streamId, value.profileId, value.name};
				return submit<api::Group>(request, [this, request](auto cb) {
					api.createGroup(request, mutationReceiver, cb);
				});
			}
			// request содержит лишь явно изменённые связи и имя.
			api::GroupPatch request;
			request.expectedRevision = revision;
			if (fields.contains(QStringLiteral("name")))
				request.name = value.name;
			if (fields.contains(QStringLiteral("stream_id")))
				request.streamId = value.streamId;
			if (fields.contains(QStringLiteral("profile_id")))
				request.profileId = value.profileId;
			return submit<api::Group>(request, [this, request](auto cb) {
				api.patchGroup(model.id, request, mutationReceiver, cb);
			});
		}
		case ConfigurationKind::Profile: {
			// value содержит полный профиль с исходными байтами незатронутых полей.
			const auto value = std::get<api::Profile>(*model.draft);
			if (!model.creating && fields.contains(QStringLiteral("profile"))) {
				// base сохраняет признаки материализованного нового профиля при полной замене.
				const auto &base = std::get<api::Profile>(*model.base).profile;
				if ((base.liveAudioMode.hasValue() && !value.profile.liveAudioMode.hasValue()) ||
				    (base.encoding.hasValue() && !value.profile.encoding.hasValue()) ||
				    (base.liveAudioMode.isMissing() && value.profile.liveAudioMode.isMissing() &&
				     base.mode != value.profile.mode)) {
					model.messageKey = QStringLiteral("Configuration.InvalidField");
					model.outcome = ConfigurationOutcome::Rejected;
					publish();
					return false;
				}
			}
			if (model.creating) {
				// request создаёт полный именованный профиль.
				const api::ProfileCreate request{value.name, value.profile};
				return submit<api::Profile>(request, [this, request](auto cb) {
					api.createProfile(request, mutationReceiver, cb);
				});
			}
			// request не отправляет profile при одном лишь переименовании.
			api::ProfilePatch request;
			request.expectedRevision = revision;
			if (fields.contains(QStringLiteral("name")))
				request.name = value.name;
			if (fields.contains(QStringLiteral("profile")))
				request.profile = value.profile;
			return submit<api::Profile>(request, [this, request](auto cb) {
				api.patchProfile(model.id, request, mutationReceiver, cb);
			});
		}
		}
		return false;
	}

	// После 409 принимает только явно разрешённую новую форму, не выполняя автоматической записи.
	bool confirmConflict(ConfigurationEntity value, QSet<QString> fields)
	{
		if (!available() || model.writing || model.loading || model.context != context ||
		    !model.serverConflict || !matches(model.kind, value))
			return false;
		// allowed ограничивает явное разрешение конфликта публичными изменяемыми полями.
		const auto allowed = editableFields(model.kind);
		// field адресует одно намерение новой формы после сравнения.
		for (const auto &field : fields)
			if (!allowed.contains(field))
				return false;
		if (model.kind == ConfigurationKind::Profile && fields.contains(QStringLiteral("profile"))) {
			// server содержит свежий рецепт; resolved — явно выбранный целый рецепт пользователя.
			const auto &server = std::get<api::Profile>(*model.serverConflict).profile;
			const auto &resolved = std::get<api::Profile>(value).profile;
			if ((server.liveAudioMode.hasValue() && !resolved.liveAudioMode.hasValue()) ||
			    (server.encoding.hasValue() && !resolved.encoding.hasValue())) {
				model.messageKey = QStringLiteral("Configuration.InvalidField");
				publish();
				return false;
			}
		}
		model.base = model.serverConflict;
		model.serverConflict.reset();
		model.draft = std::move(value);
		model.dirtyFields = std::move(fields);
		model.outcome = ConfigurationOutcome::None;
		model.messageKey.clear();
		publish();
		return true;
	}

	// Удаляет запись по точной исходной ревизии, без каскадных операций и автоматического выбора.
	bool remove()
	{
		if (!available() || !state.configurationSettled(context.origin) || unknownWrites.size() >= 64 ||
		    model.context != context || model.writing || model.loading || model.creating || !model.base ||
		    model.serverConflict || model.requiresReconciliation())
			return false;
		// revision задаёт серверное предусловие, id адресует запись, kind выбирает коллекцию удаления.
		const auto revision = record(*model.base).revision;
		const auto id = model.id;
		const auto kind = model.kind;
		// expected ограждает результат от смены сервера/UUID/рабочего Stream.
		const auto expected = context;
		if (!acquireWrite())
			return false;
		// sequence отличает это удаление от следующего намерения.
		const auto sequence = mutationGeneration;
		model.writing = true;
		model.outcome = ConfigurationOutcome::None;
		model.messageKey.clear();
		model.fieldIssues.clear();
		// guard защищает дальнейшую работу после синхронных сигналов.
		const QPointer<ConfigurationController> guard = &owner;
		state.invalidateConfiguration();
		if (!guard || sequence != mutationGeneration)
			return false;
		publish();
		if (!guard || sequence != mutationGeneration)
			return false;
		// callback подтверждает лишь успешный 204 и удаляет только исходную запись кеша.
		auto callback = [this, id, kind, expected, sequence](auto, api::Result<api::Empty> result) {
			if (stopped || context != expected || sequence != mutationGeneration)
				return;
			if (!result) {
				failed(*result.error);
				return;
			}
			// next содержит последние данные всех прочих коллекций.
			auto next = state.configuration();
			switch (kind) {
			case ConfigurationKind::Stream:
				next.streams.remove(id);
				break;
			case ConfigurationKind::Destination:
				next.destinations.remove(id);
				break;
			case ConfigurationKind::Group:
				next.groups.remove(id);
				break;
			case ConfigurationKind::Profile:
				next.profiles.remove(id);
				break;
			}
			model.writing = false;
			model.base.reset();
			model.draft.reset();
			model.dirtyFields.clear();
			model.outcome = ConfigurationOutcome::Deleted;
			model.messageKey = QStringLiteral("Configuration.Deleted");
			// writeGuard защищает завершение после освобождения общего канала.
			const QPointer<ConfigurationController> writeGuard = &owner;
			releaseWrite();
			if (!writeGuard)
				return;
			// guard отслеживает время жизни во время уведомлений кеша/выбора.
			const QPointer<ConfigurationController> guard = &owner;
			state.invalidateConfiguration();
			if (!guard || context != expected || mutationGeneration != sequence)
				return;
			state.setConfiguration(std::move(next));
			if (!guard || context != expected || mutationGeneration != sequence)
				return;
			publish();
			if (!guard || context != expected || mutationGeneration != sequence)
				return;
			if (kind == ConfigurationKind::Stream && state.selectedStreamId() == id)
				connection.selectStream({});
			if (!guard)
				return;
			refresh(kind);
			if (!guard)
				return;
			connection.refreshState();
			if (!guard)
				return;
			restream.refreshFallbackAfterConfiguration();
		};
		if (!available() || context != expected ||
		    !state.ownsConfigurationWrite(writeOrigin, QStringLiteral("Configuration"), writeToken))
			return false;
		switch (kind) {
		case ConfigurationKind::Stream:
			api.deleteStream(id, revision, mutationReceiver, callback);
			break;
		case ConfigurationKind::Destination:
			api.deleteDestination(id, revision, mutationReceiver, callback);
			break;
		case ConfigurationKind::Group:
			api.deleteGroup(id, revision, mutationReceiver, callback);
			break;
		case ConfigurationKind::Profile:
			api.deleteProfile(id, revision, mutationReceiver, callback);
			break;
		}
		return true;
	}
};

// Создаёт независимый координатор конфигурации без изменения рабочего выбора.
ConfigurationController::ConfigurationController(StateStore &state, api::APIClient &api,
						 ConnectionController &connection, RestreamController &restream,
						 QObject *parent)
	: QObject(parent),
	  impl_(std::make_unique<Impl>(*this, state, api, connection, restream))
{
}
// Отменяет сетевые ожидания перед освобождением реализации.
ConfigurationController::~ConfigurationController()
{
	shutdown();
}
// Возвращает прежний адрес также во время channelClosing.
ConfigurationContext ConfigurationController::channelContext() const
{
	return impl_->context;
}
// Возвращает копию текущего транспортного поколения.
api::RequestContext ConfigurationController::channelRequestContext() const
{
	return impl_->apiContext;
}
// Указывает, что configure уже завершена и глобальная отмена не выполняется.
bool ConfigurationController::channelIsReady() const
{
	return impl_->channelReady && !impl_->stopped && !impl_->changingChannel;
}
// Запускает типизированное чтение выбранной коллекции.
void ConfigurationController::refresh(ConfigurationKind kind)
{
	impl_->refresh(kind);
}
// Открывает адресованную карточку с явным решением о dirty draft.
bool ConfigurationController::open(ConfigurationKind kind, QString id, bool discardDirty)
{
	return impl_->open(kind, std::move(id), discardDirty);
}
// Начинает локальную форму создания.
bool ConfigurationController::beginCreate(ConfigurationKind kind, bool discardDirty)
{
	return impl_->beginCreate(kind, discardDirty);
}
// Заменяет пользовательский ввод без HTTP.
bool ConfigurationController::setDraft(ConfigurationEntity draft, QSet<QString> fields)
{
	return impl_->setDraft(std::move(draft), std::move(fields));
}
// Возвращает точное поколение формы, которое пользователь собирается подтвердить.
quint64 ConfigurationController::editorGeneration() const
{
	return impl_->editorGeneration;
}
// Отбрасывает лишь подтверждённый ввод в RAM, не отправляя конфигурационную запись.
bool ConfigurationController::discardDraft(quint64 expectedGeneration)
{
	return impl_->discardDraft(expectedGeneration);
}
// Паркует ввод в RAM, не меняя серверную конфигурацию.
bool ConfigurationController::parkDraft()
{
	return impl_->parkDraft();
}
// Возвращает карточку только из RAM нынешнего контекста.
bool ConfigurationController::restoreDraft(ConfigurationKind kind, const QString &id)
{
	return impl_->restoreDraft(kind, id);
}
// Фиксирует участие назначения в следующем эфире без runtime-команды.
bool ConfigurationController::setDestinationEnabled(const QString &id, bool enabled)
{
	return impl_->setDestinationEnabled(id, enabled);
}
// Передаёт одно новое явное намерение create/PATCH.
bool ConfigurationController::save()
{
	return impl_->save();
}
// Передаёт одно новое явное намерение удаления.
bool ConfigurationController::remove()
{
	return impl_->remove();
}
// Сверяет текущее серверное содержимое, сохраняя dirty draft.
bool ConfigurationController::reload()
{
	return impl_->reload();
}
// Принимает явно разрешённый пользователем результат сравнения.
bool ConfigurationController::confirmConflict(ConfigurationEntity draft, QSet<QString> fields)
{
	return impl_->confirmConflict(std::move(draft), std::move(fields));
}
// Необратимо завершает независимый канал конфигурации.
void ConfigurationController::shutdown()
{
	if (!impl_->stopped)
		impl_->cancel(true);
}
} // namespace gost
