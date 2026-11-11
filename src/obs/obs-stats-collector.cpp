#include "obs-stats-collector.hpp"

#include <cmath>
#include <limits>

namespace gost {
namespace {

bool regressed(const std::optional<FrameCounters> &current, const std::optional<FrameCounters> &previous)
{
	return current && previous && (current->skipped < previous->skipped || current->total < previous->total);
}

} // namespace

ObsStatistics OBSStatsCollector::sample(const ObsStatisticsSample &sample, bool streamingActive, Clock::time_point now,
					std::optional<FrontendEvent> event) noexcept
{
	// Новая попытка запуска output отделяется от повторного Starting той же попытки. Эпоха видео
	// зависит от доступности/идентичности подсистемы, эпоха выхода — также от непрерывности сетевых счётчиков.
	const bool newRun = event == FrontendEvent::StreamingStarting && (!transition_ || !transition_->targetActive);
	updatePhase(streamingActive, now, event);

	ObsStatistics result;
	result.observed = true;
	result.phase = phase_;
	const auto videoIdentity = sample.videoAvailable ? sample.videoIdentity : std::nullopt;
	if (sample.videoAvailable != videoAvailable_ || videoIdentity != videoIdentity_) {
		++videoEpoch_;
		videoAvailable_ = sample.videoAvailable;
		videoIdentity_ = videoIdentity;
	}
	result.videoEpoch = videoEpoch_;
	if (videoAvailable_) {
		if (sample.fps && std::isfinite(*sample.fps) && *sample.fps >= 0.0)
			result.fps = sample.fps;
		result.renderTimeNs = sample.renderTimeNs;
		result.render = sample.render;
		result.encode = sample.encode;
		result.renderLimited = sample.renderLimited;
		result.encodeLimited = sample.encodeLimited;
	}

	const auto outputIdentity = streamingActive && phase_ != ObsStreamingPhase::Stopped ? sample.outputIdentity
											    : std::nullopt;
	if (outputIdentity) {
		result.sentBytes = sample.sentBytes;
		result.network = sample.network;
		result.networkLimited = sample.networkLimited;
	}
	const auto maxBytes = std::numeric_limits<quint64>::max();
	const bool bytesUsable = result.sentBytes && *result.sentBytes != maxBytes;
	const bool bytesDiscontinuous = previousBytes_ && (!bytesUsable || *result.sentBytes < previousBytes_->bytes);
	// Замена выхода, новый запуск или разрыв счётчиков запрещают вычислять скорость между разными периодами.
	if (newRun || outputIdentity != outputIdentity_ || bytesDiscontinuous ||
	    regressed(result.network, previousNetwork_)) {
		++outputEpoch_;
		previousBytes_.reset();
	}
	outputIdentity_ = outputIdentity;
	previousNetwork_ = result.network;
	result.outputEpoch = outputEpoch_;
	if (!bytesUsable)
		return result;

	if (previousBytes_ && now > previousBytes_->time) {
		// Используется фактический интервал между выборками; перевод байтов в биты выполняется до деления.
		const auto seconds = std::chrono::duration<double>(now - previousBytes_->time).count();
		const auto bytes = *result.sentBytes - previousBytes_->bytes;
		result.bitrate = static_cast<double>(bytes) * 8.0 / seconds;
	} else {
		result.bitrateWarming = true;
	}
	previousBytes_ = BytesSample{*result.sentBytes, now};
	return result;
}

void OBSStatsCollector::updatePhase(bool streamingActive, Clock::time_point now,
				    std::optional<FrontendEvent> event) noexcept
{
	if (event == FrontendEvent::StreamingStarted || event == FrontendEvent::StreamingStopped) {
		transition_.reset();
		phase_ = event == FrontendEvent::StreamingStarted ? ObsStreamingPhase::Streaming
								  : ObsStreamingPhase::Stopped;
		return;
	}

	if (event == FrontendEvent::StreamingStarting || event == FrontendEvent::StreamingStopping) {
		const bool targetActive = event == FrontendEvent::StreamingStarting;
		if (!transition_ || transition_->targetActive != targetActive)
			transition_ = Transition{targetActive, now + TransitionTimeout};
	}

	if (transition_ && transition_->targetActive != streamingActive) {
		// Событие начала перехода ещё не подтверждает целевое состояние; затянувшийся переход становится неизвестным.
		if (now >= transition_->deadline)
			phase_ = ObsStreamingPhase::Unknown;
		else
			phase_ = transition_->targetActive ? ObsStreamingPhase::Starting : ObsStreamingPhase::Stopping;
		return;
	}
	transition_.reset();
	phase_ = streamingActive ? ObsStreamingPhase::Streaming : ObsStreamingPhase::Stopped;
}

} // namespace gost
