#include "restream-controller.hpp"

#include "api/api-client.hpp"
#include "api/profile-rules.hpp"
#include "core/connection-controller.hpp"
#include "core/output-operations.hpp"
#include "core/state-store.hpp"
#include "obs/obs-controller.hpp"

#include <QPointer>
#include <QSet>
#include <QThread>
#include <QTimer>
#include <QUuid>

#include <algorithm>
#include <tuple>
#include <utility>

namespace gost {
namespace {

// Проверяет непрозрачную идентичность до включения в адресуемый шаг.
bool validId(const QString &id)
{
	return !id.isEmpty() && id.size() <= 4096 && !id.contains(QChar::Null);
}

// Только эти состояния обозначают продолжающуюся или ещё не очищенную Session.
bool activeSession(const QString &state)
{
	return state == QStringLiteral("STARTING") || state == QStringLiteral("RUNNING") ||
	       state == QStringLiteral("STOPPING");
}

// Возвращает точное backend kind для разрешённой команды; None не может попасть в POST.
QString commandKind(OperationAction action)
{
	switch (action) {
	case OperationAction::StartFallback:
	case OperationAction::StartDestination:
		return QStringLiteral("start_session");
	case OperationAction::AddOutput:
		return QStringLiteral("add_output");
	case OperationAction::Pause:
		return QStringLiteral("pause_session");
	case OperationAction::Resume:
		return QStringLiteral("resume_session");
	case OperationAction::StopAll:
		return QStringLiteral("stop_session");
	case OperationAction::StartOutput:
		return QStringLiteral("start_output");
	case OperationAction::StopOutput:
		return QStringLiteral("stop_output");
	case OperationAction::DetachOutput:
		return QStringLiteral("detach_output");
	default:
		return {};
	}
}

} // namespace

/**
 * Ведёт один workflow в RAM и один живой шаг. stepToken меняется до отмены HTTP; каждый обработчик
 * дополнительно проверяет исходную тройку выбора. publish может синхронно удалить owner,
 * поэтому возвращает разрешение продолжать и вызывается последним перед выходом либо проверяется.
 */
class RestreamController::Impl {
public:
	// Сохраняет заимствованные сервисы и подключает только наблюдение; POST в конструкторе отсутствует.
	Impl(RestreamController &owner, StateStore &state, api::APIClient &api, ConnectionController &connection,
	     OBSController &obs, Limits limits)
		: owner(owner),
		  state(state),
		  api(api),
		  connection(connection),
		  obs(obs),
		  limits(limits)
	{
		deadline.setSingleShot(true);
		poll.setSingleShot(true);
		// Изменения наблюдений прерывают только новые шаги; известную квитанцию можно дочитать без SSE.
		QObject::connect(&state, &StateStore::serverStateChanged, &owner, [this] { observeState(); });
		// Конфигурационная запись отзывает Ready, но не отменяет уже принятые команды.
		QObject::connect(&state, &StateStore::configurationInvalidated, &owner,
				 [this](quint64) { invalidateFallback(); });
		// Освобождение общего барьера возобновляет одну ранее отложенную проверку.
		QObject::connect(&state, &StateStore::configurationActivityChanged, &owner, [this] {
			if (deferredPrecheck && configurationSettled())
				refreshFallbackAfterConfiguration();
		});
		// Явный disconnect отменяет command API, в отличие от терминального HTTP 204 подписки.
		QObject::connect(&connection, &ConnectionController::explicitlyDisconnected, &owner,
				 [this] { disconnectExplicitly(); });
		observeState();
	}

	// Неизменяемое намерение одного POST; при ручном повторе изменяется лишь бюджет ожидания.
	struct LiveStep {
		// action задаёт тип команды одного шага.
		OperationAction action = OperationAction::None;
		// sessionId адресует исходную Session, не текущий выбор UI.
		QString sessionId;
		// outputId адресует выбранный Output. При StartDestination он закрепляется после полного
		// чтения Outputs собственной Session; у остальных команд Session остаётся пустым.
		QString outputId;
		// requestId остаётся прежним при явном повторе неоднозначного POST.
		QString requestId;
		// destinations содержит фиксированные IDs для Session start; пуст у других команд.
		QStringList destinations;
		// commandId заполняется только согласованной квитанцией.
		QString commandId;
		// resultIndex связывает шаг с результатом Output; -1 обозначает действие Session.
		qsizetype resultIndex = -1;
		// sent отделяет неотправленное намерение от возможного принятия сервером.
		bool sent = false;
		// destinationId фиксирует назначение для add-output либо singleton-запуска новой Session.
		// Оно связывает результат выбранного запуска с Destination; Output имеет отдельный ID.
		QString destinationId;
		// failureNotified запрещает повторный popup при повторном чтении той же терминальной квитанции.
		bool failureNotified = false;
		// Эпоха исходной конфигурации ограничивает только первое add-output, не повтор прежнего payload.
		quint64 configurationGeneration = 0;
	};

	// Приватное доказательство пригодности ровно одного выбранного назначения; общий precheck не изменяется.
	struct SelectedStartProof {
		// configurationGeneration фиксирует завершённую локальную конфигурацию до первого POST.
		quint64 configurationGeneration = 0;
		// profile содержит полный рецепт выбранной Group для сравнения с текущим вариантом.
		api::GroupProfile profile;
		// assetId адресует Asset политики выбранного Stream.
		QString assetId;
		// generationId адресует опубликованное current, без подмены pending.
		QString generationId;
	};

	// owner ограничивает callbacks временем жизни публичного фасада.
	RestreamController &owner;
	// state принимает value-результаты и предоставляет наблюдаемый исходный выбор.
	StateStore &state;
	// api является отдельным заимствованным клиентом команд с очередью 1/8.
	api::APIClient &api;
	// connection выполняет объединённое обновление наблюдаемого состояния после команды.
	ConnectionController &connection;
	// obs подтверждает остановку исходного запуска штатного output.
	OBSController &obs;
	// limits задаёт конечные сроки исполнителя.
	Limits limits;
	// deadline включает очередь HTTP и весь оракул одного шага.
	QTimer deadline;
	// poll задаёт паузу между чтениями квитанции/состояния.
	QTimer poll;
	// context задаёт единственную логическую конфигурацию command API, независимую от SSE recovery.
	DockContext context;
	// apiContext меняется при новом логическом выборе, но не при обычном обрыве подписки.
	api::RequestContext apiContext;
	// model копируется в StateStore только после завершения локальных изменений.
	OperationsState model;
	// generation ограждает callbacks предыдущего намерения.
	quint64 generation = 0;
	// stepToken отличает строки и попытки внутри одного workflow; меняется до отмены прежнего шага.
	quint64 stepToken = 0;
	// precheckRevision ограждает показанный в диалоге набор назначений.
	quint64 precheckRevision = 0;
	// stopped необратимо запрещает действия при shutdown.
	bool stopped = false;
	// busy запрещает параллельные намерения, кроме явного Stop All.
	bool busy = false;
	// explicitDisconnect отменяет старый транспорт по явному действию пользователя.
	bool explicitDisconnect = false;
	// observedStatus различает восстановление после disconnect и сигналы ещё не изменённого Connected.
	ConnectionStatus observedStatus = ConnectionStatus::Disconnected;
	// stopNewSteps не сбрасывается восстановлением SSE или ручным повтором.
	bool stopNewSteps = false;
	// replacementSessionObserved отдельно запрещает общий успех Stop All при обнаружении следующего эфира.
	bool replacementSessionObserved = false;
	// live удерживает единственный активный шаг; неизвестные части пакетных строк сохраняются отдельно.
	std::optional<LiveStep> live;
	// plan задаёт последовательность без собственного таймера/транспорта.
	QVector<OutputOperations::Step> plan;
	// nextStep указывает следующий ещё не начатый шаг фиксированного плана.
	qsizetype nextStep = 0;
	// variants относятся только к точному текущему поколению предварительной проверки.
	QVector<api::Variant> variants;
	// destinations временно сохраняет прочитанные DTO для проверки Group; в UI передаются лишь ID/имя.
	QVector<api::Destination> destinations;
	// destinationIndex задаёт последовательное чтение точных Group/Profile для предварительной проверки.
	qsizetype destinationIndex = 0;
	// selectedStart существует только внутри намерения запуска одного Destination.
	std::optional<SelectedStartProof> selectedStart;
	// Сохраняет ровно один неоднозначный отправленный шаг строки вместе с собственной квитанцией.
	struct SavedUnknown {
		// step удерживает исходную команду, её параметры, request/command IDs и закреплённый Output.
		LiveStep step;
		// receipt сохраняет подтверждённую квитанцию отдельно от ещё не подтверждённого состояния.
		std::optional<api::Receipt> receipt;
	};
	// batch существует только для нового пакета показанных строк; старые одиночные планы не меняются.
	std::optional<OutputBatchRequest> batch;
	// batchUnknown ограничен исходным числом строк и не записывается на диск.
	QVector<std::optional<SavedUnknown>> batchUnknown;
	// batchNextIndex указывает следующую ещё не рассмотренную исходную строку.
	qsizetype batchNextIndex = 0;
	// batchOwnSession отдельно подтверждает собственную Session до оракула её первого Output.
	bool batchOwnSession = false;
	// batchBootstrapSent запрещает вторую попытку создать Session после отправки singleton.
	bool batchBootstrapSent = false;
	// batchRetryOnly запрещает явному повтору запускать зависимый start либо оставшиеся строки.
	bool batchRetryOnly = false;
	// Одна отложенная проверка объединяет несколько успешных конфигурационных изменений.
	bool deferredPrecheck = false;

	// Не допускает проверку пригодности до завершения записи и сверки её неизвестного результата.
	bool configurationSettled() const { return state.configurationSettled(context.origin); }

	// Снимает пригодность прежнего precheck немедленно, сохраняя текущий workflow.
	void invalidateFallback()
	{
		if (stopped)
			return;
		model.fallback.readiness = FallbackReadiness::Unknown;
		model.fallback.messageKey = QStringLiteral("Fallback.Incomplete");
		++precheckRevision;
		publish();
	}

	// Планирует одну проверку после освобождения исполнителя, без отмены живого шага.
	void refreshFallbackAfterConfiguration()
	{
		if (stopped)
			return;
		deferredPrecheck = true;
		if (busy)
			return;
		// Отложенный обработчик объединяет запросы обновления в одном обороте GUI.
		QTimer::singleShot(0, &owner, [this] {
			if (stopped || busy || !deferredPrecheck || !available(context) || !configurationSettled())
				return;
			// Сохранённый неизвестный шаг ещё нужен явному повтору; автоматическая проверка не стирает его payload.
			if (model.current && model.current->outcome == OperationOutcome::Unknown &&
			    model.current->retryAvailable)
				return;
			deferredPrecheck = false;
			checkFallback(context);
		});
	}

	// Проверяет stepToken и логический выбор; статус SSE здесь намеренно не участвует.
	bool current(quint64 expected) const
	{
		return !stopped && stepToken == expected && model.current && model.current->context == context &&
		       context == state.dockContext() && !explicitDisconnect;
	}

	// Публикует копию до синхронного сигнала и сообщает, пережил ли этот вызов исходный workflow.
	bool publish()
	{
		// guard обнаруживает уничтожение owner, expected — замену операции из обработчика сигнала.
		const QPointer<RestreamController> guard = &owner;
		const auto expected = generation;
		state.setOperations(model);
		return guard && !stopped && generation == expected;
	}

	// Допускает новое намерение только для точного связанного выбора и подтверждённой подписки.
	bool available(const DockContext &expected) const
	{
		return !stopped && !busy && expected.valid() && expected == context &&
		       expected == state.dockContext() && !explicitDisconnect &&
		       state.connectionStatus() == ConnectionStatus::Connected && state.selectionAvailable();
	}

	// Проверяет свежий однозначный Session ID перед фиксацией действия пользователя.
	bool sessionTarget(const QString &id, bool runningOnly) const
	{
		// snapshot заимствован до возврата; здесь нет сигналов или асинхронных вызовов.
		const auto &snapshot = state.dockServer();
		return validId(id) && snapshot.sessionStatus.availability == RuntimeAvailability::Fresh &&
		       snapshot.sessionStatus.complete && snapshot.session && snapshot.session->id == id &&
		       (runningOnly ? snapshot.session->state == QStringLiteral("RUNNING")
				    : activeSession(snapshot.session->state));
	}

