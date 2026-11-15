#pragma once

#include "api/models.hpp"
#include "core/dock-server-state.hpp"

#include <QStringList>
#include <optional>

namespace gost {

// Намерение пользователя; наблюдение OBS само по себе не создаёт ни одного из этих действий.
enum class OperationAction {
	// None обозначает отсутствие явного пользовательского действия.
	None,
	// CheckFallback проверяет пригодность всех Enabled назначений без запуска.
	CheckFallback,
	// StartFallback запускает зафиксированный набор прежней общей проверки.
	StartFallback,
	// AddOutput прикрепляет одно назначение к уже выбранной RUNNING Session.
	AddOutput,
	// Pause задаёт серверную паузу исходной Session.
	Pause,
	// Resume снимает серверную паузу исходной Session.
	Resume,
	// StopAll останавливает OBS и исходную Session с отдельным подтверждением обеих частей.
	StopAll,
	// StartOutput запускает точный уже прикреплённый остановленный Output.
	StartOutput,
	// StopOutput останавливает выбранный Output без изменения соседних выходов.
	StopOutput,
	// RestartOutput выполняет stop и start того же Output с разными квитанциями.
	RestartOutput,
	// DetachOutput отсоединяет выбранный Output, сохраняя конфигурацию назначения.
	DetachOutput,
	// StartStopped фиксирует и последовательно запускает остановленные прикреплённые Outputs.
	StartStopped,
	// RestartAll фиксирует и последовательно перезапускает выбранные прикреплённые Outputs.
	RestartAll,
	// StartDestination создаёт Session только с одним выбранным назначением без запуска OBS.
	StartDestination,
	// StartAllOutputs запускает только замороженные показанные строки, не прерывая desired=true.
	StartAllOutputs,
	// StopAllOutputs останавливает только выходы, сохраняя Session и передачу OBS.
	StopAllOutputs,
	// RestartAllOutputs выполняет stop/start attached и добавляет ещё не исполняющиеся назначения.
	RestartAllOutputs
};
// Фаза единственного исполнителя; Finished сохраняет результат, но не блокирует новое намерение.
enum class OperationPhase { Idle, Prechecking, Sending, PollingReceipt, ConfirmingState, StoppingObs, Finished };
// Unknown сохраняет неопределённость принятия/завершения; NotSent относится только к неотправленным шагам.
enum class OperationOutcome { None, Pending, Completed, Failed, Unknown, NotSent };
// Пригодность предварительной проверки; Ready остаётся предварительным заключением клиента.
enum class FallbackReadiness { Unknown, Checking, Ready, Unavailable };

// Безопасные подписи и точные ID назначений, показанные пользователю перед запуском.
struct FallbackDestination {
	// ID назначения входит в неизменяемый payload Session start.
	QString id;
	// Имя применяется только для отображения, а не для адресации команды.
	QString name;
};

// Значение последней проверки выбранного Stream; секреты назначения здесь отсутствуют.
struct FallbackPrecheck {
	// Контекст, в котором выполнена цепочка чтений.
	DockContext context;
	// Номер завершённой/текущей проверки для защиты подтверждения в диалоге.
	quint64 revision = 0;
	// Эпоха конфигурации запрещает старой проверке вернуть Ready после начала записи.
	quint64 configurationGeneration = 0;
	// Предварительное заключение, не доказательство целостности медиа на сервере.
	FallbackReadiness readiness = FallbackReadiness::Unknown;
	// assetId адресует проверенный Asset политики.
	QString assetId;
	// generationId адресует точное текущее поколение без подмены подготавливаемым.
	QString generationId;
	// Непустой фиксированный набор включённых назначений для команды запуска.
	QVector<FallbackDestination> destinations;
	// Ключ безопасного сообщения для локализации; полученные failure.message не копируются.
	QString messageKey;
	// READY не подтверждает версию генератора; необходимость rebuild объясняется в справке перед запуском.
	bool rebuildWarning = true;
};

// Адресация и безопасная подпись одной показанной строки; имя не участвует в выборе цели.
struct OutputBatchTarget {
	// outputId адресует исторический attached Output исходной Session; пуст у каталоговой строки.
	QString outputId;
	// destinationId адресует назначение независимо от наличия текущего исполнения.
	QString destinationId;
	// name сохраняет показанную подпись для результатов и явного повтора.
	QString name;
};

// Неизменяемое намерение над всем показанным списком, включая строки за прокруткой.
struct OutputBatchRequest {
	// context фиксирует origin, профиль OBS и выбранный Stream до подтверждения.
	DockContext context;
	// sessionId фиксирует исходную RUNNING Session; пуст при noActiveSession.
	QString sessionId;
	// noActiveSession означает полное свежее подтверждение отсутствия активного эфира.
	bool noActiveSession = false;
	// configurationGeneration запрещает создавать новые снимки после изменения конфигурации.
	quint64 configurationGeneration = 0;
	// targets сохраняет исходный порядок и точные раздельные IDs всех показанных строк.
	QVector<OutputBatchTarget> targets;
};

// Раздельные результаты одного Output: успех команды не равен успеху публикации площадки.
struct OutputOperationResult {
	// Точная сохранённая идентичность Output.
	QString outputId;
	// Безопасное отображаемое имя из снимка списка.
	QString name;
	// Итог элемента; у restart Completed возможен лишь после stop и start.
	OperationOutcome outcome = OperationOutcome::NotSent;
	// destinationId остаётся отдельным адресом назначения даже до появления Output.
	QString destinationId;
	// stopOutcome показывает результат обязательной stop-части; None означает неприменимость.
	OperationOutcome stopOutcome = OperationOutcome::None;
	// startOutcome не становится Completed только из-за успеха предшествующего stop.
	OperationOutcome startOutcome = OperationOutcome::None;
	// noOp отмечает подтверждённое отсутствие необходимости посылать команду этой строки.
	bool noOp = false;
	// messageKey содержит только локальную безопасную причину, без failure.message сервера.
	QString messageKey;
};

// Публичное значение текущей операции; исходные тела POST остаются только в RAM исполнителя.
struct WorkflowResult {
	// Номер намерения отличает поздние callbacks предыдущего workflow.
	quint64 generation = 0;
	// Исходный выбор, который нельзя перенаправлять после подтверждения.
	DockContext context;
	// action задаёт вид явного действия пользователя.
	OperationAction action = OperationAction::None;
	// phase описывает текущую стадию исполнения этого действия.
	OperationPhase phase = OperationPhase::Idle;
	// Общий результат не становится Completed при частичном пакете.
	OperationOutcome outcome = OperationOutcome::None;
	// sessionId адресует исходную Session; start узнаёт её из собственной квитанции.
	QString sessionId;
	// outputId адресует текущий шаг Output внутри исходной Session.
	QString outputId;
	// Destination фиксируется при прикреплении и не подменяется другим назначением после ответа.
	QString destinationId;
	// requestId связывает одинаковые payload при явном ручном повторе.
	QString requestId;
	// commandId адресует проверенную квитанцию живого шага.
	QString commandId;
	// Проверенная квитанция хранится отдельно от подтверждения долговечного состояния.
	std::optional<api::Receipt> receipt;
	// stateConfirmed отдельно подтверждает долговечный оракул после SUCCEEDED.
	bool stateConfirmed = false;
	// obsOutcome подтверждает локальную остановку исходного запуска OBS в Stop All.
	OperationOutcome obsOutcome = OperationOutcome::None;
	// serverOutcome подтверждает серверную очистку исходной Session отдельно от OBS.
	OperationOutcome serverOutcome = OperationOutcome::None;
	// Каждый элемент фиксированного пакета получает самостоятельный итог.
	QVector<OutputOperationResult> outputs;
	// Запрет новых шагов не объявляет уже принятую серверную команду отменённой.
	bool remainingStopped = false;
	// Наличие живого RAM payload допускает явный повтор с тем же request_id.
	bool retryAvailable = false;
	// Имя первой сохранённой Unknown строки поясняет цель следующего явного повтора.
	QString retryTargetName;
	// Безопасная локальная причина для UI.
	QString messageKey;
};

// Ограниченная value-модель: текущий результат и один последний прерванный workflow.
struct OperationsState {
	// Последняя предварительная проверка выбранного Stream.
	FallbackPrecheck fallback;
	// Текущий или последний законченный workflow.
	std::optional<WorkflowResult> current;
	// Вытесненное Stop All либо сменой контекста намерение, без очереди replay.
	std::optional<WorkflowResult> lastInterrupted;
};

} // namespace gost
