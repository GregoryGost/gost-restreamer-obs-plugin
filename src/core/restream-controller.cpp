#include "restream-controller.hpp"

#include "api/api-client.hpp"
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

// Сравнивает полный формат, включая байтовые ProfileText и точную рациональную частоту кадров.
bool sameFormat(const api::Format &a, const api::Format &b)
{
	return std::tie(a.video.codec, a.video.width, a.video.height, a.video.frameRate.numerator,
			a.video.frameRate.denominator, a.video.codecProfile, a.video.level, a.video.tier,
			a.video.pixelFormat, a.video.bitDepth, a.video.color.range, a.video.color.primaries,
			a.video.color.transfer, a.video.color.matrix, a.audio.codec, a.audio.codecProfile,
			a.audio.sampleRateHz, a.audio.channels, a.audio.channelLayout) ==
	       std::tie(b.video.codec, b.video.width, b.video.height, b.video.frameRate.numerator,
			b.video.frameRate.denominator, b.video.codecProfile, b.video.level, b.video.tier,
			b.video.pixelFormat, b.video.bitDepth, b.video.color.range, b.video.color.primaries,
			b.video.color.transfer, b.video.color.matrix, b.audio.codec, b.audio.codecProfile,
			b.audio.sampleRateHz, b.audio.channels, b.audio.channelLayout);
}

// Сравнивает рецепт encoding без подстановки значений для missing/null вложенных настроек.
bool sameEncoding(const api::Encoding &a, const api::Encoding &b)
{
	if (std::tie(a.version, a.videoEncoder, a.audioEncoder, a.rateControl, a.rescaleOutput, a.videoCustom,
		     a.audioCustom, a.nvenc.state, a.svtAv1.state) !=
	    std::tie(b.version, b.videoEncoder, b.audioEncoder, b.rateControl, b.rescaleOutput, b.videoCustom,
		     b.audioCustom, b.nvenc.state, b.svtAv1.state))
		return false;
	if (a.nvenc && std::tie(a.nvenc->preset, a.nvenc->tuning, a.nvenc->multipass, a.nvenc->lookaheadFrames,
				a.nvenc->enableSpatialAq, a.nvenc->enableTemporalAq, a.nvenc->aqStrength) !=
			       std::tie(b.nvenc->preset, b.nvenc->tuning, b.nvenc->multipass, b.nvenc->lookaheadFrames,
					b.nvenc->enableSpatialAq, b.nvenc->enableTemporalAq, b.nvenc->aqStrength))
		return false;
	return !a.svtAv1 ||
	       std::tie(a.svtAv1->preset, a.svtAv1->tuning, a.svtAv1->aqMode, a.svtAv1->lookaheadFrames) ==
		       std::tie(b.svtAv1->preset, b.svtAv1->tuning, b.svtAv1->aqMode, b.svtAv1->lookaheadFrames);
}

// Полное структурное равенство профиля: liveAudioMode Missing не эквивалентен явному copy.
bool sameProfile(const api::GroupProfile &a, const api::GroupProfile &b)
{
	return std::tie(a.mode, a.liveAudioMode.state, a.videoEncoding.targetBitrateBps, a.videoEncoding.maxBitrateBps,
			a.videoEncoding.bufferBits, a.videoEncoding.gopFrames, a.videoEncoding.maxBFrames,
			a.videoEncoding.closedGop, a.audioEncoding.bitrateBps, a.encoding.state) ==
		       std::tie(b.mode, b.liveAudioMode.state, b.videoEncoding.targetBitrateBps,
				b.videoEncoding.maxBitrateBps, b.videoEncoding.bufferBits, b.videoEncoding.gopFrames,
				b.videoEncoding.maxBFrames, b.videoEncoding.closedGop, b.audioEncoding.bitrateBps,
				b.encoding.state) &&
	       (!a.liveAudioMode || *a.liveAudioMode == *b.liveAudioMode) && sameFormat(a.format, b.format) &&
	       (!a.encoding || sameEncoding(*a.encoding, *b.encoding));
}