	// Заполняет текущий результат до первого сигнала, сохраняя один предыдущий неизвестный результат.
	void begin(OperationAction action, QString sessionId = {})
	{
		if (model.current && model.current->outcome == OperationOutcome::Unknown) {
			model.lastInterrupted = model.current;
			model.lastInterrupted->retryAvailable = false;
		}
		++generation;
		++stepToken;
		batch.reset();
		batchUnknown.clear();
		batchNextIndex = 0;
		batchOwnSession = false;
		batchBootstrapSent = false;
		batchRetryOnly = false;
		busy = true;
		stopNewSteps = false;
		replacementSessionObserved = false;
		live.reset();
		selectedStart.reset();
		plan.clear();
		nextStep = 0;
		model.current = WorkflowResult{};
		model.current->generation = generation;
		model.current->context = context;
		model.current->action = action;
		model.current->sessionId = std::move(sessionId);
		model.current->outcome = OperationOutcome::Pending;
	}

	// Сохраняет вытесненный результат без заявления об отмене уже принятой сервером команды.
	void archiveInterrupted()
	{
		if (!model.current || (!busy && model.current->outcome != OperationOutcome::Unknown))
			return;
		if (busy) {
			// Отменённые чтения больше не завершают проверку: интерфейс сразу теряет признак Checking.
			if (model.current->action == OperationAction::CheckFallback &&
			    model.fallback.readiness == FallbackReadiness::Checking) {
				model.fallback.readiness = FallbackReadiness::Unknown;
				model.fallback.messageKey = QStringLiteral("Fallback.Incomplete");
			}
			model.current->phase = OperationPhase::Finished;
			model.current->outcome = OperationOutcome::Unknown;
			model.current->remainingStopped = true;
			model.current->retryAvailable = false;
			if (live && live->resultIndex >= 0 &&
			    model.current->outputs[live->resultIndex].outcome == OperationOutcome::Pending) {
				model.current->outputs[live->resultIndex].outcome =
					live->sent ? OperationOutcome::Unknown : OperationOutcome::NotSent;
				if (batch)
					recordBatchPart(live->sent ? OperationOutcome::Unknown
								   : OperationOutcome::NotSent);
			}
			if (batch) {
				model.current->messageKey = QStringLiteral("Operation.RemainingStopped");
				// row сохраняет известный успех; только незавершённые строки получают причину прерывания.
				for (auto &row : model.current->outputs)
					if (row.outcome != OperationOutcome::Completed && row.messageKey.isEmpty())
						row.messageKey = QStringLiteral("Operation.RemainingStopped");
			}
		}
		model.lastInterrupted = model.current;
		model.lastInterrupted->retryAvailable = false;
	}

	// Обрабатывает смену тройки и потерю связи. Восстановление SSE не возобновляет остановленный пакет.
	void observeState()
	{
		if (stopped)
			return;
		// reconnected устанавливается только при переходе статуса, а не при промежуточном stale-сигнале.
		const bool reconnected = observedStatus != ConnectionStatus::Connected &&
					 state.connectionStatus() == ConnectionStatus::Connected;
		observedStatus = state.connectionStatus();
		if (context != state.dockContext()) {
			deferredPrecheck = false;
			archiveInterrupted();
			++generation;
			++stepToken;
			batch.reset();
			batchUnknown.clear();
			busy = false;
			live.reset();
			plan.clear();
			deadline.stop();
			poll.stop();
			context = state.dockContext();
			model.current.reset();
			model.fallback = {};
			explicitDisconnect = false;
			++apiContext.selection;
			api.configure(QUrl(context.origin), apiContext);
			publish();
			return;
		}
		if (explicitDisconnect && reconnected)
			explicitDisconnect = false;
		if (state.connectionStatus() != ConnectionStatus::Connected &&
		    model.fallback.readiness == FallbackReadiness::Ready) {
			model.fallback.readiness = FallbackReadiness::Unknown;
			model.fallback.messageKey = QStringLiteral("Fallback.Incomplete");
			if (!publish())
				return;
		}
		if (!busy || !model.current) {
			if (deferredPrecheck && reconnected)
				refreshFallbackAfterConfiguration();
			return;
		}
		// replacement фиксирует именно следующую Session, а не временную потерю SSE при прежнем эфире.
		const bool replacement =
			state.dockServer().sessionStatus.availability == RuntimeAvailability::Fresh &&
			state.dockServer().session && !model.current->sessionId.isEmpty() &&
			state.dockServer().session->id != model.current->sessionId &&
			(!batch || !batch->noActiveSession || activeSession(state.dockServer().session->state));
		replacementSessionObserved = replacementSessionObserved || replacement;
		// Смена Session запрещает следующие шаги; квитанция команды и исходное состояние остаются читаемыми.
		if (state.connectionStatus() != ConnectionStatus::Connected ||
		    (state.dockServer().sessionStatus.availability == RuntimeAvailability::Fresh &&
		     !model.current->sessionId.isEmpty() &&
		     ((state.dockServer().session && state.dockServer().session->id != model.current->sessionId &&
		       (!batch || !batch->noActiveSession || activeSession(state.dockServer().session->state))) ||
		      (batch && state.dockServer().session &&
		       state.dockServer().session->id == model.current->sessionId &&
		       state.dockServer().session->state != QStringLiteral("RUNNING") &&
		       !(batch->noActiveSession && state.dockServer().session->state == QStringLiteral("STARTING"))) ||
		      (!state.dockServer().session && state.dockServer().sessionStatus.complete &&
		       model.current->action != OperationAction::StopAll && !(batch && batch->noActiveSession))))) {
			if (!stopNewSteps) {
				stopNewSteps = true;
				model.current->remainingStopped = true;
				publish();
			}
		}
	}

	// Отменяет RAM workflow только при явном отключении, сохраняя неопределённость исходного результата.
	void disconnectExplicitly()
	{
		if (stopped)
			return;
		explicitDisconnect = true;
		archiveInterrupted();
		++generation;
		++stepToken;
		batchUnknown.clear();
		deadline.stop();
		poll.stop();
		busy = false;
		live.reset();
		plan.clear();
		api.cancelAll();
		if (model.current)
			model.current->retryAvailable = false;
		publish();
	}

	// Завершает один workflow; неизвестный отправленный шаг остаётся доступен только явному повтору.
	void finish(OperationOutcome outcome, QString message = {})
	{
		if (stopped || !busy || !model.current)
			return;
		if (batch) {
			finishBatchStep(outcome, std::move(message));
			return;
		}
		++stepToken;
		deadline.stop();
		poll.stop();
		// Отмена прекращает только локальное ожидание, а не принятую серверную команду.
		api.cancelAll();
		busy = false;
		model.current->phase = OperationPhase::Finished;
		model.current->outcome = outcome;
		model.current->messageKey = std::move(message);
		model.current->retryAvailable = outcome == OperationOutcome::Unknown && live && live->sent &&
						!model.current->stateConfirmed && !explicitDisconnect &&
						context == model.current->context;
		if (live && live->resultIndex >= 0 &&
		    model.current->outputs[live->resultIndex].outcome == OperationOutcome::Pending)
			model.current->outputs[live->resultIndex].outcome = outcome;
		if (model.current->action == OperationAction::StopAll) {
			model.current->serverOutcome = outcome;
			if (outcome == OperationOutcome::Completed &&
			    (model.current->obsOutcome != OperationOutcome::Completed || replacementSessionObserved))
				model.current->outcome = OperationOutcome::Unknown;
		}
		if (model.current->action == OperationAction::CheckFallback &&
		    model.fallback.readiness == FallbackReadiness::Checking) {
			model.fallback.readiness = FallbackReadiness::Unknown;
			model.fallback.messageKey = model.current->messageKey;
		}
		variants.clear();
		destinations.clear();
		// guard ограничивает отложенное обновление временем жизни контроллера.
		const QPointer<RestreamController> guard = &owner;
		publish();
		if (guard && deferredPrecheck)
			refreshFallbackAfterConfiguration();
	}

	// Выставляет новый общий срок до постановки первого HTTP запроса в очередь.
	void armDeadline()
	{
		QObject::disconnect(&deadline, nullptr, &owner, nullptr);
		// expected привязывает timeout к исходному шагу, включая отдельный бюджет ручного повтора.
		const auto expected = stepToken;
		// Обработчик не позволяет старому timeout закончить следующую строку.
		QObject::connect(&deadline, &QTimer::timeout, &owner, [this, expected] {
			if (current(expected) && busy)
				finish(OperationOutcome::Unknown, QStringLiteral("Operation.Timeout"));
		});
		deadline.start(std::max(1, limits.stepTimeoutMs));
	}

	// Планирует только чтение текущего command/state и не создаёт повторной команды.
	void schedulePoll()
	{
		if (!busy || !live)
			return;
		QObject::disconnect(&poll, nullptr, &owner, nullptr);
		// expected не даёт отложенному GET прежней строки прочитать новую команду.
		const auto expected = stepToken;
		// Обработчик выполняет только чтение сохранённого текущего шага.
		QObject::connect(&poll, &QTimer::timeout, &owner, [this, expected] {
			if (current(expected) && busy)
				pollCurrent();
		});
		poll.start(std::max(1, limits.pollIntervalMs));
	}

	// Проверяет точную принадлежность квитанции. У start Session ID узнаётся из первого ответа.
	bool matchingReceipt(const api::Receipt &receipt) const
	{
		if (!live || !validId(receipt.id) || receipt.requestId != live->requestId ||
		    receipt.kind != commandKind(live->action) ||
		    (!live->commandId.isEmpty() && receipt.id != live->commandId))
			return false;
		if (live->action == OperationAction::StartFallback || live->action == OperationAction::StartDestination)
			return receipt.targetId == context.streamId && receipt.outputId.isEmpty() &&
			       (receipt.sessionId.isEmpty() || validId(receipt.sessionId)) &&
			       (live->sessionId.isEmpty() || receipt.sessionId == live->sessionId);
		if (receipt.sessionId != live->sessionId)
			return false;
		if (live->action == OperationAction::AddOutput)
			return receipt.targetId == live->sessionId && validId(receipt.outputId) &&
			       (live->outputId.isEmpty() || receipt.outputId == live->outputId);
		return live->outputId.isEmpty()
			       ? receipt.targetId == live->sessionId && receipt.outputId.isEmpty()
			       : receipt.targetId == live->outputId && receipt.outputId == live->outputId;
	}

	// Публикует отказ только живого шага. После сигнала повторно проверяет контекст и время жизни:
	// обработчик может синхронно завершить плагин либо вытеснить workflow.
	bool notifyFailure(OperationFailure failure)
	{
		if (!busy || !live || live->failureNotified || !current(stepToken))
			return false;
		live->failureNotified = true;
		failure.source = OperationFailureSource::Runtime;
		failure.context = context;
		failure.generation = generation;
		failure.stepIdentity = live->requestId + QLatin1Char('/') + QString::number(stepToken);
		// guard обнаруживает уничтожение фасада, expected — замену шага из обработчика.
		const QPointer<RestreamController> guard = &owner;
		const auto expected = stepToken;
		emit owner.operationFailed(failure);
		return guard && current(expected) && busy;
	}

	// Передаёт доступную причину отказа session через уведомление живого шага.
	// Вызывающий оракул уже проверил принадлежность Session; без failure обработчик
	// уведомления выбирает общее пояснение. Возвращает разрешение продолжить работу после сигнала.
	bool notifySessionFailure(const api::Session &session)
	{
		// failure копирует полученные поля, не создавая отсутствующую причину или диагностическое тело.
		OperationFailure failure;
		if (session.failure) {
			failure.code = session.failure->code;
			failure.reason = session.failure->message;
			failure.action = session.failure->action;
			failure.field = session.failure->field;
			failure.ids = session.failure->context;
			failure.diagnosticBody = session.failure->diagnosticBody;
		}
		return notifyFailure(std::move(failure));
	}

