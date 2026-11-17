#pragma once
#include "api/profile-rules.hpp"

// Матрица следует encoding_expanded.go, encoding_hdr.go и encoding_audio.go закреплённой версии сервера.
// Строки описывают независимые продуктовые привязки, а не результат вызова каталога продукта.
void ConfigurationApiTest::completeBindingMatrix_data()
{
	QTest::addColumn<QString>("encoder");
	QTest::addColumn<QString>("codec");
	QTest::addColumn<QString>("mode");
	QTest::addColumn<QString>("control");
	QTest::addColumn<QString>("audio");
	QTest::addColumn<QString>("videoProfile");
	QTest::addColumn<QString>("pixel");
	QTest::addColumn<int>("depth");
	// tuple перечисляет разрешённые семейства кодировщиков с режимами LIVE видео.
	for (const auto &tuple : {QStringLiteral("h264_nvenc|h264|transcode"), QStringLiteral("libx264|h264|transcode"),
				  QStringLiteral("libsvtav1|av1|copy"), QStringLiteral("libaom-av1|av1|copy"),
				  QStringLiteral("libx265|hevc|copy"), QStringLiteral("libx265|hevc|transcode"),
				  QStringLiteral("hevc_nvenc|hevc|copy"), QStringLiteral("hevc_nvenc|hevc|transcode"),
				  QStringLiteral("libvpx-vp9|vp9|copy"), QStringLiteral("libvpx-vp9|vp9|transcode")}) {
		// parts содержит encoder/codec/mode; control перебирает четыре режима качества и битрейта.
		const auto parts = tuple.split(QLatin1Char('|'));
		for (const auto &control :
		     {QStringLiteral("cbr"), QStringLiteral("vbr"), QStringLiteral("cqp"), QStringLiteral("crf")}) {
			if ((parts[0] == QStringLiteral("h264_nvenc") || parts[0] == QStringLiteral("hevc_nvenc")) &&
			    control == QStringLiteral("crf"))
				continue;
			if ((parts[0] == QStringLiteral("libaom-av1") || parts[0] == QStringLiteral("libvpx-vp9")) &&
			    control == QStringLiteral("cqp"))
				continue;
			// formats перечисляет точные сочетания профиля, пиксельного формата и глубины из серверной матрицы.
			QStringList formats;
			if (parts[0] == QStringLiteral("h264_nvenc"))
				formats = {QStringLiteral("baseline|yuv420p|8"), QStringLiteral("main|yuv420p|8"),
					   QStringLiteral("high|yuv420p|8"), QStringLiteral("high444|yuv444p|8")};
			else if (parts[0] == QStringLiteral("libx264"))
				formats = {QStringLiteral("constrained-baseline|yuv420p|8"),
					   QStringLiteral("main|yuv420p|8"),
					   QStringLiteral("high|yuv420p|8"),
					   QStringLiteral("high10|yuv420p10le|10"),
					   QStringLiteral("high422|yuv422p|8"),
					   QStringLiteral("high422|yuv422p10le|10"),
					   QStringLiteral("high444|yuv444p|8"),
					   QStringLiteral("high444|yuv444p10le|10")};
			else if (parts[0] == QStringLiteral("libsvtav1"))
				formats = {QStringLiteral("main|yuv420p|8"), QStringLiteral("main|yuv420p10le|10")};
			else if (parts[0] == QStringLiteral("libaom-av1"))
				formats = {QStringLiteral("main|yuv420p|8"),
					   QStringLiteral("main|yuv420p10le|10"),
					   QStringLiteral("main|gray|8"),
					   QStringLiteral("high|yuv444p|8"),
					   QStringLiteral("high|yuv444p10le|10"),
					   QStringLiteral("professional|yuv422p|8"),
					   QStringLiteral("professional|yuv422p10le|10")};
			else if (parts[1] == QStringLiteral("hevc"))
				formats = {QStringLiteral("main|yuv420p|8"), QStringLiteral("main10|yuv420p10le|10")};
			else
				formats = {QStringLiteral("profile0|yuv420p|8"),
					   QStringLiteral("profile2|yuv420p10le|10")};
			// format связывает точную пару пикселей с глубиной, values содержит её технические поля.
			for (const auto &format : formats) {
				const auto values = format.split(QLatin1Char('|'));
				// audio задаёт каждую поддержанную привязку, в том числе FLAC с обязательным transcode.
				for (const auto &audio :
				     {QStringLiteral("aac"), QStringLiteral("opus"), QStringLiteral("flac"),
				      QStringLiteral("mp3"), QStringLiteral("ac3")})
					QTest::newRow(qPrintable(tuple + QLatin1Char('|') + control + QLatin1Char('|') +
								 format + QLatin1Char('|') + audio))
						<< parts[0] << parts[1] << parts[2] << control << audio << values[0]
						<< values[1] << values[2].toInt();
			}
		}
	}
}

