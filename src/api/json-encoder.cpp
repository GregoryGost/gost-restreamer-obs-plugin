#include "api/json-encoder.hpp"
#include "api/json-decoder.hpp"
#include "api/profile-rules.hpp"
#include <limits>

namespace gost::api {
namespace {
/** Возвращает локальную ошибку обязательного поля без включения его значения. */
Result<JsonValue> invalid(const QString &field)
{
	// error содержит локальную причину; problem и entry сохраняют публичный путь поля.
	auto error = makeError(ErrorKind::Contract);
	Problem problem;
	problem.code = QStringLiteral("invalid_field");
	problem.field = field;
	FieldError entry;
	entry.location = field;
	entry.messageKey = QStringLiteral("Validation.Required");
	problem.errors.append(std::move(entry));
	error.problem = std::move(problem);
	return Result<JsonValue>::failure(std::move(error));
}
/** Возвращает строку из проверенных UTF-8 байтов профиля. */
JsonValue text(const ProfileText &value)
{
	return JsonValue(QString::fromUtf8(value.bytes));
}
/** Добавляет optional-поле только при явно заданном намерении, включая false и пустую строку. */
template<class T> void optional(JsonValue::Object &object, const char *key, const std::optional<T> &value)
{
	if (value)
		object.insert(QLatin1String(key), JsonValue(*value));
}
/** Создаёт оболочку PATCH с точной числовой ревизией; положительность проверяет wrapper. */
JsonValue::Object patch(quint64 revision)
{
	return {{QStringLiteral("expected_revision"), JsonValue(revision)}};
}
/** Кодирует полный явный encoding после единой предметной валидации профиля. */
JsonValue encoding(const Encoding &value)
{
	// object содержит общие настройки; необязательное качество не подменяется нулём.
	JsonValue::Object object{{QStringLiteral("video_encoder"), value.videoEncoder},
				 {QStringLiteral("audio_encoder"), value.audioEncoder},
				 {QStringLiteral("rate_control"), value.rateControl},
				 {QStringLiteral("rescale_output"), value.rescaleOutput},
				 {QStringLiteral("video_custom"), value.videoCustom},
				 {QStringLiteral("audio_custom"), value.audioCustom}};
	if (value.qp.hasValue())
		object.insert(QStringLiteral("qp"), JsonValue(value.qp.value));
	if (value.crfHundredths.hasValue()) {
		// hundredths является неотрицательным качеством, уже проверенным validateProfile.
		const auto hundredths = value.crfHundredths.value;
		object.insert(QStringLiteral("crf"),
			      JsonValue::number(QByteArray::number(hundredths / 100) + '.' +
						QByteArray::number(hundredths % 100).rightJustified(2, '0')));
	}
	if (value.nvenc.hasValue()) {
		// n заимствует проверенный полный блок параметров NVENC.
		const auto &n = value.nvenc.value;
		object.insert(QStringLiteral("nvenc"),
			      JsonValue::Object{{QStringLiteral("preset"), n.preset},
						{QStringLiteral("tuning"), n.tuning},
						{QStringLiteral("multipass"), n.multipass},
						{QStringLiteral("lookahead_frames"), n.lookaheadFrames},
						{QStringLiteral("enable_spatial_aq"), n.enableSpatialAq},
						{QStringLiteral("enable_temporal_aq"), n.enableTemporalAq},
						{QStringLiteral("aq_strength"), n.aqStrength}});
	}
	if (value.svtAv1.hasValue()) {
		// s заимствует проверенный полный блок SVT-AV1.
		const auto &s = value.svtAv1.value;
		object.insert(QStringLiteral("svt_av1"),
			      JsonValue::Object{{QStringLiteral("preset"), s.preset},
						{QStringLiteral("tuning"), s.tuning},
						{QStringLiteral("aq_mode"), s.aqMode},
						{QStringLiteral("lookahead_frames"), s.lookaheadFrames}});
	}
	return JsonValue(std::move(object));
}
} // namespace

/** Проверяет полный desired-профиль общими правилами и кодирует только его редактируемую проекцию. */
Result<JsonValue> encodeProfile(const GroupProfile &profile)
{
	// error передаёт точный путь предметного нарушения из единого модуля правил.
	Error error;
	if (!validateProfile(profile, error))
		return Result<JsonValue>::failure(std::move(error));
	// video/audio задают формат; ve содержит битрейты и структуру GOP без double.
	const auto &video = profile.format.video;
	const auto &audio = profile.format.audio;
	const auto &ve = profile.videoEncoding;
	// object не использует originalDocument: полная замена проходит действующую валидацию.
	JsonValue::Object object{
		{QStringLiteral("mode"), profile.mode},
		{QStringLiteral("live_audio_mode"), profile.liveAudioMode.value},
		{QStringLiteral("format"),
		 JsonValue::Object{
			 {QStringLiteral("video"),
			  JsonValue::Object{
				  {QStringLiteral("codec"), video.codec},
				  {QStringLiteral("width"), video.width},
				  {QStringLiteral("height"), video.height},
				  {QStringLiteral("frame_rate"),
				   JsonValue::Object{{QStringLiteral("numerator"), video.frameRate.numerator},
						     {QStringLiteral("denominator"), video.frameRate.denominator}}},
				  {QStringLiteral("codec_profile"), text(video.codecProfile)},
				  {QStringLiteral("level"), text(video.level)},
				  {QStringLiteral("tier"), text(video.tier)},
				  {QStringLiteral("pixel_format"), text(video.pixelFormat)},
				  {QStringLiteral("bit_depth"), video.bitDepth},
				  {QStringLiteral("color"),
				   JsonValue::Object{{QStringLiteral("range"), text(video.color.range)},
						     {QStringLiteral("primaries"), text(video.color.primaries)},
						     {QStringLiteral("transfer"), text(video.color.transfer)},
						     {QStringLiteral("matrix"), text(video.color.matrix)}}}}},
			 {QStringLiteral("audio"),
			  JsonValue::Object{{QStringLiteral("codec"), audio.codec},
					    {QStringLiteral("codec_profile"), text(audio.codecProfile)},
					    {QStringLiteral("sample_rate_hz"), audio.sampleRateHz},
					    {QStringLiteral("channels"), audio.channels},
					    {QStringLiteral("channel_layout"), text(audio.channelLayout)}}}}},
		{QStringLiteral("video_encoding"),
		 JsonValue::Object{{QStringLiteral("target_bitrate_bps"), ve.targetBitrateBps},
				   {QStringLiteral("max_bitrate_bps"), ve.maxBitrateBps},
				   {QStringLiteral("buffer_bits"), ve.bufferBits},
				   {QStringLiteral("gop_frames"), ve.gopFrames},
				   {QStringLiteral("max_b_frames"), ve.maxBFrames},
				   {QStringLiteral("closed_gop"), ve.closedGop}}},
		{QStringLiteral("audio_encoding"),
		 JsonValue::Object{{QStringLiteral("bitrate_bps"), profile.audioEncoding.bitrateBps}}},
		{QStringLiteral("encoding"), encoding(profile.encoding.value)}};
	return Result<JsonValue>::success(JsonValue(std::move(object)));
}
/** Кодирует отображаемое имя нового Asset; пустая строка не отправляется. */
Result<JsonValue> encodeRequest(const AssetCreate &request)
{
	if (request.name.isEmpty())
		return invalid(QStringLiteral("name"));
	return Result<JsonValue>::success(JsonValue::Object{{QStringLiteral("name"), request.name}});
}
/** Кодирует полную политику либо явную очистку с точной положительной ревизией Stream.
 * Все интервалы сохраняются без double в положительном диапазоне серверного time.Duration.
 */
Result<JsonValue> encodeRequest(const PolicyPut &request)
{
	if (!request.expectedRevision)
		return invalid(QStringLiteral("expected_revision"));
	// object всегда содержит fallback, даже когда назначение намеренно очищается.
	auto object = patch(request.expectedRevision);
	object.insert(QStringLiteral("fallback"), JsonValue(nullptr));
	if (request.fallback) {
		if (request.fallback->assetId.isEmpty())
			return invalid(QStringLiteral("fallback.asset_id"));
		// watchdog сохраняет исходные четыре интервала, а duration связывает каждое значение с публичным полем.
		const auto &watchdog = request.fallback->watchdog;
		for (const auto &duration : {std::pair{"video_timeout_ns", watchdog.videoTimeoutNs},
					     std::pair{"audio_timeout_ns", watchdog.audioTimeoutNs},
					     std::pair{"recovery_window_ns", watchdog.recoveryWindowNs},
					     std::pair{"keyframe_timeout_ns", watchdog.keyframeTimeoutNs}})
			if (!duration.second || duration.second > quint64(std::numeric_limits<qint64>::max()))
				return invalid(QStringLiteral("fallback.watchdog.") + QLatin1String(duration.first));
		object.insert(QStringLiteral("fallback"),
			      JsonValue::Object{
				      {QStringLiteral("asset_id"), request.fallback->assetId},
				      {QStringLiteral("watchdog"),
				       JsonValue::Object{
					       {QStringLiteral("video_timeout_ns"), watchdog.videoTimeoutNs},
					       {QStringLiteral("audio_timeout_ns"), watchdog.audioTimeoutNs},
					       {QStringLiteral("recovery_window_ns"), watchdog.recoveryWindowNs},
					       {QStringLiteral("keyframe_timeout_ns"), watchdog.keyframeTimeoutNs}}}});
	}
	return Result<JsonValue>::success(JsonValue(std::move(object)));
}
/** Кодирует обязательные поля нового Stream без изменения ключа публикации. */
Result<JsonValue> encodeRequest(const StreamCreate &request)
{
	if (request.name.isEmpty() || request.publicationKey.isEmpty())
		return invalid(request.name.isEmpty() ? QStringLiteral("name") : QStringLiteral("publication_key"));
	return Result<JsonValue>::success(
		JsonValue::Object{{QStringLiteral("name"), request.name},
				  {QStringLiteral("publication_key"), request.publicationKey}});
}
/** Кодирует только явно изменённое имя Stream и положительную ревизию. */
Result<JsonValue> encodeRequest(const StreamPatch &request)
{
	if (!request.expectedRevision)
		return invalid(QStringLiteral("expected_revision"));
	// object сохраняет отсутствие необязательного имени.
	auto object = patch(request.expectedRevision);
	optional(object, "name", request.name);
	return Result<JsonValue>::success(JsonValue(std::move(object)));
}
/** Передаёт точные строки и идентичности нового назначения. */
Result<JsonValue> encodeRequest(const DestinationCreate &request)
{
	// pair связывает обязательное имя поля с его значением без включения значения в ошибку.
	for (const auto &pair : {std::pair{"stream_id", &request.streamId}, std::pair{"group_id", &request.groupId},
				 std::pair{"name", &request.name}, std::pair{"platform", &request.platform},
				 std::pair{"endpoint", &request.endpoint}})
		if (pair.second->isEmpty())
			return invalid(QLatin1String(pair.first));
	return Result<JsonValue>::success(JsonValue::Object{{QStringLiteral("stream_id"), request.streamId},
							    {QStringLiteral("group_id"), request.groupId},
							    {QStringLiteral("name"), request.name},
							    {QStringLiteral("platform"), request.platform},
							    {QStringLiteral("endpoint"), request.endpoint},
							    {QStringLiteral("stream_key"), request.streamKey},
							    {QStringLiteral("enabled"), request.enabled}});
}
/** Передаёт только выбранные изменения назначения, включая false и очищенный ключ. */
Result<JsonValue> encodeRequest(const DestinationPatch &request)
{
	if (!request.expectedRevision)
		return invalid(QStringLiteral("expected_revision"));
	// object содержит числовую ревизию и только явно заданные поля.
	auto object = patch(request.expectedRevision);
	optional(object, "stream_id", request.streamId);
	optional(object, "group_id", request.groupId);
	optional(object, "name", request.name);
	optional(object, "platform", request.platform);
	optional(object, "endpoint", request.endpoint);
	optional(object, "stream_key", request.streamKey);
	optional(object, "enabled", request.enabled);
	return Result<JsonValue>::success(JsonValue(std::move(object)));
}
/** Создаёт связь Group с существующими Stream и Profile. */
Result<JsonValue> encodeRequest(const GroupCreate &request)
{
	if (request.streamId.isEmpty())
		return invalid(QStringLiteral("stream_id"));
	if (request.profileId.isEmpty())
		return invalid(QStringLiteral("profile_id"));
	if (request.name.isEmpty())
		return invalid(QStringLiteral("name"));
	return Result<JsonValue>::success(JsonValue::Object{{QStringLiteral("stream_id"), request.streamId},
							    {QStringLiteral("profile_id"), request.profileId},
							    {QStringLiteral("name"), request.name}});
}
/** Кодирует только выбранные связи и имя Group с положительной ревизией. */
Result<JsonValue> encodeRequest(const GroupPatch &request)
{
	if (!request.expectedRevision)
		return invalid(QStringLiteral("expected_revision"));
	// object содержит ревизию и выбранные изменения без копирования прочитанной сущности.
	auto object = patch(request.expectedRevision);
	optional(object, "stream_id", request.streamId);
	optional(object, "profile_id", request.profileId);
	optional(object, "name", request.name);
	return Result<JsonValue>::success(JsonValue(std::move(object)));
}
/** Кодирует именованный профиль только после полной предметной проверки. */
Result<JsonValue> encodeRequest(const ProfileCreate &request)
{
	if (request.name.isEmpty())
		return invalid(QStringLiteral("name"));
	// profile содержит полный проверенный рецепт либо локальный отказ до передачи в сеть.
	auto profile = encodeProfile(request.profile);
	if (!profile)
		return profile;
	return Result<JsonValue>::success(JsonValue::Object{{QStringLiteral("name"), request.name},
							    {QStringLiteral("profile"), std::move(*profile.value)}});
}
/** Переименование не включает profile; полная замена проходит ту же проверку, что создание. */
Result<JsonValue> encodeRequest(const ProfilePatch &request)
{
	if (!request.expectedRevision)
		return invalid(QStringLiteral("expected_revision"));
	// object остаётся минимальным PATCH, если требуется только имя.
	auto object = patch(request.expectedRevision);
	optional(object, "name", request.name);
	if (request.profile) {
		// profile не заменяется исходным документом с неизвестными настройками.
		auto profile = encodeProfile(*request.profile);
		if (!profile)
			return profile;
		object.insert(QStringLiteral("profile"), std::move(*profile.value));
	}
	return Result<JsonValue>::success(JsonValue(std::move(object)));
}
} // namespace gost::api
