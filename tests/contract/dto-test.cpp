#include "api/json-decoder.hpp"
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
	void decimalBoundaries_data();
	void decimalBoundaries();
	void nonCanonicalDecimals_data();
	void nonCanonicalDecimals();
	void decimalsRejectJsonNumbersAndNull();
	void nanosecondsAndTimestampsStayExact();
	void rationalFpsAndBinaryProfileText();
	void invalidProfileText_data();
	void invalidProfileText();
	void fieldStatesRemainDistinct();
	void legacyAndExplicitAudioEncoding();
	void invalidAudioModes_data();
	void invalidAudioModes();
	void keysAndEndpointAreExact();
	void incompatibleSecretsAreUnavailableOrRejected();
	void historicalSnapshotsOwnTheirValues();
	void ackMetricsRemainExactAndUnavailableIsNotZero();
	void malformedJsonAndUnicodeAreRejected();
	void pinnedSchemaAndProvenanceDescribeReadbackContract();
	void unnamedFfmpegRoleIsAValidPublicObservation();
	void systemRemovalHasNoSessionOrGpuPayload();
};

void DtoTest::decimalBoundaries_data()
{
	QTest::addColumn<QString>("text");
	QTest::addColumn<quint64>("expected");
	QTest::newRow("zero") << QStringLiteral("0") << quint64(0);
	// Число на единицу больше 2^53 выявляет потерю точности при промежуточном преобразовании через double.
	QTest::newRow("above-double") << QStringLiteral("9007199254740993") << quint64(9007199254740993ULL);
	QTest::newRow("max") << QStringLiteral("18446744073709551615") << std::numeric_limits<quint64>::max();
}
void DtoTest::decimalBoundaries()
{
	QFETCH(QString, text);
	QFETCH(quint64, expected);
	quint64 result = 42;
	Error error;
	QVERIFY(fromJson(QJsonValue(text), result, error));
	QCOMPARE(result, expected);
}
void DtoTest::nonCanonicalDecimals_data()
{
	// Внешне числовые строки со знаком, пробелом, экспонентой и Unicode-цифрой запрещены:
	// контракт требует одну каноническую десятичную запись uint64, а не общее преобразование числа.
	QTest::addColumn<QString>("text");
	for (const auto *value : {"", "00", "01", "+1", "-1", "1.0", "1e3", " 1", "1 ", "18446744073709551616",
				  "999999999999999999999999999999999999999999"})
		QTest::newRow(value) << QString::fromLatin1(value);
	QTest::newRow("unicode-digit") << QStringLiteral("١");
}
void DtoTest::nonCanonicalDecimals()
{
	QFETCH(QString, text);
	quint64 result = 42;
	Error error;
	QVERIFY(!fromJson(QJsonValue(text), result, error));
}
void DtoTest::decimalsRejectJsonNumbersAndNull()
{
	// Даже представимое без потерь JSON-число 0 недопустимо для строкового uint64. Это проверка
	// формы контракта, а не только переполнения на большом значении.
	for (const auto &input : {QJsonValue(0), QJsonValue(9007199254740992.0), QJsonValue(true),
				  QJsonValue(QJsonValue::Null), QJsonValue(QJsonValue::Undefined)}) {
		quint64 result = 42;
		Error error;
		QVERIFY(!fromJson(input, result, error));
	}
}
void DtoTest::nanosecondsAndTimestampsStayExact()
{
	const auto policy = fixture::object(R"({"asset_id":"asset","watchdog":{"video_timeout_ns":"9007199254740993",
"audio_timeout_ns":"1234567890123456789","recovery_window_ns":"0","keyframe_timeout_ns":"18446744073709551615"}})");
	const auto result = decode<Policy>(fixture::bytes(policy));
	QVERIFY(result);
	QCOMPARE(result.value->watchdog.videoTimeoutNs, quint64(9007199254740993ULL));
	QCOMPARE(result.value->watchdog.audioTimeoutNs, quint64(1234567890123456789ULL));
	QCOMPARE(result.value->watchdog.keyframeTimeoutNs, std::numeric_limits<quint64>::max());
	const auto record = decode<Record>(fixture::bytes(fixture::record(QStringLiteral("record"))));
	QVERIFY(record);
	QCOMPARE(record.value->createdAt, QStringLiteral("2026-11-10T01:02:03.123456789Z"));
}
void DtoTest::rationalFpsAndBinaryProfileText()
{
	// 60000/1001 сохраняется дробью; двоичный ProfileText включает NUL и не-UTF-8 байты.
	// Соседнее строковое поле с эмодзи проверяет, что текст и двоичная оболочка не смешиваются.
	auto json = fixture::format();
	auto video = json.value(QStringLiteral("video")).toObject();
	video.insert(QStringLiteral("codec_profile"), fixture::object(R"({"encoding":"base64","value":"AP+AQQ=="})"));
	video.insert(QStringLiteral("level"), QStringLiteral("уровень 😀"));
	json.insert(QStringLiteral("video"), video);
	const auto result = decode<Format>(fixture::bytes(json));
	QVERIFY(result);
	QCOMPARE(result.value->video.frameRate.numerator, quint32(60000));
	QCOMPARE(result.value->video.frameRate.denominator, quint32(1001));
	QCOMPARE(result.value->video.codecProfile.bytes, QByteArray::fromHex("00ff8041"));
	QCOMPARE(result.value->video.level.bytes, QStringLiteral("уровень 😀").toUtf8());
	Error error;
	FrameRate invalid;
	QVERIFY(!fromJson(fixture::object(R"({"numerator":60,"denominator":0})"), invalid, error));
}
void DtoTest::invalidProfileText_data()
{
	// Zh== имеет неканонические значимые биты заполнения; Zg без заполнения и лишние ключи оболочки
	// также отвергаются, хотя либеральные base64-декодеры могут принять часть таких значений.
	QTest::addColumn<QJsonValue>("input");
	QTest::newRow("null") << QJsonValue(QJsonValue::Null);
	QTest::newRow("number") << QJsonValue(123);
	for (const auto *json : {R"({"encoding":"base64","value":"Zh=="})", R"({"encoding":"base64","value":"Zg"})",
				 R"({"encoding":"base64","value":"Zg==!"})", R"({"encoding":"hex","value":"00"})",
				 R"({"encoding":"base64","value":"AA==","extra":true})"})
		QTest::newRow(json) << QJsonValue(fixture::object(json));
}
void DtoTest::invalidProfileText()
{
	QFETCH(QJsonValue, input);
	ProfileText value;
	Error error;
	QVERIFY(!fromJson(input, value, error));
}
void DtoTest::fieldStatesRemainDistinct()
{
	// Один и тот же JsonField последовательно получает Missing, Null и Value(0). Логическое состояние
	// не должно зависеть от физического содержимого value или истинности самого числа.
	Error error;
	JsonField<quint64> value;
	QVERIFY(fromJson(QJsonValue(QJsonValue::Undefined), value, error));
	QVERIFY(value.isMissing());
	QVERIFY(fromJson(QJsonValue(QJsonValue::Null), value, error));
	QVERIFY(value.isNull());
	QVERIFY(fromJson(QJsonValue(QStringLiteral("0")), value, error));
	QVERIFY(value.hasValue());
	QCOMPARE(value.value, quint64(0));
}
void DtoTest::legacyAndExplicitAudioEncoding()
{
	// Чтение старого профиля не должно подставлять новый режим аудио или преобразовывать encoding=null.
	auto json = fixture::groupProfile();
	const auto legacy = decode<GroupProfile>(fixture::bytes(json));
	QVERIFY(legacy);
	QVERIFY(legacy.value->liveAudioMode.isMissing());
	QVERIFY(legacy.value->encoding.isNull());
	json.insert(QStringLiteral("live_audio_mode"), QStringLiteral("copy"));
	json.insert(QStringLiteral("encoding"), fixture::encoding());
	const auto modern = decode<GroupProfile>(fixture::bytes(json));
	QVERIFY(modern);
	QVERIFY(modern.value->liveAudioMode.hasValue());
	QCOMPARE(modern.value->liveAudioMode.value, QStringLiteral("copy"));
	QVERIFY(modern.value->encoding.hasValue());
	QCOMPARE(modern.value->encoding.value.version, 2);
	QCOMPARE(modern.value->encoding.value.nvenc.value.lookaheadFrames, 16);
	QCOMPARE(modern.value->encoding.value.audioCustom, QStringLiteral("aac_coder=twoloop"));
	json.insert(QStringLiteral("live_audio_mode"), QStringLiteral("transcode"));
	const auto audio = decode<GroupProfile>(fixture::bytes(json));
	QVERIFY(audio);
	QCOMPARE(audio.value->liveAudioMode.value, QStringLiteral("transcode"));
	QCOMPARE(legacy.value->liveAudioMode.state, FieldState::Missing);
}
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
void DtoTest::invalidAudioModes()
{
	QFETCH(QString, mode);
	QFETCH(QJsonValue, audio);
	auto json = fixture::groupProfile();
	json.insert(QStringLiteral("mode"), mode);
	json.insert(QStringLiteral("live_audio_mode"), audio);
	QVERIFY(!decode<GroupProfile>(fixture::bytes(json)));
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
	QCOMPARE(output.value->profile.profile.liveAudioMode.state, FieldState::Missing);
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
	QFile schemaFile(QStringLiteral(GOST_TEST_FIXTURE_DIR) + QStringLiteral("/backend/openapi.json"));
	QFile provenanceFile(QStringLiteral(GOST_TEST_FIXTURE_DIR) + QStringLiteral("/backend/provenance.json"));
	QVERIFY(schemaFile.open(QIODevice::ReadOnly));
	QVERIFY(provenanceFile.open(QIODevice::ReadOnly));
	const auto bytes = schemaFile.readAll();
	const auto provenance = fixture::object(provenanceFile.readAll());
	QCOMPARE(QCryptographicHash::hash(bytes, QCryptographicHash::Sha256).toHex().toUpper(),
		 provenance.value(QStringLiteral("schema_sha256")).toString().toLatin1());
	QCOMPARE(provenance.value(QStringLiteral("commit")).toString(),
		 QStringLiteral("103d839b11591fbb786fd027aefa143a2ff79036"));
	const auto schema = fixture::object(bytes)
				    .value(QStringLiteral("components"))
				    .toObject()
				    .value(QStringLiteral("schemas"))
				    .toObject();
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
	QVERIFY(!required("GroupProfileDTO", "live_audio_mode"));
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

QTEST_GUILESS_MAIN(DtoTest)
#include "dto-test.moc"
