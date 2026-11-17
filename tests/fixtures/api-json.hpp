#pragma once

#include "api/models.hpp"
#include "api/json-value.hpp"
#include <QRegularExpression>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>

namespace fixture {
// Синтетические документы собираются независимо от декодера. Функции возвращают новые JSON-значения,
// которые сценарий может менять для отрицательной проверки. object предназначен для литералов фикстуры,
// а не для проверки разбора: он не сообщает ошибку JSON; валидность входа обеспечивает автор сценария.
// Числа до I64 сохраняет Qt; явные маркеры больших U64 разворачиваются только в байтовой фикстуре.
inline QJsonObject object(const QByteArray &json)
{
	return QJsonDocument::fromJson(json).object();
}
// Сериализует независимое дерево фикстуры, разворачивая только явные @u64 маркеры больших чисел.
inline QByteArray bytes(const QJsonObject &json)
{
	// result содержит независимый JSON Qt; marker адресует только явно помеченные большие числа фикстуры.
	QByteArray result = QJsonDocument(json).toJson(QJsonDocument::Compact);
	const QRegularExpression marker(QStringLiteral("\"@u64:([0-9]+)\""));
	return QString::fromUtf8(result).replace(marker, QStringLiteral("\\1")).toUtf8();
}
// Преобразует синтетическое значение Qt в типизированный вход, сохраняя его JSON-тип.
inline gost::api::JsonValue wire(const QJsonValue &value)
{
	if (value.isUndefined())
		return {};
	// parsed содержит ровно один элемент массива, чтобы поддержать также скалярные фикстуры.
	gost::api::JsonValue parsed;
	gost::api::parseJson(QJsonDocument(QJsonArray{value}).toJson(QJsonDocument::Compact), parsed);
	return parsed.toArray().first();
}
// Даёт структурное представление успешного writer для UI-сравнений небольших чисел.
// Точность U64 проверяется отдельно по raw байтам, этот адаптер не используется для wire ingress.
inline gost::api::Result<QJsonObject> qtResult(const gost::api::Result<gost::api::JsonValue> &encoded)
{
	if (!encoded)
		return gost::api::Result<QJsonObject>::failure(*encoded.error);
	return gost::api::Result<QJsonObject>::success(object(gost::api::serializeJson(*encoded.value)));
}
// Возвращает запись с числовой положительной ревизией и точными строковыми timestamp.
inline QJsonObject record(const QString &id)
{
	return {{QStringLiteral("id"), id},
		{QStringLiteral("revision"), QJsonValue(qint64(1))},
		{QStringLiteral("created_at"), QStringLiteral("2026-11-10T01:02:03.123456789Z")},
		{QStringLiteral("updated_at"), QStringLiteral("2026-11-10T01:02:03.123456789Z")}};
}
// Создаёт Stream с числовой ревизией, точным ключом и отсутствующим fallback.
inline QJsonObject stream(const QString &id = QStringLiteral("stream"))
{
	// result содержит независимый изменяемый документ текущей сущности.
	auto result = record(id);
	result.insert(QStringLiteral("name"), QStringLiteral("Поток"));
	result.insert(QStringLiteral("publication_key"), QStringLiteral("publish+%2F/ключ"));
	result.insert(QStringLiteral("fallback"), QJsonValue::Null);
	return result;
}
inline QString endpoint()
{
	// Разный регистр, учётные данные и вложенное кодирование выявляют незаметную нормализацию при чтении адреса.
	return QStringLiteral("rtmps://user:p%40ss@EXAMPLE.com:443/live/%2f?q=a%2Bb&token=%252F");
}
// Создаёт назначение с отдельными точными endpoint и stream_key.
inline QJsonObject destination()
{
	// result содержит независимый изменяемый документ текущей сущности.
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
// Задаёт независимые полные H.264/AAC дорожки с исходным рациональным FPS.
inline QJsonObject format()
{
	return object(R"({"video":{"codec":"h264","width":2560,"height":1440,
"frame_rate":{"numerator":60000,"denominator":1001},"codec_profile":"high","level":"5.1",
"tier":"","pixel_format":"yuv420p","bit_depth":8,
"color":{"range":"tv","primaries":"bt709","transfer":"bt709","matrix":"bt709"}},
"audio":{"codec":"aac","codec_profile":"LC","sample_rate_hz":48000,"channels":2,"channel_layout":"stereo"}})");
}
// Возвращает полный числовой рецепт H.264 с явной обработкой LIVE audio.
inline QJsonObject encoding();
// Собирает полный versionless профиль с явным LIVE audio copy.
inline QJsonObject groupProfile()
{
	// result задаёт полный versionless рецепт независимо от сериализатора продукта.
	// result содержит независимый изменяемый документ текущей сущности.
	auto result = object(R"({"mode":"transcode","live_audio_mode":"copy",
"video_encoding":{"target_bitrate_bps":6000000,"max_bitrate_bps":8000000,"buffer_bits":12000000,
"gop_frames":120,"max_b_frames":2,"closed_gop":true},"audio_encoding":{"bitrate_bps":192000}})");
	result.insert(QStringLiteral("format"), format());
	result.insert(QStringLiteral("encoding"), encoding());
	return result;
}
// Задаёт полный NVENC VBR рецепт с обязательными применимыми настройками.
inline QJsonObject encoding()
{
	return object(R"({"video_encoder":"h264_nvenc","audio_encoder":"aac","rate_control":"vbr",
"rescale_output":false,"video_custom":"nonref_p=1","audio_custom":"aac_coder=twoloop",
"nvenc":{"preset":"p5","tuning":"hq","multipass":"fullres","lookahead_frames":16,
"enable_spatial_aq":true,"enable_temporal_aq":true,"aq_strength":8}})");
}
// Оборачивает независимый полный рецепт записью Profile.
inline QJsonObject profile()
{
	// result содержит независимый изменяемый документ текущей сущности.
	auto result = record(QStringLiteral("profile"));
	result.insert(QStringLiteral("name"), QStringLiteral("Профиль"));
	result.insert(QStringLiteral("profile"), groupProfile());
	return result;
}
// Связывает Stream и Profile отдельной конфигурационной записью Group.
inline QJsonObject group()
{
	// result содержит независимый изменяемый документ текущей сущности.
	auto result = record(QStringLiteral("group"));
	result.insert(QStringLiteral("stream_id"), QStringLiteral("stream"));
	result.insert(QStringLiteral("profile_id"), QStringLiteral("profile"));
	result.insert(QStringLiteral("name"), QStringLiteral("Группа"));
	return result;
}
// Собирает исторические снимки Session по значению, без связи с изменяемыми DTO.
inline QJsonObject session()
{
	// Исторические вложенные Stream/Group/Profile собраны по значению, не ссылками на изменяемый реестр.
	// result содержит независимый изменяемый документ текущей сущности.
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
// Собирает долговечный Output с собственными снимками назначения и профиля.
inline QJsonObject output()
{
	// result содержит независимый изменяемый документ текущей сущности.
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
// Создаёт метаданные Asset с числовым размером.
inline QJsonObject asset()
{
	// result содержит независимый изменяемый документ текущей сущности.
	auto result = record(QStringLiteral("asset"));
	// key перечисляет обязательные строковые метаданные синтетического Asset.
	for (const auto &key : {QStringLiteral("name"), QStringLiteral("kind"), QStringLiteral("state"),
				QStringLiteral("current_generation_id"), QStringLiteral("pending_generation_id")})
		result.insert(key, QStringLiteral("value"));
	result.insert(QStringLiteral("size_bytes"), QJsonValue(qint64(1024)));
	return result;
}
// Создаёт READY Variant с полным desired рецептом и независимым измеренным format.
inline QJsonObject variant()
{
	// result содержит независимый изменяемый документ текущей сущности.
	auto result = record(QStringLiteral("variant"));
	result.insert(QStringLiteral("asset_id"), QStringLiteral("asset"));
	result.insert(QStringLiteral("generation_id"), QStringLiteral("generation"));
	result.insert(QStringLiteral("state"), QStringLiteral("READY"));
	result.insert(QStringLiteral("profile"), groupProfile());
	result.insert(QStringLiteral("format"), format());
	result.insert(QStringLiteral("size_bytes"), QJsonValue(qint64(1024)));
	return result;
}
// Задаёт наблюдение instance/revision/state с числовой ревизией и точной временной строкой.
inline QJsonObject observation(const QString &instance = QStringLiteral("epoch:1"), quint64 revision = 1,
			       const QString &state = QStringLiteral("RUNNING"))
{
	return {{QStringLiteral("instance_id"), instance},
		{QStringLiteral("revision"), QJsonValue(qint64(revision))},
		{QStringLiteral("observed_at"), QStringLiteral("2026-11-10T01:02:03.123456789Z")},
		{QStringLiteral("state"), state},
		{QStringLiteral("failure"), QJsonValue::Null}};
}
// Различает измеренные нулевые накопления и пока недоступные скорости.
inline QJsonObject media()
{
	// Нулевые накопления доступны отдельно от warming_up rates с null. Это различие не даёт UI
	// подменять отсутствие окна измерения фиктивной скоростью 0.
	// rate задаёт unavailable значение скорости до накопления окна.
	const auto rate = object(R"({"status":"warming_up","reason":"","value":null,"window_start":null,
"window_end":null,"duration_ns":0})");
	// result содержит независимый изменяемый документ текущей сущности.
	auto result = object(R"({"sampled_at":null,"video_frames":0,"payload_bytes":0,"written_bytes":0,
"audio_units":0,"saturated":false,"last_frame":null,
"format":{"video_codec":"h264","audio_codec":"aac","width":2560,"height":1440,"sample_rate_hz":48000,"channels":2}})");
	result.insert(QStringLiteral("rates"), QJsonObject{{QStringLiteral("fps"), rate},
							   {QStringLiteral("payload_bitrate"), rate},
							   {QStringLiteral("written_bitrate"), rate}});
	return result;
}
// Содержит точные предельные ACK U64 и nullable время последней аномалии.
inline QJsonObject outputMetrics()
{
	// ACK включает предел uint64 и число выше 2^53, чтобы промежуточный double ломал ожидания.
	// Карты потерь здесь пусты; для проверки отображения известного нуля нужна явная категория с нулевым значением.
	// result содержит независимый изменяемый документ текущей сущности.
	auto result = object(R"({"counters":{"reconnects":2},"queue_drops":{},"delivery_drops":{},"resync":false,
"last_write":null,"last_media":null,"ack_ahead_events_total":"@u64:18446744073709551615",
"ack_ahead_bytes_last":9007199254740993,"ack_ahead_bytes_max":"@u64:18446744073709551614","ack_last_anomaly_at":null})");
	result.insert(QStringLiteral("media"), media());
	return result;
}
// Создаёт структурное runtime Session событие с неприменимыми nullable блоками.
inline QJsonObject event(const QString &kind = QStringLiteral("state"),
			 const QString &cursor = QStringLiteral("opaque/next"))
{
	// Базовое Session-событие без содержательного блока данных. Для metrics/input/gpu сценарий обязан
	// установить согласованные owner, идентификаторы и блок данных; одна смена kind не создаёт валидное событие.
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
		{QStringLiteral("source_revision"), QJsonValue(qint64(0))},
		{QStringLiteral("role"), QString{}},
		{QStringLiteral("attempt_active"), false},
		{QStringLiteral("reason"), QString{}},
		{QStringLiteral("input"), QJsonValue::Null},
		{QStringLiteral("group"), QJsonValue::Null},
		{QStringLiteral("output"), QJsonValue::Null},
		{QStringLiteral("ffmpeg"), QJsonValue::Null},
		{QStringLiteral("gpu"), QJsonValue::Null}};
}
// Упаковывает независимый JSON payload в SSE кадр с управляемыми event/id.
inline QByteArray frame(const QJsonObject &event, const QByteArray &eventName = {}, const QByteArray &id = {})
{
	// Непустые переопределения намеренно могут противоречить JSON для проверки EventClient. Пустое переопределение
	// означает взять поле JSON; для теста отсутствующего/пустого id нужно собрать сетевой кадр вручную.
	return "event: " + (eventName.isEmpty() ? event.value(QStringLiteral("kind")).toString().toUtf8() : eventName) +
	       "\nid: " + (id.isEmpty() ? event.value(QStringLiteral("cursor")).toString().toUtf8() : id) +
	       "\ndata: " + bytes(event) + "\n\n";
}
// Создаёт страницу items с непрозрачным следующим cursor.
inline QByteArray page(const QJsonArray &items, const QString &cursor = {})
{
	return bytes({{QStringLiteral("items"), items}, {QStringLiteral("next_cursor"), cursor}});
}
// Возвращает независимые непроверенные возможности четырёх серверных компонентов.
inline QByteArray capabilities()
{
	// capability содержит общий шаблон непроверенного компонента.
	const auto capability = object(R"({"status":"unverified","reason":"","checked_at":null})");
	return bytes({{QStringLiteral("ffmpeg"), capability},
		      {QStringLiteral("ffprobe"), capability},
		      {QStringLiteral("helper"), capability},
		      {QStringLiteral("nvidia"), capability}});
}
} // namespace fixture