	// Принимает только согласованную квитанцию и разделяет её терминальный статус с оракулом состояния.
	void acceptReceipt(api::Receipt receipt)
	{
		if (!matchingReceipt(receipt)) {
			finish(OperationOutcome::Unknown, QStringLiteral("Operation.ReceiptMismatch"));
			return;
		}
		live->commandId = receipt.id;
		if (live->action == OperationAction::AddOutput) {
			live->outputId = receipt.outputId;
			model.current->outputId = receipt.outputId;
		}
		if ((live->action == OperationAction::StartFallback ||
		     live->action == OperationAction::StartDestination) &&
		    !receipt.sessionId.isEmpty()) {
			live->sessionId = receipt.sessionId;
			model.current->sessionId = receipt.sessionId;
		}
		model.current->commandId = receipt.id;
		// failure переносит диагностику в однократное событие до сохранения value-модели квитанции.
		OperationFailure failure;
		if (receipt.failure) {
			failure.code = receipt.failure->code;
			failure.reason = receipt.failure->message;
			failure.action = receipt.failure->action;
			failure.field = receipt.failure->field;
			failure.ids = receipt.failure->context;
			failure.diagnosticBody = std::move(receipt.failure->diagnosticBody);
		}
		receipt.failure.value.diagnosticBody.clear();
		model.current->receipt = receipt;
		if (receipt.status == QStringLiteral("FAILED") || receipt.status == QStringLiteral("INTERRUPTED")) {
			if (!notifyFailure(std::move(failure)))
				return;
			finish(OperationOutcome::Failed, QStringLiteral("Operation.CommandFailed"));
			return;
		}
		if (receipt.status == QStringLiteral("SUCCEEDED")) {
			if (!validId(live->sessionId)) {
				finish(OperationOutcome::Unknown, QStringLiteral("Operation.ReceiptMismatch"));
				return;
			}
			model.current->phase = OperationPhase::ConfirmingState;
			if (publish())
				readOracle();
			return;
		}
		if (receipt.status != QStringLiteral("PENDING") && receipt.status != QStringLiteral("CLAIMED") &&
		    receipt.status != QStringLiteral("ISSUED")) {
			finish(OperationOutcome::Unknown, QStringLiteral("Operation.ReceiptStatusUnknown"));
			return;
		}
		model.current->phase = OperationPhase::PollingReceipt;
		if (publish())
			schedulePoll();
	}

	// Читает известную квитанцию независимо от состояния SSE; ошибки GET не повторяют POST.
	void pollCurrent()
	{
		if (!busy || !live || stopped)
			return;
		if (model.current->phase == OperationPhase::ConfirmingState) {
			readOracle();
			return;
		}
		if (live->commandId.isEmpty())
			return;
		// expected сохраняет поколение для обработки ответа квитанции.
		const auto expected = stepToken;
		// callback отбрасывает старый workflow, а временная ошибка лишь откладывает следующий GET.
		api.getCommand(live->commandId, &owner, [this, expected](auto, api::Result<api::Receipt> result) {
			if (!current(expected) || !busy)
				return;
			if (!result) {
				schedulePoll();
				return;
			}
			acceptReceipt(std::move(*result.value));
		});
	}

	// Посылает один типизированный POST с уже замороженными ID и payload; автоматических повторов нет.
	void sendPost()
	{
		if (!live || !busy || stopped)
			return;
		if (batch && !live->sent &&
		    (!batchConnectionAllowed() || ((live->action == OperationAction::AddOutput ||
						    live->action == OperationAction::StartDestination) &&
						   !batchConfigurationAllowed()))) {
			finish(OperationOutcome::NotSent, QStringLiteral("Operation.RemainingStopped"));
			return;
		}
		// firstAdd ограничивает новый снимок назначения; явный повтор прежнего payload не получает этот запрет.
		const bool firstAdd = live->action == OperationAction::AddOutput && !live->sent;
		if (firstAdd &&
		    (!configurationSettled() || live->configurationGeneration != state.configurationGeneration())) {
			finish(OperationOutcome::Failed, QStringLiteral("Fallback.Incomplete"));
			return;
		}
		// firstSelected отличает первый POST выбранного назначения от повтора уже отправленного payload.
		const bool firstSelected = live->action == OperationAction::StartDestination && !live->sent;
		if (firstSelected && !selectedStartAllowed()) {
			finish(OperationOutcome::Failed, QStringLiteral("Fallback.Incomplete"));
			return;
		}
		// firstFallback отличает новый запуск от явной сверки прежнего отправленного request_id.
		const bool firstFallback = live->action == OperationAction::StartFallback && !live->sent;
		if (firstFallback && (!configurationSettled() ||
				      model.fallback.configurationGeneration != state.configurationGeneration())) {
			finish(OperationOutcome::Failed, QStringLiteral("Fallback.Incomplete"));
			return;
		}
		live->sent = true;
		model.current->phase = OperationPhase::Sending;
		if (!publish())
			return;
		// Обработчик сигнала мог начать запись конфигурации до фактической передачи HTTP.
		if (firstFallback && (!configurationSettled() ||
				      model.fallback.configurationGeneration != state.configurationGeneration())) {
			live->sent = false;
			finish(OperationOutcome::Failed, QStringLiteral("Fallback.Incomplete"));
			return;
		}
		if (firstAdd &&
		    (!configurationSettled() || live->configurationGeneration != state.configurationGeneration())) {
			live->sent = false;
			finish(OperationOutcome::Failed, QStringLiteral("Fallback.Incomplete"));
			return;
		}
		// Повторная проверка учитывает запись/смену наблюдений из синхронного обработчика Sending.
		if (firstSelected && !selectedStartAllowed()) {
			live->sent = false;
			finish(OperationOutcome::Failed, QStringLiteral("Fallback.Incomplete"));
			return;
		}
		if (batch &&
		    (!batchConnectionAllowed() || ((live->action == OperationAction::AddOutput ||
						    live->action == OperationAction::StartDestination) &&
						   !batchConfigurationAllowed())) &&
		    !batchRetryOnly) {
			live->sent = false;
			finish(OperationOutcome::NotSent, QStringLiteral("Operation.RemainingStopped"));
			return;
		}
		if (batch && live->action == OperationAction::StartDestination)
			batchBootstrapSent = true;
		// expected ограждает результат от вытеснения Stop All/контекстом.
		const auto expected = stepToken;
		// callback принимает квитанцию либо завершает неоднозначную потерю ответа без нового POST.
		auto callback = [this, expected](auto, api::Result<api::Receipt> result) {
			if (!current(expected) || !busy)
				return;
			if (!result) {
				// rejected устанавливается только по HTTP-отказу, а не потере ответа после отправки.
				const bool rejected = result.error && mutationRejected(*result.error);
				// conflict разрешает адресное обновление, сохраняя исходный предметный код отказа.
				const bool conflict = result.error && result.error->httpStatus == 409;
				if (rejected && !notifyFailure(operationFailure(*result.error)))
					return;
				// guard защищает продолжение после публикации результата и refreshState.
				const QPointer<RestreamController> guard = &owner;
				finish(rejected ? OperationOutcome::Failed : OperationOutcome::Unknown,
				       rejected ? QStringLiteral("Operation.CommandFailed")
						: QStringLiteral("Operation.Unknown"));
				if (guard && conflict)
					connection.refreshState();
				return;
			}
			acceptReceipt(std::move(*result.value));
		};
		switch (live->action) {
		case OperationAction::StartFallback:
		case OperationAction::StartDestination:
			api.startSession(context.streamId, live->requestId, live->destinations, &owner, callback);
			break;
		case OperationAction::AddOutput:
			api.addOutput(live->sessionId, live->destinationId, live->requestId, &owner, callback);
			break;
		case OperationAction::Pause:
			api.pauseSession(live->sessionId, live->requestId, &owner, callback);
			break;
		case OperationAction::Resume:
			api.resumeSession(live->sessionId, live->requestId, &owner, callback);
			break;
		case OperationAction::StopAll:
			api.stopSession(live->sessionId, live->requestId, &owner, callback);
			break;
		case OperationAction::StartOutput:
			api.startOutput(live->sessionId, live->outputId, live->requestId, &owner, callback);
			break;
		case OperationAction::StopOutput:
			api.stopOutput(live->sessionId, live->outputId, live->requestId, &owner, callback);
			break;
		case OperationAction::DetachOutput:
			api.detachOutput(live->sessionId, live->outputId, live->requestId, &owner, callback);
			break;
		default:
			finish(OperationOutcome::Failed, QStringLiteral("Operation.InvalidTarget"));
			break;
		}
	}

	// Проверяет внутренние связи исторического Output; текущие изменяемые Group/Profile здесь не участвуют.
	bool matchingSelectedOutput(const api::Output &output) const
	{
		return validId(output.id) && output.sessionId == live->sessionId &&
		       output.destination.id == live->destinationId &&
		       output.destination.streamId == context.streamId && validId(output.group.id) &&
		       output.destination.groupId == output.group.id && output.group.streamId == context.streamId &&
		       validId(output.profile.id) && output.group.profileId == output.profile.id;
	}

	// Подтверждает закреплённый Output адресным GET; detach/re-add не может заменить сохранённый ID.
	void readSelectedOutput()
	{
		// expected ограждает ответ точного Output от смены workflow или контекста.
		const auto expected = stepToken;
		// Обработчик разделяет долговечный desired/attached и отдельное наблюдение фактической публикации.
		api.getOutput(live->sessionId, live->outputId, &owner,
			      [this, expected](auto, api::Result<api::OutputDetail> output) {
				      if (!current(expected) || !busy)
					      return;
				      if (!output || output.value->id != live->outputId ||
					  !matchingSelectedOutput(*output.value) || !output.value->attached ||
					  !output.value->desiredRunning) {
					      schedulePoll();
					      return;
				      }
				      stepCompleted();
			      });
	}

	// После собственной SUCCEEDED подтверждает точную RUNNING Session и единственный выбранный Output.
	void readSelectedStartOracle()
	{
		// expected связывает последовательные чтения только с собственной квитанцией запуска.
		const auto expected = stepToken;
		// Обработчик отвергает другую Session даже при совпадении выбора Dock; отказ своей Session
		// передаёт общему уведомлению до завершения шага.
		api.getSession(live->sessionId, &owner, [this, expected](auto, api::Result<api::Session> session) {
			if (!current(expected) || !busy)
				return;
			if (!session || session.value->id != live->sessionId ||
			    session.value->streamId != context.streamId ||
			    session.value->stream.id != context.streamId) {
				if (batch)
					batchOwnSession = false;
				schedulePoll();
				return;
			}
			if (session.value->state == QStringLiteral("FAILED") ||
			    session.value->state == QStringLiteral("INTERRUPTED")) {
				if (batch)
					stopNewSteps = true;
				if (!notifySessionFailure(*session.value))
					return;
				finish(OperationOutcome::Failed, QStringLiteral("Operation.CommandFailed"));
				return;
			}
			if (session.value->state != QStringLiteral("RUNNING")) {
				if (batch)
					batchOwnSession = false;
				schedulePoll();
				return;
			}
			if (batch)
				batchOwnSession = true;
			if (!live->outputId.isEmpty()) {
				readSelectedOutput();
				return;
			}
			// Обработчик считает все записи выбранного Destination, включая detached историю.
			// Ни первый элемент, ни единственный attached не доказывают принадлежность исходному запуску.
			api.getAllOutputs(live->sessionId, &owner,
					  [this, expected](auto, api::Result<api::Collection<api::Output>> outputs) {
						  if (!current(expected) || !busy)
							  return;
						  if (!outputs || !outputs.value->complete) {
							  schedulePoll();
							  return;
						  }
						  // ids обнаруживает дубли между страницами неатомарного чтения.
						  QSet<QString> ids;
						  // selected указывает на единственную запись выбранного Destination до окончания callback.
						  const api::Output *selected = nullptr;
						  // output проверяется на принадлежность собственной Session, а выбранный — на связи снимков.
						  for (const auto &output : outputs.value->items) {
							  if (!validId(output.id) || ids.contains(output.id) ||
							      output.sessionId != live->sessionId ||
							      output.destination.streamId != context.streamId) {
								  schedulePoll();
								  return;
							  }
							  ids.insert(output.id);
							  if (output.destination.id == live->destinationId) {
								  if (selected || !matchingSelectedOutput(output)) {
									  schedulePoll();
									  return;
								  }
								  selected = &output;
							  }
						  }
						  if (!selected) {
							  schedulePoll();
							  return;
						  }
						  live->outputId = selected->id;
						  model.current->outputId = selected->id;
						  if (publish())
							  readSelectedOutput();
					  });
		});
	}

