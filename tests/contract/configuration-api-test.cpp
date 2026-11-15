#include "api/api-client.hpp"
#include "api/json-decoder.hpp"
#include "api/json-encoder.hpp"
#include "fixtures/api-json.hpp"
#include "fixtures/http-server.hpp"
#include "fixtures/operation-json.hpp"

#include <QJsonDocument>
#include <QTest>
#include <QUrlQuery>
#include <limits>

using namespace gost::api;

// Проверяет исходящие документы и настоящий Qt Network на локальных HTTP-ответах.
// Ожидания взяты из контрактов редактирования, ProfileText и команд, а не из сериализатора.
class ConfigurationApiTest final : public QObject {
	Q_OBJECT
private slots:
	// Отсутствующее поле сохраняется, а false и пустой ключ передаются как явное изменение.
	void patchesPreserveIntentAndExactRevision();
	// Задаёт старые и новые профили, включая отсутствующие значения и двоичный текст.
	void fullProfilesRoundTrip_data();
	// Полная замена сохраняет независимый LIVE AAC, FPS, encoding и исходные байты.
	void fullProfilesRoundTrip();
	// Задаёт нарушения документированных диапазонов и разрешённых custom-параметров.
	void invalidProfilesAreRejectedLocally_data();
	// Некорректный полный рецепт не превращается в допустимый исходящий документ.
	void invalidProfilesAreRejectedLocally();
	// Задаёт допустимое равенство CBR и отклонения по обе стороны от максимального битрейта.
	void cbrBitrateBoundaryKeepsSpecificLocalReason_data();
	// CBR проверяется до транспорта с точным путём, причиной и без изменения введённых битрейтов.
	void cbrBitrateBoundaryKeepsSpecificLocalReason();
	// Задаёт все четыре семейства CRUD и их публичные маршруты.
	void typedCrudUsesEntityStatuses_data();
	// Create/PATCH декодируют DTO, а DELETE принимает пустой 204 без JSON/MIME.
	void typedCrudUsesEntityStatuses();
	// Задаёт ответы, которые нельзя выдать за подтверждённую успешную запись.
	void mutationFailuresRememberWhetherTheyWereSent_data();
	// HTTP/contract/decode/transport отказ сохраняет факт отправки и не запускает повтор.
	void mutationFailuresRememberWhetherTheyWereSent();
	// Локальный отказ до транспорта явно отличается от неоднозначного результата записи.
	void localValidationNeverClaimsRequestWasSent();
	// Problem сохраняет пути и сообщения полей, не переносит серверное value в общую ошибку.
	void fieldProblemsKeepLocationsWithoutEchoingValues();
	// AddOutput передаёт только назначение и request_id точной Session и сохраняет начальный Output ID.
	void addOutputUsesSessionTargetAndInitialOutputIdentity();
	// Новый REST timeout действует на новые задания и не продлевает уже отправленный запрос.
	void changingTimeoutDoesNotExtendAnAcceptedJob();
};

