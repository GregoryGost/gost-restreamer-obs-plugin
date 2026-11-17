#include "api/json-decoder.hpp"
#include "api/json-encoder.hpp"
#include "fixtures/api-json.hpp"

#include <QTest>
#include <QCryptographicHash>
#include <QFile>
#include <limits>

using namespace gost::api;

// Контрактные тесты декодера используют независимо составленные фикстуры JSON и закреплённую
// OpenAPI. Они проверяют точность/различимость состояний и владение значениями, но не совместимость
// произвольного работающего backend и не пригодность реального кодировщика/медиатракта.
class DtoTest final : public QObject {
	Q_OBJECT
private slots:
	// Ограниченный parser сохраняет старое значение при синтаксическом/ресурсном отказе.
	void exactParserRejectsMalformedAndDeepDocuments();
	// Задаёт точные U64 границы и эквивалентные целочисленные лексемы.
	void decimalBoundaries_data();
	// Читает U64 без double и сохраняет исходную числовую лексему.
	void decimalBoundaries();
	// Задаёт строки, дроби, отрицательный ноль и переполнения U64.
	void nonCanonicalDecimals_data();
	// Отклоняет недопустимый U64 без изменения прежнего значения.
	void nonCanonicalDecimals();
	// Проверяет физические пределы U8/I64 и signed отрицательный ноль.
	void decimalsRejectJsonNumbersAndNull();
	// Сохраняет Duration выше2^53 и наносекундный timestamp без округления.
	void nanosecondsAndTimestampsStayExact();
	// Сохраняет рациональный FPS и UTF-8 строку с Unicode и NUL.
	void rationalFpsAndUnicodeProfileText();
	// Задаёт неподдержанные типы публичного строкового ProfileText.
	void invalidProfileText_data();
	// Отклоняет числовой/null ProfileText и бинарную оболочку.
	void invalidProfileText();
	// Различает Missing, Null и числовой ноль JsonField.
	void fieldStatesRemainDistinct();
	// Требует полный encoding и явный LIVE audio mode в ответе профиля.
	void responsesRequireCompleteEncodingAndAudio();
	// Задаёт неизвестные режимы и несовместимое video/audio сочетание.
	void invalidAudioModes_data();
	// Сохраняет неизвестную строку ответа, блокируя недопустимую полную замену.
	void invalidAudioModes();
	// Читает точные endpoint/ключи, различая Missing и пустую строку.
	void keysAndEndpointAreExact();
	// Не восстанавливает секрет по configured и отвергает неверный тип поля.
	void incompatibleSecretsAreUnavailableOrRejected();
	// Сохраняет независимые Session/Output снимки после изменения текущей конфигурации.
	void historicalSnapshotsOwnTheirValues();
	// Сохраняет предельные ACK U64, отличая null скорости от нуля.
	void ackMetricsRemainExactAndUnavailableIsNotZero();
	// Отклоняет повреждённый JSON и UTF-8 без публикации частичного DTO.
	void malformedJsonAndUnicodeAreRejected();
	// Проверяет точный снимок схемы и различает источник контракта от неизвестного binary commit сервера.
	void pinnedSchemaAndProvenanceDescribeReadbackContract();
	// Принимает пустую неприменимую role, сохраняя точную worker identity.
	void unnamedFfmpegRoleIsAValidPublicObservation();
	// Проверяет форму системного removed без Session и GPU payload.
	void systemRemovalHasNoSessionOrGpuPayload();
};

