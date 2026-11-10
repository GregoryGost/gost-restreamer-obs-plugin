#include "obs-frontend.hpp"

#include <util/bmem.h>

#include <QByteArray>
#include <QWidget>

#include <memory>
#include <optional>

namespace gost {
namespace {

std::optional<FrontendEvent> translateEvent(obs_frontend_event event)
{
	switch (event) {
	case OBS_FRONTEND_EVENT_FINISHED_LOADING:
		return FrontendEvent::FinishedLoading;
	case OBS_FRONTEND_EVENT_PROFILE_CHANGING:
		return FrontendEvent::ProfileChanging;
	case OBS_FRONTEND_EVENT_PROFILE_CHANGED:
		return FrontendEvent::ProfileChanged;
	case OBS_FRONTEND_EVENT_PROFILE_RENAMED:
		return FrontendEvent::ProfileRenamed;
	case OBS_FRONTEND_EVENT_PROFILE_LIST_CHANGED:
		return FrontendEvent::ProfileListChanged;
	case OBS_FRONTEND_EVENT_STREAMING_STARTING:
		return FrontendEvent::StreamingStarting;
	case OBS_FRONTEND_EVENT_STREAMING_STARTED:
		return FrontendEvent::StreamingStarted;
	case OBS_FRONTEND_EVENT_STREAMING_STOPPING:
		return FrontendEvent::StreamingStopping;
	case OBS_FRONTEND_EVENT_STREAMING_STOPPED:
		return FrontendEvent::StreamingStopped;
	case OBS_FRONTEND_EVENT_EXIT:
		return FrontendEvent::Exit;
	default:
		return std::nullopt;
	}
}

} // namespace

QWidget *OBSFrontend::mainWindow() const
{
	return static_cast<QWidget *>(obs_frontend_get_main_window());
}

bool OBSFrontend::addDock(const char *id, const QString &title, QWidget *widget)
{
	const auto utf8 = title.toUtf8();
	return obs_frontend_add_dock_by_id(id, utf8.constData(), widget);
}

void OBSFrontend::removeDock(const char *id)
{
	obs_frontend_remove_dock(id);
}

void OBSFrontend::addEventCallback(EventCallback callback, void *cookie)
{
	callback_ = callback;
	cookie_ = cookie;
	obs_frontend_add_event_callback(&OBSFrontend::onEvent, this);
}

void OBSFrontend::removeEventCallback(EventCallback callback, void *cookie)
{
	if (callback_ != callback || cookie_ != cookie) {
		return;
	}
	obs_frontend_remove_event_callback(&OBSFrontend::onEvent, this);
	callback_ = nullptr;
	cookie_ = nullptr;
}

QString OBSFrontend::currentProfile() const
{
	const std::unique_ptr<char, decltype(&bfree)> profile(obs_frontend_get_current_profile(), &bfree);
	return profile ? QString::fromUtf8(profile.get()) : QString{};
}

QString OBSFrontend::currentProfilePath() const
{
	const std::unique_ptr<char, decltype(&bfree)> path(obs_frontend_get_current_profile_path(), &bfree);
	return path ? QString::fromUtf8(path.get()) : QString{};
}

QStringList OBSFrontend::profiles() const
{
	const std::unique_ptr<char *, decltype(&bfree)> names(obs_frontend_get_profiles(), &bfree);
	QStringList result;
	if (names) {
		for (char **name = names.get(); *name; ++name) {
			result.append(QString::fromUtf8(*name));
		}
	}
	return result;
}

bool OBSFrontend::streamingActive() const
{
	return obs_frontend_streaming_active();
}

void OBSFrontend::onEvent(obs_frontend_event event, void *cookie) noexcept
{
	auto *self = static_cast<OBSFrontend *>(cookie);
	const auto translated = translateEvent(event);
	if (translated && self->callback_) {
		self->callback_(*translated, self->cookie_);
	}
}

} // namespace gost