void ConfigurationApiTest::patchesPreserveIntentAndExactRevision()
{
	// revision намеренно превышает точность double; endpoint содержит чувствительные части URL.
	const auto revision = std::numeric_limits<quint64>::max();
	const auto endpoint = fixture::endpoint();
	// untouched меняет только имя, не выдавая незаданные секретные поля за пустую строку.
	DestinationPatch untouched;
	untouched.expectedRevision = revision;
	untouched.name = QStringLiteral("Новое имя");
	// first содержит частичный PATCH, который должен состоять ровно из двух полей.
	const auto first = encodeRequest(untouched);
	QVERIFY(first);
	QCOMPARE(*first.value,
		 QJsonObject({{QStringLiteral("expected_revision"), QStringLiteral("18446744073709551615")},
			      {QStringLiteral("name"), QStringLiteral("Новое имя")}}));
	untouched.enabled = false;
	untouched.streamKey = QString{};
	untouched.endpoint = endpoint;
	// explicitChange фиксирует намеренные false, очистку ключа и точный URL.
	const auto explicitChange = encodeRequest(untouched);
	QVERIFY(explicitChange);
	QCOMPARE(explicitChange.value->value(QStringLiteral("enabled")), QJsonValue(false));
	QCOMPARE(explicitChange.value->value(QStringLiteral("stream_key")), QJsonValue(QString{}));
	QCOMPARE(explicitChange.value->value(QStringLiteral("endpoint")).toString(), endpoint);
	QCOMPARE(explicitChange.value->size(), 5);
	// nameOnly не должен включать полную замену профиля при обычном переименовании.
	const auto nameOnly = encodeRequest(ProfilePatch{revision, QStringLiteral("Имя"), std::nullopt});
	QVERIFY(nameOnly);
	QVERIFY(!nameOnly.value->contains(QStringLiteral("profile")));
	QCOMPARE(nameOnly.value->value(QStringLiteral("expected_revision")).toString(),
		 QStringLiteral("18446744073709551615"));
	// stream не предоставляет полей изменения постоянного publication_key или fallback-policy.
	const auto stream = encodeRequest(StreamPatch{revision, QStringLiteral("Имя")});
	QVERIFY(stream);
	QCOMPARE(stream.value->size(), 2);
	QVERIFY(!stream.value->contains(QStringLiteral("publication_key")));
	QVERIFY(!stream.value->contains(QStringLiteral("fallback")));
}

void ConfigurationApiTest::fullProfilesRoundTrip_data()
{
	QTest::addColumn<QJsonObject>("document");
	// legacy сохраняет encoding:null и отсутствие live_audio_mode без подстановки defaults.
	auto legacy = fixture::groupProfile();
	QTest::newRow("legacy-null") << legacy;
	legacy.remove(QStringLiteral("encoding"));
	QTest::newRow("legacy-missing") << legacy;
	// modern представляет полный уже материализованный сервером рецепт v2.
	auto modern = fixture::groupProfile();
	modern.insert(QStringLiteral("live_audio_mode"), QStringLiteral("copy"));
	modern.insert(QStringLiteral("encoding"), fixture::encoding());
	QTest::newRow("explicit-copy-v2") << modern;
	modern.insert(QStringLiteral("live_audio_mode"), QStringLiteral("transcode"));
	QTest::newRow("explicit-transcode-v2") << modern;
	// format/video получают независимую копию, чтобы проверить непредставимый в UTF-8 ProfileText.
	auto format = modern.value(QStringLiteral("format")).toObject();
	auto video = format.value(QStringLiteral("video")).toObject();
	video.insert(QStringLiteral("level"), fixture::object(R"({"encoding":"base64","value":"/wBh"})"));
	format.insert(QStringLiteral("video"), video);
	modern.insert(QStringLiteral("format"), format);
	QTest::newRow("binary-profile-text") << modern;
	// av1 описывает copy-видео с серверным SVT-AV1 рецептом только для заглушки.
	auto av1 = fixture::groupProfile();
	av1.insert(QStringLiteral("mode"), QStringLiteral("copy"));
	av1.insert(QStringLiteral("live_audio_mode"), QStringLiteral("copy"));
	format = fixture::format();
	video = format.value(QStringLiteral("video")).toObject();
	video.insert(QStringLiteral("codec"), QStringLiteral("av1"));
	video.insert(QStringLiteral("codec_profile"), QStringLiteral("main"));
	format.insert(QStringLiteral("video"), video);
	av1.insert(QStringLiteral("format"), format);
	av1.insert(
		QStringLiteral("video_encoding"),
		fixture::object(
			R"({"target_bitrate_bps":"6000000","max_bitrate_bps":"0","buffer_bits":"0","gop_frames":120,"max_b_frames":0,"closed_gop":true})"));
	av1.insert(
		QStringLiteral("encoding"),
		fixture::object(
			R"({"version":2,"video_encoder":"libsvtav1","audio_encoder":"aac","rate_control":"vbr","rescale_output":false,"video_custom":"lp=0","audio_custom":"aac_coder=fast","svt_av1":{"preset":13,"tuning":"vq","aq_mode":2,"lookahead_frames":-1}})"));
	QTest::newRow("av1-vbr-copy") << av1;
}

