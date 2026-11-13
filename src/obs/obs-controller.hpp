#pragma once

#include "obs-stats-collector.hpp"

#include <QObject>
#include <QTimer>
#include <QPointer>
#include <QElapsedTimer>
#include <functional>

namespace gost {

class StateStore;

/**
 * Публикует локальные наблюдения OBS и достоверные снимки профиля для ConnectionController.
 * Frontend/StateStore заимствованы и переживают объект. QObject и таймер живут в GUI-потоке.
 * AwaitingFrontend допускает регистрацию без чтения OBS; FinishedLoading включает периодический
 * опрос раз в секунду. События жизненного цикла дополнительно дают немедленную выборку.
 * Stopped необратим и запрещает позднему FinishedLoading снова запустить обращения к OBS.
 */
class OBSController final : public QObject {
	Q_OBJECT

public:
	// Связывает заимствованные frontend/stateStore и подготавливает GUI-таймеры без раннего чтения OBS.
	OBSController(Frontend &frontend, StateStore &stateStore, QObject *parent = nullptr);
	// Останавливает таймеры и освобождает подписки до уничтожения полей.
	~OBSController() override;
	// В GUI-потоке обновляет OBS state/статистику; профильные события выдают profileObserved.
	// reliableProfile=false сохраняет лишь отображаемое имя, запрещая подтверждать идентичность
	// по пути/inventory, захваченным уже после переноса исходного события. Exit вызывает shutdown.
	void handleEvent(FrontendEvent event, bool reliableProfile = true);
	// Останавливает таймер и освобождает подписки статистики; повторный вызов не обращается к OBS.
	void shutdown() noexcept;
	/**
	 * В GUI/Ready запрашивает остановку только текущего поколения запуска OBS и ждёт до timeoutMs (1..5000 мс).
	 * receiver ограничивает время жизни callback: true подтверждает Stopped без active/reconnecting,
	 * false означает недоступность, смену запуска или истечение срока. При shutdown callback подавляется.
	 * Повтор при уже занятом ожидании возвращает false и не изменяет исходную остановку.
	 */
	void stopStreaming(QObject *receiver, std::function<void(bool confirmed)> callback, int timeoutMs = 5000);

signals:
	// Передаёт профильный event и единый снимок profile для разрешения устойчивой идентичности.
	void profileObserved(gost::FrontendEvent event, const gost::ProfileDescriptor &profile);

private:
	// Рабочая фаза контроллера запрещает чтение Frontend до загрузки и после Exit.
	enum class Lifecycle { AwaitingFrontend, Ready, Stopped };
	// Фаза Frontend не позволяет считать временные false-флаги во время Starting подтверждённой остановкой.
	enum class StreamingPhase { Starting, Started, Stopping, Stopped };
	// Проверяет одно ожидание; запрашивает stop лишь после активности исходного поколения.
	void checkStreamingStop();
	// Снимает ожидание до вызова callback с confirmed; получатель может уничтожить контроллер.
	void finishStreamingStop(bool confirmed);
	// Останавливает собственные таймеры и снимает receiver connection без вызова callback.
	void cancelStreamingStop() noexcept;

	// Вызывает адаптер только в рабочем состоянии; общий источник steady_clock задаёт фактический
	// интервал bitrate. Записывает готовое значение в StateStore, не меняя серверную телеметрию.
	void publishStatistics(bool active, std::optional<FrontendEvent> event = std::nullopt);

	// Заимствованный адаптер OBS переживает контроллер.
	Frontend &frontend_;
	// Заимствованный получатель независимых локальных наблюдений OBS.
	StateStore &stateStore_;
	// Сохраняет предыдущую выборку для вычисления интервальных счётчиков.
	OBSStatsCollector statsCollector_;
	// Секундный таймер статистики работает только в Ready.
	QTimer statsTimer_;
	// Необратимый Stopped запрещает позднему FinishedLoading открыть OBS заново.
	Lifecycle lifecycle_ = Lifecycle::AwaitingFrontend;
	// Последняя наблюдаемая фаза Frontend; при первой выборке уточняется по активному выходу.
	StreamingPhase streamingPhase_ = StreamingPhase::Stopped;
	// Событие трансляции могло прийти до FinishedLoading; первая выборка не должна стереть его фазу.
	bool streamingEventObserved_ = false;
	// Номер запуска увеличивается на Starting; старый Stop All не преследует новый пользовательский запуск.
	quint64 streamingGeneration_ = 0;
	// Поколение, зафиксированное единственным текущим ожиданием остановки.
	quint64 stopGeneration_ = 0;
	// Живой получатель результата остановки; контроллер не удерживает его существование.
	QPointer<QObject> stopReceiver_;
	// Обработчик результата; пустое значение означает отсутствие текущего ожидания.
	std::function<void(bool)> stopCallback_;
	// Частые выборки ограничены временем жизни одного stop, а не постоянным фоновым опросом.
	QTimer stopPollTimer_;
	// Абсолютный предел одного ожидания остановки штатного выхода.
	QTimer stopDeadlineTimer_;
	// Монотонное начало ожидания защищает срок, даже если GUI задержал обработку таймеров.
	QElapsedTimer stopElapsed_;
	// Фактический предел ожидания в миллисекундах после ограничения диапазоном 1..5000.
	int stopTimeoutMs_ = 5000;
	// Подписка на уничтожение receiver немедленно прекращает ожидание без дальнейшего stop.
	QMetaObject::Connection stopReceiverConnection_;
	// Запрос уже отправлен для stopGeneration_; повторять void API при каждой выборке не требуется.
	bool stopDispatched_ = false;
};

} // namespace gost