	// Подтверждает точную сохранённую Session/Output, не устанавливая приватные GET в runtime.
	void readOracle()
	{
		if (!busy || !live)
			return;
		if (live->action == OperationAction::StartDestination) {
			readSelectedStartOracle();
			return;
		}
		// expected ограничивает оба варианта оракула текущим workflow.
		const auto expected = stepToken;
		if (!live->outputId.isEmpty()) {
			// callback сверяет ID выхода и родительской Session, затем сохранённые desired/attached; runtime может быть null.
			api.getOutput(live->sessionId, live->outputId, &owner,
				      [this, expected](auto, api::Result<api::OutputDetail> result) {
					      if (!current(expected) || !busy)
						      return;
					      if (!result) {
						      schedulePoll();
						      return;
					      }
					      if (result.value->id != live->outputId ||
						  result.value->sessionId != live->sessionId ||
						  result.value->destination.streamId != context.streamId ||
						  (batch && !matchingSelectedOutput(*result.value)) ||
						  (live->action == OperationAction::AddOutput &&
						   (result.value->destination.id != live->destinationId ||
						    result.value->group.streamId != context.streamId ||
						    result.value->group.id != result.value->destination.groupId))) {
						      finish(OperationOutcome::Unknown,
							     QStringLiteral("Operation.InvalidTarget"));
						      return;
					      }
					      // desired и attached задают оракул конкретной команды, независимо от платформенного runtime.
					      const bool desired = live->action == OperationAction::StartOutput ||
								   live->action == OperationAction::AddOutput;
					      const bool attached = live->action != OperationAction::DetachOutput;
					      if (result.value->desiredRunning == desired &&
						  result.value->attached == attached)
						      stepCompleted();
					      else
						      schedulePoll();
				      });
			return;
		}
		// callback проверяет RUNNING/desiredPaused либо STOPPED исходной Session, без проверки LIVE/FALLBACK группы.
		// При её отказе передаёт доступную причину общему уведомлению до завершения шага.
		api.getSession(live->sessionId, &owner, [this, expected](auto, api::Result<api::Session> result) {
			if (!current(expected) || !busy)
				return;
			if (!result) {
				schedulePoll();
				return;
			}
			if (result.value->id != live->sessionId || result.value->streamId != context.streamId ||
			    result.value->stream.id != context.streamId) {
				finish(OperationOutcome::Unknown, QStringLiteral("Operation.InvalidTarget"));
				return;
			}
			// confirmed относится только к долговечному состоянию указанной Session.
			const bool confirmed = live->action == OperationAction::StopAll
						       ? result.value->state == QStringLiteral("STOPPED")
						       : result.value->state == QStringLiteral("RUNNING") &&
								 (live->action == OperationAction::StartFallback ||
								  result.value->desiredPaused ==
									  (live->action == OperationAction::Pause));
			if (confirmed)
				stepCompleted();
			else if (result.value->state == QStringLiteral("FAILED") ||
				 result.value->state == QStringLiteral("INTERRUPTED")) {
				if (!notifySessionFailure(*result.value))
					return;
				finish(OperationOutcome::Failed, QStringLiteral("Operation.CommandFailed"));
			} else
				schedulePoll();
		});
	}

	// Завершает шаг и разрешает следующий только при непрерывно пригодном исходном контексте.
	void stepCompleted()
	{
		if (batch) {
			completeBatchStep();
			return;
		}
		++stepToken;
		deadline.stop();
		model.current->stateConfirmed = true;
		if (live->resultIndex >= 0) {
			// hasContinuation отличает stop половину restart от законченного элемента.
			const bool hasContinuation = nextStep < plan.size() &&
						     plan[nextStep].resultIndex == live->resultIndex;
			model.current->outputs[live->resultIndex].outcome =
				hasContinuation ? OperationOutcome::NotSent : OperationOutcome::Completed;
		}
		// guard защищает обращение к исполнителю после сигнала обновления наблюдений.
		const QPointer<RestreamController> guard = &owner;
		// expected запрещает продолжение после смены поколения workflow обработчиком сигнала.
		const auto expected = stepToken;
		connection.refreshState();
		if (!guard || !current(expected) || !busy)
			return;
		if (nextStep < plan.size()) {
			if (stopNewSteps || state.connectionStatus() != ConnectionStatus::Connected) {
				// Уже выполненная stop половина restart не подтверждает целый элемент.
				finish(OperationOutcome::Unknown, QStringLiteral("Operation.RemainingStopped"));
				return;
			}
			startNextOutput();
			return;
		}
		finish(OperationOutcome::Completed);
	}

	// Сбрасывает только публичные сведения шага; идентичность workflow/результаты предыдущих Output сохраняются.
	void exposeStep()
	{
		model.current->outputId = live->outputId;
		model.current->requestId = live->requestId;
		model.current->commandId = live->commandId;
		model.current->receipt.reset();
		model.current->stateConfirmed = false;
		model.current->retryAvailable = false;
		model.current->phase = OperationPhase::Sending;
		if (live->resultIndex >= 0)
			model.current->outputs[live->resultIndex].outcome = OperationOutcome::Pending;
	}

	// Перед каждой новой командой выхода проверяет исходную Session и сохранённое состояние точного Output.
	void validateOutputStep()
	{
		// expected ограничивает вложенное последовательное чтение одной попыткой.
		const auto expected = stepToken;
		// callback не разрешает команду новой/завершённой Session на основании старого UI снимка.
		api.getSession(live->sessionId, &owner, [this, expected](auto, api::Result<api::Session> result) {
			if (!current(expected) || !busy)
				return;
			if (!result || result.value->id != live->sessionId ||
			    result.value->streamId != context.streamId || result.value->stream.id != context.streamId ||
			    result.value->state != QStringLiteral("RUNNING")) {
				if (batch)
					stopNewSteps = true;
				finish(OperationOutcome::Failed, QStringLiteral("Operation.InvalidTarget"));
				return;
			}
			// callback проверяет прикрепление и допускает start только при desiredRunning=false.
			api.getOutput(
				live->sessionId, live->outputId, &owner,
				[this, expected](auto, api::Result<api::OutputDetail> output) {
					if (!current(expected) || !busy)
						return;
					if (!output || output.value->id != live->outputId ||
					    output.value->sessionId != live->sessionId ||
					    output.value->destination.streamId != context.streamId ||
					    !output.value->attached ||
					    (live->action == OperationAction::StartOutput &&
					     output.value->desiredRunning &&
					     (!batch || model.current->action != OperationAction::StartAllOutputs)) ||
					    (batch && !matchingSelectedOutput(*output.value))) {
						finish(OperationOutcome::Failed,
						       QStringLiteral("Operation.InvalidTarget"));
						return;
					}
					if (batch && live->action == OperationAction::StartOutput &&
					    output.value->desiredRunning &&
					    model.current->action == OperationAction::StartAllOutputs) {
						model.current->outputs[live->resultIndex].noOp = true;
						model.current->outputs[live->resultIndex].messageKey =
							QStringLiteral("Operation.AlreadyRequested");
						stepCompleted();
						return;
					}
					if (stopNewSteps || state.connectionStatus() != ConnectionStatus::Connected) {
						finish(OperationOutcome::Unknown,
						       QStringLiteral("Operation.RemainingStopped"));
						return;
					}
					sendPost();
				});
		});
	}

	// Извлекает следующий фиксированный шаг, создавая request_id ровно один раз до сетевой очереди.
	void startNextOutput()
	{
		if (nextStep >= plan.size())
			return;
		// entry копирует план до публикации сигналов, способных заменить workflow.
		const auto entry = plan[nextStep++];
		++stepToken;
		live = LiveStep{entry.action,
				model.current->sessionId,
				entry.outputId,
				QUuid::createUuid().toString(QUuid::WithoutBraces),
				{},
				{},
				entry.resultIndex,
				false};
		exposeStep();
		armDeadline();
		if (publish())
			validateOutputStep();
	}

	// Возвращает разрешение новых POST в прежнем контексте; восстановление связи запрет не снимает.
	bool batchConnectionAllowed() const
	{
		return batch && !stopNewSteps && !explicitDisconnect && context == state.dockContext() &&
		       state.connectionStatus() == ConnectionStatus::Connected && state.selectionAvailable();
	}

	// Новое прикрепление и создание Session используют только завершённую исходную конфигурационную эпоху.
	bool batchConfigurationAllowed() const
	{
		return batch && configurationSettled() &&
		       batch->configurationGeneration == state.configurationGeneration();
	}

	// Отмечает результат конкретной части строки; None остаётся у неприменимой части.
	void recordBatchPart(OperationOutcome outcome)
	{
		if (!live || live->resultIndex < 0)
			return;
		// row сохраняет результат одной исходной строки, независимо от более поздних успехов.
		auto &row = model.current->outputs[live->resultIndex];
		if (live->action == OperationAction::StopOutput)
			row.stopOutcome = outcome;
		else
			row.startOutcome = outcome;
		if (!live->outputId.isEmpty())
			row.outputId = live->outputId;
	}

	// Обновляет цель Retry по первому Unknown исходного порядка, без очереди автоматического повтора.
	void exposeBatchRetry()
	{
		model.current->retryAvailable = false;
		model.current->retryTargetName.clear();
		// index сопоставляет сохранённый неизвестный шаг с исходной подписью строки.
		for (qsizetype index = 0; index < batchUnknown.size(); ++index) {
			if (!batchUnknown[index])
				continue;
			model.current->retryAvailable = !explicitDisconnect && context == state.dockContext();
			model.current->retryTargetName = model.current->outputs[index].name;
			break;
		}
	}

	// Завершает пакет честной агрегацией всего исходного состава и сохраняет неизвестные тела POST в RAM.
	void finishBatch()
	{
		++stepToken;
		deadline.stop();
		poll.stop();
		api.cancelAll();
		busy = false;
		batchRetryOnly = false;
		model.current->phase = OperationPhase::Finished;
		model.current->outcome = OperationOutcome::Completed;
		// pendingRow получает безопасную причину пропуска, если её команда ещё не начиналась.
		for (auto &pendingRow : model.current->outputs)
			if (pendingRow.outcome == OperationOutcome::NotSent && pendingRow.messageKey.isEmpty())
				pendingRow.messageKey = stopNewSteps ? QStringLiteral("Operation.RemainingStopped")
								     : QStringLiteral("Operation.InvalidTarget");
		// row участвует в общем результате даже если её обязательный POST не был отправлен.
		for (const auto &row : model.current->outputs) {
			if (row.outcome == OperationOutcome::Unknown || row.outcome == OperationOutcome::Pending) {
				model.current->outcome = OperationOutcome::Unknown;
				break;
			}
			if (row.outcome == OperationOutcome::Failed)
				model.current->outcome = OperationOutcome::Failed;
			else if (row.outcome != OperationOutcome::Completed &&
				 model.current->outcome == OperationOutcome::Completed)
				model.current->outcome = OperationOutcome::NotSent;
		}
		model.current->messageKey = model.current->outcome == OperationOutcome::Completed
						    ? QString{}
						    : QStringLiteral("Operation.Partial");
		exposeBatchRetry();
		// guard защищает от синхронного уничтожения контроллера при публикации итогов.
		const QPointer<RestreamController> guard = &owner;
		publish();
		if (guard && deferredPrecheck)
			refreshFallbackAfterConfiguration();
	}

	// Передаёт следующую строку следующему обороту GUI, чтобы синхронный отказ API не накапливал стек.
	// Продолжение проверяет stepToken завершённой части и не переживает смену контекста либо StopAll.
	void queueNextBatchRow()
	{
		// expected связывает только это отложенное продолжение с неизменившимся исполнителем.
		const auto expected = stepToken;
		// Обработчик рассматривает следующую исходную строку, не повторяя завершённую команду.
		QTimer::singleShot(0, &owner, [this, expected] {
			if (current(expected) && busy && batch)
				startNextBatchRow();
		});
	}