// Числовая JSON-лексема читается без double, включая эквивалентные целочисленные формы.
void DtoTest::decimalBoundaries_data()
{
	QTest::addColumn<QString>("text");
	QTest::addColumn<quint64>("expected");
	QTest::newRow("zero") << QStringLiteral("0") << quint64(0);
	QTest::newRow("above-double") << QStringLiteral("9007199254740993") << quint64(9007199254740993ULL);
	QTest::newRow("max") << QStringLiteral("18446744073709551615") << std::numeric_limits<quint64>::max();
	QTest::newRow("decimal-integral") << QStringLiteral("9007199254740993.000") << quint64(9007199254740993ULL);
	QTest::newRow("exponent-integral") << QStringLiteral("900719925474099300e-2") << quint64(9007199254740993ULL);
	QTest::newRow("max-exponent")
		<< QStringLiteral("184467440737095516150e-1") << std::numeric_limits<quint64>::max();
}
// Проверяет числовой тип и ожидаемое значение независимо от сериализатора продукта.
void DtoTest::decimalBoundaries()
{
	// text задаёт исходный токен; expected — точное независимо заданное целое.
	QFETCH(QString, text);
	QFETCH(quint64, expected);
	// json хранит исходную лексему; result и error принимают итог чтения.
	JsonValue json;
	quint64 result = 42;
	Error error;
	QVERIFY(parseJson(text.toUtf8(), json));
	QVERIFY(fromJson(json, result, error));
	QCOMPARE(result, expected);
	QCOMPARE(serializeJson(json), text.toUtf8());
}
// Задаёт неверные типы, дроби и значения за физическим пределом U64.
void DtoTest::nonCanonicalDecimals_data()
{
	QTest::addColumn<QString>("text");
	// token содержит независимый контрпример, в том числе округляемую double дробь.
	for (const auto *token : {"\"1\"", "-0", "-1", "0.0000000000000000001", "9007199254740993.1",
				  "18446744073709551616", "1e1000000000", "1e-1000000000", "true", "null"})
		QTest::newRow(token) << QString::fromLatin1(token);
}
// Синтаксически корректный JSON не делает дробь, строку или отрицательный ноль допустимым U64.
void DtoTest::nonCanonicalDecimals()
{
	// text задаёт контрпример; json хранит его тип; result сохраняет прежнее значение; error принимает отказ.
	QFETCH(QString, text);
	JsonValue json;
	quint64 result = 42;
	Error error;
	QVERIFY(parseJson(text.toUtf8(), json));
	QVERIFY(!fromJson(json, result, error));
	QCOMPARE(result, quint64(42));
}
// Узкие физические границы не округляются и не переполняются.
void DtoTest::decimalsRejectJsonNumbersAndNull()
{
	// error принимает отказ чтения; small проверяет U8; signedValue — весь I64 и signed -0.
	Error error;
	quint8 small = 0;
	qint64 signedValue = 0;
	QVERIFY(fromJson(JsonValue::number("255.0"), small, error));
	QCOMPARE(small, quint8(255));
	QVERIFY(!fromJson(JsonValue::number("256"), small, error));
	QVERIFY(fromJson(JsonValue::number("-9223372036854775808"), signedValue, error));
	QCOMPARE(signedValue, std::numeric_limits<qint64>::min());
	QVERIFY(fromJson(JsonValue::number("9223372036854775807"), signedValue, error));
	QCOMPARE(signedValue, std::numeric_limits<qint64>::max());
	QVERIFY(!fromJson(JsonValue::number("9223372036854775808"), signedValue, error));
	QVERIFY(fromJson(JsonValue::number("-0"), signedValue, error));
	QCOMPARE(signedValue, qint64(0));
}
// Duration ограничен I64.max, timestamp остаётся строкой наносекундной точности.
void DtoTest::nanosecondsAndTimestampsStayExact()
{
	// policy задаёт raw wire без промежуточного дерева QJson.
	const QByteArray policy = R"({"asset_id":"asset","watchdog":{"video_timeout_ns":9007199254740993,
"audio_timeout_ns":1234567890123456789,"recovery_window_ns":1,"keyframe_timeout_ns":9223372036854775807}})";
	// result подтверждает верхнюю допустимую Duration и значения выше 2^53.
	const auto result = decode<Policy>(policy);
	QVERIFY(result);
	QCOMPARE(result.value->watchdog.videoTimeoutNs, quint64(9007199254740993ULL));
	QCOMPARE(result.value->watchdog.audioTimeoutNs, quint64(1234567890123456789ULL));
	QCOMPARE(result.value->watchdog.recoveryWindowNs, quint64(1));
	QCOMPARE(result.value->watchdog.keyframeTimeoutNs, quint64(9223372036854775807ULL));
	QVERIFY(!decode<Policy>(QByteArray(policy).replace("\"recovery_window_ns\":1", "\"recovery_window_ns\":0")));
	QVERIFY(!decode<Policy>(QByteArray(policy).replace("9223372036854775807", "9223372036854775808")));
	// record сохраняет календарную строку без преобразования в миллисекунды.
	const auto record = decode<Record>(fixture::bytes(fixture::record(QStringLiteral("record"))));
	QVERIFY(record);
	QCOMPARE(record.value->createdAt, QStringLiteral("2026-11-10T01:02:03.123456789Z"));
}
// UTF-8 ProfileText и исходная дробь FPS сохраняются независимо.
void DtoTest::rationalFpsAndUnicodeProfileText()
{
	// json и video задают Unicode-строку с NUL, которая допустима как JSON string.
	auto json = fixture::format();
	auto video = json.value(QStringLiteral("video")).toObject();
	video.insert(QStringLiteral("codec_profile"), QString::fromUtf8("профиль 😀") + QChar(0));
	json.insert(QStringLiteral("video"), video);
	// result проверяет структурное чтение, не валидность неизвестного desired профиля.
	const auto result = decode<Format>(fixture::bytes(json));
	QVERIFY(result);
	QCOMPARE(result.value->video.frameRate.numerator, quint32(60000));
	QCOMPARE(result.value->video.frameRate.denominator, quint32(1001));
	QCOMPARE(result.value->video.codecProfile.bytes, QString::fromUtf8("профиль 😀").toUtf8() + '\0');
	// invalid и error получают отказ нулевого знаменателя.
	FrameRate invalid;
	Error error;
	QVERIFY(!fromJson(fixture::wire(fixture::object(R"({"numerator":60,"denominator":0})")), invalid, error));
}
// Публичный ProfileText допускает строку, но не прежнюю бинарную оболочку.
void DtoTest::invalidProfileText_data()
{
	QTest::addColumn<QJsonValue>("input");
	QTest::newRow("null") << QJsonValue(QJsonValue::Null);
	QTest::newRow("number") << QJsonValue(123);
	QTest::newRow("binary-envelope") << QJsonValue(fixture::object(R"({"encoding":"base64","value":"AA=="})"));
}
// Отказ неверного типа не превращает его в пустую допустимую строку.
void DtoTest::invalidProfileText()
{
	// input задаёт неподдержанный тип; value и error принимают результат.
	QFETCH(QJsonValue, input);
	ProfileText value;
	Error error;
	QVERIFY(!fromJson(fixture::wire(input), value, error));
}
// Missing, Null и числовой ноль имеют отдельные состояния.
void DtoTest::fieldStatesRemainDistinct()
{
	// error принимает отказ; value сохраняет присутствие отдельно от физического числа.
	Error error;
	JsonField<quint64> value;
	QVERIFY(fromJson(JsonValue{}, value, error));
	QVERIFY(value.isMissing());
	QVERIFY(fromJson(JsonValue(nullptr), value, error));
	QVERIFY(value.isNull());
	QVERIFY(fromJson(JsonValue(0), value, error));
	QVERIFY(value.hasValue());
	QCOMPARE(value.value, quint64(0));
}
// Ответ полного профиля не получает клиентские legacy defaults.
void DtoTest::responsesRequireCompleteEncodingAndAudio()
{
	// json задаёт полноценный профиль; modern сохраняет его исходный lossless документ.
	auto json = fixture::groupProfile();
	const auto modern = decode<GroupProfile>(fixture::bytes(json));
	QVERIFY(modern);
	QCOMPARE(modern.value->liveAudioMode.value, QStringLiteral("copy"));
	QVERIFY(modern.value->encoding.hasValue());
	QVERIFY(!modern.value->originalDocument.isUndefined());
	QCOMPARE(modern.value->encoding->nvenc->lookaheadFrames, 16);
	json.remove(QStringLiteral("live_audio_mode"));
	QVERIFY(!decode<GroupProfile>(fixture::bytes(json)));
	json = fixture::groupProfile();
	json.remove(QStringLiteral("encoding"));
	QVERIFY(!decode<GroupProfile>(fixture::bytes(json)));
	json.insert(QStringLiteral("encoding"), QJsonValue::Null);
	QVERIFY(!decode<GroupProfile>(fixture::bytes(json)));
}
// Задаёт отсутствие обязательного режима звука и неизвестные либо несовместимые строковые значения.
void DtoTest::invalidAudioModes_data()
{
	QTest::addColumn<QString>("mode");
	QTest::addColumn<QJsonValue>("audio");
	QTest::newRow("null") << QStringLiteral("transcode") << QJsonValue(QJsonValue::Null);
	QTest::newRow("empty") << QStringLiteral("transcode") << QJsonValue(QString{});
	QTest::newRow("unknown") << QStringLiteral("transcode") << QJsonValue(QStringLiteral("automatic"));
	QTest::newRow("copy-video-transcode-audio")
		<< QStringLiteral("copy") << QJsonValue(QStringLiteral("transcode"));
}
// Сохраняет неизвестные строковые значения ответа, но запрещает отправку несовместимого полного рецепта.
void DtoTest::invalidAudioModes()
{
	// mode задаёт обработку видео; audio — исходное значение обязательного режима LIVE audio.
	QFETCH(QString, mode);
	QFETCH(QJsonValue, audio);
	// json содержит полный профиль, в котором изменены только два проверяемых режима.
	auto json = fixture::groupProfile();
	json.insert(QStringLiteral("mode"), mode);
	json.insert(QStringLiteral("live_audio_mode"), audio);
	// decoded сохраняет неизвестную строковую настройку; полная замена требует общей проверки матрицы.
	const auto decoded = decode<GroupProfile>(fixture::bytes(json));
	if (audio.isNull())
		QVERIFY(!decoded);
	else {
		QVERIFY(decoded);
		QVERIFY(!encodeProfile(*decoded.value));
	}
}
void DtoTest::keysAndEndpointAreExact()
{
	// Зарезервированные символы/регистр/процентное кодирование сравниваются побайтово после чтения.
	// Пустой полученный ключ допустим и отличается от недоступного поля, в том числе внутри списка.
	const auto stream = decode<Stream>(fixture::bytes(fixture::stream()));
	QVERIFY(stream);
	QCOMPARE(stream.value->publicationKey.value, QStringLiteral("publish+%2F/ключ"));
	const auto destination = decode<Destination>(fixture::bytes(fixture::destination()));
	QVERIFY(destination);
	QCOMPARE(destination.value->endpoint.value, fixture::endpoint());
	QCOMPARE(destination.value->streamKey.value, QStringLiteral("key/%2F+="));
	auto empty = fixture::destination();
	empty.insert(QStringLiteral("stream_key"), QString{});
	const auto cleared = decode<Destination>(fixture::bytes(empty));
	QVERIFY(cleared);
	QVERIFY(cleared.value->streamKey.hasValue());
	QVERIFY(cleared.value->streamKey.value.isEmpty());
	const auto listed = decode<Page<Destination>>(fixture::page({fixture::destination(), empty}));
	QVERIFY(listed);
	QCOMPARE(listed.value->items[0].endpoint.value, fixture::endpoint());
	QVERIFY(listed.value->items[1].streamKey.hasValue());
}
void DtoTest::incompatibleSecretsAreUnavailableOrRejected()
{
	// Старый *_configured не восстанавливает значение секрета. Missing остаётся недоступным,
	// а присутствующее поле неверного типа/null должно отвергать документ, не превращаться в пустую строку.
	auto json = fixture::stream();
	json.remove(QStringLiteral("publication_key"));
	json.insert(QStringLiteral("publication_key_configured"), true);
	const auto unavailable = decode<Stream>(fixture::bytes(json));
	QVERIFY(unavailable);
	QVERIFY(unavailable.value->publicationKey.isMissing());
	for (const auto &bad : {QJsonValue(QJsonValue::Null), QJsonValue(true), QJsonValue(123)}) {
		json.insert(QStringLiteral("publication_key"), bad);
		QVERIFY(!decode<Stream>(fixture::bytes(json)));
		for (const auto *key : {"stream_key", "endpoint"}) {
			auto destination = fixture::destination();
			destination.insert(QLatin1String(key), bad);
			QVERIFY(!decode<Destination>(fixture::bytes(destination)));
		}
	}
	auto destination = fixture::destination();
	destination.remove(QStringLiteral("endpoint"));
	destination.remove(QStringLiteral("stream_key"));
	const auto old = decode<Destination>(fixture::bytes(destination));
	QVERIFY(old);
	QVERIFY(old.value->endpoint.isMissing());
	QVERIFY(old.value->streamKey.isMissing());
}
void DtoTest::historicalSnapshotsOwnTheirValues()
{
	// Изменение отдельно прочитанной текущей конфигурации не должно менять вложенные значения
	// Session/Output. Проверяется именно независимость снимков, включая ключи и наследуемый аудиорежим.
	const auto history = decode<Session>(fixture::bytes(fixture::session()));
	const auto output = decode<Output>(fixture::bytes(fixture::output()));
	QVERIFY(history);
	QVERIFY(output);
	auto currentStream = decode<Stream>(fixture::bytes(fixture::stream()));
	auto currentDestination = decode<Destination>(fixture::bytes(fixture::destination()));
	currentStream.value->name = QStringLiteral("Изменённый поток");
	currentStream.value->publicationKey.value.clear();
	currentDestination.value->endpoint.value.clear();
	currentDestination.value->streamKey.value.clear();
	QCOMPARE(history.value->stream.name, QStringLiteral("Поток"));
	QCOMPARE(history.value->stream.publicationKey.value, QStringLiteral("publish+%2F/ключ"));
	QCOMPARE(output.value->destination.endpoint.value, fixture::endpoint());
	QCOMPARE(output.value->destination.streamKey.value, QStringLiteral("key/%2F+="));
	QCOMPARE(output.value->profile.profile.liveAudioMode.value, QStringLiteral("copy"));
}
void DtoTest::ackMetricsRemainExactAndUnavailableIsNotZero()
{
	// Большие ACK-значения проверяют точность, а null времени и скорости отделяют отсутствие наблюдения от нуля.
	const auto result = decode<OutputMetrics>(fixture::bytes(fixture::outputMetrics()));
	QVERIFY(result);
	QCOMPARE(result.value->ackAheadEventsTotal, std::numeric_limits<quint64>::max());
	QCOMPARE(result.value->ackAheadBytesLast, quint64(9007199254740993ULL));
	QCOMPARE(result.value->ackAheadBytesMax, quint64(18446744073709551614ULL));
	QVERIFY(result.value->ackLastAnomalyAt.isNull());
	QCOMPARE(result.value->media.rates.fps.status, QStringLiteral("warming_up"));
	QVERIFY(result.value->media.rates.fps.value.isNull());
	QCOMPARE(result.value->media.videoFrames, quint64(0));
}
void DtoTest::malformedJsonAndUnicodeAreRejected()
{
	QVERIFY(!decode<Stream>("{"));
	QVERIFY(!decode<Stream>("[]"));
	QVERIFY(!decode<Stream>(fixture::bytes(fixture::stream()) + "{}"));
	auto invalid = fixture::bytes(fixture::stream());
	invalid.replace(QStringLiteral("Поток").toUtf8(), QByteArray::fromHex("ff"));
	QVERIFY(!decode<Stream>(invalid));
}

