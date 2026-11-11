#pragma once

#include <QtGlobal>

#include <optional>

namespace gost {

struct FrameCounters {
	quint64 skipped = 0;
	quint64 total = 0;

	bool operator==(const FrameCounters &) const = default;
};

enum class ObsStreamingPhase { Unknown, Stopped, Starting, Streaming, Stopping };

struct ObsStatisticsSample {
	bool videoAvailable = false;
	std::optional<quint64> videoIdentity;
	std::optional<quint64> outputIdentity;
	std::optional<double> fps;
	std::optional<quint64> renderTimeNs;
	std::optional<quint64> sentBytes;
	std::optional<FrameCounters> render;
	std::optional<FrameCounters> encode;
	std::optional<FrameCounters> network;
	bool renderLimited = false;
	bool encodeLimited = false;
	bool networkLimited = false;
};

struct ObsStatistics {
	ObsStreamingPhase phase = ObsStreamingPhase::Unknown;
	quint64 videoEpoch = 0;
	quint64 outputEpoch = 0;
	std::optional<double> fps;
	std::optional<double> bitrate;
	std::optional<quint64> renderTimeNs;
	std::optional<quint64> sentBytes;
	std::optional<FrameCounters> render;
	std::optional<FrameCounters> encode;
	std::optional<FrameCounters> network;
	bool renderLimited = false;
	bool encodeLimited = false;
	bool networkLimited = false;
	bool bitrateWarming = false;
	bool observed = false;

	bool operator==(const ObsStatistics &) const = default;
};

} // namespace gost