	// Прекращает текущую часть, сохраняя Unknown, затем допускает только независимые строки.
	// Неопределённость общей Session или её создания и потеря связи закрывают весь неотправленный остаток.
	void finishBatchStep(OperationOutcome outcome, QString message)
	{
		++stepToken;
		deadline.stop();
		poll.stop();
		api.cancelAll();
		if (!live || live->resultIndex < 0) {
			stopNewSteps = true;
			finishBatch();
			return;
		}
		// index фиксирует строку до возможной замены единственного live следующей строкой.
		const auto index = live->resultIndex;
		// row хранит безопасную причину и итог обеих частей restart.
		auto &row = model.current->outputs[index];
		recordBatchPart(live->sent ? outcome : OperationOutcome::NotSent);
		row.outcome = outcome;
		row.messageKey = std::move(message);
		if (outcome == OperationOutcome::Unknown && live->sent)
			batchUnknown[index] = SavedUnknown{*live, model.current->receipt};
		if (live->action == OperationAction::StartDestination &&
		    (batchBootstrapSent || outcome == OperationOutcome::Unknown) && !batchOwnSession)
			stopNewSteps = true;
		if (batchRetryOnly || !batchConnectionAllowed()) {
			model.current->remainingStopped = batchNextIndex < batch->targets.size() || batchRetryOnly;
			finishBatch();
			return;
		}
		queueNextBatchRow();
	}

	// Создаёт команду action для исходной строки index и точного outputId; пустой ID допустим при создании.
	// Инвалидирует обработчики прежней части до отмены и назначает новый request_id только новому шагу.
	void prepareBatchStep(OperationAction action, qsizetype index, const QString &outputId = {})
	{
		++stepToken;
		deadline.stop();
		poll.stop();
		api.cancelAll();
		live = LiveStep{action,
				model.current->sessionId,
				outputId,
				QUuid::createUuid().toString(QUuid::WithoutBraces),
				{},
				{},
				index,
				false,
				batch->targets[index].destinationId};
		live->configurationGeneration = batch->configurationGeneration;
		if (action == OperationAction::StartDestination)
			live->destinations = {live->destinationId};
		exposeStep();
		model.current->destinationId = live->destinationId;
		armDeadline();
	}

	// После подтверждения части restart отправляет start только в обычном исполнении пакета.
	// Явный Retry сверяет прежний шаг и никогда не восполняет пропущенные зависимые команды.
	void completeBatchStep()
	{
		++stepToken;
		deadline.stop();
		poll.stop();
		api.cancelAll();
		model.current->stateConfirmed = true;
		recordBatchPart(OperationOutcome::Completed);
		// guard сохраняет безопасность при синхронных уведомлениях обновления наблюдателя.
		const QPointer<RestreamController> guard = &owner;
		// expected относится к завершённой части до создания следующего шага.
		const auto expected = stepToken;
		connection.refreshState();
		if (!guard || !current(expected) || !busy)
			return;
		// index сохраняет исходную строку перед созданием нового live.
		const auto index = live->resultIndex;
		// restartStop означает выполненную лишь первую обязательную часть attached restart.
		const bool restartStop = model.current->action == OperationAction::RestartAllOutputs &&
					 live->action == OperationAction::StopOutput;
		if (restartStop) {
			model.current->outputs[index].outcome = OperationOutcome::NotSent;
			if (!batchRetryOnly && batchConnectionAllowed()) {
				// outputId сохраняет тот же исторический Output для второй части restart.
				const auto outputId = live->outputId;
				prepareBatchStep(OperationAction::StartOutput, index, outputId);
				if (publish())
					validateOutputStep();
				return;
			}
		} else {
			model.current->outputs[index].outcome = OperationOutcome::Completed;
			if (!model.current->outputs[index].noOp)
				model.current->outputs[index].messageKey.clear();
		}
		if (batchRetryOnly || !batchConnectionAllowed()) {
			model.current->remainingStopped = batchNextIndex < batch->targets.size() || restartStop;
			finishBatch();
			return;
		}
		queueNextBatchRow();
	}

	// Перед пропуском каталоговой строки при StopAllOutputs подтверждает отсутствие её attached Output.
	void validateBatchAbsentOutput()
	{
		// expected ограждает оба последовательных GET от другой строки и явного повторения.
		const auto expected = stepToken;
		// Обработчик подтверждает прежнюю общую Session, даже если предыдущая строка осталась Unknown.
		api.getSession(
			model.current->sessionId, &owner, [this, expected](auto, api::Result<api::Session> session) {
				if (!current(expected) || !busy)
					return;
				if (!session || session.value->id != model.current->sessionId ||
				    session.value->streamId != context.streamId ||
				    session.value->stream.id != context.streamId ||
				    session.value->state != QStringLiteral("RUNNING")) {
					stopNewSteps = true;
					finish(OperationOutcome::NotSent, QStringLiteral("Operation.InvalidTarget"));
					return;
				}
				// Обработчик проверяет весь список: поздний чужой attach не выбирается вместо исходной строки.
				api.getAllOutputs(
					model.current->sessionId, &owner,
					[this, expected](auto, api::Result<api::Collection<api::Output>> outputs) {
						if (!current(expected) || !busy)
							return;
						if (!outputs || !outputs.value->complete) {
							finish(OperationOutcome::NotSent,
							       QStringLiteral("Operation.InvalidTarget"));
							return;
						}
						// ids исключает повтор Output между страницами.
						QSet<QString> ids;
						// output проверяется по точной Session; чужой attach даёт отказ этой строки.
						for (const auto &output : outputs.value->items) {
							if (!validId(output.id) || ids.contains(output.id) ||
							    output.sessionId != model.current->sessionId ||
							    output.destination.streamId != context.streamId ||
							    (output.attached &&
							     output.destination.id == live->destinationId)) {
								finish(OperationOutcome::NotSent,
								       QStringLiteral("Operation.InvalidTarget"));
								return;
							}
							ids.insert(output.id);
						}
						if (!batchConnectionAllowed()) {
							finish(OperationOutcome::NotSent,
							       QStringLiteral("Operation.RemainingStopped"));
							return;
						}
						model.current->outputs[live->resultIndex].noOp = true;
						model.current->outputs[live->resultIndex].messageKey =
							QStringLiteral("Operation.NoExecution");
						completeBatchStep();
					});
			});
	}

	// Рассматривает только следующий исходный элемент; ошибки не расширяют состав из каталога/runtime.
	void startNextBatchRow()
	{
		while (batchNextIndex < batch->targets.size()) {
			if (!batchConnectionAllowed()) {
				model.current->remainingStopped = true;
				finishBatch();
				return;
			}
			// index — исходный порядковый номер; target содержит замороженные IDs, row — публичный итог строки.
			// Эти ссылки используются до сигналов, способных заменить workflow.
			const auto index = batchNextIndex++;
			const auto &target = batch->targets[index];
			auto &row = model.current->outputs[index];
			if (!validId(target.destinationId) ||
			    (!target.outputId.isEmpty() && !validId(target.outputId))) {
				row.messageKey = QStringLiteral("Operation.InvalidTarget");
				continue;
			}
			if (batch->noActiveSession && !batchOwnSession) {
				if (model.current->action == OperationAction::StopAllOutputs) {
					row.outcome = OperationOutcome::Completed;
					row.stopOutcome = OperationOutcome::Completed;
					row.noOp = true;
					row.messageKey = QStringLiteral("Operation.NoExecution");
					continue;
				}
				if (batchBootstrapSent) {
					model.current->remainingStopped = true;
					finishBatch();
					return;
				}
				if (!batchConfigurationAllowed()) {
					row.messageKey = QStringLiteral("Fallback.Incomplete");
					continue;
				}
				selectedStart = SelectedStartProof{};
				selectedStart->configurationGeneration = batch->configurationGeneration;
				prepareBatchStep(OperationAction::StartDestination, index);
				model.current->phase = OperationPhase::Prechecking;
				if (publish())
					readSelectedDestination();
				return;
			}
			if (!batch->noActiveSession && !target.outputId.isEmpty()) {
				prepareBatchStep(model.current->action == OperationAction::StartAllOutputs
							 ? OperationAction::StartOutput
							 : OperationAction::StopOutput,
						 index, target.outputId);
				if (publish())
					validateOutputStep();
				return;
			}
			if (model.current->action == OperationAction::StopAllOutputs) {
				prepareBatchStep(OperationAction::StopOutput, index);
				if (publish())
					validateBatchAbsentOutput();
				return;
			}
			if (!batchConfigurationAllowed()) {
				row.messageKey = QStringLiteral("Fallback.Incomplete");
				continue;
			}
			prepareBatchStep(OperationAction::AddOutput, index);
			if (publish())
				validateAddOutput();
			return;
		}
		finishBatch();
	}

	// Фиксирует весь показанный набор до первого сигнала. Неподтверждённые строки остаются NotSent,
	// request задаёт только показанные строки, action — одно из трёх пакетных действий OperationAction.
	// Дубли Output запрещены для всего пакета; разные Outputs одного Destination сохраняются раздельно.
	bool controlOutputs(const OutputBatchRequest &request, OperationAction action)
	{
		if (!available(request.context) || request.targets.isEmpty() || request.targets.size() > 10000 ||
		    (action != OperationAction::StartAllOutputs && action != OperationAction::StopAllOutputs &&
		     action != OperationAction::RestartAllOutputs))
			return false;
		// snapshot содержит подтверждение исходного режима; текущий неизвестный статус не означает отсутствие.
		const auto &snapshot = state.dockServer();
		if (request.noActiveSession) {
			if (!request.sessionId.isEmpty() ||
			    snapshot.sessionStatus.availability != RuntimeAvailability::Fresh ||
			    !snapshot.sessionStatus.complete ||
			    (snapshot.session && snapshot.session->state != QStringLiteral("STOPPED") &&
			     snapshot.session->state != QStringLiteral("FAILED") &&
			     snapshot.session->state != QStringLiteral("INTERRUPTED")))
				return false;
		} else if (!sessionTarget(request.sessionId, true)) {
			return false;
		}
		// ids исключает повтор точного Output, не схлопывая разные исторические исполнения назначения.
		QSet<QString> ids;
		// target проверяется только как исходная адресация, без добавления новых строк.
		for (const auto &target : request.targets) {
			if (!target.outputId.isEmpty()) {
				if (ids.contains(target.outputId))
					return false;
				ids.insert(target.outputId);
			}
		}
		begin(action, request.sessionId);
		batch = request;
		batchUnknown.resize(request.targets.size());
		// target переносит безопасную подпись и раздельные IDs в публичный результат.
		for (const auto &target : request.targets) {
			// row содержит обе обязательные части attached restart до их фактической отправки.
			OutputOperationResult row;
			row.outputId = request.noActiveSession ? QString{} : target.outputId;
			row.destinationId = target.destinationId;
			row.name = target.name;
			if (action == OperationAction::StopAllOutputs ||
			    (action == OperationAction::RestartAllOutputs && !request.noActiveSession &&
			     !target.outputId.isEmpty()))
				row.stopOutcome = OperationOutcome::NotSent;
			if (action != OperationAction::StopAllOutputs)
				row.startOutcome = OperationOutcome::NotSent;
			model.current->outputs.append(std::move(row));
		}
		startNextBatchRow();
		return true;
	}

	// Начинает одиночное/пакетное действие после полной синхронной проверки исходного списка IDs.
	bool startOutputs(const DockContext &expected, const QString &sessionId, const QStringList &ids,
			  OperationAction action)
	{
		if (!available(expected) || !sessionTarget(sessionId, true))
			return false;
		// steps/results являются проверенным локальным кандидатом, ещё не меняющим общий workflow.
		QVector<OutputOperations::Step> steps;
		QVector<OutputOperationResult> results;
		if (!OutputOperations::makePlan(action, ids, state.dockServer(), steps, results))
			return false;
		begin(action, sessionId);
		plan = std::move(steps);
		model.current->outputs = std::move(results);
		startNextOutput();
		return true;
	}

	// Создаёт точный Session шаг, не отправляя команду до публикации и проверок цели.
	void prepareSessionStep(OperationAction action)
	{
		++stepToken;
		live = LiveStep{action, model.current->sessionId,
				{},     QUuid::createUuid().toString(QUuid::WithoutBraces),
				{},     {},
				-1,     false};
		exposeStep();
		armDeadline();
	}

	// Повторно проверяет исходную Session перед pause/resume либо её остановкой.
	void validateSessionStep()
	{
		// expected связывает ответ с неизменяемым Session шагом.
		const auto expected = stepToken;
		// callback не перенаправляет stop на следующую Session и не трактует GET как собственный успех.
		api.getSession(live->sessionId, &owner, [this, expected](auto, api::Result<api::Session> result) {
			if (!current(expected) || !busy)
				return;
			if (!result || result.value->id != live->sessionId ||
			    result.value->streamId != context.streamId || result.value->stream.id != context.streamId ||
			    (live->action != OperationAction::StopAll &&
			     result.value->state != QStringLiteral("RUNNING"))) {
				finish(OperationOutcome::Failed, QStringLiteral("Operation.InvalidTarget"));
				return;
			}
			if (live->action != OperationAction::StopAll &&
			    (stopNewSteps || state.connectionStatus() != ConnectionStatus::Connected)) {
				finish(OperationOutcome::Unknown, QStringLiteral("Operation.RemainingStopped"));
				return;
			}
			sendPost();
		});
	}

