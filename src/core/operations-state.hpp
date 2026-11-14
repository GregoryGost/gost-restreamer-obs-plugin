#pragma once

#include "api/models.hpp"
#include "core/dock-server-state.hpp"

#include <QStringList>
#include <optional>

namespace gost {

// Намерение пользователя; наблюдение OBS само по себе не создаёт ни одного из этих действий.
enum class OperationAction {
	None,
	CheckFallback,
	StartFallback,
	AddOutput,
	Pause,
	Resume,
	StopAll,
	StartOutput,
	StopOutput,
	RestartOutput,
	DetachOutput,
	StartStopped,
	RestartAll
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

// Раздельные результаты одного Output: успех команды не равен успеху публикации площадки.
struct OutputOperationResult {
	// Точная сохранённая идентичность Output.
	QString outputId;
	// Безопасное отображаемое имя из снимка списка.
	QString name;
	// Итог элемента; у restart Completed возможен лишь после stop и start.
	OperationOutcome outcome = OperationOutcome::NotSent;
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
