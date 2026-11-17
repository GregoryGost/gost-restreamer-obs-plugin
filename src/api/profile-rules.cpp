#include "profile-rules.hpp"
#include "profile-arithmetic.hpp"

#include <QSet>
#include <algorithm>
#include <initializer_list>
#include <limits>
#include <numeric>
#include <tuple>

namespace gost::api {
namespace {

// Преобразует точные UTF-8 байты поля в текст; декодер профиля отдельно гарантирует корректность UTF-8.
QString text(const ProfileText &value)
{
	return QString::fromUtf8(value.bytes);
}

// Создаёт каталог из перечисленных wire-значений без добавления текущего значения черновика.
QStringList values(std::initializer_list<const char *> entries)
{
	// result накапливает Unicode-представления ASCII-значений контракта.
	QStringList result;
	// entry содержит очередное точное техническое значение.
	for (const auto *entry : entries)
		result.append(QString::fromLatin1(entry));
	return result;
}

// Возвращает конечный целочисленный каталог от first до last включительно.
QStringList integers(int first, int last)
{
	// result содержит десятичные значения для типизированных селекторов.
	QStringList result;
	// number обозначает очередное разрешённое целое.
	for (int number = first; number <= last; ++number)
		result.append(QString::number(number));
	return result;
}

// Возвращает выбранный encoding либо нулевую проекцию для исправления неполного черновика.
const Encoding &recipe(const GroupProfile &profile)
{
	// empty предоставляет неизменяемую пустую проекцию без материализации серверных defaults.
	static const Encoding empty{};
	return profile.encoding.hasValue() ? *profile.encoding : empty;
}

// Возвращает точные pixel_format выбранного encoder/profile без проверки других полей черновика.
QStringList pixels(const GroupProfile &profile)
{
	// encoder выбирает конкретную матрицу, codecProfile — её строку.
	const auto &encoder = recipe(profile).videoEncoder;
	const QString codecProfile = text(profile.format.video.codecProfile);
	if (encoder == "libsvtav1" && codecProfile == "main")
		return values({"yuv420p", "yuv420p10le"});
	if (encoder == "libaom-av1") {
		if (codecProfile == "main")
			return values({"yuv420p", "yuv420p10le", "gray"});
		if (codecProfile == "high")
			return values({"yuv444p", "yuv444p10le"});
		if (codecProfile == "professional")
			return values({"yuv422p", "yuv422p10le"});
	}
	if (encoder == "libx264") {
		if (values({"constrained-baseline", "main", "high"}).contains(codecProfile))
			return values({"yuv420p"});
		if (codecProfile == "high10")
			return values({"yuv420p10le"});
		if (codecProfile == "high422")
			return values({"yuv422p", "yuv422p10le"});
		if (codecProfile == "high444")
			return values({"yuv444p", "yuv444p10le"});
	}
	if (encoder == "h264_nvenc") {
		if (values({"baseline", "main", "high"}).contains(codecProfile))
			return values({"yuv420p"});
		if (codecProfile == "high444")
			return values({"yuv444p"});
	}
	if (encoder == "libx265" || encoder == "hevc_nvenc") {
		if (codecProfile == "main")
			return values({"yuv420p"});
		if (codecProfile == "main10")
			return values({"yuv420p10le"});
	}
	if (encoder == "libvpx-vp9") {
		if (codecProfile == "profile0")
			return values({"yuv420p"});
		if (codecProfile == "profile2")
			return values({"yuv420p10le"});
	}
	return {};
}

// Возвращает число каналов точной желаемой AAC-раскладки; ноль означает неизвестную раскладку.
int layoutChannels(const QString &layout)
{
	if (layout == "mono")
		return 1;
	if (layout == "stereo")
		return 2;
	if (layout == "2.1" || layout == "3.0")
		return 3;
	if (layout == "4.0")
		return 4;
	if (layout == "5.0")
		return 5;
	if (layout == "5.1")
		return 6;
	if (layout == "7.1")
		return 8;
	return 0;
}

// Заполняет error локальным доверенным отказом, не включая пользовательские значения в текст.
bool invalid(Error &error, const QString &field, const char *key = "Validation.ProfileValue",
	     const char *code = "invalid_profile")
{
	error = {};
	error.kind = ErrorKind::Contract;
	error.message = QString::fromLatin1(key);
	// problem содержит точный публичный путь и локальный ключ пояснения для UI.
	Problem problem;
	problem.code = QString::fromLatin1(code);
	problem.field = "profile." + field;
	problem.errors.append({problem.field, {}, QString::fromLatin1(key)});
	error.problem = std::move(problem);
	return false;
}

// Проверяет структурную полноту измеренного формата; каталоги desired к нему не применяются.
bool structuralFormat(const Format &format)
{
	// v и a обозначают независимые дорожки измерения.
	const auto &v = format.video;
	const auto &a = format.audio;
	return values({"av1", "h264", "hevc", "vp9"}).contains(v.codec) && v.width && v.height &&
	       v.frameRate.numerator && v.frameRate.denominator && !text(v.codecProfile).trimmed().isEmpty() &&
	       !text(v.pixelFormat).trimmed().isEmpty() && v.bitDepth && !text(v.color.range).trimmed().isEmpty() &&
	       !text(v.color.primaries).trimmed().isEmpty() && !text(v.color.transfer).trimmed().isEmpty() &&
	       !text(v.color.matrix).trimmed().isEmpty() &&
	       values({"aac", "opus", "flac", "mp3", "ac3"}).contains(a.codec) &&
	       !text(a.codecProfile).trimmed().isEmpty() && a.sampleRateHz && a.channels &&
	       !text(a.channelLayout).trimmed().isEmpty();
}

// Распознаёт конфликтующие имена типизированных параметров в закрытом серверном каталоге.
bool ownedCustom(const QString &name);

// Проверяет нормализованную строку закрытого custom-каталога выбранного encoder.
// Разрешены только точные канонические пары; CLI, неизвестные параметры и дубликаты отклоняются.
bool custom(const QString &encoder, const QString &input, const QString &field, Error &error)
{
	if (input.toUtf8().size() > 4096)
		return invalid(error, field, "Validation.ProfileCustom", "invalid_custom");
	if (encoder != "h264_nvenc" && encoder != "libsvtav1" && encoder != "aac")
		return input.isEmpty() || invalid(error, field, "Validation.ProfileCustom", "invalid_custom");
	// character проверяет ASCII-алфавит без управляющих символов и экранирования.
	for (const QChar character : input) {
		if (character.unicode() < 32 || character.unicode() >= 127 || character == '"' || character == '\'' ||
		    character == '\\')
			return invalid(error, field, "Validation.ProfileCustom", "invalid_custom");
	}
	// normalized накапливает пары для канонического порядка; seen обнаруживает повтор одного имени.
	QStringList normalized;
	QSet<QString> seen;
	// pair обозначает очередную непустую пару name=value.
	for (const QString &pair : input.split(' ', Qt::SkipEmptyParts)) {
		// separator задаёт единственный знак равенства; name и raw сохраняют его стороны.
		const qsizetype separator = pair.indexOf('=');
		const QString name = pair.left(separator);
		const QString raw = pair.mid(separator + 1);
		if (separator <= 0 || name.size() > 64 || raw.isEmpty() || raw.size() > 128 || raw.contains('=') ||
		    normalized.size() == 32)
			return invalid(error, field, "Validation.ProfileCustom", "invalid_custom");
		// character обозначает очередной ASCII-символ имени разрешённой пары.
		for (const QChar character : name) {
			if (!(character >= 'a' && character <= 'z') && !(character >= 'A' && character <= 'Z') &&
			    !(character >= '0' && character <= '9') && !QStringLiteral("_-:.").contains(character))
				return invalid(error, field, "Validation.ProfileCustom", "invalid_custom");
		}
		if (seen.contains(name))
			return invalid(error, field, "Validation.ProfileCustom", "duplicate_custom");
		seen.insert(name);
		if (ownedCustom(name))
			return invalid(error, field, "Validation.ProfileCustom", "custom_conflict");
		if ((encoder == "h264_nvenc" && (name == "nonref_p" || name == "strict_gop")) ||
		    (encoder == "libsvtav1" && name == "lp")) {
			// ok подтверждает точный целочисленный разбор, number задаёт значение конечного параметра.
			bool ok = false;
			const qint64 number = raw.toLongLong(&ok);
			if (!ok || number < 0 || number > (name == "lp" ? 6 : 1))
				return invalid(error, field, "Validation.ProfileCustom", "invalid_custom");
			normalized.append(name + '=' + QString::number(number));
		} else if (!(encoder == "aac" && name == "aac_coder" && values({"twoloop", "fast"}).contains(raw))) {
			return invalid(error, field, "Validation.ProfileCustom", "unknown_custom");
		} else
			normalized.append(pair);
	}
	std::sort(normalized.begin(), normalized.end());
	return normalized.join(' ') == input || invalid(error, field, "Validation.ProfileCustom");
}

// Проверяет аудиопривязку profile, включая предельный AAC-битрейт и фиксированные остальные рецепты.
bool audio(const GroupProfile &profile, Error &error)
{
	// a задаёт желаемую дорожку, e — явный рецепт, bitrate — целевой битрейт аудио.
	const auto &a = profile.format.audio;
	const auto &e = recipe(profile);
	const quint64 bitrate = profile.audioEncoding.bitrateBps;
	if (a.codec == "aac") {
		if (!bitrate || bitrate > 6 * quint64(a.sampleRateHz) * a.channels)
			return invalid(error, "audio_encoding.bitrate_bps", "Validation.ProfileAudio");
	} else {
		if ((a.codec == "flac" && (profile.mode != "transcode" || *profile.liveAudioMode != "transcode")) ||
		    bitrate != (a.codec == "ac3"    ? 192000u
				: a.codec == "flac" ? 0u
						    : 128000u))
			return invalid(error,
				       a.codec == "flac" && profile.mode != "transcode" ? "live_audio_mode"
											: "audio_encoding.bitrate_bps",
				       "Validation.ProfileAudio");
	}
	return custom(e.audioEncoder, e.audioCustom, "encoding.audio_custom", error);
}

// Проверяет присутствие и точную шкалу качества выбранного encoder без скрытого VBV.
bool quality(const GroupProfile &profile, Error &error)
{
	// e содержит качество и RC, ve — битрейты и буфер; av1 отличает шкалу 1/0..63 от 0..51.
	const auto &e = recipe(profile);
	const auto &ve = profile.videoEncoding;
	const bool av1 = e.videoEncoder == "libsvtav1" || e.videoEncoder == "libaom-av1" ||
			 e.videoEncoder == "libvpx-vp9";
	// minimum и maximum задают границы качества выбранной реализации.
	const int minimum = e.videoEncoder == "libsvtav1" ? 1 : 0;
	const int maximum = av1 ? 63 : 51;
	if (e.qp.isNull() || e.crfHundredths.isNull())
		return invalid(error, "encoding", "Validation.ProfileQuality");
	if (e.rateControl == "cbr" || e.rateControl == "vbr")
		return (!e.qp.hasValue() && !e.crfHundredths.hasValue()) ||
		       invalid(error, "encoding", "Validation.ProfileQuality");
	if (e.rateControl == "cqp") {
		if (!e.qp.hasValue() || e.crfHundredths.hasValue() || *e.qp < minimum || *e.qp > maximum)
			return invalid(error, "encoding.qp", "Validation.ProfileQuality");
	} else if (e.rateControl == "crf") {
		if (!e.crfHundredths.hasValue() || e.qp.hasValue() || *e.crfHundredths < minimum * 100 ||
		    *e.crfHundredths > maximum * 100 ||
		    (e.videoEncoder != "libx264" && e.videoEncoder != "libx265" && *e.crfHundredths % 100 != 0))
			return invalid(error, "encoding.crf", "Validation.ProfileQuality");
	} else
		return invalid(error, "encoding.rate_control");
	if (ve.targetBitrateBps || ve.maxBitrateBps || ve.bufferBits)
		return invalid(error, "video_encoding", "Validation.ProfileQuality");
	if (e.videoEncoder == "libx264" &&
	    ((e.qp.hasValue() && *e.qp == 0) || (e.crfHundredths.hasValue() && *e.crfHundredths == 0)) &&
	    text(profile.format.video.codecProfile) != "high444")
		return invalid(error, "format.video.codec_profile", "Validation.ProfileQuality");
	return true;
}

// Проверяет H.264 6.x по макроблокам, частоте и VCL-бюджетам выбранного профиля.
bool h264Level(const GroupProfile &profile, Error &error)
{
	// v и ve содержат точный формат и настройки битрейта; level выбирает строку нормативной таблицы.
	const auto &v = profile.format.video;
	const auto &ve = profile.videoEncoding;
	const QString level = text(v.level);
	if (!values({"6.0", "6.1", "6.2"}).contains(level))
		return true;
	// mbps и rate задают предел макроблоков/с и базовый VCL-бюджет.
	const quint64 mbps = level == "6.0" ? 4177920 : level == "6.1" ? 8355840 : 16711680;
	const quint64 rate = level == "6.0" ? 240000 : level == "6.1" ? 480000 : 800000;
	// width и height округляют стороны к макроблокам; size содержит макроблоки кадра.
	const quint64 width = (quint64(v.width) + 15) / 16;
	const quint64 height = (quint64(v.height) + 15) / 16;
	const quint64 size = width * height;
	if (ve.maxBFrames || !values({"cbr", "vbr"}).contains(recipe(profile).rateControl) || size > 139264 ||
	    width * width > 8 * 139264 || height * height > 8 * 139264 ||
	    !profile_math::productLessEqual(size, v.frameRate.numerator, mbps, v.frameRate.denominator))
		return invalid(error, "format.video.level", "Validation.ProfileLevel");
	// codecProfile выбирает строку профиля; factor переводит её VCL-бюджет из таблицы в биты.
	const QString codecProfile = text(v.codecProfile);
	const quint64 factor = codecProfile == "high"                                   ? 1250
			       : codecProfile == "high10"                               ? 3000
			       : codecProfile == "high422" || codecProfile == "high444" ? 4000
											: 1000;
	return (ve.maxBitrateBps <= rate * factor && ve.bufferBits <= rate * factor) ||
	       invalid(error, "video_encoding", "Validation.ProfileLevel");
}

// Содержит строку таблицы AV1 Annex A для выбранного SVT high-рецепта.
struct HighLimit {
	// level обозначает точный уровень; picture, width и height ограничивают геометрию яркости.
	const char *level;
	quint64 picture, width, height;
	// display и decode ограничивают отсчёты/с; bitrate задаёт биты/с; tiles и columns ограничивают сетку.
	quint64 display, decode, bitrate, tiles, columns;
};

// Возвращает строку точного high-уровня либо nullptr, если категория для него не поддержана.
const HighLimit *highLimit(const QString &level)
{
	// limits содержит независимо закреплённые нормативные границы AV1 high.
	static const HighLimit limits[]{
		{"4.0", 2359296, 6144, 3456, 70778880, 77856768, 30000000, 32, 8},
		{"4.1", 2359296, 6144, 3456, 141557760, 155713536, 50000000, 32, 8},
		{"5.0", 8912896, 8192, 4352, 267386880, 273715200, 100000000, 64, 8},
		{"5.1", 8912896, 8192, 4352, 534773760, 547430400, 160000000, 64, 8},
		{"5.2", 8912896, 8192, 4352, 1069547520, 1094860800, 240000000, 64, 8},
		{"5.3", 8912896, 8192, 4352, 1069547520, 1176502272, 240000000, 64, 8},
		{"6.0", 35651584, 16384, 8704, 1069547520, 1176502272, 240000000, 128, 16},
		{"6.1", 35651584, 16384, 8704, 2139095040, 2189721600, 480000000, 128, 16},
		{"6.2", 35651584, 16384, 8704, 4278190080ULL, 4379443200ULL, 800000000, 128, 16},
		{"6.3", 35651584, 16384, 8704, 4278190080ULL, 4706009088ULL, 800000000, 128, 16}};
	// entry обозначает очередной уровень, сопоставляемый буквально.
	for (const auto &entry : limits)
		if (level == QLatin1String(entry.level))
			return &entry;
	return nullptr;
}

// Распознаёт публичные и FFmpeg-псевдонимы параметров, принадлежащих типизированному рецепту.
bool ownedCustom(const QString &name)
{
	// key удаляет нечувствительные к сравнению регистр и разделители алиасов.
	QString key = name.toLower();
	key.remove('-');
	key.remove('_');
	key.remove(':');
	key.remove('.');
	// names содержит закрытый серверный перечень конфликтующих имён без произвольных CLI-параметров.
	static const QStringList names =
		QString::fromLatin1(
			"version mode encoding videoencoder audioencoder encoder codec c cv ca vcodec acodec "
			"rc ratecontrol rcmode ratemode bitrate averagebitrate targetbitrate targetbitratebps b bv ba vb ab tbr mbr maxbitrate maxbitratebps maxrate maxratev maxratea minrate minratev bufsize bufsizev buffer bufferbits vbvbufsize vbvbuffersize vbvmaxrate vbvinit bufsz bufinitialsz bufoptimalsz crf cq qp q qscale qv qa "
			"g gv gop gopsize gopframes keyint keyintmin keyintmax intra intraperiod intraperiodlength keyframeinterval keyintsec bf bfv bframes maxbframes closedgop opengop cgop flags flagsv flags2 flags2v "
			"profile profilev profilea codecprofile level levelv tier tierv pixfmt pixfmtv pixelformat bitdepth inputdepth width height s sv size r rv fps framerate fpsnum fpsdenom fpnum fpden timebase enctimebase enctimebasev fpsmode fpsmodev rescaleoutput "
			"colorrange colorrangev colorprimaries colorprimariesv colortrc colortrcv colorspace colorspacev primaries transfer matrix range colorformat fullrange "
			"ar ara samplerate sampleratehz ac aca channels channellayout channellayouta audiobitrate samplefmt cutoff "
			"preset presetv encmode tune tuning multipass lookahead enablelookahead lookaheadframes rclookahead lad laginframes aq enableaq aqmode aqstrength spatialaq temporalaq enablespatialaq enabletemporalaq adaptivequantization adaptquant "
			"predstruct predictionstructure hierarchicallevels hierarchy overlays enableoverlays dg enabledg tf enabletf scd scdetection scenecut noscenecut forcedidr forcekeyframes surfaces delay zerolatency brefmode bref badapt iadapt rcinitoccupancy "
			"i input output f format map vf af filter filterv filtera filtercomplex filtercomplexscript filterthreads filtercomplexthreads filterbufferedframes path filename url protocolwhitelist protocolblacklist safe y n stdin nostdin hidebanner loglevel progress stats statperiod statsperiod pass passlogfile vstats vstatsfile statsfile threads threadtype cpuflags gpu hwaccel hwacceloutputformat hwacceldevice inithwdevice filterhwdevice svtav1params svtav1opts x264params x264opts x265params params copyts avoidnegativets movflags videotracktimescale movietimescale useeditlist t to ss itsoffset streamloop loop re readrate shortest an vn sn dn fflags flags2a")
			.split(' ');
	return names.contains(key);
}

// Проверяет существование единой равномерной сетки AV1 для суперблоков 64 и 128 без изменения профиля.
bool tileLayout(const VideoFormat &v, const HighLimit &limit)
{
	// sbColumns и sbRows округляют геометрию к консервативному суперблоку 128.
	const quint64 sbColumns = (quint64(v.width) + 127) / 128;
	const quint64 sbRows = (quint64(v.height) + 127) / 128;
	// total задаёт сумму показателей степени; colLog — показатель столбцов.
	for (int total = 0; total <= 12; ++total) {
		for (int colLog = 0; colLog <= 6; ++colLog) {
			// rowLog задаёт показатель строк того же кандидата сетки.
			const int rowLog = total - colLog;
			if (rowLog < 0 || rowLog > 6 || (colLog > 0 && (quint64(1) << (colLog - 1)) >= sbColumns) ||
			    (rowLog > 0 && (quint64(1) << (rowLog - 1)) >= sbRows))
				continue;
			// columns и rows задают степени сетки; width и height — самый большой округлённый тайл.
			const quint64 columns = quint64(1) << colLog;
			const quint64 rows = quint64(1) << rowLog;
			const quint64 width = (sbColumns + columns - 1) / columns * 128;
			const quint64 height = (sbRows + rows - 1) / rows * 128;
			if (width > 4096 || width * height > 4096 * 2304 ||
			    !profile_math::productLessEqual(width * height, v.frameRate.numerator, 588251136,
							    v.frameRate.denominator) ||
			    columns > limit.columns || columns * rows > limit.tiles ||
			    !profile_math::productLessEqual(columns * rows, v.frameRate.numerator, limit.tiles * 120,
							    v.frameRate.denominator))
				continue;
			// validCrop отмечает достаточный остаток крайних тайлов обоих нативных размеров суперблока.
			bool validCrop = true;
			// blockSize выбирает размер суперблока без смены степеней сетки.
			for (const quint64 blockSize : {64u, 128u}) {
				// blockColumns и blockRows задают число суперблоков; tileWidth и tileHeight — округлённый тайл.
				const quint64 blockColumns = (quint64(v.width) + blockSize - 1) / blockSize;
				const quint64 blockRows = (quint64(v.height) + blockSize - 1) / blockSize;
				const quint64 tileWidth = (blockColumns + columns - 1) / columns * blockSize;
				const quint64 tileHeight = (blockRows + rows - 1) / rows * blockSize;
				// actualColumns и actualRows считают непустые тайлы после обрезки к геометрии кадра.
				const quint64 actualColumns = (quint64(v.width) + tileWidth - 1) / tileWidth;
				const quint64 actualRows = (quint64(v.height) + tileHeight - 1) / tileHeight;
				if (v.width - (actualColumns - 1) * tileWidth < 8 ||
				    v.height - (actualRows - 1) * tileHeight < 8)
					validCrop = false;
			}
			if (validCrop)
				return true;
		}
	}
	return false;
}

// Проверяет геометрию, частоты, битрейт и сетку high-уровня SVT выбранного profile.
bool svtHigh(const GroupProfile &profile, Error &error)
{
	// v задаёт видео, ve — битрейт/буфер, limit выбирает нормативный уровень.
	const auto &v = profile.format.video;
	const auto &ve = profile.videoEncoding;
	const auto *limit = highLimit(text(v.level));
	if (!limit || recipe(profile).rateControl != "cbr")
		return invalid(error, "format.video.tier", "Validation.ProfileLevel");
	if (ve.targetBitrateBps > std::min(quint64(100000000), limit->bitrate) || ve.bufferBits > ve.targetBitrateBps)
		return invalid(error, "video_encoding", "Validation.ProfileLevel");
	// picture содержит число отсчётов яркости кадра; произведения с FPS сравниваются в 128 битах.
	const quint64 picture = quint64(v.width) * v.height;
	if (picture > limit->picture || v.width > limit->width || v.height > limit->height ||
	    !profile_math::productLessEqual(picture, v.frameRate.numerator, limit->display, v.frameRate.denominator) ||
	    !profile_math::productLessEqual(picture, v.frameRate.numerator, limit->decode, v.frameRate.denominator) ||
	    !profile_math::productLessEqual(v.frameRate.numerator, 1, 300, v.frameRate.denominator) ||
	    !tileLayout(v, *limit))
		return invalid(error, "format.video", "Validation.ProfileLevel");
	return true;
}

// Проверяет битрейт/GOP и настройки конкретного encoder после общей проверки каталога и качества.
bool video(const GroupProfile &profile, Error &error)
{
	// e задаёт видеорецепт, v — формат, ve — GOP и битрейты; controlled отличает CBR/VBR от чистого качества.
	const auto &e = recipe(profile);
	const auto &v = profile.format.video;
	const auto &ve = profile.videoEncoding;
	const bool controlled = e.rateControl == "cbr" || e.rateControl == "vbr";
	if ((e.videoEncoder != "h264_nvenc" && !e.nvenc.isMissing()) ||
	    (e.videoEncoder != "libsvtav1" && !e.svtAv1.isMissing()))
		return invalid(error, "encoding", "Validation.ProfileEncoder");
	if (!quality(profile, error))
		return false;
	if (v.codec == "h264" || v.codec == "hevc") {
		if (v.codec == "h264") {
			if (ve.maxBFrames > 4 || ve.maxBFrames >= ve.gopFrames ||
			    (values({"baseline", "constrained-baseline"}).contains(text(v.codecProfile)) &&
			     ve.maxBFrames))
				return invalid(error, "video_encoding.max_b_frames", "Validation.ProfileGop");
			if (!controlled && text(v.level) != "" && text(v.level) != "auto")
				return invalid(error, "format.video.level", "Validation.ProfileQuality");
		} else if (ve.maxBFrames)
			return invalid(error, "video_encoding.max_b_frames", "Validation.ProfileGop");
		if (controlled) {
			// Несовпадение CBR относится к max, даже если target одновременно превышает его.
			if (e.rateControl == "cbr" && ve.targetBitrateBps != ve.maxBitrateBps)
				return invalid(error, "video_encoding.max_bitrate_bps",
					       "Validation.CbrBitrateMismatch");
			if (!ve.targetBitrateBps || ve.targetBitrateBps > ve.maxBitrateBps ||
			    ve.maxBitrateBps > std::numeric_limits<quint32>::max() || !ve.bufferBits ||
			    ve.bufferBits > std::numeric_limits<qint32>::max())
				return invalid(error, "video_encoding", "Validation.ProfileBitrate");
			if ((e.videoEncoder == "libx264" || e.videoEncoder == "libx265") &&
			    (ve.targetBitrateBps % 1000 || ve.maxBitrateBps % 1000 || ve.bufferBits % 1000))
				return invalid(error, "video_encoding", "Validation.ProfileBitrate");
		}
		if (e.videoEncoder == "h264_nvenc") {
			if (!e.nvenc.hasValue())
				return invalid(error, "encoding.nvenc", "Validation.ProfileEncoder");
			// n содержит применимый запрос NVENC без предположений об оборудовании.
			const auto &n = *e.nvenc;
			if (e.rateControl == "cqp") {
				if (n.preset != "p4" || !values({"hq", "lossless"}).contains(n.tuning) ||
				    n.multipass != "disabled" || n.lookaheadFrames || n.enableSpatialAq ||
				    n.enableTemporalAq || n.aqStrength != 8 || !e.videoCustom.isEmpty())
					return invalid(error, "encoding.nvenc", "Validation.ProfileEncoder");
				if (n.tuning == "lossless" &&
				    (text(v.codecProfile) != "high444" || text(v.pixelFormat) != "yuv444p" ||
				     *e.qp != 0 || ve.maxBFrames))
					return invalid(error, "encoding.nvenc.tuning", "Validation.ProfileQuality");
			} else if (n.lookaheadFrames < 0 || n.lookaheadFrames > 31 ||
				   (n.lookaheadFrames > 0 && quint64(n.lookaheadFrames) + ve.maxBFrames > 31) ||
				   n.aqStrength < 1 || n.aqStrength > 15 || (!n.enableSpatialAq && n.aqStrength != 8))
				return invalid(error, "encoding.nvenc", "Validation.ProfileEncoder");
		}
		if (v.codec == "h264" && !h264Level(profile, error))
			return false;
	} else {
		if (ve.maxBFrames)
			return invalid(error, "video_encoding.max_b_frames", "Validation.ProfileGop");
		if (e.videoEncoder == "libsvtav1") {
			if (v.width < 64 || v.width > 16384 || v.height < 64 || v.height > 8704 ||
			    quint64(v.frameRate.numerator) > 240 * quint64(v.frameRate.denominator) || ve.gopFrames < 2)
				return invalid(error, "format.video", "Validation.ProfileGop");
			if (!e.svtAv1.hasValue())
				return invalid(error, "encoding.svt_av1", "Validation.ProfileEncoder");
			// s содержит применимые настройки предварительного анализа SVT.
			const auto &s = *e.svtAv1;
			if ((e.rateControl == "cqp" ? s.aqMode != 0 : s.aqMode != 2) ||
			    (e.rateControl == "cbr"
				     ? s.lookaheadFrames != 0
				     : s.lookaheadFrames != -1 && (s.lookaheadFrames < 6 || s.lookaheadFrames > 120)))
				return invalid(error, "encoding.svt_av1", "Validation.ProfileEncoder");
			if (controlled && (!ve.targetBitrateBps || ve.targetBitrateBps > 100000000))
				return invalid(error, "video_encoding.target_bitrate_bps", "Validation.ProfileBitrate");
			if (controlled && e.rateControl == "vbr" && (ve.maxBitrateBps || ve.bufferBits))
				return invalid(error, "video_encoding", "Validation.ProfileBitrate");
			if (e.rateControl == "cbr" &&
			    (ve.maxBitrateBps != ve.targetBitrateBps || !ve.bufferBits ||
			     ve.bufferBits > ve.targetBitrateBps * 10 || ve.bufferBits * 1000 % ve.targetBitrateBps ||
			     ve.bufferBits * 1000 / ve.targetBitrateBps < 20))
				return invalid(error, "video_encoding", "Validation.ProfileBitrate");
			if (text(v.tier) == "high" && !svtHigh(profile, error))
				return false;
		} else if (controlled) {
			// maximum ограничивает target нативного AOM или VP9 до операций с буфером.
			const quint64 maximum = e.videoEncoder == "libvpx-vp9" ? 1000000000 : 100000000;
			if (!ve.targetBitrateBps || ve.targetBitrateBps > maximum || ve.targetBitrateBps % 1000)
				return invalid(error, "video_encoding.target_bitrate_bps", "Validation.ProfileBitrate");
			if (e.rateControl == "vbr") {
				if (ve.maxBitrateBps || ve.bufferBits)
					return invalid(error, "video_encoding", "Validation.ProfileBitrate");
			} else if (ve.maxBitrateBps != ve.targetBitrateBps || !ve.bufferBits ||
				   ve.bufferBits > std::numeric_limits<qint32>::max() ||
				   ve.bufferBits * 1000 % ve.targetBitrateBps ||
				   !(ve.bufferBits * 1000 / ve.targetBitrateBps) ||
				   (e.videoEncoder == "libvpx-vp9" &&
				    ve.bufferBits * 1000 / ve.targetBitrateBps > std::numeric_limits<quint32>::max()))
				return invalid(error, "video_encoding", "Validation.ProfileBitrate");
		}
	}
	return custom(e.videoEncoder, e.videoCustom, "encoding.video_custom", error);
}

// Сравнивает FPS математически; нулевые дроби не являются готовым измерением.
bool equalRate(const FrameRate &a, const FrameRate &b)
{
	return a.numerator && a.denominator && b.numerator && b.denominator &&
	       quint64(a.numerator) * b.denominator == quint64(b.numerator) * a.denominator;
}

// Сравнивает буквальные поля обеих дорожек и математический FPS без нормализации desired-строк.
bool equalFormat(const Format &a, const Format &b)
{
	return std::tie(a.video.codec, a.video.width, a.video.height, a.video.codecProfile, a.video.level, a.video.tier,
			a.video.pixelFormat, a.video.bitDepth, a.video.color.range, a.video.color.primaries,
			a.video.color.transfer, a.video.color.matrix, a.audio.codec, a.audio.codecProfile,
			a.audio.sampleRateHz, a.audio.channels, a.audio.channelLayout) ==
		       std::tie(b.video.codec, b.video.width, b.video.height, b.video.codecProfile, b.video.level,
				b.video.tier, b.video.pixelFormat, b.video.bitDepth, b.video.color.range,
				b.video.color.primaries, b.video.color.transfer, b.video.color.matrix, b.audio.codec,
				b.audio.codecProfile, b.audio.sampleRateHz, b.audio.channels, b.audio.channelLayout) &&
	       equalRate(a.video.frameRate, b.video.frameRate);
}

} // namespace

// Возвращает зависимый конечный каталог fieldPath даже для частично некорректного profile.
QStringList profileOptions(const GroupProfile &profile, const QString &fieldPath)
{
	// e задаёт выбранный рецепт, v и a — его дорожки; rc и encoder определяют зависимые каталоги.
	const auto &e = recipe(profile);
	const auto &v = profile.format.video;
	const auto &a = profile.format.audio;
	const auto &rc = e.rateControl;
	const auto &encoder = e.videoEncoder;
	// hdr выбирает обязательные BT.2020-поля, extended отмечает фиксированный SDR HEVC/VP9.
	const bool hdr = values({"smpte2084", "arib-std-b67"}).contains(text(v.color.transfer));
	const bool extended = v.codec == "hevc" || v.codec == "vp9";
	if (fieldPath == "mode")
		return values({"copy", "transcode"});
	if (fieldPath == "live_audio_mode")
		return profile.mode == "copy" ? values({"copy"})
		       : profile.mode == "transcode"
			       ? (a.codec == "flac" ? values({"transcode"}) : values({"copy", "transcode"}))
			       : QStringList{};
	if (fieldPath == "format.video.codec")
		return profile.mode == "copy"        ? values({"av1", "hevc", "vp9"})
		       : profile.mode == "transcode" ? values({"h264", "hevc", "vp9"})
						     : QStringList{};
	if (fieldPath == "encoding.video_encoder") {
		if (v.codec == "h264" && profile.mode == "transcode")
			return values({"h264_nvenc", "libx264"});
		if (v.codec == "av1" && profile.mode == "copy")
			return values({"libsvtav1", "libaom-av1"});
		if (v.codec == "hevc" && values({"copy", "transcode"}).contains(profile.mode))
			return values({"libx265", "hevc_nvenc"});
		if (v.codec == "vp9" && values({"copy", "transcode"}).contains(profile.mode))
			return values({"libvpx-vp9"});
		return {};
	}
	if (fieldPath == "format.video.codec_profile") {
		if (!profileOptions(profile, "encoding.video_encoder").contains(encoder))
			return {};
		if ((encoder == "h264_nvenc" && rc == "cqp" && e.nvenc.hasValue() && e.nvenc->tuning == "lossless") ||
		    (encoder == "libx264" && ((rc == "cqp" && e.qp.hasValue() && *e.qp == 0) ||
					      (rc == "crf" && e.crfHundredths.hasValue() && *e.crfHundredths == 0))))
			return values({"high444"});
		if (encoder == "h264_nvenc")
			return values({"baseline", "main", "high", "high444"});
		if (encoder == "libx264")
			return values({"constrained-baseline", "main", "high", "high10", "high422", "high444"});
		if (encoder == "libsvtav1")
			return values({"main"});
		if (encoder == "libaom-av1")
			return values({"main", "high", "professional"});
		if (encoder == "libx265" || encoder == "hevc_nvenc")
			return values({"main", "main10"});
		if (encoder == "libvpx-vp9")
			return values({"profile0", "profile2"});
		return {};
	}
	if (fieldPath == "format.video.pixel_format")
		return profileOptions(profile, "format.video.codec_profile").contains(text(v.codecProfile))
			       ? pixels(profile)
			       : QStringList{};
	if (fieldPath == "format.video.bit_depth")
		return pixels(profile).contains(text(v.pixelFormat))
			       ? values({text(v.pixelFormat).endsWith("10le") ? "10" : "8"})
			       : QStringList{};
	if (fieldPath == "encoding.rate_control") {
		if (!profileOptions(profile, "encoding.video_encoder").contains(encoder))
			return {};
		if (encoder == "h264_nvenc" || encoder == "hevc_nvenc")
			return values({"cbr", "vbr", "cqp"});
		if (encoder == "libx264" || encoder == "libx265" || encoder == "libsvtav1")
			return values({"cbr", "vbr", "cqp", "crf"});
		if (encoder == "libaom-av1" || encoder == "libvpx-vp9")
			return values({"cbr", "vbr", "crf"});
		return {};
	}
	if (fieldPath == "format.video.level") {
		if (v.codec == "h264" && (rc == "cbr" || rc == "vbr"))
			return values({"",    "auto", "1",   "1.0", "1b",  "1.0b", "1.1", "1.2", "1.3", "2",
				       "2.0", "2.1",  "2.2", "3",   "3.0", "3.1",  "3.2", "4",   "4.0", "4.1",
				       "4.2", "5",    "5.0", "5.1", "5.2", "6.0",  "6.1", "6.2"});
		if (v.codec == "av1" && encoder == "libsvtav1") {
			if (text(v.tier) == "high")
				return rc == "cbr" ? values({"4.0", "4.1", "5.0", "5.1", "5.2", "5.3", "6.0", "6.1",
							     "6.2", "6.3"})
						   : QStringList{};
			// result содержит специальные значения и 24 нормативных обозначения AV1.
			QStringList result = values({"", "auto"});
			// major и minor задают обе цифры уровня без произвольных промежуточных значений.
			for (int major = 2; major <= 7; ++major)
				for (int minor = 0; minor <= 3; ++minor)
					result.append(QString::number(major) + '.' + QString::number(minor));
			return result;
		}
		return values({"av1", "h264", "hevc", "vp9"}).contains(v.codec) ? values({"", "auto"}) : QStringList{};
	}
	if (fieldPath == "format.video.tier") {
		if (v.codec == "av1")
			return encoder == "libsvtav1" && rc == "cbr" ? values({"", "main", "high"})
								     : values({"", "main"});
		if (v.codec == "hevc")
			return values({"", "main"});
		return v.codec == "h264" || v.codec == "vp9" ? values({""}) : QStringList{};
	}
	if (fieldPath == "format.video.color.range")
		return values({"limited", "full", "tv", "pc"});
	if (fieldPath == "format.video.color.primaries")
		return hdr        ? values({"bt2020"})
		       : extended ? values({"bt709"})
				  : values({"bt709", "bt470m", "bt470bg", "smpte170m", "smpte240m", "film", "bt2020"});
	if (fieldPath == "format.video.color.matrix") {
		if (hdr)
			return values({"bt2020nc"});
		if (extended)
			return values({"bt709"});
		return text(v.pixelFormat) != "gray" && values({"bt709", "smpte170m", "bt2020-10", "bt2020-12"})
								.contains(text(v.color.transfer))
			       ? values({"bt709", "fcc", "bt470bg", "smpte170m", "smpte240m", "bt2020nc", "bt2020c"})
			       : values({"bt709", "fcc", "bt470bg", "smpte170m", "smpte240m", "bt2020nc"});
	}
	if (fieldPath == "format.video.color.transfer") {
		// result перечисляет SDR передаточные функции, затем добавляет HDR только для 10 бит.
		QStringList result = extended ? values({"bt709"})
					      : values({"bt709", "gamma22", "gamma28", "smpte170m", "smpte240m",
							"linear", "iec61966-2-1", "bt2020-10", "bt2020-12"});
		if (v.bitDepth == 10)
			result.append(values({"smpte2084", "arib-std-b67"}));
		return result;
	}
	if (fieldPath == "format.audio.codec")
		return values({"aac", "opus", "flac", "mp3", "ac3"});
	if (fieldPath == "format.audio.codec_profile") {
		if (a.codec == "aac")
			return values({"LC"});
		if (a.codec == "opus")
			return values({"celt"});
		if (a.codec == "flac")
			return values({"s16"});
		if (a.codec == "mp3")
			return values({"mpeg1-layer3"});
		if (a.codec == "ac3")
			return values({"ac3"});
		return {};
	}
	if (fieldPath == "encoding.audio_encoder") {
		if (a.codec == "aac")
			return values({"aac"});
		if (a.codec == "opus")
			return values({"libopus"});
		if (a.codec == "flac")
			return values({"flac"});
		if (a.codec == "mp3")
			return values({"libmp3lame"});
		if (a.codec == "ac3")
			return values({"ac3"});
		return {};
	}
	if (fieldPath == "format.audio.sample_rate_hz")
		return a.codec == "aac" ? values({"7350", "8000", "11025", "12000", "16000", "22050", "24000", "32000",
						  "44100", "48000", "64000"})
		       : values({"opus", "flac", "mp3", "ac3"}).contains(a.codec) ? values({"48000"})
										  : QStringList{};
	if (fieldPath == "format.audio.channels")
		return a.codec == "aac" ? values({"1", "2", "3", "4", "5", "6", "8"})
		       : values({"opus", "flac", "mp3", "ac3"}).contains(a.codec) ? values({"2"})
										  : QStringList{};
	if (fieldPath == "format.audio.channel_layout") {
		// result содержит только раскладки выбранного числа AAC-каналов.
		QStringList result;
		if (a.codec == "aac") {
			// layout обозначает очередную допустимую желаемую схему AAC.
			for (const QString &layout :
			     values({"mono", "stereo", "2.1", "3.0", "4.0", "5.0", "5.1", "7.1"}))
				if (layoutChannels(layout) == a.channels)
					result.append(layout);
		} else if (values({"opus", "flac", "mp3", "ac3"}).contains(a.codec))
			result.append("stereo");
		return result;
	}
	if (fieldPath == "video_encoding.closed_gop")
		return values({"true"});
	if (fieldPath == "video_encoding.max_b_frames") {
		if (v.codec != "h264")
			return values({"av1", "hevc", "vp9"}).contains(v.codec) ? values({"0"}) : QStringList{};
		if (values({"baseline", "constrained-baseline"}).contains(text(v.codecProfile)) ||
		    values({"6.0", "6.1", "6.2"}).contains(text(v.level)) ||
		    (encoder == "h264_nvenc" && e.nvenc.hasValue() && e.nvenc->tuning == "lossless"))
			return values({"0"});
		return profile.videoEncoding.gopFrames
			       ? integers(0, int(std::min(quint32(4), profile.videoEncoding.gopFrames - 1)))
			       : QStringList{};
	}
	if (!profileFieldApplicable(profile, fieldPath))
		return {};
	if (fieldPath == "encoding.rescale_output")
		return profile.mode == "transcode" ? values({"false", "true"}) : values({"false"});
	if (fieldPath == "encoding.nvenc.preset")
		return rc == "cqp" ? values({"p4"}) : values({"p1", "p2", "p3", "p4", "p5", "p6", "p7"});
	if (fieldPath == "encoding.nvenc.tuning")
		return rc == "cqp" ? values({"hq", "lossless"}) : values({"hq", "ll", "ull"});
	if (fieldPath == "encoding.nvenc.multipass")
		return rc == "cqp" ? values({"disabled"}) : values({"disabled", "qres", "fullres"});
	if (fieldPath == "encoding.nvenc.lookahead_frames")
		return rc == "cqp" ? values({"0"})
				   : integers(0, int(31 - std::min(quint32(31), profile.videoEncoding.maxBFrames)));
	if (fieldPath == "encoding.nvenc.enable_spatial_aq" || fieldPath == "encoding.nvenc.enable_temporal_aq")
		return rc == "cqp" ? values({"false"}) : values({"false", "true"});
	if (fieldPath == "encoding.nvenc.aq_strength")
		return rc == "cqp" || !e.nvenc.hasValue() || !e.nvenc->enableSpatialAq ? values({"8"})
										       : integers(1, 15);
	if (fieldPath == "encoding.svt_av1.preset")
		return integers(0, 13);
	if (fieldPath == "encoding.svt_av1.tuning")
		return rc == "crf"   ? values({"psnr", "vq", "ssim", "ms_ssim", "vmaf"})
		       : rc == "vbr" ? values({"psnr", "vq"})
				     : values({"psnr"});
	if (fieldPath == "encoding.svt_av1.aq_mode")
		return rc == "cqp" ? values({"0"}) : values({"2"});
	if (fieldPath == "encoding.svt_av1.lookahead_frames")
		return rc == "cbr" ? values({"0"}) : QStringList{"-1"} + integers(6, 120);
	if (fieldPath == "encoding.video_custom.nonref_p" || fieldPath == "encoding.video_custom.strict_gop")
		return values({"", "0", "1"});
	if (fieldPath == "encoding.video_custom.lp")
		return QStringList{""} + integers(0, 6);
	if (fieldPath == "encoding.audio_custom.aac_coder")
		return values({"", "twoloop", "fast"});
	return {};
}

// Определяет применимость поля по выбранному кодировщику и RC, сохраняя возможность исправить черновик.
bool profileFieldApplicable(const GroupProfile &profile, const QString &fieldPath)
{
	// e содержит выбранный encoder/RC; применимость не требует его полной корректности.
	const auto &e = recipe(profile);
	if (fieldPath == "encoding.qp")
		return e.rateControl == "cqp";
	if (fieldPath == "encoding.crf")
		return e.rateControl == "crf";
	if (fieldPath == "encoding.nvenc" || fieldPath.startsWith("encoding.nvenc."))
		return e.videoEncoder == "h264_nvenc";
	if (fieldPath == "encoding.svt_av1" || fieldPath.startsWith("encoding.svt_av1."))
		return e.videoEncoder == "libsvtav1";
	if (fieldPath == "encoding.video_custom.nonref_p" || fieldPath == "encoding.video_custom.strict_gop")
		return e.videoEncoder == "h264_nvenc" && e.rateControl != "cqp";
	if (fieldPath == "encoding.video_custom.lp")
		return e.videoEncoder == "libsvtav1";
	if (fieldPath == "encoding.audio_custom.aac_coder")
		return e.audioEncoder == "aac";
	if (fieldPath == "encoding.video_custom")
		return e.videoEncoder == "libsvtav1" || (e.videoEncoder == "h264_nvenc" && e.rateControl != "cqp");
	if (fieldPath == "encoding.audio_custom")
		return e.audioEncoder == "aac";
	if (fieldPath == "encoding.rescale_output")
		return profile.mode == "copy" || profile.mode == "transcode";
	if (fieldPath == "video_encoding.target_bitrate_bps" || fieldPath == "video_encoding.max_bitrate_bps" ||
	    fieldPath == "video_encoding.buffer_bits")
		return true;
	return values({"mode",
		       "live_audio_mode",
		       "encoding",
		       "encoding.video_encoder",
		       "encoding.audio_encoder",
		       "encoding.rate_control",
		       "format.video.codec",
		       "format.video.width",
		       "format.video.height",
		       "format.video.frame_rate",
		       "format.video.frame_rate.numerator",
		       "format.video.frame_rate.denominator",
		       "format.video.codec_profile",
		       "format.video.level",
		       "format.video.tier",
		       "format.video.pixel_format",
		       "format.video.bit_depth",
		       "format.video.color.range",
		       "format.video.color.primaries",
		       "format.video.color.transfer",
		       "format.video.color.matrix",
		       "format.audio.codec",
		       "format.audio.codec_profile",
		       "format.audio.sample_rate_hz",
		       "format.audio.channels",
		       "format.audio.channel_layout",
		       "video_encoding.gop_frames",
		       "video_encoding.max_b_frames",
		       "video_encoding.closed_gop",
		       "audio_encoding.bitrate_bps"})
		.contains(fieldPath);
}

// Проверяет полный profile по каталогам и нативным ограничениям; error содержит первый доверенный отказ.
bool validateProfile(const GroupProfile &profile, Error &error)
{
	if (!profile.encoding.hasValue())
		return invalid(error, "encoding", "Validation.ProfileEncoder");
	if (!profile.liveAudioMode.hasValue())
		return invalid(error, "live_audio_mode");
	// e, v и a содержат полные проверяемые блоки; их значения не материализуются автоматически.
	const auto &e = *profile.encoding;
	const auto &v = profile.format.video;
	const auto &a = profile.format.audio;
	// fields связывает каждый конечный wire-путь с его текущим значением для проверки по зависимому каталогу.
	const std::pair<QString, QString> fields[]{{"mode", profile.mode},
						   {"live_audio_mode", *profile.liveAudioMode},
						   {"format.video.codec", v.codec},
						   {"encoding.video_encoder", e.videoEncoder},
						   {"format.video.codec_profile", text(v.codecProfile)},
						   {"format.video.pixel_format", text(v.pixelFormat)},
						   {"format.video.bit_depth", QString::number(v.bitDepth)},
						   {"encoding.rate_control", e.rateControl},
						   {"format.video.level", text(v.level)},
						   {"format.video.tier", text(v.tier)},
						   {"format.video.color.range", text(v.color.range)},
						   {"format.video.color.primaries", text(v.color.primaries)},
						   {"format.video.color.transfer", text(v.color.transfer)},
						   {"format.video.color.matrix", text(v.color.matrix)},
						   {"format.audio.codec", a.codec},
						   {"format.audio.codec_profile", text(a.codecProfile)},
						   {"format.audio.channels", QString::number(a.channels)},
						   {"format.audio.channel_layout", text(a.channelLayout)},
						   {"format.audio.sample_rate_hz", QString::number(a.sampleRateHz)},
						   {"encoding.audio_encoder", e.audioEncoder}};
	// field обозначает очередную проверяемую пару пути и значения.
	for (const auto &field : fields)
		if (!profileOptions(profile, field.first).contains(field.second))
			return invalid(error, field.first);
	if (!structuralFormat(profile.format) || !v.width || !v.height || v.width > 65534 || v.height > 65534 ||
	    v.width % 2 || v.height % 2)
		return invalid(error, "format", "Validation.ProfileGeometry");
	if (!profile_math::validFrameRate(v.frameRate, a.sampleRateHz))
		return invalid(error, "format.video.frame_rate", "Validation.ProfileFrameRate");
	if (!profile.videoEncoding.closedGop || !profile.videoEncoding.gopFrames ||
	    profile.videoEncoding.gopFrames > static_cast<quint32>(std::numeric_limits<qint32>::max()))
		return invalid(error, "video_encoding.gop_frames", "Validation.ProfileGop");
	if (profile.mode == "copy" && e.rescaleOutput)
		return invalid(error, "encoding.rescale_output");
	if (e.videoEncoder == "h264_nvenc" && e.nvenc.hasValue()) {
		// n содержит применимый блок; settings перечисляет его конечные поля для общего каталога.
		const auto &n = *e.nvenc;
		const std::pair<QString, QString> settings[]{
			{"preset", n.preset},
			{"tuning", n.tuning},
			{"multipass", n.multipass},
			{"lookahead_frames", QString::number(n.lookaheadFrames)},
			{"enable_spatial_aq", n.enableSpatialAq ? "true" : "false"},
			{"enable_temporal_aq", n.enableTemporalAq ? "true" : "false"},
			{"aq_strength", QString::number(n.aqStrength)}};
		// setting выбирает очередное поле полного запроса NVENC.
		for (const auto &setting : settings)
			if (!profileOptions(profile, "encoding.nvenc." + setting.first).contains(setting.second))
				return invalid(error, "encoding.nvenc." + setting.first, "Validation.ProfileEncoder");
	}
	if (e.videoEncoder == "libsvtav1" && e.svtAv1.hasValue()) {
		// s содержит применимые настройки SVT; settings связывает значения с конечным каталогом.
		const auto &s = *e.svtAv1;
		const std::pair<QString, QString> settings[]{{"preset", QString::number(s.preset)},
							     {"tuning", s.tuning},
							     {"aq_mode", QString::number(s.aqMode)},
							     {"lookahead_frames", QString::number(s.lookaheadFrames)}};
		// setting обозначает очередной параметр полного запроса SVT.
		for (const auto &setting : settings)
			if (!profileOptions(profile, "encoding.svt_av1." + setting.first).contains(setting.second))
				return invalid(error, "encoding.svt_av1." + setting.first, "Validation.ProfileEncoder");
	}
	if (!audio(profile, error) || !video(profile, error))
		return false;
	error = {};
	return true;
}

// Сравнивает полный рецепт left и right после независимой проверки обеих сторон.
bool sameProfile(const GroupProfile &left, const GroupProfile &right)
{
	// error используется только для проверки корректности обеих полных сторон сравнения.
	Error error;
	if (!validateProfile(left, error) || !validateProfile(right, error))
		return false;
	// a и b обозначают полные рецепты; va и vb — GOP/битрейты.
	const auto &a = *left.encoding;
	const auto &b = *right.encoding;
	const auto &va = left.videoEncoding;
	const auto &vb = right.videoEncoding;
	if (left.mode != right.mode || *left.liveAudioMode != *right.liveAudioMode ||
	    !equalFormat(left.format, right.format) ||
	    std::tie(va.targetBitrateBps, va.maxBitrateBps, va.bufferBits, va.gopFrames, va.maxBFrames, va.closedGop) !=
		    std::tie(vb.targetBitrateBps, vb.maxBitrateBps, vb.bufferBits, vb.gopFrames, vb.maxBFrames,
			     vb.closedGop) ||
	    left.audioEncoding.bitrateBps != right.audioEncoding.bitrateBps ||
	    std::tie(a.videoEncoder, a.audioEncoder, a.rateControl, a.rescaleOutput, a.videoCustom, a.audioCustom) !=
		    std::tie(b.videoEncoder, b.audioEncoder, b.rateControl, b.rescaleOutput, b.videoCustom,
			     b.audioCustom) ||
	    a.qp.state != b.qp.state || (a.qp.hasValue() && *a.qp != *b.qp) ||
	    a.crfHundredths.state != b.crfHundredths.state ||
	    (a.crfHundredths.hasValue() && *a.crfHundredths != *b.crfHundredths))
		return false;
	if (a.videoEncoder == "h264_nvenc") {
		// x и y содержат уже проверенные применимые блоки NVIDIA.
		const auto &x = *a.nvenc;
		const auto &y = *b.nvenc;
		if (std::tie(x.preset, x.tuning, x.multipass, x.lookaheadFrames, x.enableSpatialAq, x.enableTemporalAq,
			     x.aqStrength) != std::tie(y.preset, y.tuning, y.multipass, y.lookaheadFrames,
						       y.enableSpatialAq, y.enableTemporalAq, y.aqStrength))
			return false;
	}
	if (a.videoEncoder == "libsvtav1") {
		// x и y содержат уже проверенные применимые блоки SVT.
		const auto &x = *a.svtAv1;
		const auto &y = *b.svtAv1;
		if (std::tie(x.preset, x.tuning, x.aqMode, x.lookaheadFrames) !=
		    std::tie(y.preset, y.tuning, y.aqMode, y.lookaheadFrames))
			return false;
	}
	return true;
}

// Проверяет actual относительно полного desired; только нормативные псевдонимы и пустые level/tier допускают различие.
bool compatibleMeasuredFormat(const Format &actual, const GroupProfile &desired)
{
	// error подтверждает корректность полного desired до проверки независимого измеренного формата.
	Error error;
	if (!validateProfile(desired, error) || !structuralFormat(actual) || !structuralFormat(desired.format))
		return false;
	// a и d содержат копии форматов для нормативной нормализации без изменения входных значений.
	Format a = actual;
	Format d = desired.format;
	if (!equalRate(a.video.frameRate, d.video.frameRate))
		return false;
	// format обозначает очередную сторону, для которой нормализуются только range и transfer.
	for (Format *format : {&a, &d}) {
		if (format->video.color.range.bytes == "tv")
			format->video.color.range.bytes = "limited";
		if (format->video.color.range.bytes == "pc")
			format->video.color.range.bytes = "full";
		if (format->video.color.transfer.bytes == "bt470m")
			format->video.color.transfer.bytes = "gamma22";
		if (format->video.color.transfer.bytes == "bt470bg")
			format->video.color.transfer.bytes = "gamma28";
	}
	if (d.video.level.bytes.isEmpty() || d.video.level.bytes == "auto")
		a.video.level = d.video.level;
	if (d.video.tier.bytes.isEmpty())
		a.video.tier = d.video.tier;
	return equalFormat(a, d);
}

} // namespace gost::api