	// Запускает pause/resume выбранной RUNNING Session, сохраняя source группы отдельным наблюдением.
	bool sessionAction(const DockContext &expected, const QString &sessionId, OperationAction action)
	{
		if (!available(expected) || !sessionTarget(sessionId, true))
			return false;
		begin(action, sessionId);
		prepareSessionStep(action);
		if (publish())
			validateSessionStep();
		return true;
	}

	// Проверяет актуальные Destination/Group и полный состав выходов исходной RUNNING Session.
	void validateAddOutput()
	{
		// expected ограничивает всю цепочку проверки одним живым намерением.
		const auto expected = stepToken;
		// Обработчик подтверждает Session, не заменяя её новым текущим эфиром.
		api.getSession(live->sessionId, &owner, [this, expected](auto, api::Result<api::Session> session) {
			if (!current(expected) || !busy)
				return;
			if (!session || session.value->id != live->sessionId ||
			    session.value->streamId != context.streamId ||
			    session.value->stream.id != context.streamId ||
			    session.value->state != QStringLiteral("RUNNING")) {
				if (batch)
					stopNewSteps = true;
				finish(OperationOutcome::Failed, QStringLiteral("Operation.InvalidTarget"));
				return;
			}
			// Обработчик читает текущее назначение, включая выключенное для автоматического запуска.
			api.getDestination(
				live->destinationId, &owner,
				[this, expected](auto, api::Result<api::Destination> destination) {
					if (!current(expected) || !busy)
						return;
					if (!destination || destination.value->id != live->destinationId ||
					    destination.value->streamId != context.streamId ||
					    !validId(destination.value->groupId)) {
						finish(OperationOutcome::Failed,
						       QStringLiteral("Operation.InvalidTarget"));
						return;
					}
					// groupId фиксирует связь назначения для последующего точного GET.
					const auto groupId = destination.value->groupId;
					// Обработчик подтверждает принадлежность Group выбранному Stream.
					api.getGroup(groupId, &owner, [this, expected, groupId](auto, api::Result<api::Group> group) {
						if (!current(expected) || !busy)
							return;
						if (!group || group.value->id != groupId ||
						    group.value->streamId != context.streamId) {
							finish(OperationOutcome::Failed,
							       QStringLiteral("Operation.InvalidTarget"));
							return;
						}
						// Обработчик исключает уже attached назначение по полному списку, не по наличию runtime.
						api.getAllOutputs(
							live->sessionId, &owner,
							[this,
							 expected](auto,
								   api::Result<api::Collection<api::Output>> outputs) {
								if (!current(expected) || !busy)
									return;
								if (!outputs || !outputs.value->complete) {
									finish(OperationOutcome::Failed,
									       QStringLiteral(
										       "Operation.InvalidTarget"));
									return;
								}
								// ids обнаруживает повтор идентичности между страницами неатомарного списка.
								QSet<QString> ids;
								// output проверяется по исходной Session и снимку назначения.
								for (const auto &output : outputs.value->items) {
									if (!validId(output.id) ||
									    ids.contains(output.id) ||
									    output.sessionId != live->sessionId ||
									    output.destination.streamId !=
										    context.streamId ||
									    (output.attached &&
									     output.destination.id ==
										     live->destinationId)) {
										finish(OperationOutcome::Failed,
										       QStringLiteral(
											       "Operation.InvalidTarget"));
										return;
									}
									ids.insert(output.id);
								}
								if (stopNewSteps ||
								    (!batch && !sessionTarget(live->sessionId, true)) ||
								    (batch && !batchConfigurationAllowed()) ||
								    state.connectionStatus() !=
									    ConnectionStatus::Connected) {
									finish(OperationOutcome::Failed,
									       QStringLiteral(
										       "Operation.InvalidTarget"));
									return;
								}
								sendPost();
							});
					});
				});
		});
	}

	// Фиксирует точную цель, request_id и payload прикрепления до каких-либо сетевых запросов.
	bool addOutput(const DockContext &expected, const QString &sessionId, const QString &destinationId)
	{
		if (!available(expected) || !configurationSettled() || !sessionTarget(sessionId, true) ||
		    !validId(destinationId))
			return false;
		begin(OperationAction::AddOutput, sessionId);
		prepareSessionStep(OperationAction::AddOutput);
		live->configurationGeneration = state.configurationGeneration();
		live->destinationId = destinationId;
		model.current->destinationId = destinationId;
		if (publish())
			validateAddOutput();
		return true;
	}

	// Ограниченно ждёт остановки OBS, затем обязательно адресует исходную Session.
	bool stopAll(const DockContext &expected, const QString &sessionId)
	{
		if (stopped || !expected.valid() || expected != context || expected != state.dockContext() ||
		    explicitDisconnect || !sessionTarget(sessionId, false))
			return false;
		archiveInterrupted();
		++stepToken;
		api.cancelAll();
		deadline.stop();
		poll.stop();
		begin(OperationAction::StopAll, sessionId);
		model.current->phase = OperationPhase::StoppingObs;
		model.current->obsOutcome = OperationOutcome::Pending;
		model.current->serverOutcome = OperationOutcome::NotSent;
		if (!publish())
			return true;
		// expectedGeneration фиксирует намерение Stop All, чтобы поздний callback OBS не продолжил новую операцию.
		const auto expectedGeneration = stepToken;
		// callback сохраняет отдельный локальный итог; false не препятствует stop исходной серверной Session.
		obs.stopStreaming(
			&owner,
			[this, expectedGeneration](bool confirmed) {
				if (!current(expectedGeneration) || !busy)
					return;
				model.current->obsOutcome = confirmed ? OperationOutcome::Completed
								      : OperationOutcome::Unknown;
				model.current->serverOutcome = OperationOutcome::Pending;
				prepareSessionStep(OperationAction::StopAll);
				if (publish())
					validateSessionStep();
			},
			std::max(1, limits.obsStopTimeoutMs));
		return true;
	}

	// Завершает предварительную проверку безопасным локальным сообщением без серверного failure.message.
	void finishPrecheck(FallbackReadiness readiness, QString message)
	{
		if (readiness == FallbackReadiness::Ready &&
		    (stopNewSteps || !configurationSettled() ||
		     state.connectionStatus() != ConnectionStatus::Connected ||
		     model.fallback.configurationGeneration != state.configurationGeneration())) {
			readiness = FallbackReadiness::Unknown;
			message = QStringLiteral("Fallback.Incomplete");
		}
		model.fallback.readiness = readiness;
		model.fallback.messageKey = std::move(message);
		finish(readiness == FallbackReadiness::Ready     ? OperationOutcome::Completed
		       : readiness == FallbackReadiness::Unknown ? OperationOutcome::Unknown
								 : OperationOutcome::Failed,
		       model.fallback.messageKey);
	}

	// Последовательно проверяет Group/Profile каждого зафиксированного Enabled назначения.
	void checkNextDestination()
	{
		if (destinationIndex == destinations.size()) {
			finishPrecheck(FallbackReadiness::Ready, QStringLiteral("Fallback.PreliminaryReady"));
			return;
		}
		// destination копирует точную конфигурацию для двух связанных асинхронных GET.
		const auto destination = destinations[destinationIndex];
		// expected связывает оба ответа с одной предварительной проверкой.
		const auto expected = stepToken;
		// callback проверяет точную Group и её принадлежность Stream до чтения полного профиля.
		api.getGroup(
			destination.groupId, &owner,
			[this, expected, destination](auto, api::Result<api::Group> group) {
				if (!current(expected) || !busy)
					return;
				if (!group || group.value->id != destination.groupId ||
				    group.value->streamId != context.streamId || !validId(group.value->profileId)) {
					finishPrecheck(FallbackReadiness::Unknown,
						       QStringLiteral("Fallback.Incomplete"));
					return;
				}
				// profileId фиксирует связь Group до следующего GET, callback не использует текущий выбор UI.
				const auto profileId = group.value->profileId;
				// callback сравнивает всю структуру профиля с READY-вариантом точного текущего поколения.
				api.getProfile(
					profileId, &owner,
					[this, expected, profileId](auto, api::Result<api::Profile> profile) {
						if (!current(expected) || !busy)
							return;
						if (!profile || profile.value->id != profileId) {
							finishPrecheck(FallbackReadiness::Unknown,
								       QStringLiteral("Fallback.Incomplete"));
							return;
						}
						// compatible учитывает профиль/формат; READY всё равно остаётся предварительным допуском.
						const bool compatible = std::any_of(
							variants.cbegin(), variants.cend(),
							// variant сравнивается целиком по рецепту, без выдуманного generator_version.
							[&profile](const api::Variant &variant) {
								return variant.state == QStringLiteral("READY") &&
								       api::sameProfile(variant.profile,
											profile.value->profile) &&
								       api::compatibleMeasuredFormat(
									       variant.format, profile.value->profile);
							});
						if (!compatible) {
							finishPrecheck(FallbackReadiness::Unavailable,
								       QStringLiteral("Fallback.NoCompatibleVariant"));
							return;
						}
						++destinationIndex;
						checkNextDestination();
					});
			});
	}

	// Фиксирует полный непустой набор Enabled и отображаемые имена до проверки соответствующих профилей.
	void readEnabledDestinations()
	{
		// expected ограждает ответ полного списка назначений.
		const auto expected = stepToken;
		// callback отвергает неполноту/дубли; пустой список никогда не отправляется в команде запуска.
		api.getAllStreamDestinations(
			context.streamId, &owner,
			[this, expected](auto, api::Result<api::Collection<api::Destination>> result) {
				if (!current(expected) || !busy)
					return;
				if (!result || !result.value->complete) {
					finishPrecheck(FallbackReadiness::Unknown,
						       QStringLiteral("Fallback.Incomplete"));
					return;
				}
				// ids запрещает скрытое удвоение одного назначения в фиксированном наборе.
				QSet<QString> ids;
				// destination относится к полному списку выбранного Stream.
				for (const auto &destination : result.value->items) {
					if (!validId(destination.id) || ids.contains(destination.id) ||
					    destination.streamId != context.streamId) {
						finishPrecheck(FallbackReadiness::Unknown,
							       QStringLiteral("Fallback.Incomplete"));
						return;
					}
					ids.insert(destination.id);
					if (destination.enabled) {
						if (!validId(destination.groupId)) {
							finishPrecheck(FallbackReadiness::Unknown,
								       QStringLiteral("Fallback.Incomplete"));
							return;
						}
						destinations.append(destination);
						model.fallback.destinations.append({destination.id, destination.name});
					}
				}
				if (destinations.isEmpty()) {
					finishPrecheck(FallbackReadiness::Unavailable,
						       QStringLiteral("Fallback.NoEnabledDestinations"));
					return;
				}
				checkNextDestination();
			});
	}

	// Читает все страницы вариантов точного текущего поколения; неполный ответ не доказывает пригодность.
	void readVariants()
	{
		// query замораживает generation_id для каждой страницы обхода.
		api::ListQuery query;
		query.generationId = model.fallback.generationId;
		// expected ограждает весь обход от новой проверки/смены контекста.
		const auto expected = stepToken;
		// callback сверяет точные ID Asset/Generation каждого варианта, прежде чем сравнивать профили.
		api.getAllVariants(
			model.fallback.assetId, &owner,
			[this, expected](auto, api::Result<api::Collection<api::Variant>> result) {
				if (!current(expected) || !busy)
					return;
				if (!result || !result.value->complete) {
					finishPrecheck(FallbackReadiness::Unknown,
						       QStringLiteral("Fallback.Incomplete"));
					return;
				}
				// ids обнаруживает дубли варианта между страницами без предположения об атомарности списка.
				QSet<QString> ids;
				// variant проверяется на точное опубликованное поколение.
				for (const auto &variant : result.value->items) {
					if (!validId(variant.id) || ids.contains(variant.id) ||
					    variant.assetId != model.fallback.assetId ||
					    variant.generationId != model.fallback.generationId) {
						finishPrecheck(FallbackReadiness::Unknown,
							       QStringLiteral("Fallback.Incomplete"));
						return;
					}
					ids.insert(variant.id);
				}
				variants = std::move(result.value->items);
				readEnabledDestinations();
			},
			std::move(query));
	}