// Проверяет полный запрос, а затем нарушения привязки кодировщика, аудиоформата и неприменимый VBV.
void ConfigurationApiTest::completeBindingMatrix()
{
	// encoder/codec/mode/control/audio задают обработку дорожек; videoProfile/pixel/depth — точную видеопривязку.
	QFETCH(QString, encoder);
	QFETCH(QString, codec);
	QFETCH(QString, mode);
	QFETCH(QString, control);
	QFETCH(QString, audio);
	QFETCH(QString, videoProfile);
	QFETCH(QString, pixel);
	QFETCH(int, depth);
	// source/profile дают полноценный исходный DTO, поля ниже полностью задают выбранную привязку.
	const auto source = decode<GroupProfile>(fixture::bytes(fixture::groupProfile()));
	QVERIFY(source);
	auto profile = *source.value;
	profile.mode = mode;
	profile.liveAudioMode.value = audio == QStringLiteral("flac") ? QStringLiteral("transcode")
								      : QStringLiteral("copy");
	profile.format.video.codec = codec;
	profile.format.video.codecProfile.bytes = videoProfile.toUtf8();
	profile.format.video.pixelFormat.bytes = pixel.toUtf8();
	profile.format.video.bitDepth = quint8(depth);
	profile.format.video.level.bytes = "auto";
	profile.format.video.tier.bytes.clear();
	profile.videoEncoding.maxBFrames = 0;
	profile.encoding->videoEncoder = encoder;
	profile.encoding->videoCustom.clear();
	profile.encoding->audioCustom.clear();
	profile.encoding->rateControl = control;
	profile.encoding->rescaleOutput = false;
	profile.encoding->nvenc.state = FieldState::Missing;
	profile.encoding->svtAv1.state = FieldState::Missing;
	if (encoder == QStringLiteral("h264_nvenc"))
		profile.encoding->nvenc = {FieldState::Value,
					   {QStringLiteral("p4"), QStringLiteral("hq"), QStringLiteral("disabled"), 0,
					    false, false, 8}};
	if (encoder == QStringLiteral("libsvtav1"))
		profile.encoding->svtAv1 = {FieldState::Value,
					    {8, QStringLiteral("psnr"), control == QStringLiteral("cqp") ? 0 : 2,
					     control == QStringLiteral("cbr") ? 0 : -1}};
	profile.videoEncoding.targetBitrateBps = 6000000;
	profile.videoEncoding.maxBitrateBps = control == QStringLiteral("cbr") ? 6000000 : 8000000;
	profile.videoEncoding.bufferBits = 12000000;
	if (control == QStringLiteral("vbr") && (codec == QStringLiteral("av1") || codec == QStringLiteral("vp9"))) {
		profile.videoEncoding.maxBitrateBps = 0;
		profile.videoEncoding.bufferBits = 0;
	}
	if (control == QStringLiteral("cqp") || control == QStringLiteral("crf")) {
		profile.videoEncoding.targetBitrateBps = 0;
		profile.videoEncoding.maxBitrateBps = 0;
		profile.videoEncoding.bufferBits = 0;
		if (control == QStringLiteral("cqp"))
			profile.encoding->qp = {FieldState::Value, 24};
		else
			profile.encoding->crfHundredths = {
				FieldState::Value,
				encoder == QStringLiteral("libx264") || encoder == QStringLiteral("libx265") ? 2225
													     : 2200};
	}
	profile.format.audio.codec = audio;
	profile.format.audio.codecProfile.bytes = audio == QStringLiteral("aac")    ? "LC"
						  : audio == QStringLiteral("opus") ? "celt"
						  : audio == QStringLiteral("flac") ? "s16"
						  : audio == QStringLiteral("mp3")  ? "mpeg1-layer3"
										    : "ac3";
	profile.encoding->audioEncoder = audio == QStringLiteral("opus")  ? QStringLiteral("libopus")
					 : audio == QStringLiteral("mp3") ? QStringLiteral("libmp3lame")
									  : audio;
	profile.audioEncoding.bitrateBps = audio == QStringLiteral("flac")                                    ? 0
					   : audio == QStringLiteral("aac") || audio == QStringLiteral("ac3") ? 192000
													      : 128000;
	// expected отклоняет FLAC при копировании видео; error содержит локальный отказ.
	const bool expected = audio != QStringLiteral("flac") || mode == QStringLiteral("transcode");
	Error error;
	QCOMPARE(validateProfile(profile, error), expected);
	QCOMPARE(bool(encodeProfile(profile)), expected);
	if (!expected)
		return;
	// hdr проверяет обе передаточные характеристики HDR с цветами BT.2020 без изменения 10-битной привязки.
	if (depth == 10) {
		auto hdr = profile;
		hdr.format.video.color.primaries.bytes = "bt2020";
		hdr.format.video.color.matrix.bytes = "bt2020nc";
		hdr.format.video.color.transfer.bytes = "smpte2084";
		QVERIFY(validateProfile(hdr, error));
		hdr.format.video.color.transfer.bytes = "arib-std-b67";
		QVERIFY(validateProfile(hdr, error));
		hdr.format.video.color.matrix.bytes = "bt709";
		QVERIFY(!validateProfile(hdr, error));
	}
	// changed получает несовместимую аудиопривязку без замены её остальных параметров.
	auto changed = profile;
	changed.format.audio.channels = 1;
	QVERIFY(!validateProfile(changed, error));
	changed = profile;
	changed.encoding->audioEncoder = QStringLiteral("foreign_encoder");
	QVERIFY(!validateProfile(changed, error));
	changed = profile;
	changed.format.video.bitDepth = depth == 8 ? 10 : 8;
	QVERIFY(!validateProfile(changed, error));
	changed = profile;
	if (control == QStringLiteral("cqp") || control == QStringLiteral("crf")) {
		changed.videoEncoding.targetBitrateBps = 1;
		QVERIFY(!validateProfile(changed, error));
		changed = profile;
		changed.encoding->qp.state = FieldState::Missing;
		changed.encoding->crfHundredths.state = FieldState::Missing;
		QVERIFY(!validateProfile(changed, error));
	} else {
		changed.encoding->qp = {FieldState::Value, 0};
		QVERIFY(!validateProfile(changed, error));
	}
}

