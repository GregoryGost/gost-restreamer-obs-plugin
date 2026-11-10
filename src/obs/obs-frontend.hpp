#pragma once

#include "frontend.hpp"

#include <obs-frontend-api.h>

namespace gost {

class OBSFrontend final : public Frontend {
public:
	QWidget *mainWindow() const override;
	bool addDock(const char *id, const QString &title, QWidget *widget) override;
	void removeDock(const char *id) override;
	void addEventCallback(EventCallback callback, void *cookie) override;
	void removeEventCallback(EventCallback callback, void *cookie) override;
	QString currentProfile() const override;
	QString currentProfilePath() const override;
	QStringList profiles() const override;
	bool streamingActive() const override;

private:
	static void onEvent(obs_frontend_event event, void *cookie) noexcept;
	EventCallback callback_ = nullptr;
	void *cookie_ = nullptr;
};

} // namespace gost