void ConfigurationApiTest::fullProfilesRoundTrip()
{
	// document содержит независимо заданный полный серверный профиль выбранного сценария.
	QFETCH(QJsonObject, document);
	// decoded моделирует открытие сохранённого профиля, encoded — его полную замену без изменений.
	const auto decoded = decode<GroupProfile>(fixture::bytes(document));
	QVERIFY(decoded);
	const auto encoded = encodeProfile(*decoded.value);
	QVERIFY(encoded);
	QCOMPARE(*encoded.value, document);
	// replacement проверяет тот же рецепт в реальном конверте PATCH с точной ревизией.
	const auto replacement = encodeRequest(ProfilePatch{9007199254740993ULL, std::nullopt, *decoded.value});
	QVERIFY(replacement);
	QCOMPARE(replacement.value->value(QStringLiteral("profile")).toObject(), document);
	QCOMPARE(replacement.value->value(QStringLiteral("expected_revision")).toString(),
		 QStringLiteral("9007199254740993"));
}

void ConfigurationApiTest::invalidProfilesAreRejectedLocally_data()
{
	QTest::addColumn<QString>("violation");
	// violation обозначает отдельный независимо заданный запрет публичного профиля.
	for (const auto *violation : {"null-audio",
				      "unknown-audio",
				      "copy-transcode-audio",
				      "odd-width",
				      "zero-fps",
				      "h264-max-overflow",
				      "h264-buffer-overflow",
				      "cbr-unequal",
				      "bframes-gop",
				      "nvenc-lookahead",
				      "nvenc-aq",
				      "foreign-encoder-block",
				      "unknown-custom",
				      "duplicate-custom",
				      "custom-field-conflict",
				      "custom-control",
				      "custom-too-long",
				      "invalid-aac-rate",
				      "invalid-aac-layout",
				      "av1-vbr-max",
				      "av1-vbr-buffer",
				      "av1-vbr-lookahead",
				      "av1-cbr-buffer",
				      "av1-fps",
				      "av1-rescale",
				      "av1-custom"})
		QTest::newRow(violation) << QString::fromLatin1(violation);
}

