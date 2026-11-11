#pragma once

#include <QtGlobal>

#include <optional>

namespace gost {

// Числитель и знаменатель относятся к одному этапу: рендерингу, кодированию или сетевой отправке OBS.
struct FrameCounters {
	quint64 skipped = 0;
	quint64 total = 0;

	bool operator==(const FrameCounters &) const = default;
};

enum class ObsStreamingPhase { Unknown, Stopped, Starting, Streaming, Stopping };

// Необработанная выборка адаптера: optional отделяет отсутствие измерения от достоверного нуля.
// videoIdentity и outputIdentity различают ресурсы и их перезапуски.
// renderTimeNs задан в наносекундах, sentBytes — в байтах.
struct ObsStatisticsSample {
	// Эти идентичности — поколения адаптера, не адреса, пригодные к разыменованию. Сброс видео и
	// переподключение или замена выхода разрывают соответствующую последовательность независимо друг от друга.
	bool videoAvailable = false;
	std::optional<quint64> videoIdentity;
	std::optional<quint64> outputIdentity;
	std::optional<double> fps;
	std::optional<quint64> renderTimeNs;
	std::optional<quint64> sentBytes;
	std::optional<FrameCounters> render;
	std::optional<FrameCounters> encode;
	std::optional<FrameCounters> network;
	// Признак ограничения сохраняет факт достижения предела исходного API даже при отсутствующей паре счётчиков.
	// Такой предел нельзя считать измеренным нулём или обычным отсутствием выхода.
	bool renderLimited = false;
	bool encodeLimited = false;
	bool networkLimited = false;
};

// Производная выборка для UI: FPS — кадры/с, bitrate — бит/с; эпохи задают непрерывность накоплений.
// bitrateWarming означает отсутствие пары пригодных выборок, observed — выполнение хотя бы одного опроса.
struct ObsStatistics {
	// phase описывает только штатный streaming output OBS; серверный ingest и Session независимы.
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