void DtoTest::pinnedSchemaAndProvenanceDescribeReadbackContract()
{
	// Проверяется закреплённый снимок со своим происхождением; это не подтверждение сборки работающего сервера.
	// schemaFile читает полученную OpenAPI, provenanceFile — метаданные её происхождения.
	QFile schemaFile(QStringLiteral(GOST_TEST_FIXTURE_DIR) + QStringLiteral("/backend/openapi.json"));
	QFile provenanceFile(QStringLiteral(GOST_TEST_FIXTURE_DIR) + QStringLiteral("/backend/provenance.json"));
	QVERIFY(schemaFile.open(QIODevice::ReadOnly));
	QVERIFY(provenanceFile.open(QIODevice::ReadOnly));
	// bytes сохраняет точное тело схемы для SHA256; provenance содержит независимо закреплённые сведения получения.
	const auto bytes = schemaFile.readAll();
	const auto provenance = fixture::object(provenanceFile.readAll());
	QCOMPARE(QCryptographicHash::hash(bytes, QCryptographicHash::Sha256).toHex().toUpper(),
		 provenance.value(QStringLiteral("schema_sha256")).toString().toLatin1());
	QVERIFY(provenance.value(QStringLiteral("commit")).isNull());
	QCOMPARE(provenance.value(QStringLiteral("contract_source_commit")).toString(),
		 QStringLiteral("252669d054401f8c6081be49521f1c7f44410bec"));
	QCOMPARE(provenance.value(QStringLiteral("binary_hash_verified")), QJsonValue(false));
	// schema адресует определения DTO без изменения исходного документа.
	const auto schema = fixture::object(bytes)
				    .value(QStringLiteral("components"))
				    .toObject()
				    .value(QStringLiteral("schemas"))
				    .toObject();
	// required проверяет обязательность field у выбранного type по точному массиву required OpenAPI.
	const auto required = [&](const char *type, const char *field) {
		return schema.value(QLatin1String(type))
			.toObject()
			.value(QStringLiteral("required"))
			.toArray()
			.contains(QString::fromLatin1(field));
	};
	QVERIFY(required("StreamDTO", "publication_key"));
	QVERIFY(required("DestinationDTO", "stream_key"));
	QVERIFY(required("DestinationDTO", "endpoint"));
	QVERIFY(required("OutputMetricsDTO", "ack_ahead_events_total"));
	QVERIFY(required("OutputMetricsDTO", "ack_ahead_bytes_last"));
	QVERIFY(required("OutputMetricsDTO", "ack_ahead_bytes_max"));
	QVERIFY(required("OutputMetricsDTO", "ack_last_anomaly_at"));
	QVERIFY(required("GroupProfileDTO", "live_audio_mode"));
	QVERIFY(schema.value(QStringLiteral("GroupProfileDTO"))
			.toObject()
			.value(QStringLiteral("properties"))
			.toObject()
			.contains(QStringLiteral("live_audio_mode")));
}

