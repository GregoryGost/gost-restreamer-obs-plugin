#pragma once

#include "obs-stats-collector.hpp"

#include <QObject>
#include <QTimer>

namespace gost {

class StateStore;

class OBSController final : public QObject {
	Q_OBJECT

public:
	OBSController(Frontend &frontend, StateStore &stateStore, QObject *parent = nullptr);
	~OBSController() override;
	void handleEvent(FrontendEvent event, bool reliableProfile = true);
	void shutdown() noexcept;

signals:
	void profileObserved(gost::FrontendEvent event, const gost::ProfileDescriptor &profile);

private:
	enum class Lifecycle { AwaitingFrontend, Ready, Stopped };

	void publishStatistics(bool active, std::optional<FrontendEvent> event = std::nullopt);

	Frontend &frontend_;
	StateStore &stateStore_;
	OBSStatsCollector statsCollector_;
	QTimer statsTimer_;
	Lifecycle lifecycle_ = Lifecycle::AwaitingFrontend;
};

} // namespace gost
