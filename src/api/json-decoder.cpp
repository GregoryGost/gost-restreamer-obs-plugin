#include "api/json-decoder.hpp"
#include <QDate>
#include <QRegularExpression>
#include <QTime>
namespace gost::api {
/** Создаёт классифицированную ошибку без необработанных серверных строк. */
Error makeError(ErrorKind kind, int status)
{
	// message содержит локальное общее пояснение без содержимого ответа.
	QString message;
	switch (kind) {
	case ErrorKind::Transport:
		message = QStringLiteral("Соединение с сервером прервано.");
		break;
	case ErrorKind::Tls:
		message = QStringLiteral("Не удалось установить защищённое соединение с сервером.");
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
/** Разбирает локальный канонический десятичный ввод с полной проверкой U64. */
bool parseDecimal(const QString &text, quint64 &value)
{
	// Локальный десятичный ввод разбирается без double, чтобы сохранить старшие разряды.
	if (text.isEmpty() || (text.size() > 1 && text.front() == QLatin1Char('0')))
		return false;
	// result накапливает точное целое до публикации; c и digit обозначают текущую цифру.
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
/** Проверяет календарное UTC-время с сохранением исходной точности строки. */
bool validTimestamp(const QString &text)
{
	// Проверяем UTC-формат со знаком Z, сохраняя исходную строку и до девяти знаков долей секунды без округления.
	// pattern задаёт UTC с наносекундной дробью; match хранит календарные компоненты строки.
	static const QRegularExpression pattern(
		QStringLiteral("^(\\d{4})-(\\d{2})-(\\d{2})T(\\d{2}):(\\d{2}):(\\d{2})(?:\\.\\d{1,9})?Z$"));
	const auto match = pattern.match(text);
	return match.hasMatch() &&
	       QDate(match.captured(1).toInt(), match.captured(2).toInt(), match.captured(3).toInt()).isValid() &&
	       QTime(match.captured(4).toInt(), match.captured(5).toInt(), match.captured(6).toInt()).isValid();
}
/** Принимает строку Unicode без изолированных суррогатов; другой тип отвергается. */
bool fromJson(const JsonValue &json, QString &value, Error &error)
{
	// Проверка UTF-8 тела не исключает изолированных суррогатов внутри экранированных последовательностей JSON.
	// QString должен содержать целые пары UTF-16, иначе последующее toUtf8 потеряло бы исходные данные.
	if (!json.isString()) {
		error = makeError(ErrorKind::Decode);
		return false;
	}
	value = json.toString();
	// i проходит кодовые единицы UTF-16, проверяя каждую пару суррогатов.
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
/** Принимает только логическое JSON-значение без числовых или строковых подстановок. */
bool fromJson(const JsonValue &json, bool &value, Error &error)
{
	if (!json.isBool()) {
		error = makeError(ErrorKind::Decode);
		return false;
	}
	value = json.toBool();
	return true;
}
/** Читает точное U64 из JSON-числа, отвергая дробь, переполнение и любой знак минус. */
bool fromJson(const JsonValue &json, quint64 &value, Error &error)
{
	if (!json.toUnsigned(value)) {
		error = makeError(ErrorKind::Decode);
		return false;
	}
	return true;
}
/** Читает конечное неотрицательное измерение float64; целые DTO используют отдельный точный разбор. */
bool fromJson(const JsonValue &json, double &value, Error &error)
{
	// ok отличает корректный конечный float64 от переполнения или неверного JSON-типа.
	bool ok = false;
	// parsed применяется только для настоящих дробных измерений, не для целых или CRF.
	const double parsed = json.isNumber() ? json.numberLexeme().toDouble(&ok) : 0;
	if (!ok || !std::isfinite(parsed) || parsed < 0) {
		error = makeError(ErrorKind::Decode);
		return false;
	}
	value = parsed;
	return true;
}
/** Читает строковый параметр профиля и сохраняет его корректные UTF-8 байты. */
bool fromJson(const JsonValue &json, ProfileText &value, Error &error)
{
	// text содержит проверенную строку Unicode; объект Base64 не относится к этому контракту.
	QString text;
	if (!fromJson(json, text, error))
		return false;
	value.bytes = text.toUtf8();
	return true;
}
// Оставляет только опубликованные виды ID; неизвестные ключи не становятся доверенным контекстом.
/** Читает поля QMap<QString, QString> из типизированного JSON; нарушение формы прекращает разбор с Error. */
bool fromJson(const JsonValue &json, QMap<QString, QString> &value, Error &error)
{
	value.clear();
	if (json.isUndefined())
		return true;
	if (!json.isObject()) {
		error = makeError(ErrorKind::Decode);
		return false;
	}
	// key перечисляет фиксированные имена, text принимает только строковое значение известного ID.
	for (const auto *key : {"stream_id", "session_id", "output_id", "group_id", "profile_id", "asset_id",
				"generation_id", "variant_id", "destination_id", "command_id"}) {
		if (!json.toObject().contains(QLatin1String(key)))
			continue;
		// text получает строковое значение одного разрешённого контекстного ID.
		QString text;
		if (!fromJson(json.toObject().value(QLatin1String(key)), text, error))
			return false;
		if (!text.isEmpty())
			value.insert(QLatin1String(key), std::move(text));
	}
	return true;
}
/** Читает поля QMap<QString, quint64> из типизированного JSON; нарушение формы прекращает разбор с Error. */
bool fromJson(const JsonValue &json, QMap<QString, quint64> &value, Error &error)
{
	if (!json.isObject()) {
		error = makeError(ErrorKind::Decode);
		return false;
	}
	value.clear();
	// object содержит словарь счётчиков; it указывает текущую пару, entry получает точное U64.
	const auto &object = json.toObject();
	for (auto it = object.begin(); it != object.end(); ++it) {
		quint64 entry;
		if (!fromJson(it.value(), entry, error))
			return false;
		value.insert(it.key(), entry);
	}
	return true;
}
namespace {
/** Пропускает дополнительную проверку времени для значения иного типа. */
template<class T> bool timestampField(const T &)
{
	return true;
}
/** Проверяет календарное UTC-время обязательной строки. */
bool timestampField(const QString &v)
{
	return validTimestamp(v);
}
/** Проверяет время только при наличии значения; Missing/null не становятся датой. */
bool timestampField(const JsonField<QString> &v)
{
	return !v.hasValue() || validTimestamp(v.value);
}
// Общий последовательный читатель объекта: field требует тип конкретной перегрузки, time дополнительно
// проверяет календарное UTC-время. JsonField допускает Undefined/Null, обычное поле требует значение.
// После первого отказа valid_ запрещает дальнейшее заполнение и сохраняет причину ошибки чтения.
// Неизвестные ключи не перечисляются и допускают добавление сервером совместимых полей.
class Reader {
public:
	/** Начинает чтение объекта и фиксирует ошибку, если передан другой JSON-тип. */
	Reader(const JsonValue &json, Error &error) : error_(error)
	{
		valid_ = json.isObject();
		if (valid_)
			object_ = json.toObject();
		else
			error_ = makeError(ErrorKind::Decode);
	}
	/** Читает обязательное поле выбранной перегрузкой; после первого отказа больше не изменяет DTO. */
	template<class T> bool field(const char *key, T &value)
	{
		if (!valid_ || !fromJson(object_.value(QLatin1String(key)), value, error_)) {
			valid_ = false;
			return false;
		}
		return true;
	}
	/** Читает необязательное строковое поле без подстановки отсутствующих данных. */
	bool optional(const char *key, QString &value)
	{
		value.clear();
		return !object_.contains(QLatin1String(key)) || field(key, value);
	}
	/** Читает time.Duration с физической границей знакового I64 и предметной нижней границей. */
	bool duration(const char *key, quint64 &value, bool positive = false)
	{
		if (!field(key, value))
			return false;
		if (value > quint64(std::numeric_limits<qint64>::max()) || (positive && !value)) {
			valid_ = false;
			error_ = makeError(ErrorKind::Decode);
		}
		return valid_;
	}
	/** Читает поле и проверяет календарную форму UTC без преобразования его строки. */
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
	/** Возвращает итог всех уже выполненных проверок полей. */
	bool good() const { return valid_; }

private:
	// object_ содержит именованные значения читаемого объекта.
	JsonValue::Object object_;
	// error_ заимствует место первой ошибки вызывающей стороны.
	Error &error_;
	// valid_ запрещает дальнейшую запись после первого несовместимого поля.
	bool valid_ = false;
};
} // namespace
/** Читает идентичность, положительную ревизию и исходные временные метки записи. */
bool fromJson(const JsonValue &json, Record &value, Error &error)
{
	// Последующие перегрузки читают поля DTO в порядке объявления. Record/Observation разбираются
	// первыми для производных типов; их отказ прекращает чтение оставшейся части объекта.
	// r последовательно читает поля и сохраняет первую ошибку формы документа.
	Reader r(json, error);
	r.field("id", value.id);
	r.field("revision", value.revision);
	r.time("created_at", value.createdAt);
	r.time("updated_at", value.updatedAt);
	if (r.good() && value.revision == 0) {
		error = makeError(ErrorKind::Decode);
		return false;
	}
	return r.good();
}
/** Читает структурированную причину и известные ID; диагностическое тело сюда не копируется. */
bool fromJson(const JsonValue &json, Failure &value, Error &error)
{
	// r последовательно читает поля и сохраняет первую ошибку формы документа.
	Reader r(json, error);
	r.field("code", value.code);
	r.field("message", value.message);
	r.optional("action", value.action);
	r.optional("field", value.field);
	r.field("context", value.context);
	value.diagnosticBody.clear();
	return r.good();
}
/** Читает положительные интервалы watchdog в пределах time.Duration. */
bool fromJson(const JsonValue &json, Watchdog &value, Error &error)
{
	// r последовательно читает поля и сохраняет первую ошибку формы документа.
	Reader r(json, error);
	r.duration("video_timeout_ns", value.videoTimeoutNs, true);
	r.duration("audio_timeout_ns", value.audioTimeoutNs, true);
	r.duration("recovery_window_ns", value.recoveryWindowNs, true);
	r.duration("keyframe_timeout_ns", value.keyframeTimeoutNs, true);
	return r.good();
}
/** Читает поля Policy из типизированного JSON; нарушение формы прекращает разбор с Error. */
bool fromJson(const JsonValue &json, Policy &value, Error &error)
{
	// r последовательно читает поля и сохраняет первую ошибку формы документа.
	Reader r(json, error);
	r.field("asset_id", value.assetId);
	r.field("watchdog", value.watchdog);
	return r.good();
}
/** Читает поля Stream из типизированного JSON; нарушение формы прекращает разбор с Error. */
bool fromJson(const JsonValue &json, Stream &value, Error &error)
{
	if (!fromJson(json, static_cast<Record &>(value), error))
		return false;
	// r последовательно читает поля и сохраняет первую ошибку формы документа.
	Reader r(json, error);
	r.field("name", value.name);
	r.field("publication_key", value.publicationKey);
	r.field("fallback", value.fallback);
	// Отсутствующий ключ старого ответа остаётся явно недоступным; null нарушает контракт строкового значения.
	if (value.publicationKey.isNull()) {
		error = makeError(ErrorKind::Contract);
		return false;
	}
	return r.good();
}
/** Читает поля Destination из типизированного JSON; нарушение формы прекращает разбор с Error. */
bool fromJson(const JsonValue &json, Destination &value, Error &error)
{
	if (!fromJson(json, static_cast<Record &>(value), error))
		return false;
	// r последовательно читает поля и сохраняет первую ошибку формы документа.
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
/** Читает поля FrameRate из типизированного JSON; нарушение формы прекращает разбор с Error. */
bool fromJson(const JsonValue &json, FrameRate &value, Error &error)
{
	// r последовательно читает поля и сохраняет первую ошибку формы документа.
	Reader r(json, error);
	r.field("numerator", value.numerator);
	r.field("denominator", value.denominator);
	if (value.numerator && !value.denominator) {
		error = makeError(ErrorKind::Contract);
		return false;
	}
	return r.good();
}
/** Читает поля Color из типизированного JSON; нарушение формы прекращает разбор с Error. */
bool fromJson(const JsonValue &json, Color &value, Error &error)
{
	// r последовательно читает поля и сохраняет первую ошибку формы документа.
	Reader r(json, error);
	r.field("range", value.range);
	r.field("primaries", value.primaries);
	r.field("transfer", value.transfer);
	r.field("matrix", value.matrix);
	return r.good();
}
/** Читает поля VideoFormat из типизированного JSON; нарушение формы прекращает разбор с Error. */
bool fromJson(const JsonValue &json, VideoFormat &value, Error &error)
{
	// r последовательно читает поля и сохраняет первую ошибку формы документа.
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
/** Читает поля AudioFormat из типизированного JSON; нарушение формы прекращает разбор с Error. */
bool fromJson(const JsonValue &json, AudioFormat &value, Error &error)
{
	// r последовательно читает поля и сохраняет первую ошибку формы документа.
	Reader r(json, error);
	r.field("codec", value.codec);
	r.field("codec_profile", value.codecProfile);
	r.field("sample_rate_hz", value.sampleRateHz);
	r.field("channels", value.channels);
	r.field("channel_layout", value.channelLayout);
	return r.good();
}
/** Читает поля Format из типизированного JSON; нарушение формы прекращает разбор с Error. */
bool fromJson(const JsonValue &json, Format &value, Error &error)
{
	// r последовательно читает поля и сохраняет первую ошибку формы документа.
	Reader r(json, error);
	r.field("video", value.video);
	r.field("audio", value.audio);
	return r.good();
}
/** Читает поля VideoEncoding из типизированного JSON; нарушение формы прекращает разбор с Error. */
bool fromJson(const JsonValue &json, VideoEncoding &value, Error &error)
{
	// r последовательно читает поля и сохраняет первую ошибку формы документа.
	Reader r(json, error);
	r.field("target_bitrate_bps", value.targetBitrateBps);
	r.field("max_bitrate_bps", value.maxBitrateBps);
	r.field("buffer_bits", value.bufferBits);
	r.field("gop_frames", value.gopFrames);
	r.field("max_b_frames", value.maxBFrames);
	r.field("closed_gop", value.closedGop);
	return r.good();
}
/** Читает поля AudioEncoding из типизированного JSON; нарушение формы прекращает разбор с Error. */
bool fromJson(const JsonValue &json, AudioEncoding &value, Error &error)
{
	// r последовательно читает поля и сохраняет первую ошибку формы документа.
	Reader r(json, error);
	r.field("bitrate_bps", value.bitrateBps);
	return r.good();
}
/** Читает поля Group из типизированного JSON; нарушение формы прекращает разбор с Error. */
bool fromJson(const JsonValue &json, Group &value, Error &error)
{
	if (!fromJson(json, static_cast<Record &>(value), error))
		return false;
	// r последовательно читает поля и сохраняет первую ошибку формы документа.
	Reader r(json, error);
	r.field("stream_id", value.streamId);
	r.field("profile_id", value.profileId);
	r.field("name", value.name);
	return r.good();
}
/** Читает квитанцию команды и временно прикладывает поддерево failure для однократного журнала. */
bool fromJson(const JsonValue &json, Receipt &value, Error &error)
{
	if (!fromJson(json, static_cast<Record &>(value), error))
		return false;
	// r последовательно читает поля и сохраняет первую ошибку формы документа.
	Reader r(json, error);
	r.field("request_id", value.requestId);
	r.field("kind", value.kind);
	r.field("target_id", value.targetId);
	r.field("session_id", value.sessionId);
	r.field("output_id", value.outputId);
	r.field("status", value.status);
	r.field("failure", value.failure);
	if (r.good() && value.failure.hasValue())
		value.failure.value.diagnosticBody = serializeJson(json.toObject().value(QStringLiteral("failure")));
	return r.good();
}
/** Читает поля Asset из типизированного JSON; нарушение формы прекращает разбор с Error. */
bool fromJson(const JsonValue &json, Asset &value, Error &error)
{
	if (!fromJson(json, static_cast<Record &>(value), error))
		return false;
	// r последовательно читает поля и сохраняет первую ошибку формы документа.
	Reader r(json, error);
	r.field("name", value.name);
	r.field("kind", value.kind);
	r.field("state", value.state);
	r.field("current_generation_id", value.currentGenerationId);
	r.field("pending_generation_id", value.pendingGenerationId);
	r.field("size_bytes", value.sizeBytes);
	return r.good();
}
/** Читает точное поколение и временно прикладывает исходное failure для однократного уведомления. */
bool fromJson(const JsonValue &json, Generation &value, Error &error)
{
	if (!fromJson(json, static_cast<Record &>(value), error))
		return false;
	// r последовательно читает поля и сохраняет первую ошибку формы документа.
	Reader r(json, error);
	r.field("asset_id", value.assetId);
	r.field("kind", value.kind);
	r.field("status", value.status);
	r.field("size_bytes", value.sizeBytes);
	r.field("failure", value.failure);
	if (r.good() && value.failure.hasValue())
		value.failure.value.diagnosticBody = serializeJson(json.toObject().value(QStringLiteral("failure")));
	return r.good();
}
/** Читает поля Rate из типизированного JSON; нарушение формы прекращает разбор с Error. */
bool fromJson(const JsonValue &json, Rate &value, Error &error)
{
	// r последовательно читает поля и сохраняет первую ошибку формы документа.
	Reader r(json, error);
	r.field("status", value.status);
	r.field("reason", value.reason);
	r.field("value", value.value);
	r.time("window_start", value.windowStart);
	r.time("window_end", value.windowEnd);
	r.duration("duration_ns", value.durationNs);
	return r.good();
}
/** Читает поля Rates из типизированного JSON; нарушение формы прекращает разбор с Error. */
bool fromJson(const JsonValue &json, Rates &value, Error &error)
{
	// r последовательно читает поля и сохраняет первую ошибку формы документа.
	Reader r(json, error);
	r.field("fps", value.fps);
	r.field("payload_bitrate", value.payloadBitrate);
	r.field("written_bitrate", value.writtenBitrate);
	return r.good();
}
/** Читает поля ObservedFormat из типизированного JSON; нарушение формы прекращает разбор с Error. */
bool fromJson(const JsonValue &json, ObservedFormat &value, Error &error)
{
	// r последовательно читает поля и сохраняет первую ошибку формы документа.
	Reader r(json, error);
	r.field("video_codec", value.videoCodec);
	r.field("audio_codec", value.audioCodec);
	r.field("width", value.width);
	r.field("height", value.height);
	r.field("sample_rate_hz", value.sampleRateHz);
	r.field("channels", value.channels);
	return r.good();
}
/** Читает поля MediaMetrics из типизированного JSON; нарушение формы прекращает разбор с Error. */
bool fromJson(const JsonValue &json, MediaMetrics &value, Error &error)
{
	// r последовательно читает поля и сохраняет первую ошибку формы документа.
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
/** Читает поля InputMetrics из типизированного JSON; нарушение формы прекращает разбор с Error. */
bool fromJson(const JsonValue &json, InputMetrics &value, Error &error)
{
	// r последовательно читает поля и сохраняет первую ошибку формы документа.
	Reader r(json, error);
	r.field("media", value.media);
	r.field("source_epoch", value.sourceEpoch);
	return r.good();
}
/** Читает поля Input из типизированного JSON; нарушение формы прекращает разбор с Error. */
bool fromJson(const JsonValue &json, Input &value, Error &error)
{
	// r последовательно читает поля и сохраняет первую ошибку формы документа.
	Reader r(json, error);
	r.field("connected", value.connected);
	r.time("changed_at", value.changedAt);
	r.field("metrics", value.metrics);
	return r.good();
}
/** Читает поля GroupMetrics из типизированного JSON; нарушение формы прекращает разбор с Error. */
bool fromJson(const JsonValue &json, GroupMetrics &value, Error &error)
{
	// r последовательно читает поля и сохраняет первую ошибку формы документа.
	Reader r(json, error);
	r.field("live_input", value.liveInput);
	r.field("fallback_input", value.fallbackInput);
	r.field("output", value.output);
	return r.good();
}
/** Читает поля OutputMetrics из типизированного JSON; нарушение формы прекращает разбор с Error. */
bool fromJson(const JsonValue &json, OutputMetrics &value, Error &error)
{
	// r последовательно читает поля и сохраняет первую ошибку формы документа.
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
/** Читает поля FFmpegProgress из типизированного JSON; нарушение формы прекращает разбор с Error. */
bool fromJson(const JsonValue &json, FFmpegProgress &value, Error &error)
{
	// r последовательно читает поля и сохраняет первую ошибку формы документа.
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
/** Читает поля FFmpegMetrics из типизированного JSON; нарушение формы прекращает разбор с Error. */
bool fromJson(const JsonValue &json, FFmpegMetrics &value, Error &error)
{
	// r последовательно читает поля и сохраняет первую ошибку формы документа.
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
/** Читает поля GPUMetric из типизированного JSON; нарушение формы прекращает разбор с Error. */
bool fromJson(const JsonValue &json, GPUMetric &value, Error &error)
{
	// r последовательно читает поля и сохраняет первую ошибку формы документа.
	Reader r(json, error);
	r.field("status", value.status);
	r.field("reason", value.reason);
	r.field("value", value.value);
	r.time("sampled_at", value.sampledAt);
	r.time("last_success_at", value.lastSuccessAt);
	r.field("sampling_period_us", value.samplingPeriodUs);
	return r.good();
}
/** Читает поля GPUDevice из типизированного JSON; нарушение формы прекращает разбор с Error. */
bool fromJson(const JsonValue &json, GPUDevice &value, Error &error)
{
	// r последовательно читает поля и сохраняет первую ошибку формы документа.
	Reader r(json, error);
	r.field("uuid", value.uuid);
	r.field("utilization", value.utilization);
	r.field("memory_utilization", value.memoryUtilization);
	r.field("encoder_utilization", value.encoderUtilization);
	r.field("decoder_utilization", value.decoderUtilization);
	r.field("memory_used_bytes", value.memoryUsedBytes);
	r.field("memory_total_bytes", value.memoryTotalBytes);
	r.field("memory_free_bytes", value.memoryFreeBytes);
	// metric указывает одну из процентных величин; байтовые показатели сохраняют весь диапазон U64.
	for (const auto *metric :
	     {&value.utilization, &value.memoryUtilization, &value.encoderUtilization, &value.decoderUtilization}) {
		if (metric->value.hasValue() && metric->value.value > 100) {
			error = makeError(ErrorKind::Decode);
			return false;
		}
	}
	return r.good();
}
/** Читает поля GPU из типизированного JSON; нарушение формы прекращает разбор с Error. */
bool fromJson(const JsonValue &json, GPU &value, Error &error)
{
	// r последовательно читает поля и сохраняет первую ошибку формы документа.
	Reader r(json, error);
	r.field("status", value.status);
	r.field("reason", value.reason);
	r.time("sampled_at", value.sampledAt);
	r.duration("stale_after_ns", value.staleAfterNs, true);
	r.field("devices", value.devices);
	return r.good();
}
/** Читает поля Observation из типизированного JSON; нарушение формы прекращает разбор с Error. */
bool fromJson(const JsonValue &json, Observation &value, Error &error)
{
	// r последовательно читает поля и сохраняет первую ошибку формы документа.
	Reader r(json, error);
	r.field("instance_id", value.instanceId);
	r.field("revision", value.revision);
	r.time("observed_at", value.observedAt);
	r.field("state", value.state);
	r.field("failure", value.failure);
	return r.good();
}
/** Читает поля RuntimeGroup из типизированного JSON; нарушение формы прекращает разбор с Error. */
bool fromJson(const JsonValue &json, RuntimeGroup &value, Error &error)
{
	if (!fromJson(json, static_cast<Observation &>(value), error))
		return false;
	// r последовательно читает поля и сохраняет первую ошибку формы документа.
	Reader r(json, error);
	r.field("group_id", value.groupId);
	r.field("profile_id", value.profileId);
	r.field("source", value.source);
	r.field("source_revision", value.sourceRevision);
	r.field("metrics", value.metrics);
	return r.good();
}
/** Читает поля RuntimeOutput из типизированного JSON; нарушение формы прекращает разбор с Error. */
bool fromJson(const JsonValue &json, RuntimeOutput &value, Error &error)
{
	if (!fromJson(json, static_cast<Observation &>(value), error))
		return false;
	// r последовательно читает поля и сохраняет первую ошибку формы документа.
	Reader r(json, error);
	r.field("output_id", value.outputId);
	r.field("metrics", value.metrics);
	return r.good();
}
/** Читает поля RuntimeFFmpeg из типизированного JSON; нарушение формы прекращает разбор с Error. */
bool fromJson(const JsonValue &json, RuntimeFFmpeg &value, Error &error)
{
	if (!fromJson(json, static_cast<Observation &>(value), error))
		return false;
	// r последовательно читает поля и сохраняет первую ошибку формы документа.
	Reader r(json, error);
	r.field("group_instance_id", value.groupInstanceId);
	r.field("role", value.role);
	r.field("metrics", value.metrics);
	return r.good();
}
/** Читает поля RuntimeSession из типизированного JSON; нарушение формы прекращает разбор с Error. */
bool fromJson(const JsonValue &json, RuntimeSession &value, Error &error)
{
	// r последовательно читает поля и сохраняет первую ошибку формы документа.
	Reader r(json, error);
	r.field("session_id", value.sessionId);
	r.field("observation", value.observation);
	r.field("input", value.input);
	r.field("groups", value.groups);
	r.field("outputs", value.outputs);
	r.field("ffmpeg", value.ffmpeg);
	return r.good();
}
/** Читает поля Anchor из типизированного JSON; нарушение формы прекращает разбор с Error. */
bool fromJson(const JsonValue &json, Anchor &value, Error &error)
{
	// r последовательно читает поля и сохраняет первую ошибку формы документа.
	Reader r(json, error);
	r.field("cursor", value.cursor);
	r.field("sessions", value.sessions);
	r.field("gpu", value.gpu);
	return r.good();
}
/** Читает поля SessionMetrics из типизированного JSON; нарушение формы прекращает разбор с Error. */
bool fromJson(const JsonValue &json, SessionMetrics &value, Error &error)
{
	// r последовательно читает поля и сохраняет первую ошибку формы документа.
	Reader r(json, error);
	r.field("availability", value.availability);
	r.field("reason", value.reason);
	r.field("snapshot", value.snapshot);
	return r.good();
}
/** Читает поля RuntimeEvent из типизированного JSON; нарушение формы прекращает разбор с Error. */
bool fromJson(const JsonValue &json, RuntimeEvent &value, Error &error)
{
	// r последовательно читает поля и сохраняет первую ошибку формы документа.
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
/** Читает поля NVENC из типизированного JSON; нарушение формы прекращает разбор с Error. */
bool fromJson(const JsonValue &json, NVENC &value, Error &error)
{
	// r последовательно читает поля и сохраняет первую ошибку формы документа.
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
/** Читает поля SVTAV1 из типизированного JSON; нарушение формы прекращает разбор с Error. */
bool fromJson(const JsonValue &json, SVTAV1 &value, Error &error)
{
	// r последовательно читает поля и сохраняет первую ошибку формы документа.
	Reader r(json, error);
	r.field("preset", value.preset);
	r.field("tuning", value.tuning);
	r.field("aq_mode", value.aqMode);
	r.field("lookahead_frames", value.lookaheadFrames);
	return r.good();
}
/** Читает полный рецепт, точное качество и применимые блоки без legacy версии. */
bool fromJson(const JsonValue &json, Encoding &value, Error &error)
{
	// r последовательно читает поля и сохраняет первую ошибку формы документа.
	Reader r(json, error);
	r.field("video_encoder", value.videoEncoder);
	r.field("audio_encoder", value.audioEncoder);
	r.field("rate_control", value.rateControl);
	r.field("qp", value.qp);
	// crf сохраняет Missing; явное значение переводится в сотые без округления.
	const auto crf = json.toObject().value(QStringLiteral("crf"));
	value.crfHundredths = {};
	if (!crf.isUndefined()) {
		// hundredths получает точное масштабированное качество с предметной границей 0..63.
		qint64 hundredths = 0;
		if (!crf.toScaledInteger(2, hundredths) || hundredths < 0 || hundredths > 6300) {
			error = makeError(ErrorKind::Decode);
			return false;
		}
		value.crfHundredths = {FieldState::Value, int(hundredths)};
	}
	r.field("rescale_output", value.rescaleOutput);
	r.field("video_custom", value.videoCustom);
	r.field("audio_custom", value.audioCustom);
	r.field("nvenc", value.nvenc);
	r.field("svt_av1", value.svtAv1);
	if (value.qp.isNull() || (value.qp.hasValue() && (value.qp.value < 0 || value.qp.value > 63)) ||
	    value.nvenc.isNull() || value.svtAv1.isNull()) {
		error = makeError(ErrorKind::Contract);
		return false;
	}
	// knownEncoder ограничивает проверку применимости известным каталогом, сохраняя новые строки сервера.
	const bool knownEncoder = QStringList{QStringLiteral("h264_nvenc"), QStringLiteral("libsvtav1"),
					      QStringLiteral("libx264"),    QStringLiteral("libaom-av1"),
					      QStringLiteral("libx265"),    QStringLiteral("hevc_nvenc"),
					      QStringLiteral("libvpx-vp9")}
					  .contains(value.videoEncoder);
	if (knownEncoder && ((value.videoEncoder == QStringLiteral("h264_nvenc")) != value.nvenc.hasValue() ||
			     (value.videoEncoder == QStringLiteral("libsvtav1")) != value.svtAv1.hasValue())) {
		error = makeError(ErrorKind::Contract);
		return false;
	}
	if ((value.rateControl == QStringLiteral("cqp") && (!value.qp.hasValue() || value.crfHundredths.hasValue())) ||
	    (value.rateControl == QStringLiteral("crf") && (!value.crfHundredths.hasValue() || value.qp.hasValue())) ||
	    ((value.rateControl == QStringLiteral("cbr") || value.rateControl == QStringLiteral("vbr")) &&
	     (value.qp.hasValue() || value.crfHundredths.hasValue()))) {
		error = makeError(ErrorKind::Contract);
		return false;
	}
	return r.good();
}
/** Читает полный ответ профиля и сохраняет исходное дерево вместе с неизвестными настройками. */
bool fromJson(const JsonValue &json, GroupProfile &value, Error &error)
{
	// Ответ требует полной структуры; неизвестные строковые настройки остаются доступными для исправления.
	// r последовательно читает поля и сохраняет первую ошибку формы документа.
	Reader r(json, error);
	r.field("mode", value.mode);
	r.field("live_audio_mode", value.liveAudioMode);
	r.field("format", value.format);
	r.field("video_encoding", value.videoEncoding);
	r.field("audio_encoding", value.audioEncoding);
	r.field("encoding", value.encoding);
	if (!r.good())
		return false;
	if (!value.liveAudioMode.hasValue() || !value.encoding.hasValue()) {
		error = makeError(ErrorKind::Contract);
		return false;
	}
	value.originalDocument = json;
	return r.good();
}
/** Читает поля Profile из типизированного JSON; нарушение формы прекращает разбор с Error. */
bool fromJson(const JsonValue &json, Profile &value, Error &error)
{
	if (!fromJson(json, static_cast<Record &>(value), error))
		return false;
	// r последовательно читает поля и сохраняет первую ошибку формы документа.
	Reader r(json, error);
	r.field("name", value.name);
	r.field("profile", value.profile);
	return r.good();
}
/** Читает поля GroupSnapshot из типизированного JSON; нарушение формы прекращает разбор с Error. */
bool fromJson(const JsonValue &json, GroupSnapshot &value, Error &error)
{
	// r последовательно читает поля и сохраняет первую ошибку формы документа.
	Reader r(json, error);
	r.field("group", value.group);
	r.field("profile", value.profile);
	return r.good();
}
/** Читает поля Session из типизированного JSON; нарушение формы прекращает разбор с Error. */
bool fromJson(const JsonValue &json, Session &value, Error &error)
{
	if (!fromJson(json, static_cast<Record &>(value), error))
		return false;
	// r последовательно читает поля и сохраняет первую ошибку формы документа.
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
/** Читает поля Output из типизированного JSON; нарушение формы прекращает разбор с Error. */
bool fromJson(const JsonValue &json, Output &value, Error &error)
{
	if (!fromJson(json, static_cast<Record &>(value), error))
		return false;
	// r последовательно читает поля и сохраняет первую ошибку формы документа.
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
/** Читает поля Variant из типизированного JSON; нарушение формы прекращает разбор с Error. */
bool fromJson(const JsonValue &json, Variant &value, Error &error)
{
	if (!fromJson(json, static_cast<Record &>(value), error))
		return false;
	// r последовательно читает поля и сохраняет первую ошибку формы документа.
	Reader r(json, error);
	r.field("asset_id", value.assetId);
	r.field("generation_id", value.generationId);
	r.field("state", value.state);
	r.field("profile", value.profile);
	r.field("format", value.format);
	r.field("size_bytes", value.sizeBytes);
	return r.good();
}
/** Читает поля Health из типизированного JSON; нарушение формы прекращает разбор с Error. */
bool fromJson(const JsonValue &json, Health &value, Error &error)
{
	// r последовательно читает поля и сохраняет первую ошибку формы документа.
	Reader r(json, error);
	r.field("status", value.status);
	r.field("reason", value.reason);
	return r.good();
}
/** Читает поля Capability из типизированного JSON; нарушение формы прекращает разбор с Error. */
bool fromJson(const JsonValue &json, Capability &value, Error &error)
{
	// r последовательно читает поля и сохраняет первую ошибку формы документа.
	Reader r(json, error);
	r.field("status", value.status);
	r.field("reason", value.reason);
	r.time("checked_at", value.checkedAt);
	return r.good();
}
/** Читает поля Capabilities из типизированного JSON; нарушение формы прекращает разбор с Error. */
bool fromJson(const JsonValue &json, Capabilities &value, Error &error)
{
	// r последовательно читает поля и сохраняет первую ошибку формы документа.
	Reader r(json, error);
	r.field("ffmpeg", value.ffmpeg);
	r.field("ffprobe", value.ffprobe);
	r.field("helper", value.helper);
	r.field("nvidia", value.nvidia);
	return r.good();
}
/** Читает поля PolicyResult из типизированного JSON; нарушение формы прекращает разбор с Error. */
bool fromJson(const JsonValue &json, PolicyResult &value, Error &error)
{
	// r последовательно читает поля и сохраняет первую ошибку формы документа.
	Reader r(json, error);
	r.field("revision", value.revision);
	r.field("fallback", value.fallback);
	if (r.good() && (!value.revision || value.fallback.isMissing() ||
			 (value.fallback.hasValue() && value.fallback.value.assetId.isEmpty()))) {
		error = makeError(ErrorKind::Decode);
		return false;
	}
	return r.good();
}
/** Читает поля OutputDetail из типизированного JSON; нарушение формы прекращает разбор с Error. */
bool fromJson(const JsonValue &json, OutputDetail &value, Error &error)
{
	if (!fromJson(json, static_cast<Output &>(value), error))
		return false;
	// r последовательно читает поля и сохраняет первую ошибку формы документа.
	Reader r(json, error);
	r.field("runtime", value.runtime);
	r.field("runtime_availability", value.runtimeAvailability);
	r.field("runtime_reason", value.runtimeReason);
	return r.good();
}
// Принимает необязательные location/message; value не копируется в клиентскую модель.
/** Читает поля FieldError из типизированного JSON; нарушение формы прекращает разбор с Error. */
bool fromJson(const JsonValue &json, FieldError &value, Error &error)
{
	if (!json.isObject()) {
		error = makeError(ErrorKind::Decode);
		return false;
	}
	// object содержит одну ошибку поля, а pair связывает допустимое имя с полем DTO.
	const auto object = json.toObject();
	for (auto pair : {std::pair{"location", &value.location}, std::pair{"message", &value.message}})
		if (object.contains(QLatin1String(pair.first)) &&
		    !fromJson(object.value(QLatin1String(pair.first)), *pair.second, error))
			return false;
	return true;
}
// Читает необязательные поля Problem; отсутствие/null списка errors означает отсутствие деталей.
// Статус тела проверяется отдельно в decodeProblem, чтобы несовместимое тело не подменяло HTTP-отказ.
/** Читает необязательные поля Problem Details и разрешённые строковые идентификаторы контекста. */
bool fromJson(const JsonValue &json, Problem &value, Error &error)
{
	if (!json.isObject()) {
		error = makeError(ErrorKind::Decode);
		return false;
	}
	// object сохраняет только проверяемый документ; pair связывает допустимое строковое имя с DTO.
	const auto object = json.toObject();
	for (auto pair :
	     {std::pair{"type", &value.type}, std::pair{"title", &value.title}, std::pair{"detail", &value.detail},
	      std::pair{"instance", &value.instance}, std::pair{"code", &value.code}, std::pair{"field", &value.field},
	      std::pair{"action", &value.action}})
		if (object.contains(QLatin1String(pair.first)) &&
		    !fromJson(object.value(QLatin1String(pair.first)), *pair.second, error))
			return false;
	if (object.contains(QStringLiteral("status")) &&
	    !fromJson(object.value(QStringLiteral("status")), value.status, error))
		return false;
	// errors не содержит value после преобразования каждого элемента в FieldError.
	const auto errors = object.value(QStringLiteral("errors"));
	value.errors.clear();
	if (!errors.isUndefined() && !errors.isNull() && !fromJson(errors, value.errors, error))
		return false;
	return fromJson(object.value(QStringLiteral("context")), value.context, error);
}
/** Читает поля Preparation из типизированного JSON; нарушение формы прекращает разбор с Error. */
bool fromJson(const JsonValue &json, Preparation &value, Error &error)
{
	// r последовательно читает поля и сохраняет первую ошибку формы документа.
	Reader r(json, error);
	r.field("asset_id", value.assetId);
	r.field("generation_id", value.generationId);
	if (r.good() && (value.assetId.isEmpty() || value.generationId.isEmpty())) {
		error = makeError(ErrorKind::Contract);
		return false;
	}
	return r.good();
}
/** Проверяет согласованность оболочки события до его применения к дереву состояния. */
bool validateRuntimeEvent(const RuntimeEvent &e)
{
	// Проверка формы связывает тип события, владельца и единственный допустимый набор данных.
	// Проверка принадлежности исполнения существующему дереву выполняется отдельно в StateStore.
	// kind выбирает вид изменения, owner определяет обязательные идентичности.
	const auto kind = e.kind, owner = e.owner;
	// payloads считает содержательные блоки, чтобы исключить смешение нескольких видов события.
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
/** Сохраняет HTTP-отказ и диагностику; Problem доверяется лишь при совпадении заявленного статуса. */
Error decodeProblem(int status, const QByteArray &body)
{
	// result сохраняет HTTP-классификацию; problem добавляется только после полного корректного разбора.
	auto result = makeError(ErrorKind::Http, status);
	auto problem = decode<Problem>(body);
	if (problem && problem.value->status == status)
		result.problem = std::move(*problem.value);
	result.diagnosticBody = body;
	return result;
}
} // namespace gost::api