// Пределы следуют таблице highAV1Level и требованиям AV1TileLayout в закреплённом av1_levels.go.
void ConfigurationApiTest::av1HighTierBoundaries_data()
{
	QTest::addColumn<QString>("level");
	QTest::addColumn<quint32>("width");
	QTest::addColumn<quint32>("height");
	QTest::addColumn<quint32>("fps");
	QTest::addColumn<quint64>("buffer");
	QTest::addColumn<bool>("accepted");
	// 4096×2176 после округления к 128 при 120 FPS превышает предел одного тайла 588251136,
	// но две строки тайлов укладываются; отбрасывание всего кадра вместо поиска сетки ошибочно.
	QTest::newRow("two-tile-grid")
		<< QStringLiteral("5.2") << quint32(4096) << quint32(2160) << quint32(120) << quint64(6000000) << true;
	// 2048×1152×30 равно пределу 70778880 отображаемых пикселей в секунду уровня 4.0; 31 FPS превышает его.
	QTest::newRow("display-exact")
		<< QStringLiteral("4.0") << quint32(2048) << quint32(1152) << quint32(30) << quint64(6000000) << true;
	QTest::newRow("display-over")
		<< QStringLiteral("4.0") << quint32(2048) << quint32(1152) << quint32(31) << quint64(6000000) << false;
	// 1001 мс допустимы для общего CBR, но SVT с tier=high разрешает не более секунды.
	QTest::newRow("high-buffer-over")
		<< QStringLiteral("5.2") << quint32(4096) << quint32(2160) << quint32(120) << quint64(6006000) << false;
}

