#pragma once

#include "obs/frontend.hpp"

#include <QObject>
#include <memory>

namespace gost {

class StateStore;
class SettingsStore;
namespace api {
class APIClient;
class EventClient;
} // namespace api

class ConnectionController final : public QObject {
	Q_OBJECT

public:
	ConnectionController(StateStore &state, SettingsStore &settings, api::APIClient &api, api::EventClient &events,
			     QObject *parent = nullptr);
	~ConnectionController() override;
	void observeProfile(FrontendEvent event, const ProfileDescriptor &descriptor);
	void connectToServer(QString origin);
	void disconnectFromServer();
	void selectStream(QString streamId);
	void setAutoConnect(bool enabled);
	void setReconnect(bool enabled);
	void shutdown();

private:
	class Impl;
	std::unique_ptr<Impl> impl_;
};

} // namespace gost
