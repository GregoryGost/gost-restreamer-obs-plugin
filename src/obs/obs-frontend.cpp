#include "obs-frontend.hpp"

#include <util/bmem.h>

#include <QByteArray>
#include <QWidget>

#include <limits>
#include <optional>
#include <string_view>
#include <type_traits>
#include <utility>

namespace gost {
namespace {

std::optional<FrontendEvent> translateEvent(obs_frontend_event event)
{
	// Неподдерживаемые события не превращаются в профильные/стриминговые по умолчанию.
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

struct FrameCounterReading {
	std::optional<FrameCounters> counters;
	bool limited = false;
};

template<typename Counter> FrameCounterReading frameCounters(Counter skipped, Counter total)
{
	// Предел проверяется в исходном типе API до расширения до quint64. Иначе максимум signed/int
	// выглядел бы обычным небольшим числом и ошибочно разрешал вычислять следующий прирост.
	if constexpr (std::is_signed_v<Counter>) {
		if (skipped < 0 || total < 0)
			return {};
	}
	const auto maximum = std::numeric_limits<Counter>::max();
	if (skipped == maximum || total == maximum)
		return {.counters = std::nullopt, .limited = true};
	if (skipped > total)
		return {};

	return {.counters = FrameCounters{static_cast<quint64>(skipped), static_cast<quint64>(total)}};
}

} // namespace

OBSFrontend::~OBSFrontend()
{
	releaseStatistics();
}

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
	// Адаптер обслуживает единственного владельца; пара хранится до соответствующего removeEventCallback().
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
	// OBS выделяет единый массив строк; копируем имена и освобождаем исходный блок через bfree.
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

ObsStatisticsSample OBSFrontend::readStatistics() noexcept
{
	// Подписка создаётся при первой разрешённой выборке, когда OBSController уже получил FinishedLoading.
	if (!videoSignals_) {
		videoSignals_ = obs_get_signal_handler();
		signal_handler_connect(videoSignals_, "video_reset", &OBSFrontend::onVideoReset, this);
	}

	ObsStatisticsSample sample;
	obs_video_info info{};
	if (obs_get_video_info(&info)) {
		if (auto *video = obs_get_video()) {
			sample.videoAvailable = true;
			sample.videoIdentity = videoIdentity_.load(std::memory_order_relaxed);
			sample.fps = obs_get_active_fps();
			sample.renderTimeNs = obs_get_average_frame_time_ns();
			const auto lagged = obs_get_lagged_frames();
			const auto render = frameCounters(lagged, obs_get_total_frames());
			sample.render = render.counters;
			sample.renderLimited = render.limited;
			const auto skipped = video_output_get_skipped_frames(video);
			const auto encode = frameCounters(skipped, video_output_get_total_frames(video));
			sample.encode = encode.counters;
			sample.encodeLimited = encode.limited;
		}
	}

	// Frontend API возвращает сильную ссылку: выход остаётся жив на время чтения и освобождается через OBS API.
	const std::unique_ptr<obs_output_t, decltype(&obs_output_release)> output(obs_frontend_get_streaming_output(),
										  &obs_output_release);
	if (!output || !obs_output_active(output.get())) {
		releaseOutputStatistics();
		return sample;
	}

	const char *outputId = obs_output_get_id(output.get());
	const bool rtmp = outputId && std::string_view(outputId) == "rtmp_output";
	// Слабая ссылка удерживает прежний блок управления даже после уничтожения выхода.
	decltype(output_) identity(obs_output_get_weak_output(output.get()), &obs_weak_output_release);
	// Удержание слабого блока между выборками отличает новый output даже при повторном использовании
	// адреса прежнего объекта. Сам output при этом не удерживается работающим сильной ссылкой.
	if (identity.get() != output_.get()) {
		releaseOutputStatistics();
		output_ = std::move(identity);
		outputIdentity_.fetch_add(1, std::memory_order_relaxed);
		if (rtmp) {
			outputSignals_ = obs_output_get_signal_handler(output.get());
			// Подписка удерживает обработчик, даже если OBS уже уничтожил выход.
			signal_handler_connect_ref(outputSignals_, "reconnect_success", &OBSFrontend::onOutputReconnect,
						   this);
		}
	}
	const auto generation = outputIdentity_.load(std::memory_order_relaxed);
	sample.outputIdentity = generation;
	if (!rtmp || obs_output_reconnecting(output.get()))
		return sample;

	const auto dropped = obs_output_get_frames_dropped(output.get());
	const auto network = frameCounters(dropped, obs_output_get_total_frames(output.get()));
	const auto bytes = obs_output_get_total_bytes(output.get());
	const bool active = obs_output_active(output.get()) && !obs_output_reconnecting(output.get());
	const auto observedGeneration = outputIdentity_.load(std::memory_order_relaxed);
	sample.outputIdentity = observedGeneration;
	// Переподключение во время чтения делает набор сетевых счётчиков несогласованным; такую выборку не выдаём.
	if (active && observedGeneration == generation) {
		sample.network = network.counters;
		sample.networkLimited = network.limited;
		sample.sentBytes = bytes;
	}
	return sample;
}

void OBSFrontend::releaseStatistics() noexcept
{
	if (videoSignals_) {
		signal_handler_disconnect(videoSignals_, "video_reset", &OBSFrontend::onVideoReset, this);
		videoSignals_ = nullptr;
	}
	releaseOutputStatistics();
}

void OBSFrontend::releaseOutputStatistics() noexcept
{
	// Подписку с удержанием обработчика снимаем до освобождения слабой ссылки; сам выход уже мог исчезнуть.
	if (outputSignals_) {
		signal_handler_disconnect(outputSignals_, "reconnect_success", &OBSFrontend::onOutputReconnect, this);
		outputSignals_ = nullptr;
	}
	output_.reset();
}

void OBSFrontend::onVideoReset(void *cookie, calldata_t *) noexcept
{
	// Поколение служит маркером разрыва, не публикацией других данных; relaxed не задаёт порядок
	// чтения внутренних полей OBS. Полную согласованность нескольких API-чтений оно не обещает.
	auto *self = static_cast<OBSFrontend *>(cookie);
	self->videoIdentity_.fetch_add(1, std::memory_order_relaxed);
}

void OBSFrontend::onOutputReconnect(void *cookie, calldata_t *) noexcept
{
	auto *self = static_cast<OBSFrontend *>(cookie);
	self->outputIdentity_.fetch_add(1, std::memory_order_relaxed);
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