void DtoTest::unnamedFfmpegRoleIsAValidPublicObservation()
{
	// Пустая role допустима контрактом и не используется как идентичность: исполнение различается
	// по observation.instanceId и точному group_instance_id.
	auto json = fixture::event();
	json.insert(QStringLiteral("owner"), QStringLiteral("ffmpeg"));
	json.insert(QStringLiteral("role"), QString{});
	json.insert(QStringLiteral("group_instance_id"), QStringLiteral("epoch:2"));
	json.insert(QStringLiteral("observation"),
		    fixture::observation(QStringLiteral("epoch:3"), 1, QStringLiteral("READY")));
	const auto result = decode<RuntimeEvent>(fixture::bytes(json));
	QVERIFY(result);
	QVERIFY(result.value->role.isEmpty());
	QVERIFY(validateRuntimeEvent(*result.value));
}

void DtoTest::systemRemovalHasNoSessionOrGpuPayload()
{
	// removed системы — изменение существования, не новое GPU-измерение. Добавление Session ID,
	// смена kind или блок данных GPU нарушают именно форму события, даже если JSON синтаксически корректен.
	auto json = fixture::event(QStringLiteral("removed"));
	json.insert(QStringLiteral("owner"), QStringLiteral("system"));
	json.insert(QStringLiteral("session_id"), QString{});
	json.insert(QStringLiteral("observation"), fixture::observation(QStringLiteral("epoch:9"), 0, QString{}));
	const auto result = decode<RuntimeEvent>(fixture::bytes(json));
	QVERIFY(result);
	QVERIFY(validateRuntimeEvent(*result.value));
	QVERIFY(result.value->sessionId.isEmpty());
	QVERIFY(!result.value->gpu.hasValue());
	auto invalid = *result.value;
	invalid.sessionId = QStringLiteral("foreign-session");
	QVERIFY(!validateRuntimeEvent(invalid));
	invalid = *result.value;
	invalid.kind = QStringLiteral("metrics");
	QVERIFY(!validateRuntimeEvent(invalid));
	invalid = *result.value;
	invalid.gpu.state = FieldState::Value;
	QVERIFY(!validateRuntimeEvent(invalid));
}

// Синтаксическая корректность, UTF-8 и глубина проверяются до публикации дерева.
void DtoTest::exactParserRejectsMalformedAndDeepDocuments()
{
	// original/parsed различают прежнее дерево и неуспешный вход; token задаёт независимую ошибку JSON.
	const JsonValue original(QStringLiteral("unchanged"));
	JsonValue parsed = original;
	for (const auto &token :
	     {QByteArray("01"), QByteArray("1."), QByteArray("1e+"), QByteArray("[1,]"), QByteArray("{\"x\":1}{}"),
	      QByteArray("\"\\ud800\""), QByteArray("\"") + QByteArray::fromHex("ff") + '"'}) {
		QVERIFY(!parseJson(token, parsed));
		QCOMPARE(parsed, original);
	}
	// deep превышает документированный бюджет вложенности, не требуя большого тела.
	const QByteArray deep = QByteArray(130, '[') + "0" + QByteArray(130, ']');
	QVERIFY(!parseJson(deep, parsed));
	QCOMPARE(parsed, original);
}

QTEST_GUILESS_MAIN(DtoTest)
#include "dto-test.moc"
