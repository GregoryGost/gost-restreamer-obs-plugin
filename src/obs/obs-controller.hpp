#pragma once

#include "obs-stats-collector.hpp"

#include <QObject>
#include <QTimer>

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
	OBSController(Frontend &frontend, StateStore &stateStore, QObject *parent = nullptr);
	~OBSController() override;
	// В GUI-потоке обновляет OBS state/статистику; профильные события выдают profileObserved.
	// reliableProfile=false сохраняет лишь отображаемое имя, запрещая подтверждать идентичность
	// по пути/inventory, захваченным уже после переноса исходного события. Exit вызывает shutdown.
	void handleEvent(FrontendEvent event, bool reliableProfile = true);
	// Останавливает таймер и освобождает подписки статистики; повторный вызов не обращается к OBS.
	void shutdown() noexcept;

signals:
	void profileObserved(gost::FrontendEvent event, const gost::ProfileDescriptor &profile);

private:
	enum class Lifecycle { AwaitingFrontend, Ready, Stopped };

	// Вызывает адаптер только в рабочем состоянии; общий источник steady_clock задаёт фактический
	// интервал bitrate. Записывает готовое значение в StateStore, не меняя серверную телеметрию.
	void publishStatistics(bool active, std::optional<FrontendEvent> event = std::nullopt);

	Frontend &frontend_;
	StateStore &stateStore_;
	OBSStatsCollector statsCollector_;
	QTimer statsTimer_;
	Lifecycle lifecycle_ = Lifecycle::AwaitingFrontend;
};

} // namespace gost