// Пригодность определяется полным рецептом, а не наличием кодировщика либо одной геометрией.
void ConfigurationApiTest::av1HighTierBoundaries()
{
	// level выбирает таблицу; width/height/fps задают формат; buffer — точный VBV; accepted — независимый итог.
	QFETCH(QString, level);
	QFETCH(quint32, width);
	QFETCH(quint32, height);
	QFETCH(quint32, fps);
	QFETCH(quint64, buffer);
	QFETCH(bool, accepted);
	// decoded даёт полный рецепт с AAC; profile заменяет только необходимые поля видео и кодировщика.
	const auto decoded = decode<GroupProfile>(fixture::bytes(fixture::groupProfile()));
	QVERIFY(decoded);
	auto profile = *decoded.value;
	profile.mode = QStringLiteral("copy");
	profile.format.video.codec = QStringLiteral("av1");
	profile.format.video.codecProfile.bytes = "main";
	profile.format.video.width = width;
	profile.format.video.height = height;
	profile.format.video.frameRate = {fps, 1};
	profile.format.video.level.bytes = level.toUtf8();
	profile.format.video.tier.bytes = "high";
	profile.videoEncoding.targetBitrateBps = 6000000;
	profile.videoEncoding.maxBitrateBps = 6000000;
	profile.videoEncoding.bufferBits = buffer;
	profile.videoEncoding.maxBFrames = 0;
	profile.encoding->videoEncoder = QStringLiteral("libsvtav1");
	profile.encoding->rateControl = QStringLiteral("cbr");
	profile.encoding->videoCustom.clear();
	profile.encoding->nvenc.state = FieldState::Missing;
	profile.encoding->svtAv1 = {FieldState::Value, {8, QStringLiteral("psnr"), 2, 0}};
	// error принимает локальный отказ; оба публичных пути должны соблюдать один нормативный результат.
	Error error;
	QCOMPARE(validateProfile(profile, error), accepted);
	QCOMPARE(bool(encodeProfile(profile)), accepted);
}

// Проверяет независимые ожидания для полного рецепта и совместимости измерения по format_compatibility.go.
void ConfigurationApiTest::recipeAndMeasuredCompatibility()
{
	// decoded/desired задают допустимый полный H.264; measured — отдельное измерение.
	const auto decoded = decode<GroupProfile>(fixture::bytes(fixture::groupProfile()));
	QVERIFY(decoded);
	auto desired = *decoded.value;
	desired.format.video.level.bytes = "auto";
	auto measured = desired.format;
	measured.video.frameRate = {120000, 2002};
	measured.video.level.bytes = "6.2";
	measured.video.tier.bytes = "high";
	measured.video.color.range.bytes = "limited";
	QVERIFY(compatibleMeasuredFormat(measured, desired));
	// equivalent отличается только математически равным FPS и исходным документом.
	auto equivalent = desired;
	equivalent.format.video.frameRate = {120000, 2002};
	equivalent.originalDocument = JsonValue{};
	QVERIFY(sameProfile(desired, equivalent));
	equivalent.videoEncoding.targetBitrateBps += 1;
	QVERIFY(!sameProfile(desired, equivalent));
	equivalent = desired;
	equivalent.encoding->audioCustom = QStringLiteral("aac_coder=fast");
	QVERIFY(!sameProfile(desired, equivalent));
	// Синоним transfer допускается только для этого поля; UNKNOWN не заменяет произвольное значение.
	desired.format.video.color.transfer.bytes = "gamma22";
	measured.video.color.transfer.bytes = "bt470m";
	QVERIFY(compatibleMeasuredFormat(measured, desired));
	measured.video.color.primaries.bytes = "bt470m";
	QVERIFY(!compatibleMeasuredFormat(measured, desired));
	measured = desired.format;
	measured.video.codecProfile.bytes = "UNKNOWN";
	QVERIFY(!compatibleMeasuredFormat(measured, desired));
	measured = desired.format;
	measured.audio.codecProfile.bytes = "HE-AAC";
	QVERIFY(!compatibleMeasuredFormat(measured, desired));
	equivalent = desired;
	equivalent.format.video.codecProfile.bytes = "UNKNOWN";
	QVERIFY(!sameProfile(equivalent, equivalent));
}

