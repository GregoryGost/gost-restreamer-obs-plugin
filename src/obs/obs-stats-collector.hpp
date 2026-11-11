#pragma once

#include "frontend.hpp"

#include <chrono>

namespace gost {

/**
 * Последовательно преобразует выборки адаптера в доступные для UI метрики без собственных OBS-вызовов.
 * Время now задаётся монотонными часами; вызовы принадлежат одному владельцу. Объект хранит предыдущую
 * пару байты/время и независимые эпохи видео/output, поэтому его нельзя разделять между источниками.
 * События Starting/Stopping задают ограниченное ожидание подтверждения, а не доказанную передачу.
 */
class OBSStatsCollector {
public:
	using Clock = std::chrono::steady_clock;
	static constexpr auto TransitionTimeout = std::chrono::seconds(10);

	// Копирует только доступные величины, отсеивает неверный FPS и вычисляет bitrate по двум пригодным
	// выборкам одной эпохи выхода. Первый замер даёт bitrateWarming; регресс/замена/насыщение
	// разрывают пару. streamingActive и event относятся к локальному OBS, не к готовности сервера.
	ObsStatistics sample(const ObsStatisticsSample &sample, bool streamingActive, Clock::time_point now,
			     std::optional<FrontendEvent> event = std::nullopt) noexcept;

private:
	struct Transition {
		bool targetActive;
		Clock::time_point deadline;
	};

	struct BytesSample {
		quint64 bytes;
		Clock::time_point time;
	};

	// StreamingStarted/StreamingStopped подтверждают фазу немедленно. Иначе проверяется целевое active; повтор того же
	// промежуточного события не продлевает TransitionTimeout, после его истечения фаза Unknown.
	void updatePhase(bool streamingActive, Clock::time_point now, std::optional<FrontendEvent> event) noexcept;

	ObsStreamingPhase phase_ = ObsStreamingPhase::Unknown;
	std::optional<Transition> transition_;
	bool videoAvailable_ = false;
	std::optional<quint64> videoIdentity_;
	std::optional<quint64> outputIdentity_;
	std::optional<FrameCounters> previousNetwork_;
	std::optional<BytesSample> previousBytes_;
	quint64 videoEpoch_ = 0;
	quint64 outputEpoch_ = 0;
};

} // namespace gost
