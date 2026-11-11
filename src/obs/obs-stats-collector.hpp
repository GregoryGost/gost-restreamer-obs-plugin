#pragma once

#include "frontend.hpp"

#include <chrono>

namespace gost {

class OBSStatsCollector {
public:
	using Clock = std::chrono::steady_clock;
	static constexpr auto TransitionTimeout = std::chrono::seconds(10);

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
