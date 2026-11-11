#pragma once

#include "frontend.hpp"

#include <obs-frontend-api.h>

#include <atomic>
#include <memory>

namespace gost {

class OBSFrontend final : public Frontend {
public:
	~OBSFrontend() override;
	QWidget *mainWindow() const override;
	bool addDock(const char *id, const QString &title, QWidget *widget) override;
	void removeDock(const char *id) override;
	void addEventCallback(EventCallback callback, void *cookie) override;
	void removeEventCallback(EventCallback callback, void *cookie) override;
	QString currentProfile() const override;
	QString currentProfilePath() const override;
	QStringList profiles() const override;
	bool streamingActive() const override;
	ObsStatisticsSample readStatistics() noexcept override;
	void releaseStatistics() noexcept override;

private:
	static void onEvent(obs_frontend_event event, void *cookie) noexcept;
	static void onVideoReset(void *cookie, calldata_t *data) noexcept;
	static void onOutputReconnect(void *cookie, calldata_t *data) noexcept;
	void releaseOutputStatistics() noexcept;

	EventCallback callback_ = nullptr;
	void *cookie_ = nullptr;
	signal_handler_t *videoSignals_ = nullptr;
	signal_handler_t *outputSignals_ = nullptr;
	std::atomic<quint64> videoIdentity_ = 1;
	std::unique_ptr<obs_weak_output_t, decltype(&obs_weak_output_release)> output_{nullptr,
										       &obs_weak_output_release};
	std::atomic<quint64> outputIdentity_ = 0;
};

} // namespace gost
