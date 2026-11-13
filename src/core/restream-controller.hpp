#pragma once

#include "core/operations-state.hpp"

#include <QObject>
#include <memory>

namespace gost {

class StateStore;
class ConnectionController;
class OBSController;
namespace api {
class APIClient;
}

/**
 * Последовательно выполняет явные намерения пользователя в GUI event loop.
 * Заимствованные зависимости переживают контроллер; shutdown вызывается до их завершения.
 * Единственный command API не отменяется восстановлением SSE. Приватные GET оракулов не
 * попадают в runtime reducer. После рестарта RAM workflow отсутствует и POST не воспроизводится.
 */
class RestreamController final : public QObject {
	Q_OBJECT
public:
	// Ограниченные интервалы исполнителя; тест может уменьшить их без новой настройки продукта.
	struct Limits {
		// Общий бюджет шага в мс, включая очередь HTTP и подтверждение состояния.
		int stepTimeoutMs = 60000;
		// Пауза между GET квитанции/состояния в мс.
		int pollIntervalMs = 250;
		// Отдельный бюджет подтверждения штатной остановки OBS в мс.
		int obsStopTimeoutMs = 5000;
	};
	// Создаёт неактивный исполнитель с обычными ограничениями, без сетевых команд.
	RestreamController(StateStore &state, api::APIClient &commandApi, ConnectionController &connection,
			   OBSController &obs, QObject *parent = nullptr);
	// Создаёт исполнитель с явными ограничениями для изолированных проверок.
	RestreamController(StateStore &state, api::APIClient &commandApi, ConnectionController &connection,
			   OBSController &obs, Limits limits, QObject *parent = nullptr);
	// Завершает callbacks и таймеры до уничтожения заимствованных сервисов.
	~RestreamController() override;
	// Читает policy/current variants/полные профили; результат публикуется через operationsChanged.
	bool checkFallback(const DockContext &context);
	// Запускает только подтверждённый непустой набор указанной проверки при отсутствии Session.
	bool startFallback(const DockContext &context, quint64 precheckRevision);
	// Адресует паузу точной RUNNING Session; не запускает OBS и не меняет group source напрямую.
	bool pause(const DockContext &context, const QString &sessionId);
	// Снимает desired_paused точной RUNNING Session без команды OBS start.
	bool resume(const DockContext &context, const QString &sessionId);
	// Вытесняет неотправленный остаток пакета, затем отдельно останавливает OBS и исходную Session.
	bool stopAll(const DockContext &context, const QString &sessionId);
	// Принимает StartOutput/StopOutput/RestartOutput/DetachOutput для точных IDs из подтверждения.
	bool outputActions(const DockContext &context, const QString &sessionId, const QString &outputId,
			   OperationAction action);
	// Фиксирует перечисленные Output и последовательно запускает только attached+desired=false.
	bool startStopped(const DockContext &context, const QString &sessionId, const QStringList &outputIds);
	// Фиксирует состав пакета и для каждого Output выполняет stop->оракул->start с разными ID.
	bool restartAll(const DockContext &context, const QString &sessionId, const QStringList &outputIds);
	// Явно повторяет только неизвестный живой шаг с прежними payload/ID и новым ограниченным сроком.
	bool retryUnknown(const DockContext &context);
	// Идемпотентно запрещает новые действия, отменяет принадлежащие запросы и прекращает callbacks.
	void shutdown();

private:
	// Реализация хранит неизменяемый живой шаг и ограниченный план в RAM.
	class Impl;
	// Единственный владелец реализации, без дополнительных QObject родителей.
	std::unique_ptr<Impl> impl_;
};

} // namespace gost