// Возвращает точное backend kind для разрешённой команды; None не может попасть в POST.
QString commandKind(OperationAction action)
{
	switch (action) {
	case OperationAction::StartFallback:
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
 * Ведёт один RAM workflow и один живой шаг. Поколение меняется до отмены HTTP; каждый callback
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
		// timeout завершает живой шаг неизвестностью, не повторяя POST.
		QObject::connect(&deadline, &QTimer::timeout, &owner,
				 [this] { finish(OperationOutcome::Unknown, QStringLiteral("Operation.Timeout")); });
		// poll повторяет только разрешённый GET текущего шага.
		QObject::connect(&poll, &QTimer::timeout, &owner, [this] { pollCurrent(); });
		// Изменения наблюдений прерывают только новые шаги; известную квитанцию можно дочитать без SSE.
		QObject::connect(&state, &StateStore::serverStateChanged, &owner, [this] { observeState(); });
		// Конфигурационная запись отзывает Ready, но не отменяет уже принятые команды.
		QObject::connect(&state, &StateStore::configurationInvalidated, &owner,
				 [this](quint64) { invalidateFallback(); });
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
		// outputId адресует Output внутри исходной Session; пуст у Session команды.
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
		// destinationId фиксирует payload прикрепления; возвращённый Output имеет отдельный ID.
		QString destinationId;
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
	// live удерживает единственный payload неизвестного шага.
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
	// Одна отложенная проверка объединяет несколько успешных конфигурационных изменений.
	bool deferredPrecheck = false;

	// Не допускает проверку пригодности до завершения записи и сверки её неизвестного результата.
	bool configurationSettled() const
	{
		return !state.configurationEditor().writing && !state.configurationEditor().reconciliationRequired;
	}

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

	// Проверяет поколение и логический выбор; статус SSE здесь намеренно не участвует.
	bool current(quint64 expected) const
	{
		return !stopped && generation == expected && model.current && model.current->context == context &&
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
		busy = true;
		stopNewSteps = false;
		replacementSessionObserved = false;
		live.reset();
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
			if (live && live->resultIndex >= 0)
				model.current->outputs[live->resultIndex].outcome =
					live->sent ? OperationOutcome::Unknown : OperationOutcome::NotSent;
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
		const bool replacement = state.dockServer().sessionStatus.availability == RuntimeAvailability::Fresh &&
					 state.dockServer().session && !model.current->sessionId.isEmpty() &&
					 state.dockServer().session->id != model.current->sessionId;
		replacementSessionObserved = replacementSessionObserved || replacement;
		// Смена Session запрещает следующие шаги; квитанция команды и исходное состояние остаются читаемыми.
		if (state.connectionStatus() != ConnectionStatus::Connected ||
		    (state.dockServer().sessionStatus.availability == RuntimeAvailability::Fresh &&
		     !model.current->sessionId.isEmpty() &&
		     ((state.dockServer().session && state.dockServer().session->id != model.current->sessionId) ||
		      (!state.dockServer().session && state.dockServer().sessionStatus.complete &&
		       model.current->action != OperationAction::StopAll)))) {
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
	void armDeadline() { deadline.start(std::max(1, limits.stepTimeoutMs)); }

	// Планирует только чтение текущего command/state и не создаёт повторной команды.
	void schedulePoll()
	{
		if (busy && live)
			poll.start(std::max(1, limits.pollIntervalMs));
	}

	// Проверяет точную принадлежность квитанции. У start Session ID узнаётся из первого ответа.
	bool matchingReceipt(const api::Receipt &receipt) const
	{
		if (!live || !validId(receipt.id) || receipt.requestId != live->requestId ||
		    receipt.kind != commandKind(live->action) ||
		    (!live->commandId.isEmpty() && receipt.id != live->commandId))
			return false;
		if (live->action == OperationAction::StartFallback)
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
		if (live->action == OperationAction::StartFallback && !receipt.sessionId.isEmpty()) {
			live->sessionId = receipt.sessionId;
			model.current->sessionId = receipt.sessionId;
		}
		model.current->commandId = receipt.id;
		model.current->receipt = receipt;
		if (receipt.status == QStringLiteral("FAILED") || receipt.status == QStringLiteral("INTERRUPTED")) {
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
		const auto expected = generation;
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
		// expected ограждает результат от вытеснения Stop All/контекстом.
		const auto expected = generation;
		// callback принимает квитанцию либо завершает неоднозначную потерю ответа без нового POST.
		auto callback = [this, expected](auto, api::Result<api::Receipt> result) {
			if (!current(expected) || !busy)
				return;
			if (!result) {
				// conflict означает определённый отказ; перечитывание не выдаёт чужой start за собственный.
				const bool conflict = result.error && result.error->httpStatus == 409;
				// guard защищает продолжение после публикации результата и refreshState.
				const QPointer<RestreamController> guard = &owner;
				finish(conflict ? OperationOutcome::Failed : OperationOutcome::Unknown,
				       conflict ? QStringLiteral("Operation.Conflict")
						: QStringLiteral("Operation.Unknown"));
				if (guard && conflict)
					connection.refreshState();
				return;
			}
			acceptReceipt(std::move(*result.value));
		};
		switch (live->action) {
		case OperationAction::StartFallback:
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

	// Подтверждает точную сохранённую Session/Output, не устанавливая приватные GET в runtime.
	void readOracle()
	{
		if (!busy || !live)
			return;
		// expected ограничивает оба варианта оракула текущим workflow.
		const auto expected = generation;
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
				 result.value->state == QStringLiteral("INTERRUPTED"))
				finish(OperationOutcome::Failed, QStringLiteral("Operation.CommandFailed"));
			else
				schedulePoll();
		});
	}

	// Завершает шаг и разрешает следующий только при непрерывно пригодном исходном контексте.
	void stepCompleted()
	{
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
		const auto expected = generation;
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
		const auto expected = generation;
		// callback не разрешает команду новой/завершённой Session на основании старого UI снимка.
		api.getSession(live->sessionId, &owner, [this, expected](auto, api::Result<api::Session> result) {
			if (!current(expected) || !busy)
				return;
			if (!result || result.value->id != live->sessionId ||
			    result.value->streamId != context.streamId || result.value->stream.id != context.streamId ||
			    result.value->state != QStringLiteral("RUNNING")) {
				finish(OperationOutcome::Failed, QStringLiteral("Operation.InvalidTarget"));
				return;
			}
			// callback проверяет прикрепление и допускает start только при desiredRunning=false.
			api.getOutput(live->sessionId, live->outputId, &owner,
				      [this, expected](auto, api::Result<api::OutputDetail> output) {
					      if (!current(expected) || !busy)
						      return;
					      if (!output || output.value->id != live->outputId ||
						  output.value->sessionId != live->sessionId ||
						  output.value->destination.streamId != context.streamId ||
						  !output.value->attached ||
						  (live->action == OperationAction::StartOutput &&
						   output.value->desiredRunning)) {
						      finish(OperationOutcome::Failed,
							     QStringLiteral("Operation.InvalidTarget"));
						      return;
					      }
					      if (stopNewSteps ||
						  state.connectionStatus() != ConnectionStatus::Connected) {
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
		const auto expected = generation;
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
		const auto expected = generation;
		// Обработчик подтверждает Session, не заменяя её новым текущим эфиром.
		api.getSession(live->sessionId, &owner, [this, expected](auto, api::Result<api::Session> session) {
			if (!current(expected) || !busy)
				return;
			if (!session || session.value->id != live->sessionId ||
			    session.value->streamId != context.streamId ||
			    session.value->stream.id != context.streamId ||
			    session.value->state != QStringLiteral("RUNNING")) {
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
								    !sessionTarget(live->sessionId, true) ||
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
		if (!available(expected) || !sessionTarget(sessionId, true) || !validId(destinationId))
			return false;
		begin(OperationAction::AddOutput, sessionId);
		prepareSessionStep(OperationAction::AddOutput);
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
		const auto expectedGeneration = generation;
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
		const auto expected = generation;
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
								       sameProfile(variant.profile,
										   profile.value->profile) &&
								       sameFormat(variant.format,
										  profile.value->profile.format);
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
		const auto expected = generation;
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
		const auto expected = generation;
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
		const auto expected = generation;
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
		const auto expected = generation;
		// callback не интерпретирует появившийся чужой Session как успех собственного запуска.
		api.getStreamSessions(
			context.streamId, &owner,
			[this, expected, index](auto, api::Result<api::Page<api::Session>> result) {
				if (!current(expected) || !busy)
					return;
				if (!result || !result.value->nextCursor.isEmpty()) {
					finish(OperationOutcome::Unknown, QStringLiteral("Operation.StateUnconfirmed"));
					return;
				}
				if (!result.value->items.isEmpty()) {
					// guard допускает refresh после синхронного уведомления finish.
					const QPointer<RestreamController> guard = &owner;
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

	// Повторяет один неизвестный RAM шаг с теми же ID/payload и новым сроком, не размораживая пакет.
	bool retryUnknown(const DockContext &expected)
	{
		if (stopped || busy || explicitDisconnect || !expected.valid() || expected != context ||
		    expected != state.dockContext() || !model.current || model.current->context != expected || !live ||
		    !model.current->retryAvailable || model.current->outcome != OperationOutcome::Unknown)
			return false;
		busy = true;
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
		const auto expected = generation;
		if (live->action == OperationAction::StartFallback) {
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