	// Запускает строго policy->Asset->current Generation->variants->Enabled Group/Profile.
	bool checkFallback(const DockContext &expectedContext)
	{
		if (!available(expectedContext) || !configurationSettled())
			return false;
		begin(OperationAction::CheckFallback);
		model.current->phase = OperationPhase::Prechecking;
		model.fallback = {};
		model.fallback.context = context;
		model.fallback.revision = ++precheckRevision;
		model.fallback.configurationGeneration = state.configurationGeneration();
		model.fallback.readiness = FallbackReadiness::Checking;
		variants.clear();
		destinations.clear();
		destinationIndex = 0;
		armDeadline();
		if (!publish())
			return true;
		// expected связывает всю вложенную цепочку с одним намерением предварительной проверки.
		const auto expected = stepToken;
		// callback различает отсутствие политики и неполученное поле.
		api.getFallbackPolicy(
			context.streamId, &owner, [this, expected](auto, api::Result<api::PolicyResult> policy) {
				if (!current(expected) || !busy)
					return;
				if (!policy || policy.value->fallback.isMissing()) {
					finishPrecheck(FallbackReadiness::Unknown,
						       QStringLiteral("Fallback.Incomplete"));
					return;
				}
				if (!policy.value->fallback || !validId(policy.value->fallback->assetId)) {
					finishPrecheck(FallbackReadiness::Unavailable,
						       QStringLiteral("Fallback.NoPolicy"));
					return;
				}
				model.fallback.assetId = policy.value->fallback->assetId;
				// callback берёт только current, никогда pending, и сверяет ID запрошенного Asset.
				api.getAsset(
					model.fallback.assetId, &owner,
					[this, expected](auto, api::Result<api::Asset> asset) {
						if (!current(expected) || !busy)
							return;
						if (!asset || asset.value->id != model.fallback.assetId) {
							finishPrecheck(FallbackReadiness::Unknown,
								       QStringLiteral("Fallback.Incomplete"));
							return;
						}
						if (!validId(asset.value->currentGenerationId)) {
							finishPrecheck(FallbackReadiness::Unavailable,
								       QStringLiteral("Fallback.NoCurrentGeneration"));
							return;
						}
						model.fallback.generationId = asset.value->currentGenerationId;
						// callback проверяет готовность выбранного текущего поколения до обхода вариантов.
						api.getGeneration(
							model.fallback.assetId, model.fallback.generationId, &owner,
							[this, expected](auto, api::Result<api::Generation> prepared) {
								if (!current(expected) || !busy)
									return;
								if (!prepared ||
								    prepared.value->id != model.fallback.generationId ||
								    prepared.value->assetId != model.fallback.assetId) {
									finishPrecheck(
										FallbackReadiness::Unknown,
										QStringLiteral("Fallback.Incomplete"));
									return;
								}
								if (prepared.value->status != QStringLiteral("ready")) {
									finishPrecheck(
										FallbackReadiness::Unavailable,
										QStringLiteral(
											"Fallback.GenerationNotReady"));
									return;
								}
								readVariants();
							});
					});
			});
		return true;
	}

	// Проверяет свежий каталог и полное отсутствие активной Session для нового выбранного запуска.
	bool selectedDestinationTarget(const QString &destinationId) const
	{
		// snapshot содержит независимые подтверждения каталога и поиска Session выбранного Stream.
		const auto &snapshot = state.dockServer();
		return validId(destinationId) &&
		       snapshot.destinationsStatus.availability == RuntimeAvailability::Fresh &&
		       snapshot.destinationsStatus.complete && snapshot.destinations.contains(destinationId) &&
		       snapshot.sessionStatus.availability == RuntimeAvailability::Fresh &&
		       snapshot.sessionStatus.complete &&
		       (!snapshot.session || snapshot.session->state == QStringLiteral("STOPPED") ||
			snapshot.session->state == QStringLiteral("FAILED") ||
			snapshot.session->state == QStringLiteral("INTERRUPTED"));
	}

	// Допускает только первый POST того же выбранного назначения и той же завершённой конфигурационной эпохи.
	bool selectedStartAllowed() const
	{
		return live && selectedStart && !stopNewSteps && configurationSettled() &&
		       state.connectionStatus() == ConnectionStatus::Connected && state.selectionAvailable() &&
		       selectedStart->configurationGeneration == state.configurationGeneration() &&
		       context == state.dockContext() && selectedDestinationTarget(live->destinationId);
	}

	// Читает полный набор вариантов current и сравнивает его только с профилем выбранной Group.
	void readSelectedVariants()
	{
		// query закрепляет точное поколение на всех страницах ограниченного обхода.
		api::ListQuery query;
		query.generationId = selectedStart->generationId;
		// expected ограждает список и последующее доказательство отсутствия Session от другого намерения.
		const auto expected = stepToken;
		// Обработчик отвергает неполноту, дубли и чужие поколения; READY сам по себе ещё не завершает запуск.
		api.getAllVariants(
			selectedStart->assetId, &owner,
			[this, expected](auto, api::Result<api::Collection<api::Variant>> result) {
				if (!current(expected) || !busy)
					return;
				if (!result || !result.value->complete) {
					finish(OperationOutcome::Unknown, QStringLiteral("Fallback.Incomplete"));
					return;
				}
				// ids обнаруживает повтор идентичности между страницами.
				QSet<QString> ids;
				// compatible означает наличие полного совпадения рецепта и формата выбранной Group.
				bool compatible = false;
				// variant проверяется на точные Asset/current и полную совместимость.
				for (const auto &variant : result.value->items) {
					if (!validId(variant.id) || ids.contains(variant.id) ||
					    variant.assetId != selectedStart->assetId ||
					    variant.generationId != selectedStart->generationId) {
						finish(OperationOutcome::Unknown,
						       QStringLiteral("Fallback.Incomplete"));
						return;
					}
					ids.insert(variant.id);
					compatible =
						compatible ||
						(variant.state == QStringLiteral("READY") &&
						 api::sameProfile(variant.profile, selectedStart->profile) &&
						 api::compatibleMeasuredFormat(variant.format, selectedStart->profile));
				}
				if (!compatible) {
					finish(OperationOutcome::Failed,
					       QStringLiteral("Fallback.NoCompatibleVariant"));
					return;
				}
				checkNoActiveSession();
			},
			std::move(query));
	}

	// Читает только опубликованное current поколение Asset выбранной политики, без подготовки или записи.
	void readSelectedAsset()
	{
		// expected связывает Asset и Generation с исходным выбранным назначением.
		const auto expected = stepToken;
		// Обработчик не принимает pending вместо отсутствующего current.
		api.getAsset(selectedStart->assetId, &owner, [this, expected](auto, api::Result<api::Asset> asset) {
			if (!current(expected) || !busy)
				return;
			if (!asset || asset.value->id != selectedStart->assetId) {
				finish(OperationOutcome::Unknown, QStringLiteral("Fallback.Incomplete"));
				return;
			}
			if (!validId(asset.value->currentGenerationId)) {
				finish(OperationOutcome::Failed, QStringLiteral("Fallback.NoCurrentGeneration"));
				return;
			}
			selectedStart->generationId = asset.value->currentGenerationId;
			// Обработчик подтверждает принадлежность и готовность точного опубликованного поколения.
			api.getGeneration(selectedStart->assetId, selectedStart->generationId, &owner,
					  [this, expected](auto, api::Result<api::Generation> prepared) {
						  if (!current(expected) || !busy)
							  return;
						  if (!prepared || prepared.value->id != selectedStart->generationId ||
						      prepared.value->assetId != selectedStart->assetId) {
							  finish(OperationOutcome::Unknown,
								 QStringLiteral("Fallback.Incomplete"));
							  return;
						  }
						  if (prepared.value->status != QStringLiteral("ready")) {
							  finish(OperationOutcome::Failed,
								 QStringLiteral("Fallback.GenerationNotReady"));
							  return;
						  }
						  readSelectedVariants();
					  });
		});
	}

	// Читает назначенную политику выбранного Stream; отсутствие политики отличается от ошибки чтения.
	void readSelectedPolicy()
	{
		// expected связывает ответ политики с текущим частным доказательством.
		const auto expected = stepToken;
		// Обработчик сохраняет лишь ID Asset; секретные серверные тексты в результат не попадают.
		api.getFallbackPolicy(
			context.streamId, &owner, [this, expected](auto, api::Result<api::PolicyResult> policy) {
				if (!current(expected) || !busy)
					return;
				if (!policy || policy.value->fallback.isMissing()) {
					finish(OperationOutcome::Unknown, QStringLiteral("Fallback.Incomplete"));
					return;
				}
				if (!policy.value->fallback || !validId(policy.value->fallback->assetId)) {
					finish(OperationOutcome::Failed, QStringLiteral("Fallback.NoPolicy"));
					return;
				}
				selectedStart->assetId = policy.value->fallback->assetId;
				readSelectedAsset();
			});
	}

	// Читает выбранный Destination независимо от enabled, затем его точные Group и полный Profile.
	void readSelectedDestination()
	{
		// expected ограничивает всю цепочку исходным контекстом и одним выбранным ID.
		const auto expected = stepToken;
		// Обработчик подтверждает принадлежность назначения Stream перед чтением связанной Group.
		api.getDestination(
			live->destinationId, &owner, [this, expected](auto, api::Result<api::Destination> destination) {
				if (!current(expected) || !busy)
					return;
				if (!destination || destination.value->id != live->destinationId ||
				    destination.value->streamId != context.streamId ||
				    !validId(destination.value->groupId)) {
					finish(batch && destination.error && destination.error->httpStatus == 404
						       ? OperationOutcome::Failed
						       : OperationOutcome::Unknown,
					       QStringLiteral("Operation.InvalidTarget"));
					return;
				}
				// groupId закрепляет связь прочитанного назначения до следующего ответа.
				const auto groupId = destination.value->groupId;
				// Обработчик подтверждает точную Group и фиксирует ID её полного профиля.
				api.getGroup(
					groupId, &owner,
					[this, expected, groupId](auto, api::Result<api::Group> group) {
						if (!current(expected) || !busy)
							return;
						if (!group || group.value->id != groupId ||
						    group.value->streamId != context.streamId ||
						    !validId(group.value->profileId)) {
							finish(batch && group.error && group.error->httpStatus == 404
								       ? OperationOutcome::Failed
								       : OperationOutcome::Unknown,
							       QStringLiteral("Fallback.Incomplete"));
							return;
						}
						// profileId адресует профиль этой Group, а не текущий выбор редактора.
						const auto profileId = group.value->profileId;
						// Обработчик сохраняет рецепт для сравнения с current, не изменяя общий FallbackPrecheck.
						api.getProfile(
							profileId, &owner,
							[this, expected, profileId](auto,
										    api::Result<api::Profile> profile) {
								if (!current(expected) || !busy)
									return;
								if (!profile || profile.value->id != profileId) {
									finish(batch && profile.error &&
											       profile.error->httpStatus ==
												       404
										       ? OperationOutcome::Failed
										       : OperationOutcome::Unknown,
									       QStringLiteral("Fallback.Incomplete"));
									return;
								}
								selectedStart->profile = profile.value->profile;
								readSelectedPolicy();
							});
					});
			});
	}

	// Запускает ровно один подтверждённый Destination при отсутствии активной Session, не вызывая OBS.
	bool startDestination(const DockContext &expected, const QString &destinationId)
	{
		if (!available(expected) || !configurationSettled() || !selectedDestinationTarget(destinationId))
			return false;
		begin(OperationAction::StartDestination);
		selectedStart = SelectedStartProof{};
		selectedStart->configurationGeneration = state.configurationGeneration();
		prepareSessionStep(OperationAction::StartDestination);
		live->destinationId = destinationId;
		live->destinations = {destinationId};
		model.current->destinationId = destinationId;
		model.current->phase = OperationPhase::Prechecking;
		if (publish())
			readSelectedDestination();
		return true;
	}

