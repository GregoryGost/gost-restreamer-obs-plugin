#pragma once

#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>

namespace fixture {
inline QJsonObject object(const QByteArray &json)
{
	return QJsonDocument::fromJson(json).object();
}
inline QByteArray bytes(const QJsonObject &json)
{
	return QJsonDocument(json).toJson(QJsonDocument::Compact);
}
inline QJsonObject record(const QString &id)
{
	return {{QStringLiteral("id"), id},
		{QStringLiteral("revision"), QStringLiteral("1")},
		{QStringLiteral("created_at"), QStringLiteral("2026-11-10T01:02:03.123456789Z")},
		{QStringLiteral("updated_at"), QStringLiteral("2026-11-10T01:02:03.123456789Z")}};
}
inline QJsonObject stream(const QString &id = QStringLiteral("stream"))
{
	auto result = record(id);
	result.insert(QStringLiteral("name"), QStringLiteral("Поток"));
	result.insert(QStringLiteral("publication_key"), QStringLiteral("publish+%2F/ключ"));
	result.insert(QStringLiteral("fallback"), QJsonValue::Null);
	return result;
}
inline QString endpoint()
{
	return QStringLiteral("rtmps://user:p%40ss@EXAMPLE.com:443/live/%2f?q=a%2Bb&token=%252F");
}
inline QJsonObject destination()
{
	auto result = record(QStringLiteral("destination"));
	result.insert(QStringLiteral("stream_id"), QStringLiteral("stream"));
	result.insert(QStringLiteral("group_id"), QStringLiteral("group"));
	result.insert(QStringLiteral("name"), QStringLiteral("Назначение"));
	result.insert(QStringLiteral("platform"), QStringLiteral("custom"));
	result.insert(QStringLiteral("endpoint"), endpoint());
	result.insert(QStringLiteral("stream_key"), QStringLiteral("key/%2F+="));
	result.insert(QStringLiteral("enabled"), true);
	return result;
}
inline QJsonObject format()
{
	return object(R"({"video":{"codec":"h264","width":2560,"height":1440,
"frame_rate":{"numerator":60000,"denominator":1001},"codec_profile":"high","level":"5.1",
"tier":"","pixel_format":"yuv420p","bit_depth":8,
"color":{"range":"tv","primaries":"bt709","transfer":"bt709","matrix":"bt709"}},
"audio":{"codec":"aac","codec_profile":"LC","sample_rate_hz":48000,"channels":2,"channel_layout":"stereo"}})");
}
inline QJsonObject groupProfile()
{
	auto result = object(R"({"mode":"transcode","encoding":null,
"video_encoding":{"target_bitrate_bps":"6000000","max_bitrate_bps":"8000000","buffer_bits":"12000000",
"gop_frames":120,"max_b_frames":2,"closed_gop":true},"audio_encoding":{"bitrate_bps":"192000"}})");
	result.insert(QStringLiteral("format"), format());
	return result;
}
inline QJsonObject encoding()
{
	return object(R"({"version":2,"video_encoder":"h264_nvenc","audio_encoder":"aac","rate_control":"vbr",
"rescale_output":false,"video_custom":"nonref_p=1","audio_custom":"aac_coder=twoloop",
"nvenc":{"preset":"p5","tuning":"hq","multipass":"fullres","lookahead_frames":16,
"enable_spatial_aq":true,"enable_temporal_aq":true,"aq_strength":8}})");
}
inline QJsonObject profile()
{
	auto result = record(QStringLiteral("profile"));
	result.insert(QStringLiteral("name"), QStringLiteral("Профиль"));
	result.insert(QStringLiteral("profile"), groupProfile());
	return result;
}
inline QJsonObject group()
{
	auto result = record(QStringLiteral("group"));
	result.insert(QStringLiteral("stream_id"), QStringLiteral("stream"));
	result.insert(QStringLiteral("profile_id"), QStringLiteral("profile"));
	result.insert(QStringLiteral("name"), QStringLiteral("Группа"));
	return result;
}
inline QJsonObject session()
{
	auto result = record(QStringLiteral("session"));
	result.insert(QStringLiteral("stream_id"), QStringLiteral("stream"));
	result.insert(QStringLiteral("state"), QStringLiteral("RUNNING"));
	result.insert(QStringLiteral("desired_paused"), false);
	result.insert(QStringLiteral("stream"), stream());
	result.insert(QStringLiteral("groups"), QJsonArray{QJsonObject{{QStringLiteral("group"), group()},
								       {QStringLiteral("profile"), profile()}}});
	result.insert(QStringLiteral("ended_at"), QJsonValue::Null);
	result.insert(QStringLiteral("failure"), QJsonValue::Null);
	return result;
}
inline QJsonObject output()
{
	auto result = record(QStringLiteral("output"));
	result.insert(QStringLiteral("session_id"), QStringLiteral("session"));
	result.insert(QStringLiteral("destination"), destination());
	result.insert(QStringLiteral("group"), group());
	result.insert(QStringLiteral("profile"), profile());
	result.insert(QStringLiteral("fallback"), QJsonValue::Null);
	result.insert(QStringLiteral("desired_running"), true);
	result.insert(QStringLiteral("attached"), true);
	return result;
}
inline QJsonObject asset()
{
	auto result = record(QStringLiteral("asset"));
	for (const auto &key : {QStringLiteral("name"), QStringLiteral("kind"), QStringLiteral("state"),
				QStringLiteral("current_generation_id"), QStringLiteral("pending_generation_id")})
		result.insert(key, QStringLiteral("value"));
	result.insert(QStringLiteral("size_bytes"), QStringLiteral("1024"));
	return result;
}
inline QJsonObject variant()
{
	auto result = record(QStringLiteral("variant"));
	result.insert(QStringLiteral("asset_id"), QStringLiteral("asset"));
	result.insert(QStringLiteral("generation_id"), QStringLiteral("generation"));
	result.insert(QStringLiteral("state"), QStringLiteral("READY"));
	result.insert(QStringLiteral("profile"), groupProfile());
	result.insert(QStringLiteral("format"), format());
	result.insert(QStringLiteral("size_bytes"), QStringLiteral("1024"));
	return result;
}
inline QJsonObject observation(const QString &instance = QStringLiteral("epoch:1"), quint64 revision = 1,
			       const QString &state = QStringLiteral("RUNNING"))
{
	return {{QStringLiteral("instance_id"), instance},
		{QStringLiteral("revision"), QString::number(revision)},
		{QStringLiteral("observed_at"), QStringLiteral("2026-11-10T01:02:03.123456789Z")},
		{QStringLiteral("state"), state},
		{QStringLiteral("failure"), QJsonValue::Null}};
}
inline QJsonObject media()
{
	const auto rate = object(R"({"status":"warming_up","reason":"","value":null,"window_start":null,
"window_end":null,"duration_ns":"0"})");
	auto result = object(R"({"sampled_at":null,"video_frames":"0","payload_bytes":"0","written_bytes":"0",
"audio_units":"0","saturated":false,"last_frame":null,
"format":{"video_codec":"h264","audio_codec":"aac","width":2560,"height":1440,"sample_rate_hz":48000,"channels":2}})");
	result.insert(QStringLiteral("rates"), QJsonObject{{QStringLiteral("fps"), rate},
							   {QStringLiteral("payload_bitrate"), rate},
							   {QStringLiteral("written_bitrate"), rate}});
	return result;
}
inline QJsonObject outputMetrics()
{
	auto result = object(R"({"counters":{"reconnects":"2"},"queue_drops":{},"delivery_drops":{},"resync":false,
"last_write":null,"last_media":null,"ack_ahead_events_total":"18446744073709551615",
"ack_ahead_bytes_last":"9007199254740993","ack_ahead_bytes_max":"18446744073709551614","ack_last_anomaly_at":null})");
	result.insert(QStringLiteral("media"), media());
	return result;
}
inline QJsonObject event(const QString &kind = QStringLiteral("state"),
			 const QString &cursor = QStringLiteral("opaque/next"))
{
	return {{QStringLiteral("cursor"), cursor},
		{QStringLiteral("kind"), kind},
		{QStringLiteral("owner"), QStringLiteral("session")},
		{QStringLiteral("session_id"), QStringLiteral("session")},
		{QStringLiteral("output_id"), QString{}},
		{QStringLiteral("group_id"), QString{}},
		{QStringLiteral("profile_id"), QString{}},
		{QStringLiteral("observation"), observation()},
		{QStringLiteral("group_instance_id"), QString{}},
		{QStringLiteral("source"), QString{}},
		{QStringLiteral("source_revision"), QStringLiteral("0")},
		{QStringLiteral("role"), QString{}},
		{QStringLiteral("attempt_active"), false},
		{QStringLiteral("reason"), QString{}},
		{QStringLiteral("input"), QJsonValue::Null},
		{QStringLiteral("group"), QJsonValue::Null},
		{QStringLiteral("output"), QJsonValue::Null},
		{QStringLiteral("ffmpeg"), QJsonValue::Null},
		{QStringLiteral("gpu"), QJsonValue::Null}};
}
inline QByteArray frame(const QJsonObject &event, const QByteArray &eventName = {}, const QByteArray &id = {})
{
	return "event: " + (eventName.isEmpty() ? event.value(QStringLiteral("kind")).toString().toUtf8() : eventName) +
	       "\nid: " + (id.isEmpty() ? event.value(QStringLiteral("cursor")).toString().toUtf8() : id) +
	       "\ndata: " + bytes(event) + "\n\n";
}
inline QByteArray page(const QJsonArray &items, const QString &cursor = {})
{
	return bytes({{QStringLiteral("items"), items}, {QStringLiteral("next_cursor"), cursor}});
}
inline QByteArray capabilities()
{
	const auto capability = object(R"({"status":"unverified","reason":"","checked_at":null})");
	return bytes({{QStringLiteral("ffmpeg"), capability},
		      {QStringLiteral("ffprobe"), capability},
		      {QStringLiteral("helper"), capability},
		      {QStringLiteral("nvidia"), capability}});
}
} // namespace fixture