void ConfigurationApiTest::invalidProfilesAreRejectedLocally()
{
	// violation задаёт одно документированное нарушение рецепта из таблицы сценариев.
	QFETCH(QString, violation);
	// decoded даёт корректный исходный рецепт; profile получает ровно одно нарушение контракта.
	const auto decoded = decode<GroupProfile>(fixture::bytes(fixture::groupProfile()));
	QVERIFY(decoded);
	auto profile = *decoded.value;
	profile.liveAudioMode = {FieldState::Value, QStringLiteral("copy")};
	// encoding нужен для негативных проверок NVENC/CBR/custom независимо от legacy.
	const auto encoding = decode<Encoding>(fixture::bytes(fixture::encoding()));
	QVERIFY(encoding);
	profile.encoding = {FieldState::Value, *encoding.value};
	if (violation.startsWith(QStringLiteral("av1-"))) {
		profile.mode = QStringLiteral("copy");
		profile.format.video.codec = QStringLiteral("av1");
		profile.format.video.codecProfile.bytes = "main";
		profile.videoEncoding.maxBFrames = 0;
		profile.videoEncoding.maxBitrateBps = 0;
		profile.videoEncoding.bufferBits = 0;
		profile.encoding->videoEncoder = QStringLiteral("libsvtav1");
		profile.encoding->videoCustom = QStringLiteral("lp=0");
		profile.encoding->rescaleOutput = false;
		profile.encoding->nvenc.state = FieldState::Missing;
		profile.encoding->svtAv1 = {FieldState::Value, {8, QStringLiteral("psnr"), 2, -1}};
	}
	if (violation == QStringLiteral("null-audio"))
		profile.liveAudioMode.state = FieldState::Null;
	if (violation == QStringLiteral("unknown-audio"))
		profile.liveAudioMode.value = QStringLiteral("automatic");
	if (violation == QStringLiteral("copy-transcode-audio")) {
		profile.mode = QStringLiteral("copy");
		profile.liveAudioMode.value = QStringLiteral("transcode");
	}
	if (violation == QStringLiteral("odd-width"))
		profile.format.video.width = 1919;
	if (violation == QStringLiteral("zero-fps"))
		profile.format.video.frameRate = {0, 1};
	if (violation == QStringLiteral("h264-max-overflow"))
		profile.videoEncoding.maxBitrateBps = 4294967296ULL;
	if (violation == QStringLiteral("h264-buffer-overflow"))
		profile.videoEncoding.bufferBits = 2147483648ULL;
	if (violation == QStringLiteral("cbr-unequal"))
		profile.encoding->rateControl = QStringLiteral("cbr");
	if (violation == QStringLiteral("bframes-gop"))
		profile.videoEncoding.maxBFrames = profile.videoEncoding.gopFrames;
	if (violation == QStringLiteral("nvenc-lookahead"))
		profile.encoding->nvenc->lookaheadFrames = 30;
	if (violation == QStringLiteral("nvenc-aq")) {
		profile.encoding->nvenc->enableSpatialAq = false;
		profile.encoding->nvenc->aqStrength = 7;
	}
	if (violation == QStringLiteral("foreign-encoder-block"))
		profile.encoding->svtAv1 = {FieldState::Value, {8, QStringLiteral("psnr"), 2, -1}};
	if (violation == QStringLiteral("unknown-custom"))
		profile.encoding->videoCustom = QStringLiteral("arbitrary=1");
	if (violation == QStringLiteral("duplicate-custom"))
		profile.encoding->videoCustom = QStringLiteral("nonref_p=1 nonref_p=0");
	if (violation == QStringLiteral("custom-field-conflict"))
		profile.encoding->videoCustom = QStringLiteral("rc=vbr");
	if (violation == QStringLiteral("custom-control"))
		profile.encoding->videoCustom = QStringLiteral("nonref_p=1\nstrict_gop=1");
	if (violation == QStringLiteral("custom-too-long"))
		profile.encoding->audioCustom = QString(4097, QLatin1Char('x'));
	if (violation == QStringLiteral("invalid-aac-rate"))
		profile.format.audio.sampleRateHz = 96000;
	if (violation == QStringLiteral("invalid-aac-layout"))
		profile.format.audio.channelLayout.bytes = "mono";
	if (violation == QStringLiteral("av1-vbr-max"))
		profile.videoEncoding.maxBitrateBps = 6000000;
	if (violation == QStringLiteral("av1-vbr-buffer"))
		profile.videoEncoding.bufferBits = 6000000;
	if (violation == QStringLiteral("av1-vbr-lookahead"))
		profile.encoding->svtAv1->lookaheadFrames = 0;
	if (violation == QStringLiteral("av1-fps"))
		profile.format.video.frameRate = {241, 1};
	if (violation == QStringLiteral("av1-rescale"))
		profile.encoding->rescaleOutput = true;
	if (violation == QStringLiteral("av1-custom"))
		profile.encoding->videoCustom = QStringLiteral("lp=7");
	if (violation == QStringLiteral("av1-cbr-buffer")) {
		profile.encoding->rateControl = QStringLiteral("cbr");
		profile.encoding->svtAv1->lookaheadFrames = 0;
		profile.videoEncoding.maxBitrateBps = 6000000;
		profile.videoEncoding.bufferBits = 123457;
	}
	// result обязан отказать до HTTP, без выдуманной корректировки пользовательских значений.
	const auto result = encodeProfile(profile);
	QVERIFY(!result);
	QVERIFY(result.error.has_value());
	QCOMPARE(result.error->kind, ErrorKind::Contract);
	QVERIFY(!result.error->requestSent);
}