	// Повторно подтверждает отсутствие активной Session тремя полными небольшими фильтрованными чтениями.
	void checkNoActiveSession(int index = 0)
	{
		if (index == 3) {
			if (stopNewSteps || state.connectionStatus() != ConnectionStatus::Connected) {
				finish(OperationOutcome::Unknown, QStringLiteral("Operation.RemainingStopped"));
				return;
			}
			sendPost();
			return;
		}
		// states задаёт все активные/неочищенные состояния; query ограничивает страницу двумя кандидатами.
		const QStringList states{QStringLiteral("STARTING"), QStringLiteral("RUNNING"),
					 QStringLiteral("STOPPING")};
		api::ListQuery query;
		query.limit = 2;
		query.state = states[index];
		// expected связывает последовательные страницы с неизменным start шагом.
		const auto expected = stepToken;
		// callback не интерпретирует появившийся чужой Session как успех собственного запуска.
		api.getStreamSessions(
			context.streamId, &owner,
			[this, expected, index](auto, api::Result<api::Page<api::Session>> result) {
				if (!current(expected) || !busy)
					return;
				if (!result || !result.value->nextCursor.isEmpty()) {
					if (batch)
						stopNewSteps = true;
					finish(OperationOutcome::Unknown, QStringLiteral("Operation.StateUnconfirmed"));
					return;
				}
				if (!result.value->items.isEmpty()) {
					// guard допускает refresh после синхронного уведомления finish.
					const QPointer<RestreamController> guard = &owner;
					if (batch)
						stopNewSteps = true;
					finish(OperationOutcome::Failed, QStringLiteral("Operation.Conflict"));
					if (guard)
						connection.refreshState();
					return;
				}
				checkNoActiveSession(index + 1);
			},
			std::move(query));
	}

	// Начинает fallback только из подтверждённой проверки с непустым фиксированным набором IDs.
	bool startFallback(const DockContext &expected, quint64 revision)
	{
		// session содержит последнее точное чтение; только известное terminal-состояние разрешает новый запуск.
		const auto &session = state.dockServer().session;
		// noActive не превращает неизвестное будущее состояние Session в подтверждённое отсутствие эфира.
		const bool noActive = !session || session->state == QStringLiteral("STOPPED") ||
				      session->state == QStringLiteral("FAILED") ||
				      session->state == QStringLiteral("INTERRUPTED");
		if (!available(expected) || !configurationSettled() || model.fallback.context != expected ||
		    model.fallback.revision != revision ||
		    model.fallback.configurationGeneration != state.configurationGeneration() ||
		    model.fallback.readiness != FallbackReadiness::Ready || model.fallback.destinations.isEmpty() ||
		    state.dockServer().sessionStatus.availability != RuntimeAvailability::Fresh ||
		    !state.dockServer().sessionStatus.complete || !noActive)
			return false;
		begin(OperationAction::StartFallback);
		prepareSessionStep(OperationAction::StartFallback);
		// destination переносит лишь уже показанный ID, никогда динамический пустой набор backend.
		for (const auto &destination : model.fallback.destinations)
			live->destinations.append(destination.id);
		if (publish())
			checkNoActiveSession();
		return true;
	}

	// Выбирает первый сохранённый Unknown в исходном порядке; одно нажатие исполняет только прежний шаг.
	bool retryBatchUnknown(const DockContext &expected)
	{
		if (stopped || busy || explicitDisconnect || !expected.valid() || expected != context ||
		    expected != state.dockContext() || !model.current || model.current->context != expected ||
		    !model.current->retryAvailable)
			return false;
		// index выбирает ровно одну неизвестную часть, не формируя нового намерения строки.
		for (qsizetype index = 0; index < batchUnknown.size(); ++index) {
			if (!batchUnknown[index])
				continue;
			++stepToken;
			live = batchUnknown[index]->step;
			// Явная попытка сохраняет request_id, но имеет собственный результат доставки отказа.
			live->failureNotified = false;
			// receipt сохраняет прежнюю собственную квитанцию до сброса публичных полей шага.
			const auto receipt = batchUnknown[index]->receipt;
			batchUnknown[index].reset();
			busy = true;
			batchRetryOnly = true;
			stopNewSteps = true;
			model.current->remainingStopped = true;
			model.current->outcome = OperationOutcome::Pending;
			model.current->messageKey.clear();
			exposeStep();
			model.current->destinationId = live->destinationId;
			model.current->receipt = receipt;
			model.current->retryTargetName = model.current->outputs[index].name;
			model.current->phase = live->commandId.isEmpty() ? OperationPhase::Sending
									 : OperationPhase::PollingReceipt;
			armDeadline();
			if (!publish())
				return true;
			if (live->commandId.isEmpty())
				validateRetryTarget();
			else
				pollCurrent();
			return true;
		}
		return false;
	}

	// Повторяет один неизвестный RAM шаг с теми же ID/payload и новым сроком, не размораживая пакет.
	bool retryUnknown(const DockContext &expected)
	{
		if (batch)
			return retryBatchUnknown(expected);
		if (stopped || busy || explicitDisconnect || !expected.valid() || expected != context ||
		    expected != state.dockContext() || !model.current || model.current->context != expected || !live ||
		    !model.current->retryAvailable || model.current->outcome != OperationOutcome::Unknown)
			return false;
		++stepToken;
		busy = true;
		live->failureNotified = false;
		stopNewSteps = true;
		model.current->remainingStopped = true;
		model.current->outcome = OperationOutcome::Pending;
		model.current->messageKey.clear();
		model.current->retryAvailable = false;
		if (live->resultIndex >= 0)
			model.current->outputs[live->resultIndex].outcome = OperationOutcome::Pending;
		model.current->phase = live->commandId.isEmpty() ? OperationPhase::Sending
								 : OperationPhase::PollingReceipt;
		armDeadline();
		if (!publish())
			return true;
		if (!live->commandId.isEmpty()) {
			pollCurrent();
			return true;
		}
		// Без command ID повторяется лишь исходный payload. У start Session уже может существовать
		// после потерянного ответа, поэтому проверяется исходный Stream без требования отсутствия новой Session.
		validateRetryTarget();
		return true;
	}

	// Проверяет цель неизвестного шага; команда уже могла исполниться, поэтому условия первого запуска неприменимы.
	void validateRetryTarget()
	{
		// expected связывает проверку с новым ограниченным ожиданием того же запроса.
		const auto expected = stepToken;
		if (live->action == OperationAction::StartFallback ||
		    live->action == OperationAction::StartDestination) {
			// callback проверяет исходный Stream; явный POST сохраняет request_id и тот же набор назначений.
			api.getStream(context.streamId, &owner, [this, expected](auto, api::Result<api::Stream> stream) {
				if (!current(expected) || !busy)
					return;
				if (!stream || stream.value->id != context.streamId) {
					finish(OperationOutcome::Unknown, QStringLiteral("Operation.InvalidTarget"));
					return;
				}
				sendPost();
			});
			return;
		}
		// callback не разрешает повтор перенаправленной/неизвестной Session.
		api.getSession(live->sessionId, &owner, [this, expected](auto, api::Result<api::Session> result) {
			if (!current(expected) || !busy)
				return;
			if (!result || result.value->id != live->sessionId ||
			    result.value->streamId != context.streamId || result.value->stream.id != context.streamId) {
				finish(OperationOutcome::Unknown, QStringLiteral("Operation.InvalidTarget"));
				return;
			}
			if (live->outputId.isEmpty()) {
				sendPost();
				return;
			}
			// callback сверяет родительскую Session выхода; изменённый desired допустим после потерянного успешного ответа.
			api.getOutput(live->sessionId, live->outputId, &owner,
				      [this, expected](auto, api::Result<api::OutputDetail> output) {
					      if (!current(expected) || !busy)
						      return;
					      if (!output || output.value->id != live->outputId ||
						  output.value->sessionId != live->sessionId ||
						  output.value->destination.streamId != context.streamId) {
						      finish(OperationOutcome::Unknown,
							     QStringLiteral("Operation.InvalidTarget"));
						      return;
					      }
					      sendPost();
				      });
		});
	}

	// Запрещает новые callbacks до отмены; не обращается к OBS или внешнему серверу при shutdown.
	void shutdown()
	{
		if (stopped)
			return;
		stopped = true;
		++generation;
		++stepToken;
		batchUnknown.clear();
		batch.reset();
		deadline.stop();
		poll.stop();
		api.cancelAll();
		busy = false;
		live.reset();
		plan.clear();
	}
};

// Создаёт контроллер с обычными конечными сроками продукта.
RestreamController::RestreamController(StateStore &state, api::APIClient &commandApi, ConnectionController &connection,
				       OBSController &obs, QObject *parent)
	: RestreamController(state, commandApi, connection, obs, Limits{}, parent)
{
}

// Создаёт единственную реализацию; зависимости не переходят во владение фасада.
RestreamController::RestreamController(StateStore &state, api::APIClient &commandApi, ConnectionController &connection,
				       OBSController &obs, Limits limits, QObject *parent)
	: QObject(parent),
	  impl_(std::make_unique<Impl>(*this, state, commandApi, connection, obs, limits))
{
}

// До разрушения реализации отменяет таймеры и принадлежащие command API запросы.
RestreamController::~RestreamController()
{
	shutdown();
}

// Передаёт явное намерение предварительной проверки точному контексту.
bool RestreamController::checkFallback(const DockContext &context)
{
	return impl_->checkFallback(context);
}

// Немедленно отзывает прежнюю пригодность без отмены командного транспорта.
void RestreamController::invalidateFallback()
{
	impl_->invalidateFallback();
}

// Объединяет запросы одной свежей проверки после конфигурационных изменений.
void RestreamController::refreshFallbackAfterConfiguration()
{
	impl_->refreshFallbackAfterConfiguration();
}

// Передаёт прикрепление точного назначения к исходной RUNNING Session.
bool RestreamController::addOutput(const DockContext &context, const QString &sessionId, const QString &destinationId)
{
	return impl_->addOutput(context, sessionId, destinationId);
}

// Передаёт выбранный Destination отдельному workflow с приватной предварительной проверкой.
bool RestreamController::startDestination(const DockContext &context, const QString &destinationId)
{
	return impl_->startDestination(context, destinationId);
}

// Запускает только показанную пользователю редакцию предварительной проверки.
bool RestreamController::startFallback(const DockContext &context, quint64 revision)
{
	return impl_->startFallback(context, revision);
}

// Передаёт серверную паузу, не воздействуя на локальный OBS output.
bool RestreamController::pause(const DockContext &context, const QString &sessionId)
{
	return impl_->sessionAction(context, sessionId, OperationAction::Pause);
}

// Передаёт серверное снятие паузы исходной Session.
bool RestreamController::resume(const DockContext &context, const QString &sessionId)
{
	return impl_->sessionAction(context, sessionId, OperationAction::Resume);
}

// Передаёт двухчастную остановку с отдельными локальным и серверным результатами.
bool RestreamController::stopAll(const DockContext &context, const QString &sessionId)
{
	return impl_->stopAll(context, sessionId);
}

// Допускает только четыре явно перечисленных действия одного Output.
bool RestreamController::outputActions(const DockContext &context, const QString &sessionId, const QString &outputId,
				       OperationAction action)
{
	if (action != OperationAction::StartOutput && action != OperationAction::StopOutput &&
	    action != OperationAction::RestartOutput && action != OperationAction::DetachOutput)
		return false;
	return impl_->startOutputs(context, sessionId, {outputId}, action);
}

// Передаёт точный состав прикреплённых остановленных выходов.
bool RestreamController::startStopped(const DockContext &context, const QString &sessionId, const QStringList &ids)
{
	return impl_->startOutputs(context, sessionId, ids, OperationAction::StartStopped);
}

// Передаёт точный состав restart пакета без нового API маршрута.
bool RestreamController::restartAll(const DockContext &context, const QString &sessionId, const QStringList &ids)
{
	return impl_->startOutputs(context, sessionId, ids, OperationAction::RestartAll);
}

// Передаёт весь замороженный показанный список единственному последовательному исполнителю.
bool RestreamController::controlOutputs(const OutputBatchRequest &request, OperationAction action)
{
	return impl_->controlOutputs(request, action);
}

// Повторяет лишь сохранённый живой неизвестный шаг после нового явного намерения.
bool RestreamController::retryUnknown(const DockContext &context)
{
	return impl_->retryUnknown(context);
}

// Идемпотентно завершает реализацию до её заимствованных зависимостей.
void RestreamController::shutdown()
{
	if (impl_)
		impl_->shutdown();
}

} // namespace gost
