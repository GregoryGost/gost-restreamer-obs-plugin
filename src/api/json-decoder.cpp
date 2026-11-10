#include "api/json-decoder.hpp"
#include <QDate>
#include <QRegularExpression>
#include <QTime>
namespace gost::api {
Error makeError(ErrorKind kind, int status)
{
	QString message;
	switch (kind) {
	case ErrorKind::Transport:
		message = QStringLiteral("Соединение с сервером прервано.");
		break;
	case ErrorKind::Timeout:
		message = QStringLiteral("Истекло время ожидания сервера.");
		break;
	case ErrorKind::Cancelled:
		message = QStringLiteral("Запрос отменён.");
		break;
	case ErrorKind::Decode:
	case ErrorKind::Contract:
		message = QStringLiteral("Ответ сервера не соответствует контракту.");
		break;
	case ErrorKind::Limit:
		message = QStringLiteral("Превышен предел обработки данных сервера.");
		break;
	case ErrorKind::Http:
		message = QStringLiteral("Сервер отклонил запрос (HTTP %1).").arg(status);
		break;
	}
	return {kind, status, message, std::nullopt};
}
bool parseDecimal(const QString &text, quint64 &value)
{
	if (text.isEmpty() || (text.size() > 1 && text.front() == QLatin1Char('0')))
		return false;
	quint64 result = 0;
	for (const auto c : text) {
		if (c < QLatin1Char('0') || c > QLatin1Char('9'))
			return false;
		const quint64 digit = c.unicode() - '0';
		if (result > (std::numeric_limits<quint64>::max() - digit) / 10)
			return false;
		result = result * 10 + digit;
	}
	value = result;
	return true;
}
bool validTimestamp(const QString &text)
{
	static const QRegularExpression pattern(
		QStringLiteral("^(\\d{4})-(\\d{2})-(\\d{2})T(\\d{2}):(\\d{2}):(\\d{2})(?:\\.\\d{1,9})?Z$"));
	const auto match = pattern.match(text);
	return match.hasMatch() &&
	       QDate(match.captured(1).toInt(), match.captured(2).toInt(), match.captured(3).toInt()).isValid() &&
	       QTime(match.captured(4).toInt(), match.captured(5).toInt(), match.captured(6).toInt()).isValid();
}
bool fromJson(const QJsonValue &json, QString &value, Error &error)
{
	if (!json.isString()) {
		error = makeError(ErrorKind::Decode);
		return false;
	}
	value = json.toString();
	for (qsizetype i = 0; i < value.size(); ++i) {
		if (value[i].isHighSurrogate()) {
			if (++i >= value.size() || !value[i].isLowSurrogate()) {
				error = makeError(ErrorKind::Decode);
				return false;
			}
		} else if (value[i].isLowSurrogate()) {
			error = makeError(ErrorKind::Decode);
			return false;
		}
	}
	return true;
}
bool fromJson(const QJsonValue &json, bool &value, Error &error)
{
	if (!json.isBool()) {
		error = makeError(ErrorKind::Decode);
		return false;
	}
	value = json.toBool();
	return true;
}
bool fromJson(const QJsonValue &json, quint64 &value, Error &error)
{
	if (!json.isString() || !parseDecimal(json.toString(), value)) {
		error = makeError(ErrorKind::Decode);
		return false;
	}
	return true;
}
bool fromJson(const QJsonValue &json, double &value, Error &error)
{
	if (!json.isDouble() || !std::isfinite(json.toDouble())) {
		error = makeError(ErrorKind::Decode);
		return false;
	}
	value = json.toDouble();
	return true;
}
bool fromJson(const QJsonValue &json, ProfileText &value, Error &error)
{
	if (json.isString()) {
		QString text;
		if (!fromJson(json, text, error))
			return false;
		value.bytes = text.toUtf8();
		return true;
	}
	if (!json.isObject()) {
		error = makeError(ErrorKind::Decode);
		return false;
	}
	const auto object = json.toObject();
	QString encoded;
	if (object.size() != 2 || object.value(QStringLiteral("encoding")) != QStringLiteral("base64") ||
	    !fromJson(object.value(QStringLiteral("value")), encoded, error)) {
		error = makeError(ErrorKind::Decode);
		return false;
	}
	const auto ascii = encoded.toLatin1();
	const auto decoded = QByteArray::fromBase64Encoding(ascii, QByteArray::AbortOnBase64DecodingErrors);
	if (!decoded || decoded.decoded.toBase64() != ascii) {
		error = makeError(ErrorKind::Decode);
		return false;
	}
	value.bytes = decoded.decoded;
	return true;
}
bool fromJson(const QJsonValue &json, QMap<QString, quint64> &value, Error &error)
{
	if (!json.isObject()) {
		error = makeError(ErrorKind::Decode);
		return false;
	}
	value.clear();
	const auto object = json.toObject();
	for (auto it = object.begin(); it != object.end(); ++it) {
		quint64 entry;
		if (!fromJson(it.value(), entry, error))
			return false;
		value.insert(it.key(), entry);
	}
	return true;
}
namespace {
template<class T> bool timestampField(const T &)
{
	return true;
}
bool timestampField(const QString &v)
{
	return validTimestamp(v);
}
bool timestampField(const JsonField<QString> &v)
{
	return !v.hasValue() || validTimestamp(v.value);
}
class Reader {
public:
	Reader(const QJsonValue &json, Error &error) : error_(error)
	{
		valid_ = json.isObject();
		if (valid_)
			object_ = json.toObject();
		else
			error_ = makeError(ErrorKind::Decode);
	}
	template<class T> bool field(const char *key, T &value)
	{
		if (!valid_ || !fromJson(object_.value(QLatin1String(key)), value, error_)) {
			valid_ = false;
			return false;
		}
		return true;
	}
	template<class T> bool time(const char *key, T &value)
	{
		if (!field(key, value))
			return false;
		if (!timestampField(value)) {
			error_ = makeError(ErrorKind::Decode);
			valid_ = false;
		}
		return valid_;
	}
	bool good() const { return valid_; }

private:
	QJsonObject object_;
	Error &error_;
	bool valid_ = false;
};
} // namespace
bool fromJson(const QJsonValue &json, Record &value, Error &error)
{
	Reader r(json, error);
	r.field("id", value.id);
	r.field("revision", value.revision);
	r.time("created_at", value.createdAt);
	r.time("updated_at", value.updatedAt);
	return r.good();
}
bool fromJson(const QJsonValue &json, Failure &value, Error &error)
{
	Reader r(json, error);
	r.field("code", value.code);
	r.field("message", value.message);
	return r.good();
}
bool fromJson(const QJsonValue &json, Watchdog &value, Error &error)
{
	Reader r(json, error);
	r.field("video_timeout_ns", value.videoTimeoutNs);
	r.field("audio_timeout_ns", value.audioTimeoutNs);
	r.field("recovery_window_ns", value.recoveryWindowNs);
	r.field("keyframe_timeout_ns", value.keyframeTimeoutNs);
	return r.good();
}
bool fromJson(const QJsonValue &json, Policy &value, Error &error)
{
	Reader r(json, error);
	r.field("asset_id", value.assetId);
	r.field("watchdog", value.watchdog);
	return r.good();
}
bool fromJson(const QJsonValue &json, Stream &value, Error &error)
{
	if (!fromJson(json, static_cast<Record &>(value), error))
		return false;
	Reader r(json, error);
	r.field("name", value.name);
	r.field("publication_key", value.publicationKey);
	r.field("fallback", value.fallback);
	if (value.publicationKey.isNull()) {
		error = makeError(ErrorKind::Contract);
		return false;
	}
	return r.good();
}
bool fromJson(const QJsonValue &json, Destination &value, Error &error)
{
	if (!fromJson(json, static_cast<Record &>(value), error))
		return false;
	Reader r(json, error);
	r.field("stream_id", value.streamId);
	r.field("group_id", value.groupId);
	r.field("name", value.name);
	r.field("platform", value.platform);
	r.field("endpoint", value.endpoint);
	r.field("stream_key", value.streamKey);
	r.field("enabled", value.enabled);
	if (value.endpoint.isNull()) {
		error = makeError(ErrorKind::Contract);
		return false;
	}
	if (value.streamKey.isNull()) {
		error = makeError(ErrorKind::Contract);
		return false;
	}
	return r.good();
}
bool fromJson(const QJsonValue &json, FrameRate &value, Error &error)
{
	Reader r(json, error);
	r.field("numerator", value.numerator);
	r.field("denominator", value.denominator);
	if (value.numerator && !value.denominator) {
		error = makeError(ErrorKind::Contract);
		return false;
	}
	return r.good();
}
bool fromJson(const QJsonValue &json, Color &value, Error &error)
{
	Reader r(json, error);
	r.field("range", value.range);
	r.field("primaries", value.primaries);
	r.field("transfer", value.transfer);
	r.field("matrix", value.matrix);
	return r.good();
}
bool fromJson(const QJsonValue &json, VideoFormat &value, Error &error)
{
	Reader r(json, error);
	r.field("codec", value.codec);
	r.field("width", value.width);
	r.field("height", value.height);
	r.field("frame_rate", value.frameRate);
	r.field("codec_profile", value.codecProfile);
	r.field("level", value.level);
	r.field("tier", value.tier);
	r.field("pixel_format", value.pixelFormat);
	r.field("bit_depth", value.bitDepth);
	r.field("color", value.color);
	return r.good();
}
bool fromJson(const QJsonValue &json, AudioFormat &value, Error &error)
{
	Reader r(json, error);
	r.field("codec", value.codec);
	r.field("codec_profile", value.codecProfile);
	r.field("sample_rate_hz", value.sampleRateHz);
	r.field("channels", value.channels);
	r.field("channel_layout", value.channelLayout);
	return r.good();
}
bool fromJson(const QJsonValue &json, Format &value, Error &error)
{
	Reader r(json, error);
	r.field("video", value.video);
	r.field("audio", value.audio);
	return r.good();
}
bool fromJson(const QJsonValue &json, VideoEncoding &value, Error &error)
{
	Reader r(json, error);
	r.field("target_bitrate_bps", value.targetBitrateBps);
	r.field("max_bitrate_bps", value.maxBitrateBps);
	r.field("buffer_bits", value.bufferBits);
	r.field("gop_frames", value.gopFrames);
	r.field("max_b_frames", value.maxBFrames);
	r.field("closed_gop", value.closedGop);
	return r.good();
}
bool fromJson(const QJsonValue &json, AudioEncoding &value, Error &error)
{
	Reader r(json, error);
	r.field("bitrate_bps", value.bitrateBps);
	return r.good();
}
bool fromJson(const QJsonValue &json, Group &value, Error &error)
{
	if (!fromJson(json, static_cast<Record &>(value), error))
		return false;
	Reader r(json, error);
	r.field("stream_id", value.streamId);
	r.field("profile_id", value.profileId);
	r.field("name", value.name);
	return r.good();
}
bool fromJson(const QJsonValue &json, Receipt &value, Error &error)
{
	if (!fromJson(json, static_cast<Record &>(value), error))
		return false;
	Reader r(json, error);
	r.field("request_id", value.requestId);
	r.field("kind", value.kind);
	r.field("target_id", value.targetId);
	r.field("session_id", value.sessionId);
	r.field("output_id", value.outputId);
	r.field("status", value.status);
	r.field("failure", value.failure);
	return r.good();
}
bool fromJson(const QJsonValue &json, Asset &value, Error &error)
{
	if (!fromJson(json, static_cast<Record &>(value), error))
		return false;
	Reader r(json, error);
	r.field("name", value.name);
	r.field("kind", value.kind);
	r.field("state", value.state);
	r.field("current_generation_id", value.currentGenerationId);
	r.field("pending_generation_id", value.pendingGenerationId);
	r.field("size_bytes", value.sizeBytes);
	return r.good();
}
bool fromJson(const QJsonValue &json, Generation &value, Error &error)
{
	if (!fromJson(json, static_cast<Record &>(value), error))
		return false;
	Reader r(json, error);
	r.field("asset_id", value.assetId);
	r.field("kind", value.kind);
	r.field("status", value.status);
	r.field("size_bytes", value.sizeBytes);
	r.field("failure", value.failure);
	return r.good();
}
bool fromJson(const QJsonValue &json, Rate &value, Error &error)
{
	Reader r(json, error);
	r.field("status", value.status);
	r.field("reason", value.reason);
	r.field("value", value.value);
	r.time("window_start", value.windowStart);
	r.time("window_end", value.windowEnd);
	r.field("duration_ns", value.durationNs);
	return r.good();
}
bool fromJson(const QJsonValue &json, Rates &value, Error &error)
{
	Reader r(json, error);
	r.field("fps", value.fps);
	r.field("payload_bitrate", value.payloadBitrate);
	r.field("written_bitrate", value.writtenBitrate);
	return r.good();
}
bool fromJson(const QJsonValue &json, ObservedFormat &value, Error &error)
{
	Reader r(json, error);
	r.field("video_codec", value.videoCodec);
	r.field("audio_codec", value.audioCodec);
	r.field("width", value.width);
	r.field("height", value.height);
	r.field("sample_rate_hz", value.sampleRateHz);
	r.field("channels", value.channels);
	return r.good();
}
bool fromJson(const QJsonValue &json, MediaMetrics &value, Error &error)
{
	Reader r(json, error);
	r.time("sampled_at", value.sampledAt);
	r.field("video_frames", value.videoFrames);
	r.field("payload_bytes", value.payloadBytes);
	r.field("written_bytes", value.writtenBytes);
	r.field("audio_units", value.audioUnits);
	r.field("saturated", value.saturated);
	r.time("last_frame", value.lastFrame);
	r.field("rates", value.rates);
	r.field("format", value.format);
	return r.good();
}
bool fromJson(const QJsonValue &json, InputMetrics &value, Error &error)
{
	Reader r(json, error);
	r.field("media", value.media);
	r.field("source_epoch", value.sourceEpoch);
	return r.good();
}
bool fromJson(const QJsonValue &json, Input &value, Error &error)
{
	Reader r(json, error);
	r.field("connected", value.connected);
	r.time("changed_at", value.changedAt);
	r.field("metrics", value.metrics);
	return r.good();
}
bool fromJson(const QJsonValue &json, GroupMetrics &value, Error &error)
{
	Reader r(json, error);
	r.field("live_input", value.liveInput);
	r.field("fallback_input", value.fallbackInput);
	r.field("output", value.output);
	return r.good();
}
bool fromJson(const QJsonValue &json, OutputMetrics &value, Error &error)
{
	Reader r(json, error);
	r.field("media", value.media);
	r.field("counters", value.counters);
	r.field("queue_drops", value.queueDrops);
	r.field("delivery_drops", value.deliveryDrops);
	r.field("resync", value.resync);
	r.time("last_write", value.lastWrite);
	r.time("last_media", value.lastMedia);
	r.field("ack_ahead_events_total", value.ackAheadEventsTotal);
	r.field("ack_ahead_bytes_last", value.ackAheadBytesLast);
	r.field("ack_ahead_bytes_max", value.ackAheadBytesMax);
	r.time("ack_last_anomaly_at", value.ackLastAnomalyAt);
	return r.good();
}
bool fromJson(const QJsonValue &json, FFmpegProgress &value, Error &error)
{
	Reader r(json, error);
	r.field("frame", value.frame);
	r.field("total_size_bytes", value.totalSizeBytes);
	r.field("dup_frames", value.dupFrames);
	r.field("drop_frames", value.dropFrames);
	r.field("fps", value.fps);
	r.field("bitrate_bps", value.bitrateBps);
	r.field("speed", value.speed);
	r.field("out_time_us", value.outTimeUs);
	r.field("end", value.end);
	return r.good();
}
bool fromJson(const QJsonValue &json, FFmpegMetrics &value, Error &error)
{
	Reader r(json, error);
	r.field("status", value.status);
	r.field("reason", value.reason);
	r.time("sampled_at", value.sampledAt);
	r.time("last_progress_at", value.lastProgressAt);
	r.field("progress_losses", value.progressLosses);
	r.field("diagnostic_losses", value.diagnosticLosses);
	r.field("progress", value.progress);
	return r.good();
}
bool fromJson(const QJsonValue &json, GPUMetric &value, Error &error)
{
	Reader r(json, error);
	r.field("status", value.status);
	r.field("reason", value.reason);
	r.field("value", value.value);
	r.time("sampled_at", value.sampledAt);
	r.time("last_success_at", value.lastSuccessAt);
	r.field("sampling_period_us", value.samplingPeriodUs);
	return r.good();
}
bool fromJson(const QJsonValue &json, GPUDevice &value, Error &error)
{
	Reader r(json, error);
	r.field("uuid", value.uuid);
	r.field("utilization", value.utilization);
	r.field("memory_utilization", value.memoryUtilization);
	r.field("encoder_utilization", value.encoderUtilization);
	r.field("decoder_utilization", value.decoderUtilization);
	r.field("memory_used_bytes", value.memoryUsedBytes);
	r.field("memory_total_bytes", value.memoryTotalBytes);
	r.field("memory_free_bytes", value.memoryFreeBytes);
	return r.good();
}
bool fromJson(const QJsonValue &json, GPU &value, Error &error)
{
	Reader r(json, error);
	r.field("status", value.status);
	r.field("reason", value.reason);
	r.time("sampled_at", value.sampledAt);
	r.field("stale_after_ns", value.staleAfterNs);
	r.field("devices", value.devices);
	return r.good();
}
bool fromJson(const QJsonValue &json, Observation &value, Error &error)
{
	Reader r(json, error);
	r.field("instance_id", value.instanceId);
	r.field("revision", value.revision);
	r.time("observed_at", value.observedAt);
	r.field("state", value.state);
	r.field("failure", value.failure);
	return r.good();
}
bool fromJson(const QJsonValue &json, RuntimeGroup &value, Error &error)
{
	if (!fromJson(json, static_cast<Observation &>(value), error))
		return false;
	Reader r(json, error);
	r.field("group_id", value.groupId);
	r.field("profile_id", value.profileId);
	r.field("source", value.source);
	r.field("source_revision", value.sourceRevision);
	r.field("metrics", value.metrics);
	return r.good();
}
bool fromJson(const QJsonValue &json, RuntimeOutput &value, Error &error)
{
	if (!fromJson(json, static_cast<Observation &>(value), error))
		return false;
	Reader r(json, error);
	r.field("output_id", value.outputId);
	r.field("metrics", value.metrics);
	return r.good();
}
bool fromJson(const QJsonValue &json, RuntimeFFmpeg &value, Error &error)
{
	if (!fromJson(json, static_cast<Observation &>(value), error))
		return false;
	Reader r(json, error);
	r.field("group_instance_id", value.groupInstanceId);
	r.field("role", value.role);
	r.field("metrics", value.metrics);
	return r.good();
}
bool fromJson(const QJsonValue &json, RuntimeSession &value, Error &error)
{
	Reader r(json, error);
	r.field("session_id", value.sessionId);
	r.field("observation", value.observation);
	r.field("input", value.input);
	r.field("groups", value.groups);
	r.field("outputs", value.outputs);
	r.field("ffmpeg", value.ffmpeg);
	return r.good();
}
bool fromJson(const QJsonValue &json, Anchor &value, Error &error)
{
	Reader r(json, error);
	r.field("cursor", value.cursor);
	r.field("sessions", value.sessions);
	r.field("gpu", value.gpu);
	return r.good();
}
bool fromJson(const QJsonValue &json, SessionMetrics &value, Error &error)
{
	Reader r(json, error);
	r.field("availability", value.availability);
	r.field("reason", value.reason);
	r.field("snapshot", value.snapshot);
	return r.good();
}
bool fromJson(const QJsonValue &json, RuntimeEvent &value, Error &error)
{
	Reader r(json, error);
	r.field("cursor", value.cursor);
	r.field("kind", value.kind);
	r.field("owner", value.owner);
	r.field("session_id", value.sessionId);
	r.field("output_id", value.outputId);
	r.field("group_id", value.groupId);
	r.field("profile_id", value.profileId);
	r.field("observation", value.observation);
	r.field("group_instance_id", value.groupInstanceId);
	r.field("source", value.source);
	r.field("source_revision", value.sourceRevision);
	r.field("role", value.role);
	r.field("attempt_active", value.attemptActive);
	r.field("reason", value.reason);
	r.field("input", value.input);
	r.field("group", value.group);
	r.field("output", value.output);
	r.field("ffmpeg", value.ffmpeg);
	r.field("gpu", value.gpu);
	return r.good();
}
bool fromJson(const QJsonValue &json, NVENC &value, Error &error)
{
	Reader r(json, error);
	r.field("preset", value.preset);
	r.field("tuning", value.tuning);
	r.field("multipass", value.multipass);
	r.field("lookahead_frames", value.lookaheadFrames);
	r.field("enable_spatial_aq", value.enableSpatialAq);
	r.field("enable_temporal_aq", value.enableTemporalAq);
	r.field("aq_strength", value.aqStrength);
	return r.good();
}
bool fromJson(const QJsonValue &json, SVTAV1 &value, Error &error)
{
	Reader r(json, error);
	r.field("preset", value.preset);
	r.field("tuning", value.tuning);
	r.field("aq_mode", value.aqMode);
	r.field("lookahead_frames", value.lookaheadFrames);
	return r.good();
}
bool fromJson(const QJsonValue &json, Encoding &value, Error &error)
{
	Reader r(json, error);
	r.field("version", value.version);
	r.field("video_encoder", value.videoEncoder);
	r.field("audio_encoder", value.audioEncoder);
	r.field("rate_control", value.rateControl);
	r.field("rescale_output", value.rescaleOutput);
	r.field("video_custom", value.videoCustom);
	r.field("audio_custom", value.audioCustom);
	r.field("nvenc", value.nvenc);
	r.field("svt_av1", value.svtAv1);
	if (value.version != 2) {
		error = makeError(ErrorKind::Contract);
		return false;
	}
	return r.good();
}
bool fromJson(const QJsonValue &json, GroupProfile &value, Error &error)
{
	Reader r(json, error);
	r.field("mode", value.mode);
	r.field("live_audio_mode", value.liveAudioMode);
	r.field("format", value.format);
	r.field("video_encoding", value.videoEncoding);
	r.field("audio_encoding", value.audioEncoding);
	r.field("encoding", value.encoding);
	if (value.liveAudioMode.isNull()) {
		error = makeError(ErrorKind::Contract);
		return false;
	}
	if (value.liveAudioMode.hasValue() && value.liveAudioMode.value != QStringLiteral("copy") &&
	    value.liveAudioMode.value != QStringLiteral("transcode")) {
		error = makeError(ErrorKind::Contract);
		return false;
	}
	if (value.mode != QStringLiteral("copy") && value.mode != QStringLiteral("transcode")) {
		error = makeError(ErrorKind::Contract);
		return false;
	}
	if (value.mode == QStringLiteral("copy") && value.liveAudioMode.hasValue() &&
	    value.liveAudioMode.value == QStringLiteral("transcode")) {
		error = makeError(ErrorKind::Contract);
		return false;
	}
	return r.good();
}
bool fromJson(const QJsonValue &json, Profile &value, Error &error)
{
	if (!fromJson(json, static_cast<Record &>(value), error))
		return false;
	Reader r(json, error);
	r.field("name", value.name);
	r.field("profile", value.profile);
	return r.good();
}
bool fromJson(const QJsonValue &json, GroupSnapshot &value, Error &error)
{
	Reader r(json, error);
	r.field("group", value.group);
	r.field("profile", value.profile);
	return r.good();
}
bool fromJson(const QJsonValue &json, Session &value, Error &error)
{
	if (!fromJson(json, static_cast<Record &>(value), error))
		return false;
	Reader r(json, error);
	r.field("stream_id", value.streamId);
	r.field("state", value.state);
	r.field("desired_paused", value.desiredPaused);
	r.field("stream", value.stream);
	r.field("groups", value.groups);
	r.time("ended_at", value.endedAt);
	r.field("failure", value.failure);
	return r.good();
}
bool fromJson(const QJsonValue &json, Output &value, Error &error)
{
	if (!fromJson(json, static_cast<Record &>(value), error))
		return false;
	Reader r(json, error);
	r.field("session_id", value.sessionId);
	r.field("destination", value.destination);
	r.field("group", value.group);
	r.field("profile", value.profile);
	r.field("fallback", value.fallback);
	r.field("desired_running", value.desiredRunning);
	r.field("attached", value.attached);
	return r.good();
}
bool fromJson(const QJsonValue &json, Variant &value, Error &error)
{
	if (!fromJson(json, static_cast<Record &>(value), error))
		return false;
	Reader r(json, error);
	r.field("asset_id", value.assetId);
	r.field("generation_id", value.generationId);
	r.field("state", value.state);
	r.field("profile", value.profile);
	r.field("format", value.format);
	r.field("size_bytes", value.sizeBytes);
	return r.good();
}
bool fromJson(const QJsonValue &json, Health &value, Error &error)
{
	Reader r(json, error);
	r.field("status", value.status);
	r.field("reason", value.reason);
	return r.good();
}
bool fromJson(const QJsonValue &json, Capability &value, Error &error)
{
	Reader r(json, error);
	r.field("status", value.status);
	r.field("reason", value.reason);
	r.time("checked_at", value.checkedAt);
	return r.good();
}
bool fromJson(const QJsonValue &json, Capabilities &value, Error &error)
{
	Reader r(json, error);
	r.field("ffmpeg", value.ffmpeg);
	r.field("ffprobe", value.ffprobe);
	r.field("helper", value.helper);
	r.field("nvidia", value.nvidia);
	return r.good();
}
bool fromJson(const QJsonValue &json, PolicyResult &value, Error &error)
{
	Reader r(json, error);
	r.field("revision", value.revision);
	r.field("fallback", value.fallback);
	return r.good();
}
bool fromJson(const QJsonValue &json, OutputDetail &value, Error &error)
{
	if (!fromJson(json, static_cast<Output &>(value), error))
		return false;
	Reader r(json, error);
	r.field("runtime", value.runtime);
	r.field("runtime_availability", value.runtimeAvailability);
	r.field("runtime_reason", value.runtimeReason);
	return r.good();
}
bool fromJson(const QJsonValue &json, Problem &value, Error &error)
{
	Reader r(json, error);
	r.field("type", value.type);
	r.field("title", value.title);
	r.field("status", value.status);
	r.field("detail", value.detail);
	if (json.isObject()) {
		const auto o = json.toObject();
		for (auto pair : {std::pair{"instance", &value.instance}, std::pair{"code", &value.code},
				  std::pair{"field", &value.field}}) {
			if (o.contains(QLatin1String(pair.first)) &&
			    !fromJson(o.value(QLatin1String(pair.first)), *pair.second, error))
				return false;
		}
	}
	return r.good();
}
bool fromJson(const QJsonValue &json, Preparation &value, Error &error)
{
	Reader r(json, error);
	r.field("asset_id", value.assetId);
	r.field("generation_id", value.generationId);
	return r.good();
}
bool validateRuntimeEvent(const RuntimeEvent &e)
{
	const auto kind = e.kind, owner = e.owner;
	const int payloads = int(e.input.hasValue()) + int(e.group.hasValue()) + int(e.output.hasValue()) +
			     int(e.ffmpeg.hasValue()) + int(e.gpu.hasValue());
	if (e.cursor.isEmpty() ||
	    !QStringList{QStringLiteral("state"), QStringLiteral("metrics"), QStringLiteral("input"),
			 QStringLiteral("gpu"), QStringLiteral("removed"), QStringLiteral("attempt")}
		     .contains(kind))
		return false;
	if ((kind == QStringLiteral("state") || kind == QStringLiteral("removed") ||
	     kind == QStringLiteral("attempt")) &&
	    payloads != 0)
		return false;
	if ((kind == QStringLiteral("metrics") || kind == QStringLiteral("input") || kind == QStringLiteral("gpu")) &&
	    payloads != 1)
		return false;
	if (owner == QStringLiteral("system"))
		return !e.observation.instanceId.isEmpty() && e.sessionId.isEmpty() && e.outputId.isEmpty() &&
		       e.groupId.isEmpty() && e.profileId.isEmpty() && e.groupInstanceId.isEmpty() &&
		       e.role.isEmpty() &&
		       ((kind == QStringLiteral("gpu") && e.gpu.hasValue()) || kind == QStringLiteral("removed"));
	if (kind == QStringLiteral("gpu"))
		return false;
	if (e.sessionId.isEmpty() || e.observation.instanceId.isEmpty())
		return false;
	if (owner == QStringLiteral("session")) {
		if (!e.outputId.isEmpty() || !e.groupId.isEmpty() || !e.profileId.isEmpty() ||
		    !e.groupInstanceId.isEmpty() || !e.role.isEmpty())
			return false;
		if (kind == QStringLiteral("input") || kind == QStringLiteral("metrics"))
			return e.input.hasValue();
		return kind == QStringLiteral("state") || kind == QStringLiteral("removed");
	}
	if (owner == QStringLiteral("group"))
		return !e.groupId.isEmpty() && !e.profileId.isEmpty() &&
		       (kind == QStringLiteral("state") || kind == QStringLiteral("removed") ||
			(kind == QStringLiteral("metrics") && e.group.hasValue()));
	if (owner == QStringLiteral("output"))
		return !e.outputId.isEmpty() && (kind == QStringLiteral("state") || kind == QStringLiteral("removed") ||
						 (kind == QStringLiteral("metrics") && e.output.hasValue()));
	if (owner == QStringLiteral("ffmpeg"))
		return !e.groupInstanceId.isEmpty() &&
		       (kind == QStringLiteral("state") || kind == QStringLiteral("removed") ||
			kind == QStringLiteral("attempt") ||
			(kind == QStringLiteral("metrics") && e.ffmpeg.hasValue()));
	return false;
}
Error decodeProblem(int status, const QByteArray &body)
{
	auto result = makeError(ErrorKind::Http, status);
	auto problem = decode<Problem>(body);
	if (problem && problem.value->status == status)
		result.problem = std::move(*problem.value);
	return result;
}
} // namespace gost::api
