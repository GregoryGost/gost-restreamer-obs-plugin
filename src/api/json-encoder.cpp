#include "api/json-encoder.hpp"
#include "api/json-decoder.hpp"
#include <QJsonValue>
#include <QSet>
#include <QStringList>
#include <limits>

namespace gost::api {
namespace {
// Возвращает локальную ошибку известного поля без включения его потенциально секретного значения.
Result<QJsonObject> invalid(const QString &field)
{
	// error содержит безопасное общее сообщение, а problem указывает только публичное поле.
	auto error = makeError(ErrorKind::Contract);
	// problem передаёт UI расположение ошибки без исходного значения.
	Problem problem;
	problem.field = field;
	problem.errors.push_back({field, QStringLiteral("Недопустимое значение поля.")});
	error.problem = std::move(problem);
	return Result<QJsonObject>::failure(std::move(error));
}
// Выдаёт UTF-8 строкой, а произвольные байты — канонической base64-оболочкой.
QJsonValue text(const ProfileText &value)
{
	// decoded используется только при точном обратном преобразовании, включая начальный BOM и NUL.
	const auto decoded = QString::fromUtf8(value.bytes);
	if (value.bytes.isValidUtf8() && decoded.toUtf8() == value.bytes)
		return decoded;
	return QJsonObject{{QStringLiteral("encoding"), QStringLiteral("base64")},
			   {QStringLiteral("value"), QString::fromLatin1(value.bytes.toBase64())}};
}
// Вставляет только явно выбранное optional-поле; false и пустая строка остаются значениями.
template<class T> void optional(QJsonObject &object, const char *key, const std::optional<T> &value)
{
	if (value.has_value())
		object.insert(QLatin1String(key), QJsonValue(*value));
}
// Создаёт общий PATCH-документ с точной ревизией, не используя JSON double.
QJsonObject patch(quint64 revision)
{
	return {{QStringLiteral("expected_revision"), QString::number(revision)}};
}
// Проверяет документированные пары name=value, разделённые только ASCII-пробелом.
// options задаёт whitelist значений; повтор имени, CLI и неизвестные имена отвергаются.
bool custom(const QString &input, const QMap<QString, QStringList> &options)
{
	if (input.toUtf8().size() > 4096)
		return false;
	// tokens содержит разрешённое число пар; пустые ASCII-пробелы не создают настройки.
	const auto tokens = input.split(QLatin1Char(' '), Qt::SkipEmptyParts);
	if (tokens.size() > 32)
		return false;
	// seen предотвращает неоднозначное повторение одной настройки.
	QSet<QString> seen;
	// token содержит одну непустую пару из исходной строки.
	for (const auto &token : tokens) {
		// parts разделяет имя и значение; дополнительные '=' не являются разрешённой парой.
		const auto parts = token.split(QLatin1Char('='));
		if (parts.size() != 2 || parts[0].toUtf8().size() > 64 || parts[1].toUtf8().size() > 128 ||
		    seen.contains(parts[0]) || !options.contains(parts[0]) ||
		    !options.value(parts[0]).contains(parts[1]))
			return false;
		seen.insert(parts[0]);
	}
	return true;
}
// Проверяет числовые ограничения v2 без арифметики double и без изменения исходных ProfileText.
// Legacy-рецепт сюда не передаётся: его совместимость и возможность запуска оценивает сервер.
Result<QJsonObject> validateRecipe(const GroupProfile &profile)
{
	// format задаёт выходную геометрию/FPS, video — битрейты/GOP, encoding — явный новый рецепт.
	const auto &format = profile.format.video;
	const auto &video = profile.videoEncoding;
	const auto &encoding = profile.encoding.value;
	// audio содержит частоту и каналы; bitrate задаёт AAC тишины и LIVE только при transcode.
	const auto &audio = profile.format.audio;
	const auto bitrate = profile.audioEncoding.bitrateBps;
	if (!format.width || !format.height || (format.width % 2) || (format.height % 2))
		return invalid(QStringLiteral("profile.format.video.width"));
	if (!format.frameRate.numerator || !format.frameRate.denominator)
		return invalid(QStringLiteral("profile.format.video.frame_rate"));
	if (!video.closedGop || !video.gopFrames || video.gopFrames > 2147483647U)
		return invalid(QStringLiteral("profile.video_encoding.gop_frames"));
	if (!video.targetBitrateBps)
		return invalid(QStringLiteral("profile.video_encoding.target_bitrate_bps"));
	if (encoding.videoEncoder == QStringLiteral("h264_nvenc")) {
		if (profile.mode != QStringLiteral("transcode") || format.codec != QStringLiteral("h264"))
			return invalid(QStringLiteral("profile.encoding.video_encoder"));
		if (format.width > 65534 || format.height > 65534)
			return invalid(QStringLiteral("profile.format.video.width"));
		if (video.targetBitrateBps > video.maxBitrateBps || video.maxBitrateBps > 4294967295ULL ||
		    !video.bufferBits || video.bufferBits > 2147483647ULL ||
		    (encoding.rateControl == QStringLiteral("cbr") && video.targetBitrateBps != video.maxBitrateBps))
			return invalid(QStringLiteral("profile.video_encoding"));
		if (video.maxBFrames > 4 || video.maxBFrames >= video.gopFrames ||
		    (format.codecProfile.bytes == QByteArrayLiteral("baseline") && video.maxBFrames != 0))
			return invalid(QStringLiteral("profile.video_encoding.max_b_frames"));
	} else if (encoding.videoEncoder == QStringLiteral("libsvtav1")) {
		if (profile.mode != QStringLiteral("copy") || format.codec != QStringLiteral("av1") ||
		    encoding.rescaleOutput)
			return invalid(QStringLiteral("profile.encoding.video_encoder"));
		if (format.width < 64 || format.width > 16384 || format.height < 64 || format.height > 8704 ||
		    quint64(format.frameRate.numerator) > 240ULL * format.frameRate.denominator)
			return invalid(QStringLiteral("profile.format.video"));
		if (video.gopFrames < 2 || video.maxBFrames || video.targetBitrateBps > 100000000ULL)
			return invalid(QStringLiteral("profile.video_encoding"));
		if (encoding.rateControl == QStringLiteral("vbr") && (video.maxBitrateBps || video.bufferBits))
			return invalid(QStringLiteral("profile.video_encoding"));
		if (encoding.rateControl == QStringLiteral("cbr")) {
			// scaledBuffer переводит биты в числитель миллисекунд; проверка границы исключает переполнение.
			if (video.bufferBits > std::numeric_limits<quint64>::max() / 1000ULL)
				return invalid(QStringLiteral("profile.video_encoding.buffer_bits"));
			const auto scaledBuffer = video.bufferBits * 1000ULL;
			if (video.maxBitrateBps != video.targetBitrateBps || scaledBuffer % video.targetBitrateBps ||
			    scaledBuffer / video.targetBitrateBps < 20 || scaledBuffer / video.targetBitrateBps > 10000)
				return invalid(QStringLiteral("profile.video_encoding.buffer_bits"));
		}
	}
	if (audio.codec != QStringLiteral("aac") ||
	    !QList<quint32>{7350, 8000, 11025, 12000, 16000, 22050, 24000, 32000, 44100, 48000, 64000}.contains(
		    audio.sampleRateHz) ||
	    (audio.channels != 1 && audio.channels != 2) || !bitrate ||
	    bitrate > 6ULL * audio.sampleRateHz * audio.channels)
		return invalid(QStringLiteral("profile.audio_encoding"));
	if (audio.channelLayout.bytes !=
	    (audio.channels == 1 ? QByteArrayLiteral("mono") : QByteArrayLiteral("stereo")))
		return invalid(QStringLiteral("profile.format.audio.channel_layout"));
	return Result<QJsonObject>::success({});
}
// Кодирует encoding v2; проверяет документированные параметры и не создаёт неприменимые блоки.
Result<QJsonObject> encodeEncoding(const GroupProfile &profile)
{
	// encoding — проверяемый рецепт, video — общие битрейты и GOP этого же профиля.
	const auto &encoding = profile.encoding.value;
	const auto &video = profile.videoEncoding;
	if (encoding.version != 2)
		return invalid(QStringLiteral("profile.encoding.version"));
	if (encoding.rateControl != QStringLiteral("cbr") && encoding.rateControl != QStringLiteral("vbr"))
		return invalid(QStringLiteral("profile.encoding.rate_control"));
	if (encoding.audioEncoder != QStringLiteral("aac"))
		return invalid(QStringLiteral("profile.encoding.audio_encoder"));
	if (!custom(encoding.audioCustom,
		    {{QStringLiteral("aac_coder"), {QStringLiteral("twoloop"), QStringLiteral("fast")}}}))
		return invalid(QStringLiteral("profile.encoding.audio_custom"));
	if (encoding.nvenc.isNull() || encoding.svtAv1.isNull())
		return invalid(QStringLiteral("profile.encoding"));
	// validation проверяет числовые взаимосвязи, не подставляя новые значения в исходный профиль.
	auto validation = validateRecipe(profile);
	if (!validation)
		return validation;
	// object содержит только документированные общие поля encoding v2.
	QJsonObject object{{QStringLiteral("version"), encoding.version},
			   {QStringLiteral("video_encoder"), encoding.videoEncoder},
			   {QStringLiteral("audio_encoder"), encoding.audioEncoder},
			   {QStringLiteral("rate_control"), encoding.rateControl},
			   {QStringLiteral("rescale_output"), encoding.rescaleOutput},
			   {QStringLiteral("video_custom"), encoding.videoCustom},
			   {QStringLiteral("audio_custom"), encoding.audioCustom}};
	if (encoding.videoEncoder == QStringLiteral("h264_nvenc")) {
		if (encoding.svtAv1.hasValue() ||
		    !custom(encoding.videoCustom,
			    {{QStringLiteral("nonref_p"), {QStringLiteral("0"), QStringLiteral("1")}},
			     {QStringLiteral("strict_gop"), {QStringLiteral("0"), QStringLiteral("1")}}}))
			return invalid(QStringLiteral("profile.encoding.video_custom"));
		if (encoding.nvenc.hasValue()) {
			// nvenc содержит применимые параметры аппаратного H264.
			const auto &nvenc = encoding.nvenc.value;
			if (!QStringList{QStringLiteral("p1"), QStringLiteral("p2"), QStringLiteral("p3"),
					 QStringLiteral("p4"), QStringLiteral("p5"), QStringLiteral("p6"),
					 QStringLiteral("p7")}
				     .contains(nvenc.preset) ||
			    !QStringList{QStringLiteral("hq"), QStringLiteral("ll"), QStringLiteral("ull")}.contains(
				    nvenc.tuning) ||
			    !QStringList{QStringLiteral("disabled"), QStringLiteral("qres"), QStringLiteral("fullres")}
				     .contains(nvenc.multipass) ||
			    nvenc.lookaheadFrames < 0 || nvenc.lookaheadFrames > 31 || nvenc.aqStrength < 1 ||
			    nvenc.aqStrength > 15 || (!nvenc.enableSpatialAq && nvenc.aqStrength != 8) ||
			    (nvenc.lookaheadFrames > 0 && quint64(nvenc.lookaheadFrames) + video.maxBFrames > 31))
				return invalid(QStringLiteral("profile.encoding.nvenc"));
			object.insert(QStringLiteral("nvenc"),
				      QJsonObject{{QStringLiteral("preset"), nvenc.preset},
						  {QStringLiteral("tuning"), nvenc.tuning},
						  {QStringLiteral("multipass"), nvenc.multipass},
						  {QStringLiteral("lookahead_frames"), nvenc.lookaheadFrames},
						  {QStringLiteral("enable_spatial_aq"), nvenc.enableSpatialAq},
						  {QStringLiteral("enable_temporal_aq"), nvenc.enableTemporalAq},
						  {QStringLiteral("aq_strength"), nvenc.aqStrength}});
		}
	} else if (encoding.videoEncoder == QStringLiteral("libsvtav1")) {
		if (encoding.nvenc.hasValue() ||
		    !custom(encoding.videoCustom,
			    {{QStringLiteral("lp"),
			      {QStringLiteral("0"), QStringLiteral("1"), QStringLiteral("2"), QStringLiteral("3"),
			       QStringLiteral("4"), QStringLiteral("5"), QStringLiteral("6")}}}))
			return invalid(QStringLiteral("profile.encoding.video_custom"));
		if (encoding.svtAv1.hasValue()) {
			// svt содержит применимые параметры программного AV1.
			const auto &svt = encoding.svtAv1.value;
			if (svt.preset < 0 || svt.preset > 13 || svt.aqMode != 2 ||
			    (svt.tuning != QStringLiteral("psnr") && svt.tuning != QStringLiteral("vq")) ||
			    (encoding.rateControl == QStringLiteral("cbr") &&
			     (svt.tuning != QStringLiteral("psnr") || svt.lookaheadFrames != 0)) ||
			    (encoding.rateControl == QStringLiteral("vbr") && svt.lookaheadFrames != -1 &&
			     (svt.lookaheadFrames < 6 || svt.lookaheadFrames > 120)))
				return invalid(QStringLiteral("profile.encoding.svt_av1"));
			object.insert(QStringLiteral("svt_av1"),
				      QJsonObject{{QStringLiteral("preset"), svt.preset},
						  {QStringLiteral("tuning"), svt.tuning},
						  {QStringLiteral("aq_mode"), svt.aqMode},
						  {QStringLiteral("lookahead_frames"), svt.lookaheadFrames}});
		}
	} else {
		return invalid(QStringLiteral("profile.encoding.video_encoder"));
	}
	return Result<QJsonObject>::success(std::move(object));
}
} // namespace

// Возвращает полный профиль без потери ProfileText, uint64 и трёх состояний encoding.
Result<QJsonObject> encodeProfile(const GroupProfile &profile)
{
	if (profile.mode != QStringLiteral("copy") && profile.mode != QStringLiteral("transcode"))
		return invalid(QStringLiteral("profile.mode"));
	if (profile.liveAudioMode.isNull() ||
	    (profile.liveAudioMode.hasValue() && profile.liveAudioMode.value != QStringLiteral("copy") &&
	     profile.liveAudioMode.value != QStringLiteral("transcode")) ||
	    (profile.mode == QStringLiteral("copy") && profile.liveAudioMode.hasValue() &&
	     profile.liveAudioMode.value != QStringLiteral("copy")))
		return invalid(QStringLiteral("profile.live_audio_mode"));
	// video/audio содержат исходные форматы, ve — битрейты и GOP без преобразования через double.
	const auto &video = profile.format.video;
	const auto &audio = profile.format.audio;
	const auto &ve = profile.videoEncoding;
	if (video.frameRate.numerator && !video.frameRate.denominator)
		return invalid(QStringLiteral("profile.format.video.frame_rate"));
	// object включает все обязательные поля полной замены, даже их нулевые и пустые значения.
	QJsonObject object{
		{QStringLiteral("mode"), profile.mode},
		{QStringLiteral("format"),
		 QJsonObject{{QStringLiteral("video"),
			      QJsonObject{{QStringLiteral("codec"), video.codec},
					  {QStringLiteral("width"), qint64(video.width)},
					  {QStringLiteral("height"), qint64(video.height)},
					  {QStringLiteral("frame_rate"),
					   QJsonObject{{QStringLiteral("numerator"), qint64(video.frameRate.numerator)},
						       {QStringLiteral("denominator"),
							qint64(video.frameRate.denominator)}}},
					  {QStringLiteral("codec_profile"), text(video.codecProfile)},
					  {QStringLiteral("level"), text(video.level)},
					  {QStringLiteral("tier"), text(video.tier)},
					  {QStringLiteral("pixel_format"), text(video.pixelFormat)},
					  {QStringLiteral("bit_depth"), video.bitDepth},
					  {QStringLiteral("color"),
					   QJsonObject{{QStringLiteral("range"), text(video.color.range)},
						       {QStringLiteral("primaries"), text(video.color.primaries)},
						       {QStringLiteral("transfer"), text(video.color.transfer)},
						       {QStringLiteral("matrix"), text(video.color.matrix)}}}}},
			     {QStringLiteral("audio"),
			      QJsonObject{{QStringLiteral("codec"), audio.codec},
					  {QStringLiteral("codec_profile"), text(audio.codecProfile)},
					  {QStringLiteral("sample_rate_hz"), qint64(audio.sampleRateHz)},
					  {QStringLiteral("channels"), audio.channels},
					  {QStringLiteral("channel_layout"), text(audio.channelLayout)}}}}},
		{QStringLiteral("video_encoding"),
		 QJsonObject{{QStringLiteral("target_bitrate_bps"), QString::number(ve.targetBitrateBps)},
			     {QStringLiteral("max_bitrate_bps"), QString::number(ve.maxBitrateBps)},
			     {QStringLiteral("buffer_bits"), QString::number(ve.bufferBits)},
			     {QStringLiteral("gop_frames"), qint64(ve.gopFrames)},
			     {QStringLiteral("max_b_frames"), qint64(ve.maxBFrames)},
			     {QStringLiteral("closed_gop"), ve.closedGop}}},
		{QStringLiteral("audio_encoding"),
		 QJsonObject{{QStringLiteral("bitrate_bps"), QString::number(profile.audioEncoding.bitrateBps)}}}};
	if (profile.liveAudioMode.hasValue())
		object.insert(QStringLiteral("live_audio_mode"), profile.liveAudioMode.value);
	if (profile.encoding.isNull())
		object.insert(QStringLiteral("encoding"), QJsonValue::Null);
	if (profile.encoding.hasValue()) {
		// encoding содержит проверенный применимый рецепт v2 либо ошибку публичного поля.
		auto encoding = encodeEncoding(profile);
		if (!encoding)
			return encoding;
		object.insert(QStringLiteral("encoding"), *encoding.value);
	}
	return Result<QJsonObject>::success(std::move(object));
}
// Создаёт Stream только с обязательными полями, исключая fallback-политику редактора.
Result<QJsonObject> encodeRequest(const StreamCreate &request)
{
	if (request.name.isEmpty() || request.publicationKey.isEmpty())
		return invalid(request.name.isEmpty() ? QStringLiteral("name") : QStringLiteral("publication_key"));
	return Result<QJsonObject>::success(
		{{QStringLiteral("name"), request.name}, {QStringLiteral("publication_key"), request.publicationKey}});
}
// Формирует минимальный PATCH Stream с точной ревизией.
Result<QJsonObject> encodeRequest(const StreamPatch &request)
{
	// object начинает документ с обязательной ревизии.
	auto object = patch(request.expectedRevision);
	optional(object, "name", request.name);
	return Result<QJsonObject>::success(std::move(object));
}
// Сохраняет секретные строки назначения ровно в том виде, как они введены.
Result<QJsonObject> encodeRequest(const DestinationCreate &request)
{
	if (request.streamId.isEmpty() || request.groupId.isEmpty() || request.name.isEmpty() ||
	    request.platform.isEmpty() || request.endpoint.isEmpty())
		return invalid(QStringLiteral("destination"));
	return Result<QJsonObject>::success({{QStringLiteral("stream_id"), request.streamId},
					     {QStringLiteral("group_id"), request.groupId},
					     {QStringLiteral("name"), request.name},
					     {QStringLiteral("platform"), request.platform},
					     {QStringLiteral("endpoint"), request.endpoint},
					     {QStringLiteral("stream_key"), request.streamKey},
					     {QStringLiteral("enabled"), request.enabled}});
}
// Передаёт только явно изменённые поля назначения, включая false и очищаемый ключ.
Result<QJsonObject> encodeRequest(const DestinationPatch &request)
{
	// object начинает документ с обязательной ревизии.
	auto object = patch(request.expectedRevision);
	optional(object, "stream_id", request.streamId);
	optional(object, "group_id", request.groupId);
	optional(object, "name", request.name);
	optional(object, "platform", request.platform);
	optional(object, "endpoint", request.endpoint);
	optional(object, "stream_key", request.streamKey);
	optional(object, "enabled", request.enabled);
	return Result<QJsonObject>::success(std::move(object));
}
// Создаёт Group без встроенной копии профиля.
Result<QJsonObject> encodeRequest(const GroupCreate &request)
{
	if (request.streamId.isEmpty() || request.profileId.isEmpty() || request.name.isEmpty())
		return invalid(QStringLiteral("group"));
	return Result<QJsonObject>::success({{QStringLiteral("stream_id"), request.streamId},
					     {QStringLiteral("profile_id"), request.profileId},
					     {QStringLiteral("name"), request.name}});
}
// Передаёт выбранные изменения связей и имени Group.
Result<QJsonObject> encodeRequest(const GroupPatch &request)
{
	// object начинает документ с обязательной ревизии.
	auto object = patch(request.expectedRevision);
	optional(object, "stream_id", request.streamId);
	optional(object, "profile_id", request.profileId);
	optional(object, "name", request.name);
	return Result<QJsonObject>::success(std::move(object));
}
// Создаёт именованный профиль после проверки полного документа.
Result<QJsonObject> encodeRequest(const ProfileCreate &request)
{
	if (request.name.isEmpty())
		return invalid(QStringLiteral("name"));
	// profile содержит сериализованный рецепт или локальную ошибку до сетевой передачи.
	auto profile = encodeProfile(request.profile);
	if (!profile)
		return profile;
	return Result<QJsonObject>::success(
		{{QStringLiteral("name"), request.name}, {QStringLiteral("profile"), *profile.value}});
}
// Name-only PATCH не содержит profile; явная замена передаёт весь рецепт.
Result<QJsonObject> encodeRequest(const ProfilePatch &request)
{
	// object начинает документ с обязательной ревизии.
	auto object = patch(request.expectedRevision);
	optional(object, "name", request.name);
	if (request.profile) {
		// profile сохраняет ошибку валидации без отправки частично построенного документа.
		auto profile = encodeProfile(*request.profile);
		if (!profile)
			return profile;
		object.insert(QStringLiteral("profile"), *profile.value);
	}
	return Result<QJsonObject>::success(std::move(object));
}
} // namespace gost::api