// Прямые байтовые токены проверяют CRF и I64 без промежуточного double.
void ConfigurationApiTest::exactQualityAndArithmeticBoundaries()
{
	// encoding задаёт независимый исходный документ; exact сохраняет ровно 22.25 без округления качества.
	const QByteArray encoding =
		R"({"video_encoder":"libx265","audio_encoder":"aac","rate_control":"crf","rescale_output":false,"video_custom":"","audio_custom":"","crf":2225e-2})";
	const auto exact = decode<Encoding>(encoding);
	QVERIFY(exact);
	QCOMPARE(exact.value->crfHundredths.value, 2225);
	QVERIFY(!decode<Encoding>(QByteArray(encoding).replace("2225e-2", "22.2501")));
	QVERIFY(!decode<Encoding>(QByteArray(encoding).replace("2225e-2", "null")));
	// source/profile задают допустимый H.264 и большую несокращённую дробь ровно 60 FPS.
	const auto source = decode<GroupProfile>(fixture::bytes(fixture::groupProfile()));
	QVERIFY(source);
	auto profile = *source.value;
	profile.format.video.frameRate = {4294967280U, 71582788U};
	// error принимает локальную причину несовместимости при последующих изменениях рецепта.
	Error error;
	QVERIFY(validateProfile(profile, error));
	// Уровень 6.0 профиля High допускает ровно 300000000 бит/с; превышение на один бит/с недопустимо.
	profile.format.video.frameRate = {60, 1};
	profile.format.video.width = 4096;
	profile.format.video.height = 2160;
	profile.format.video.level.bytes = "6.0";
	profile.videoEncoding.maxBFrames = 0;
	profile.videoEncoding.targetBitrateBps = 300000000;
	profile.videoEncoding.maxBitrateBps = 300000000;
	profile.videoEncoding.bufferBits = 300000000;
	QVERIFY(validateProfile(profile, error));
	profile.videoEncoding.maxBitrateBps += 1;
	QVERIFY(!validateProfile(profile, error));
	// quality задаёт применимый NVENC CQP0 с явными нулевыми битрейтами; ноль не является Missing.
	auto quality = *source.value;
	quality.format.video.level.bytes = "auto";
	quality.videoEncoding = {0, 0, 0, 120, 0, true};
	quality.encoding->rateControl = QStringLiteral("cqp");
	quality.encoding->videoCustom.clear();
	quality.encoding->qp = {FieldState::Value, 0};
	quality.encoding->nvenc = {FieldState::Value,
				   {QStringLiteral("p4"), QStringLiteral("hq"), QStringLiteral("disabled"), 0, false,
				    false, 8}};
	QVERIFY(validateProfile(quality, error));
	// changedQuality отличается только QP; измеряемый формат одинаков, но полный запрошенный рецепт другой.
	auto changedQuality = quality;
	changedQuality.encoding->qp.value = 1;
	QVERIFY(validateProfile(changedQuality, error));
	QVERIFY(!sameProfile(quality, changedQuality));
	QVERIFY(compatibleMeasuredFormat(quality.format, changedQuality));
	changedQuality.encoding->qp.state = FieldState::Missing;
	QVERIFY(!validateProfile(changedQuality, error));
	// fractional задаёт x265 CRF 22.25 с теми же точными сотыми на входе и в исходящем JSON.
	auto fractional = quality;
	fractional.format.video.codec = QStringLiteral("hevc");
	fractional.format.video.codecProfile.bytes = "main";
	fractional.encoding = {FieldState::Value, *exact.value};
	QVERIFY(validateProfile(fractional, error));
	// encoded содержит числовой CRF в полном исходящем документе.
	const auto encoded = encodeProfile(fractional);
	QVERIFY(encoded);
	QVERIFY(serializeJson(*encoded.value).contains("\"crf\":22.25"));
	// Неверный черновик не лишает каталога применимых путей исправления.
	fractional.format.audio.channels = 1;
	QVERIFY(!validateProfile(fractional, error));
	// options обязан сохранить обе применимые привязки независимо от ошибки другой дорожки.
	const auto options = profileOptions(fractional, QStringLiteral("encoding.video_encoder"));
	QCOMPARE(options.size(), 2);
	QVERIFY(options.contains(QStringLiteral("libx265")));
	QVERIFY(options.contains(QStringLiteral("hevc_nvenc")));
	// progress сохраняет отрицательное медиавремя I64 в микросекундах.
	const auto progress = decode<FFmpegProgress>(
		R"({"frame":null,"fps":null,"bitrate_bps":null,"total_size_bytes":null,"out_time_us":-9007199254740993,"dup_frames":null,"drop_frames":null,"speed":null,"end":false})");
	QVERIFY(progress);
	QCOMPARE(progress.value->outTimeUs.value, qint64(-9007199254740993LL));
}