// Равенство основного и максимального битрейтов обязательно для CBR независимо от направления расхождения.
void ConfigurationApiTest::cbrBitrateBoundaryKeepsSpecificLocalReason_data()
{
	QTest::addColumn<quint64>("target");
	QTest::addColumn<bool>("accepted");
	QTest::newRow("below-maximum") << quint64(10000000) << false;
	QTest::newRow("equal-maximum") << quint64(12000000) << true;
	QTest::newRow("above-maximum") << quint64(14000000) << false;
}

// Проверяет публичное правило CBR на полном запросе, сохраняя точные значения входного DTO.
void ConfigurationApiTest::cbrBitrateBoundaryKeepsSpecificLocalReason()
{
	// target задаёт основной битрейт в бит/с; accepted определяет независимый результат правила равенства CBR.
	QFETCH(quint64, target);
	QFETCH(bool, accepted);
	// decoded и recipe дают известный корректный H.264-рецепт; profile получает выбранную границу CBR.
	const auto decoded = decode<GroupProfile>(fixture::bytes(fixture::groupProfile()));
	const auto recipe = decode<Encoding>(fixture::bytes(fixture::encoding()));
	QVERIFY(decoded && recipe);
	auto profile = *decoded.value;
	profile.liveAudioMode = {FieldState::Value, QStringLiteral("copy")};
	profile.encoding = {FieldState::Value, *recipe.value};
	profile.encoding->rateControl = QStringLiteral("cbr");
	profile.videoEncoding.targetBitrateBps = target;
	profile.videoEncoding.maxBitrateBps = 12000000;
	// result содержит документ только для равных битрейтов; ошибка не означает отправку HTTP.
	const auto result = encodeRequest(ProfilePatch{1, std::nullopt, profile});
	QCOMPARE(bool(result), accepted);
	QCOMPARE(profile.videoEncoding.targetBitrateBps, target);
	QCOMPARE(profile.videoEncoding.maxBitrateBps, quint64(12000000));
	if (accepted) {
		QCOMPARE(result.value->value(QStringLiteral("profile"))
				 .toObject()
				 .value(QStringLiteral("video_encoding"))
				 .toObject()
				 .value(QStringLiteral("target_bitrate_bps"))
				 .toString(),
			 QStringLiteral("12000000"));
	} else {
		QVERIFY(result.error.has_value());
		QVERIFY(!result.error->requestSent);
		QVERIFY(result.error->problem.has_value());
		QCOMPARE(result.error->problem->field, QStringLiteral("profile.video_encoding.max_bitrate_bps"));
		QCOMPARE(result.error->problem->errors.size(), 1);
		QCOMPARE(result.error->problem->errors.first().location,
			 QStringLiteral("profile.video_encoding.max_bitrate_bps"));
		if (target < 12000000)
			QCOMPARE(result.error->problem->errors.first().messageKey,
				 QStringLiteral("Validation.CbrBitrateMismatch"));
		else {
			QVERIFY(!result.error->problem->errors.first().messageKey.isEmpty());
			QVERIFY(result.error->problem->errors.first().messageKey !=
				QStringLiteral("Configuration.InvalidField"));
		}
	}
}

void ConfigurationApiTest::typedCrudUsesEntityStatuses_data()
{
	QTest::addColumn<int>("kind");
	QTest::addColumn<int>("operation");
	QTest::addColumn<QByteArray>("route");
	// kind выбирает сущность, operation — create/PATCH/DELETE; имена строк описывают HTTP-контракт.
	for (int kind = 0; kind < 4; ++kind) {
		// route содержит публичный путь коллекции конкретной сущности.
		const auto route =
			QList<QByteArray>{"streams", "destinations", "transcode-groups", "transcode-profiles"}.at(kind);
		// operation перебирает три варианта записи одной сущности.
		for (int operation = 0; operation < 3; ++operation)
			QTest::newRow((route + '-' + QByteArray::number(operation)).constData())
				<< kind << operation << route;
	}
}

void ConfigurationApiTest::typedCrudUsesEntityStatuses()
{
	// kind выбирает сущность; operation задаёт create/PATCH/DELETE; route содержит точный путь API.
	QFETCH(int, kind);
	QFETCH(int, operation);
	QFETCH(QByteArray, route);
	// server отдаёт разные статусы согласно семейству CRUD и намеренно не отдаёт JSON для DELETE.
	HttpServer server;
	QVERIFY(server.listen());
	server.handler = [kind, operation](const HttpServer::Request &request) {
		// entities содержат независимо собранные серверные DTO.
		const QList<QJsonObject> entities{fixture::stream(), fixture::destination(), fixture::group(),
						  fixture::profile()};
		// entity возвращает точную цель PATCH, включая непрозрачный слеш внутри одного ID.
		auto entity = entities.at(kind);
		if (operation == 1)
			entity.insert(QStringLiteral("id"), QStringLiteral("id/part"));
		HttpServer::reply(request, operation == 2 ? QByteArray{} : fixture::bytes(entity),
				  operation == 0   ? 201
				  : operation == 1 ? 200
						   : 204,
				  operation == 2 ? QByteArray{} : QByteArray("application/json"));
	};
	// client работает только с петлевым сервером, received/succeeded/error фиксируют callback.
	APIClient client;
	client.configure(server.origin(), {7, 9});
	bool received = false;
	bool succeeded = false;
	std::optional<Error> error;
	// callback проверяет контекст независимо от конкретного типа CRUD-результата.
	const auto callback = [&](RequestContext context, auto result) {
		QCOMPARE(context, (RequestContext{7, 9}));
		received = true;
		succeeded = bool(result);
		error = result.error;
	};
	// profile используется только для полного создания именованного профиля.
	const auto profile = decode<GroupProfile>(fixture::bytes(fixture::groupProfile()));
	QVERIFY(profile);
	if (kind == 0 && operation == 0)
		client.createStream({QStringLiteral("Имя"), QStringLiteral("ключ")}, this, callback);
	if (kind == 0 && operation == 1)
		client.patchStream(QStringLiteral("id/part"), {9007199254740993ULL, QStringLiteral("Имя")}, this,
				   callback);
	if (kind == 0 && operation == 2)
		client.deleteStream(QStringLiteral("id/part"), 9007199254740993ULL, this, callback);
	if (kind == 1 && operation == 0)
		client.createDestination({QStringLiteral("stream"), QStringLiteral("group"), QStringLiteral("Имя"),
					  QStringLiteral("custom"), fixture::endpoint(), QString{}, false},
					 this, callback);
	if (kind == 1 && operation == 1)
		client.patchDestination(QStringLiteral("id/part"),
					{9007199254740993ULL, {}, {}, QStringLiteral("Имя"), {}, {}, QString{}, false},
					this, callback);
	if (kind == 1 && operation == 2)
		client.deleteDestination(QStringLiteral("id/part"), 9007199254740993ULL, this, callback);
	if (kind == 2 && operation == 0)
		client.createGroup({QStringLiteral("stream"), QStringLiteral("profile"), QStringLiteral("Имя")}, this,
				   callback);
	if (kind == 2 && operation == 1)
		client.patchGroup(QStringLiteral("id/part"), {9007199254740993ULL, {}, {}, QStringLiteral("Имя")}, this,
				  callback);
	if (kind == 2 && operation == 2)
		client.deleteGroup(QStringLiteral("id/part"), 9007199254740993ULL, this, callback);
	if (kind == 3 && operation == 0)
		client.createProfile({QStringLiteral("Имя"), *profile.value}, this, callback);
	if (kind == 3 && operation == 1)
		client.patchProfile(QStringLiteral("id/part"), {9007199254740993ULL, QStringLiteral("Имя"), {}}, this,
				    callback);
	if (kind == 3 && operation == 2)
		client.deleteProfile(QStringLiteral("id/part"), 9007199254740993ULL, this, callback);
	QTRY_VERIFY_WITH_TIMEOUT(received, 2000);
	QVERIFY2(succeeded, error ? qPrintable(error->message) : "Нет результата");
	QCOMPARE(server.requests.size(), 1);
	QCOMPARE(server.requests.first().method, operation == 0   ? QByteArray("POST")
						 : operation == 1 ? QByteArray("PATCH")
								  : QByteArray("DELETE"));
	QVERIFY(server.requests.first().target.startsWith("/api/v1/" + route));
	if (operation > 0)
		QVERIFY(server.requests.first().target.contains("id%2Fpart"));
	if (operation == 2) {
		QCOMPARE(QUrlQuery(QUrl::fromEncoded("http://local" + server.requests.first().target))
				 .queryItemValue(QStringLiteral("expected_revision")),
			 QStringLiteral("9007199254740993"));
		QVERIFY(server.requests.first().body.isEmpty());
	} else if (operation == 1) {
		QCOMPARE(QJsonDocument::fromJson(server.requests.first().body)
				 .object()
				 .value(QStringLiteral("expected_revision"))
				 .toString(),
			 QStringLiteral("9007199254740993"));
	}
}

void ConfigurationApiTest::mutationFailuresRememberWhetherTheyWereSent_data()
{
	QTest::addColumn<int>("status");
	QTest::addColumn<QByteArray>("body");
	QTest::addColumn<QByteArray>("mime");
	QTest::newRow("wrong-success-status")
		<< 200 << fixture::bytes(fixture::stream()) << QByteArray("application/json");
	QTest::newRow("bad-json") << 201 << QByteArray("{broken") << QByteArray("application/json");
	QTest::newRow("wrong-mime") << 201 << fixture::bytes(fixture::stream()) << QByteArray("text/plain");
	QTest::newRow("invalid-entity") << 201 << QByteArray("{}") << QByteArray("application/json");
	QTest::newRow("server-error") << 503 << QByteArray("{}") << QByteArray("application/problem+json");
	QTest::newRow("response-lost") << 0 << QByteArray{} << QByteArray{};
}

void ConfigurationApiTest::mutationFailuresRememberWhetherTheyWereSent()
{
	// status, body и mime описывают независимо управляемый HTTP-ответ; status=0 означает потерю ответа.
	QFETCH(int, status);
	QFETCH(QByteArray, body);
	QFETCH(QByteArray, mime);
	// server либо возвращает заданный отказ, либо разрывает соединение после полного POST.
	HttpServer server;
	QVERIFY(server.listen());
	server.handler = [&](const HttpServer::Request &request) {
		if (status)
			HttpServer::reply(request, body, status, mime);
		else if (request.socket)
			request.socket->abort();
	};
	// client отправляет одно намерение; result фиксирует проверенный исход после ответа/разрыва.
	APIClient client;
	client.configure(server.origin(), {});
	std::optional<Result<Stream>> result;
	client.createStream({QStringLiteral("Имя"), QStringLiteral("synthetic-secret")}, this,
			    [&](RequestContext, Result<Stream> received) { result = std::move(received); });
	QTRY_VERIFY_WITH_TIMEOUT(result.has_value(), 2000);
	QVERIFY(!*result);
	QVERIFY(result->error->requestSent);
	QCOMPARE(server.requests.size(), 1);
}

void ConfigurationApiTest::localValidationNeverClaimsRequestWasSent()
{
	// server не должен получить ни одного запроса с пустым обязательным publication_key.
	HttpServer server;
	QVERIFY(server.listen());
	// client/result сохраняют локальную ошибку отдельно от неоднозначного результата отправки.
	APIClient client;
	client.configure(server.origin(), {});
	std::optional<Result<Stream>> result;
	client.createStream({QStringLiteral("Имя"), QString{}}, this,
			    [&](RequestContext, Result<Stream> received) { result = std::move(received); });
	QTRY_VERIFY_WITH_TIMEOUT(result.has_value(), 1000);
	QVERIFY(!*result);
	QVERIFY(!result->error->requestSent);
	QVERIFY(server.requests.isEmpty());
}

// Серверный messageKey не получает доверия локального валидатора; message остаётся обычным контрактным текстом.
void ConfigurationApiTest::fieldProblemsKeepLocationsWithoutEchoingValues()
{
	// problem содержит намеренно секретное value; result должен сохранить только контрактные поля ошибки.
	const auto problem = QByteArray(
		R"({"status":422,"title":"Validation","detail":"Rejected","errors":[{"location":"body.stream_key","message":"Invalid key","value":"synthetic-secret","messageKey":"Configuration.Saved"},{"location":"body.name","message":"Required"}]})");
	const auto result = decodeProblem(422, problem);
	QVERIFY(result.problem.has_value());
	QCOMPARE(result.problem->errors.size(), 2);
	QCOMPARE(result.problem->errors.first().location, QStringLiteral("body.stream_key"));
	QCOMPARE(result.problem->errors.first().message, QStringLiteral("Invalid key"));
	QVERIFY(result.problem->errors.first().messageKey.isEmpty());
	QCOMPARE(result.problem->errors.last().location, QStringLiteral("body.name"));
	QVERIFY(result.problem->errors.last().messageKey.isEmpty());
	QVERIFY(!result.message.contains(QStringLiteral("synthetic-secret")));
}

void ConfigurationApiTest::addOutputUsesSessionTargetAndInitialOutputIdentity()
{
	// server возвращает именно начальную PENDING-квитанцию с уже выделенным Output ID.
	HttpServer server;
	QVERIFY(server.listen());
	server.handler = [](const HttpServer::Request &request) {
		HttpServer::reply(
			request,
			fixture::bytes(fixture::receipt(QStringLiteral("request"), QStringLiteral("add_output"),
							QStringLiteral("session/target"),
							QStringLiteral("session/target"), QStringLiteral("new-output"),
							QStringLiteral("command"), QStringLiteral("PENDING"))),
			202);
	};
	// client/result фиксируют одну команду, без ожидания выдуманного runtime/PUBLISHING.
	APIClient client;
	client.configure(server.origin(), {});
	std::optional<Result<Receipt>> result;
	client.addOutput(QStringLiteral("session/target"), QStringLiteral("destination"), QStringLiteral("request"),
			 this, [&](RequestContext, Result<Receipt> received) { result = std::move(received); });
	QTRY_VERIFY_WITH_TIMEOUT(result.has_value(), 2000);
	QVERIFY(*result);
	QCOMPARE(result->value->status, QStringLiteral("PENDING"));
	QCOMPARE(result->value->targetId, QStringLiteral("session/target"));
	QCOMPARE(result->value->outputId, QStringLiteral("new-output"));
	QCOMPARE(server.requests.size(), 1);
	QCOMPARE(server.requests.first().target, QByteArray("/api/v1/sessions/session%2Ftarget/outputs"));
	QCOMPARE(QJsonDocument::fromJson(server.requests.first().body).object(),
		 QJsonObject({{QStringLiteral("request_id"), QStringLiteral("request")},
			      {QStringLiteral("destination_id"), QStringLiteral("destination")}}));
}

void ConfigurationApiTest::changingTimeoutDoesNotExtendAnAcceptedJob()
{
	// server удерживает ответы: видимый результат должен определяться timeout каждого отправленного Job.
	HttpServer server;
	QVERIFY(server.listen());
	// client получает короткий срок первого запроса; result фиксирует его завершение.
	APIClient client;
	client.configure(server.origin(), {});
	client.setHttpTimeoutMs(70);
	std::optional<Result<Stream>> result;
	client.createStream({QStringLiteral("Имя"), QStringLiteral("ключ")}, this,
			    [&](RequestContext, Result<Stream> received) { result = std::move(received); });
	QTRY_COMPARE_WITH_TIMEOUT(server.requests.size(), 1, 1000);
	client.setHttpTimeoutMs(5000);
	QTRY_VERIFY_WITH_TIMEOUT(result.has_value(), 1000);
	QVERIFY(!*result);
	QCOMPARE(result->error->kind, ErrorKind::Timeout);
	QVERIFY(result->error->requestSent);
}

QTEST_GUILESS_MAIN(ConfigurationApiTest)
#include "configuration-api-test.moc"
