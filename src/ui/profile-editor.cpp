#include "profile-editor.hpp"
#include "api/profile-rules.hpp"

#include <QComboBox>
#include <QFormLayout>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QRegularExpression>
#include <QSet>
#include <QStandardItemModel>
#include <QStyle>
#include <QTextDocument>
#include <QToolButton>
#include <QToolTip>
#include <QVBoxLayout>
#include <algorithm>
#include <limits>
#include <type_traits>
#include <variant>
#include <vector>

// QVariant удерживает исходный DTO только в RAM-снимке незавершённого ввода редактора.
Q_DECLARE_METATYPE(gost::api::GroupProfile)

namespace gost {
namespace {
// Ссылка на одно типизированное поле DTO; числовые значения не проходят через double.
using ProfileValue =
	std::variant<QString *, api::ProfileText *, quint64 *, quint32 *, quint16 *, quint8 *, int *, bool *>;

// Определяет поля, отображаемые в кбит/с; буфер сохраняет единицу «биты».
bool isBitrate(const QString &path)
{
	return path == QStringLiteral("video.target") || path == QStringLiteral("video.maximum") ||
	       path == QStringLiteral("audio.bitrate");
}

// Возвращает точную десятичную запись кбит/с: остаток бит/с занимает до трёх знаков.
QString bitrateText(quint64 bits)
{
	// result содержит целую часть кбит/с, remainder — оставшиеся биты в секунду.
	QString result = QString::number(bits / 1000);
	const auto remainder = bits % 1000;
	if (remainder) {
		// fraction дополняется слева для сохранения тысячных и теряет лишь незначащие нули справа.
		auto fraction = QString::number(remainder).rightJustified(3, QLatin1Char('0'));
		while (fraction.endsWith(QLatin1Char('0')))
			fraction.chop(1);
		result += QLatin1Char('.') + fraction;
	}
	return result;
}

// Разбирает точные кбит/с с точкой либо запятой; лишние дробные нули допустимы, округления нет.
bool parseBitrate(const QString &text, quint64 &bits)
{
	// match отделяет целую часть от тысячных кбит/с и отвергает знаки, экспоненты и округление.
	const auto match = QRegularExpression(QStringLiteral("^(0|[1-9][0-9]*)(?:[.,]([0-9]{1,3}0*))?$")).match(text);
	if (!match.hasMatch())
		return false;
	// ok подтверждает представимость целой части; whole и fraction измеряются в кбит/с и бит/с соответственно.
	bool ok = false;
	const auto whole = match.captured(1).toULongLong(&ok);
	const auto fraction = match.captured(2).left(3).leftJustified(3, QLatin1Char('0')).toUInt();
	if (!ok || whole > (std::numeric_limits<quint64>::max() - fraction) / 1000)
		return false;
	bits = whole * 1000 + fraction;
	return true;
}

// Сопоставляет внутренние имена виджетов публичным путям общего каталога профиля.
QString wirePath(const QString &path)
{
	// aliases сохраняет привычные objectName, не вводя вторую таблицу допустимых значений.
	static const QMap<QString, QString> aliases{
		{QStringLiteral("audio.sample_rate"), QStringLiteral("format.audio.sample_rate_hz")},
		{QStringLiteral("video.target"), QStringLiteral("video_encoding.target_bitrate_bps")},
		{QStringLiteral("video.maximum"), QStringLiteral("video_encoding.max_bitrate_bps")},
		{QStringLiteral("video.buffer"), QStringLiteral("video_encoding.buffer_bits")},
		{QStringLiteral("video.gop"), QStringLiteral("video_encoding.gop_frames")},
		{QStringLiteral("video.b_frames"), QStringLiteral("video_encoding.max_b_frames")},
		{QStringLiteral("video.closed_gop"), QStringLiteral("video_encoding.closed_gop")},
		{QStringLiteral("audio.bitrate"), QStringLiteral("audio_encoding.bitrate_bps")},
		{QStringLiteral("encoding.rescale"), QStringLiteral("encoding.rescale_output")},
		{QStringLiteral("nvenc.lookahead"), QStringLiteral("encoding.nvenc.lookahead_frames")},
		{QStringLiteral("nvenc.spatial"), QStringLiteral("encoding.nvenc.enable_spatial_aq")},
		{QStringLiteral("nvenc.temporal"), QStringLiteral("encoding.nvenc.enable_temporal_aq")},
		{QStringLiteral("nvenc.strength"), QStringLiteral("encoding.nvenc.aq_strength")},
		{QStringLiteral("svt.aq"), QStringLiteral("encoding.svt_av1.aq_mode")},
		{QStringLiteral("svt.lookahead"), QStringLiteral("encoding.svt_av1.lookahead_frames")}};
	if (aliases.contains(path))
		return aliases.value(path);
	if (path.startsWith(QStringLiteral("video.")) || path.startsWith(QStringLiteral("audio.")))
		return QStringLiteral("format.") + path;
	if (path.startsWith(QStringLiteral("nvenc.")))
		return QStringLiteral("encoding.") + path;
	if (path.startsWith(QStringLiteral("svt.")))
		return QStringLiteral("encoding.svt_av1.") + path.mid(4);
	if (path.startsWith(QStringLiteral("custom.")))
		return (path == QStringLiteral("custom.aac_coder") ? QStringLiteral("encoding.audio_custom.")
								   : QStringLiteral("encoding.video_custom.")) +
		       path.mid(7);
	return path;
}

// Возвращает точный CRF в сотых; дробная строка никогда не проходит через double.
QString crfText(int hundredths)
{
	return QStringLiteral("%1.%2").arg(hundredths / 100).arg(hundredths % 100, 2, 10, QLatin1Char('0'));
}

// Проверяет десятичный CRF с точкой либо запятой; лишние дробные нули не меняют точные сотые.
bool parseCrf(const QString &text, int &hundredths)
{
	// match разделяет целую и дробную части; ok подтверждает диапазон промежуточного целого.
	const auto match = QRegularExpression(QStringLiteral("^(0|[1-9][0-9]*)(?:[.,]([0-9]{1,2}0*))?$")).match(text);
	bool ok = false;
	// whole и fraction измеряются соответственно в единицах CRF и сотых.
	const auto whole = match.captured(1).toULongLong(&ok);
	const auto fraction = match.captured(2).left(2).leftJustified(2, QLatin1Char('0')).toUInt();
	if (!match.hasMatch() || !ok || whole > (quint64(std::numeric_limits<int>::max()) - fraction) / 100)
		return false;
	hundredths = int(whole * 100 + fraction);
	return true;
}

// Возвращает полный проверенный профиль владельца; index выбирает H.264 1080p60, H.264 1440p60 либо AV1 1440p60.
api::GroupProfile basePreset(int index)
{
	// av1 разделяет два подтверждённых типа профиля; profile содержит полный DTO без серверных идентификаторов.
	const bool av1 = index == 2;
	api::GroupProfile profile;
	profile.mode = av1 ? QStringLiteral("copy") : QStringLiteral("transcode");
	profile.liveAudioMode = {api::FieldState::Value, QStringLiteral("copy")};
	profile.format.video = {av1 ? QStringLiteral("av1") : QStringLiteral("h264"),
				index == 0 ? 1920U : 2560U,
				index == 0 ? 1080U : 1440U,
				{60, 1},
				{av1 ? QByteArrayLiteral("main") : QByteArrayLiteral("high")},
				{},
				{},
				{QByteArrayLiteral("yuv420p")},
				8,
				{{QByteArrayLiteral("limited")},
				 {QByteArrayLiteral("bt709")},
				 {QByteArrayLiteral("bt709")},
				 {QByteArrayLiteral("bt709")}}};
	profile.format.audio = {QStringLiteral("aac"),
				{QByteArrayLiteral("LC")},
				48000,
				2,
				{QByteArrayLiteral("stereo")}};
	// target содержит точный битрейт выбранного исходного профиля; буфер равен двум секундам этого битрейта.
	const quint64 target = av1 ? 10000000 : index == 0 ? 6000000 : 12000000;
	profile.videoEncoding = {target, target, target * 2, 120, 0, true};
	profile.audioEncoding = {160000};
	profile.encoding.state = api::FieldState::Value;
	profile.encoding.value.videoEncoder = av1 ? QStringLiteral("libsvtav1") : QStringLiteral("h264_nvenc");
	profile.encoding.value.audioEncoder = QStringLiteral("aac");
	profile.encoding.value.rateControl = QStringLiteral("cbr");
	profile.encoding.value.rescaleOutput = !av1;
	if (av1)
		profile.encoding.value.svtAv1 = {api::FieldState::Value, {8, QStringLiteral("psnr"), 2, 0}};
	else
		profile.encoding.value.nvenc = {api::FieldState::Value,
						{QStringLiteral("p4"), QStringLiteral("hq"), QStringLiteral("disabled"),
						 0, false, false, 8}};
	return profile;
}

// Выбирает data; неизвестный пункт сохраняет исходное значение и по умолчанию недоступен для выбора.
void selectValue(QComboBox *combo, const QVariant &data, const QString &unknownLabel, bool disabled = true)
{
	// index адресует точное itemData, а не локализованную подпись.
	int index = combo->findData(data);
	if (index < 0) {
		combo->addItem(unknownLabel, data);
		index = combo->count() - 1;
		combo->setItemData(index, true, Qt::UserRole + 1);
		// model предоставляет флаги только добавленного исходного значения.
		if (disabled)
			if (auto *model = qobject_cast<QStandardItemModel *>(combo->model()))
				model->item(index)->setEnabled(false);
	}
	combo->setCurrentIndex(index);
}

// Удаляет временные неизвестные пункты предыдущей карточки, не затрагивая подтверждённый каталог.
void clearUnknownValues(QComboBox *combo)
{
	// index проходит список с конца, чтобы удаление не сдвигало ещё не проверенные пункты.
	for (int index = combo->count() - 1; index >= 0; --index)
		if (combo->itemData(index, Qt::UserRole + 1).toBool())
			combo->removeItem(index);
}

// Возвращает строку с заменённой или удалённой парой key=value. Известные пары сортируются для валидатора,
// а неизвестные либо повреждённые токены остаются в исходном порядке до явной очистки.
// source — исходная строка, key — имя изменяемой опции, value — новое значение или пустота для удаления.
QString replaceCustomOption(const QString &source, const QString &key, const QString &value)
{
	// pairs сохраняет все незатронутые опции; pair проходит токены серверного ASCII-space представления.
	QStringList pairs;
	for (const auto &pair : source.split(QLatin1Char(' '), Qt::SkipEmptyParts))
		if (!pair.startsWith(key + QLatin1Char('=')))
			pairs.append(pair);
	if (!value.isEmpty())
		pairs.append(key + QLatin1Char('=') + value);
	// audio выбирает допустимое имя для дорожки, а onlyKnown разрешает сортировку без изменения неизвестного ввода.
	const bool audio = key == QStringLiteral("aac_coder");
	bool onlyKnown = true;
	// pair проверяет один оставленный токен, optionName — его имя до знака равенства.
	for (const auto &pair : pairs) {
		// separator указывает единственный знак равенства в проверяемой паре.
		const auto separator = pair.indexOf(QLatin1Char('='));
		const auto optionName = pair.section(QLatin1Char('='), 0, 0);
		if (separator <= 0 || pair.indexOf(QLatin1Char('='), separator + 1) >= 0 ||
		    (audio ? optionName != QStringLiteral("aac_coder")
			   : (optionName != QStringLiteral("nonref_p") && optionName != QStringLiteral("strict_gop") &&
			      optionName != QStringLiteral("lp")))) {
			onlyKnown = false;
			break;
		}
	}
	if (onlyKnown)
		std::sort(pairs.begin(), pairs.end());
	return pairs.join(QLatin1Char(' '));
}

// Оставляет только именованные пары, для которых в форме есть отдельный контрол соответствующей дорожки.
// Значения известных параметров не исправляет: их допустимость проверяет общий каталог.
QString knownCustomOptions(const QString &source, bool audio)
{
	// result сохраняет известные пары в исходном порядке, pair обозначает один токен исходной строки.
	QStringList result;
	for (const auto &pair : source.split(QLatin1Char(' '), Qt::SkipEmptyParts)) {
		// key адресует уже существующий именованный контрол и не расширяет каталог значений.
		const auto key = pair.section(QLatin1Char('='), 0, 0);
		if (pair.indexOf(QLatin1Char('=')) > 0 &&
		    ((audio && key == QStringLiteral("aac_coder")) ||
		     (!audio && (key == QStringLiteral("nonref_p") || key == QStringLiteral("strict_gop") ||
				 key == QStringLiteral("lp")))))
			result.append(pair);
	}
	return result.join(QLatin1Char(' '));
}

// Возвращает адрес известного поля p по точному имени path; список соответствует полной форме профиля.
ProfileValue profileValue(api::GroupProfile &p, const QString &path)
{
	if (path == QStringLiteral("mode"))
		return &p.mode;
	if (path == QStringLiteral("live_audio_mode"))
		return &p.liveAudioMode.value;
	if (path == QStringLiteral("video.codec"))
		return &p.format.video.codec;
	if (path == QStringLiteral("video.width"))
		return &p.format.video.width;
	if (path == QStringLiteral("video.height"))
		return &p.format.video.height;
	if (path == QStringLiteral("video.fps_num"))
		return &p.format.video.frameRate.numerator;
	if (path == QStringLiteral("video.fps_den"))
		return &p.format.video.frameRate.denominator;
	if (path == QStringLiteral("video.codec_profile"))
		return &p.format.video.codecProfile;
	if (path == QStringLiteral("video.level"))
		return &p.format.video.level;
	if (path == QStringLiteral("video.tier"))
		return &p.format.video.tier;
	if (path == QStringLiteral("video.pixel_format"))
		return &p.format.video.pixelFormat;
	if (path == QStringLiteral("video.bit_depth"))
		return &p.format.video.bitDepth;
	if (path == QStringLiteral("video.color.range"))
		return &p.format.video.color.range;
	if (path == QStringLiteral("video.color.primaries"))
		return &p.format.video.color.primaries;
	if (path == QStringLiteral("video.color.transfer"))
		return &p.format.video.color.transfer;
	if (path == QStringLiteral("video.color.matrix"))
		return &p.format.video.color.matrix;
	if (path == QStringLiteral("audio.codec"))
		return &p.format.audio.codec;
	if (path == QStringLiteral("audio.codec_profile"))
		return &p.format.audio.codecProfile;
	if (path == QStringLiteral("audio.sample_rate"))
		return &p.format.audio.sampleRateHz;
	if (path == QStringLiteral("audio.channels"))
		return &p.format.audio.channels;
	if (path == QStringLiteral("audio.channel_layout"))
		return &p.format.audio.channelLayout;
	if (path == QStringLiteral("video.target"))
		return &p.videoEncoding.targetBitrateBps;
	if (path == QStringLiteral("video.maximum"))
		return &p.videoEncoding.maxBitrateBps;
	if (path == QStringLiteral("video.buffer"))
		return &p.videoEncoding.bufferBits;
	if (path == QStringLiteral("video.gop"))
		return &p.videoEncoding.gopFrames;
	if (path == QStringLiteral("video.b_frames"))
		return &p.videoEncoding.maxBFrames;
	if (path == QStringLiteral("video.closed_gop"))
		return &p.videoEncoding.closedGop;
	if (path == QStringLiteral("audio.bitrate"))
		return &p.audioEncoding.bitrateBps;
	if (path == QStringLiteral("encoding.video_encoder"))
		return &p.encoding.value.videoEncoder;
	if (path == QStringLiteral("encoding.audio_encoder"))
		return &p.encoding.value.audioEncoder;
	if (path == QStringLiteral("encoding.rate_control"))
		return &p.encoding.value.rateControl;
	if (path == QStringLiteral("encoding.rescale"))
		return &p.encoding.value.rescaleOutput;
	if (path == QStringLiteral("encoding.qp"))
		return &p.encoding.value.qp.value;
	if (path == QStringLiteral("encoding.crf"))
		return &p.encoding.value.crfHundredths.value;
	if (path == QStringLiteral("nvenc.preset"))
		return &p.encoding.value.nvenc.value.preset;
	if (path == QStringLiteral("nvenc.tuning"))
		return &p.encoding.value.nvenc.value.tuning;
	if (path == QStringLiteral("nvenc.multipass"))
		return &p.encoding.value.nvenc.value.multipass;
	if (path == QStringLiteral("nvenc.lookahead"))
		return &p.encoding.value.nvenc.value.lookaheadFrames;
	if (path == QStringLiteral("nvenc.spatial"))
		return &p.encoding.value.nvenc.value.enableSpatialAq;
	if (path == QStringLiteral("nvenc.temporal"))
		return &p.encoding.value.nvenc.value.enableTemporalAq;
	if (path == QStringLiteral("nvenc.strength"))
		return &p.encoding.value.nvenc.value.aqStrength;
	if (path == QStringLiteral("svt.preset"))
		return &p.encoding.value.svtAv1.value.preset;
	if (path == QStringLiteral("svt.tuning"))
		return &p.encoding.value.svtAv1.value.tuning;
	if (path == QStringLiteral("svt.aq"))
		return &p.encoding.value.svtAv1.value.aqMode;
	return &p.encoding.value.svtAv1.value.lookaheadFrames;
}
} // namespace

/** Привязывает структурный ввод к полям полного профиля. Каталоги и применимость
 * получает из profile-rules; исходный документ и неприменимый ввод остаются в RAM. */
class ProfileEditor::Impl {
public:
	// Описание одного ввода; не владеет QObject-элементами, принадлежащими внешнему виджету.
	struct Field {
		// path адресует поле DTO, label содержит локализованное название для ошибок.
		QString path;
		QString label;
		// input содержит строку, выбор либо флаг; bytes отличает точный QByteArray в itemData от строковых чисел.
		QWidget *input = nullptr;
		bool bytes = false;
		// finite отличает закрытый каталог от ввода числового диапазона.
		bool finite = false;
		// caption показывает зависимую от copy/transcode подпись, не меняя самого значения.
		QLabel *caption = nullptr;
	};
	// Создаёт разделы и привязки; owner и text задают QObject-контекст и перевод подписей.
	// nameInput, если передан, размещается перед параметрами в общей колонке ввода.
	Impl(ProfileEditor &owner, TextLookup text, QWidget *nameInput);
	// Добавляет поле path с подписью label; finite/boolean выбирают закрытый каталог общего модуля.
	// bytes сохраняет ProfileText как QByteArray в itemData.
	void add(QFormLayout *form, const char *path, const char *label, bool finite = false, bool bytes = false,
		 bool boolean = false);
	// Добавляет локализованную группу формы к общему layout.
	QFormLayout *section(QVBoxLayout *layout, const char *title);
	// Регистрирует явное изменение path и уведомляет внешний виджет, кроме периода отражения DTO.
	void touch(const QString &path);
	// Пересчитывает конечные каталоги и применимость, сохраняя видимые несовместимые исходные значения.
	void updateApplicability();
	// Меняет разрешённые пункты path, сохраняя текущий выбор как отдельное исходное значение.
	void choices(const QString &path, const QStringList &options);
	// Обновляет справку из текущего текста обоих полей, включая невалидный незавершённый ввод.
	void updateBitrateHelp();
	// Показывает актуальную справку после активации значка мышью или клавиатурой.
	void showBitrateHelp();
	// Возвращает понятную подпись подтверждённого пункта value, сохраняя path и itemData техническими.
	QString optionLabel(const QString &path, const QString &value) const;
	// Находит ввод по path среди полного известного набора полей.
	QWidget *input(const QString &path) const;
	// owner_ задаёт родителя и контекст обработчиков; text_ переводит подписи.
	ProfileEditor &owner_;
	TextLookup text_;
	// original_ содержит точный исходный DTO; fields_ описывает все поля структуры.
	api::GroupProfile original_;
	std::vector<Field> fields_;
	// dirty_ содержит только пользовательские поля; loading_ подавляет отражённые сигналы, creating_ требует новый формат.
	QSet<QString> dirty_;
	bool loading_ = false;
	bool creating_ = false;
	// preset_ заполняет форму только по activated; resolution_ принимает размеры одной строкой, fps_ хранит точную дробь.
	QComboBox *preset_ = nullptr;
	QComboBox *resolution_ = nullptr;
	QComboBox *fps_ = nullptr;
	// advanced_ раскрывает advancedContent_ без изменения доступности и несохранённого ввода.
	QToolButton *advanced_ = nullptr;
	QWidget *advancedContent_ = nullptr;
	// bitrateHelp_ показывает текущие основной и максимальный видеобитрейты без изменения черновика.
	QToolButton *bitrateHelp_ = nullptr;
	// Читает проекцию формы; strict требует корректного применимого ввода, мягкий режим нужен каталогам.
	bool project(api::GroupProfile &profile, QString &error, bool strict) const;
	// custom_ хранит только разрешённые отдельные параметры; исходные строки сохраняются до их изменения.
	QMap<QString, QComboBox *> custom_;
	// sourceCustom_ показывает неподдерживаемую исходную строку и требует явного удаления лишних пар.
	QMap<QString, QComboBox *> sourceCustom_;
	// sourceCaptions_ скрывает подписи вместе с отсутствующими неподдерживаемыми исходными параметрами.
	QMap<QString, QLabel *> sourceCaptions_;
};

// Создаёт дочерний раздел и возвращает форму для его связанных параметров.
QFormLayout *ProfileEditor::Impl::section(QVBoxLayout *layout, const char *title)
{
	// group объединяет связанные параметры; form передаётся помощнику создания отдельных полей.
	auto *group = new QGroupBox(text_(title), &owner_);
	auto *form = new QFormLayout(group);
	form->setFieldGrowthPolicy(QFormLayout::AllNonFixedFieldsGrow);
	form->setRowWrapPolicy(QFormLayout::WrapLongRows);
	layout->addWidget(group);
	return form;
}

// Создаёт ввод диапазона либо закрытый список, сохраняя техническое имя поля и пользовательское намерение.
void ProfileEditor::Impl::add(QFormLayout *form, const char *path, const char *label, bool finite, bool bytes,
			      bool boolean)
{
	// field содержит адрес DTO, подпись и созданный ввод этой строки.
	Field field{QString::fromLatin1(path), text_(label), nullptr, bytes, finite || boolean};
	if (field.finite) {
		// combo получает каталог только из profile-rules; исходный выбор устанавливается отдельно.
		auto *combo = new QComboBox(&owner_);
		combo->setEditable(false);
		field.input = combo;
		// Обработчик activated отмечает явную замену, а отражение DTO не меняет черновик.
		QObject::connect(combo, &QComboBox::activated, &owner_,
				 [this, path] { touch(QString::fromLatin1(path)); });
	} else {
		// line хранит точный ввод диапазона, в том числе незавершённую десятичную строку.
		auto *line = new QLineEdit(&owner_);
		line->setMaxLength(std::numeric_limits<int>::max());
		field.input = line;
		// Обработчик textEdited сохраняет пользовательское намерение без реакции на setText.
		QObject::connect(line, &QLineEdit::textEdited, &owner_,
				 [this, path] { touch(QString::fromLatin1(path)); });
	}
	field.input->setObjectName(field.path);
	field.input->setAccessibleName(field.label);
	field.caption = new QLabel(field.label, &owner_);
	field.caption->setBuddy(field.input);
	if (field.path == QStringLiteral("video.target")) {
		// row держит поле и справку рядом, сохраняя исходное поле ввода и его доступность.
		auto *row = new QWidget(&owner_);
		// rowLayout не добавляет внешних отступов к общей колонке формы.
		auto *rowLayout = new QHBoxLayout(row);
		rowLayout->setContentsMargins(0, 0, 0, 0);
		// bitrateHelp_ использует стандартную пиктограмму темы и доступен с клавиатуры.
		bitrateHelp_ = new QToolButton(row);
		bitrateHelp_->setObjectName(QStringLiteral("videoBitrateHelp"));
		bitrateHelp_->setIcon(owner_.style()->standardIcon(QStyle::SP_MessageBoxQuestion));
		bitrateHelp_->setAutoRaise(true);
		bitrateHelp_->setFocusPolicy(Qt::StrongFocus);
		bitrateHelp_->setAccessibleName(text_("Profile.BitrateHelpTitle"));
		// Нажатие вызывает только справку и не отмечает профиль изменённым.
		QObject::connect(bitrateHelp_, &QToolButton::clicked, &owner_, [this] { showBitrateHelp(); });
		rowLayout->addWidget(field.input);
		rowLayout->addWidget(bitrateHelp_);
		rowLayout->setStretch(0, 1);
		form->addRow(field.caption, row);
	} else {
		form->addRow(field.caption, field.input);
	}
	if (field.path == QStringLiteral("video.target") || field.path == QStringLiteral("video.maximum")) {
		// textEdited обновляет справку только при пользовательском вводе, не меняя dirty-состояние.
		QObject::connect(qobject_cast<QLineEdit *>(field.input), &QLineEdit::textEdited, &owner_,
				 [this] { updateBitrateHelp(); });
	}
	fields_.push_back(std::move(field));
}

// Создаёт основную форму и свёрнутый расширенный раздел; обработчики не выполняют HTTP.
ProfileEditor::Impl::Impl(ProfileEditor &owner, TextLookup text, QWidget *nameInput)
	: owner_(owner),
	  text_(std::move(text))
{
	// layout объединяет основной ввод и раскрываемый раздел; basics выравнивает имя и основные параметры.
	auto *layout = new QVBoxLayout(&owner_);
	layout->setContentsMargins(0, 0, 0, 0);
	auto *basics = new QFormLayout;
	basics->setFieldGrowthPolicy(QFormLayout::AllNonFixedFieldsGrow);
	basics->setRowWrapPolicy(QFormLayout::WrapLongRows);
	layout->addLayout(basics);
	if (nameInput)
		basics->addRow(text_("Editor.Name"), nameInput);
	preset_ = new QComboBox(&owner_);
	preset_->setObjectName(QStringLiteral("profilePreset"));
	preset_->addItem(text_("Profile.NoPreset"), QString{});
	preset_->addItem(QStringLiteral("H.264 1080p60"), QStringLiteral("h264-1080p60"));
	preset_->addItem(QStringLiteral("H.264 1440p60"), QStringLiteral("h264-1440p60"));
	preset_->addItem(QStringLiteral("AV1 1440p60"), QStringLiteral("av1-1440p60"));
	basics->addRow(text_("Profile.BasePreset"), preset_);
	// Явный выбор index заменяет полный редактируемый профиль, но ничего не отправляет на сервер.
	QObject::connect(preset_, &QComboBox::activated, &owner_, [this](int index) {
		if (index <= 0 || index > 3)
			return;
		owner_.setProfile(basePreset(index - 1), creating_);
		preset_->setCurrentIndex(index);
		emit owner_.changed();
	});
	add(basics, "mode", "Profile.VideoMode", true, false, false);
	add(basics, "video.codec", "Profile.Codec", true, false, false);
	resolution_ = new QComboBox(&owner_);
	resolution_->setObjectName(QStringLiteral("video.resolution"));
	resolution_->setEditable(true);
	resolution_->setInsertPolicy(QComboBox::NoInsert);
	resolution_->lineEdit()->setMaxLength(32);
	resolution_->addItems({QStringLiteral("1280 × 720"), QStringLiteral("1920 × 1080"),
			       QStringLiteral("2560 × 1440"), QStringLiteral("3840 × 2160")});
	resolution_->setAccessibleName(text_("Profile.Resolution"));
	basics->addRow(text_("Profile.Resolution"), resolution_);
	// Изменение размеров вручную и выбор готовой геометрии одинаково отмечают единое поле разрешения.
	QObject::connect(resolution_->lineEdit(), &QLineEdit::textEdited, &owner_,
			 [this] { touch(QStringLiteral("video.resolution")); });
	QObject::connect(resolution_, &QComboBox::activated, &owner_,
			 [this] { touch(QStringLiteral("video.resolution")); });
	fps_ = new QComboBox(&owner_);
	fps_->setObjectName(QStringLiteral("video.fps"));
	fps_->setAccessibleName(text_("Profile.Fps"));
	fps_->addItem(text_("Editor.Choose"), QString{});
	// rate хранит каноническую дробь; её отображение не используется для обратного вычисления FPS.
	for (const auto &rate :
	     {QStringLiteral("24/1"), QStringLiteral("25/1"), QStringLiteral("30/1"), QStringLiteral("48/1"),
	      QStringLiteral("50/1"), QStringLiteral("60/1"), QStringLiteral("120/1"), QStringLiteral("24000/1001"),
	      QStringLiteral("30000/1001"), QStringLiteral("60000/1001")}) {
		// label содержит только читаемое название типовой частоты; rate остаётся точной парой API.
		const auto label = rate == QStringLiteral("24000/1001")   ? QStringLiteral("23.976")
				   : rate == QStringLiteral("30000/1001") ? QStringLiteral("29.97")
				   : rate == QStringLiteral("60000/1001") ? QStringLiteral("59.94")
									  : rate.section(QLatin1Char('/'), 0, 0);
		fps_->addItem(label, rate);
	}
	basics->addRow(text_("Profile.Fps"), fps_);
	// Только activated отмечает намерение заменить частоту, сохраняя нестандартное исходное отношение до выбора.
	QObject::connect(fps_, &QComboBox::activated, &owner_, [this] { touch(QStringLiteral("video.fps")); });
	add(basics, "video.target", "Profile.TargetBitrate", false, false, false);
	add(basics, "live_audio_mode", "Profile.LiveAudio", true, false, false);
	add(basics, "audio.bitrate", "Profile.AudioBitrate", false, false, false);
	// notice поясняет применимость битрейта аудио и независимость текущего Output от редактирования.
	auto *notice = new QLabel(text_("Profile.AudioNotice"), &owner_);
	notice->setTextFormat(Qt::PlainText);
	notice->setWordWrap(true);
	layout->addWidget(notice);
	advanced_ = new QToolButton(&owner_);
	advanced_->setObjectName(QStringLiteral("profileAdvanced"));
	advanced_->setText(text_("Profile.Advanced"));
	advanced_->setToolButtonStyle(Qt::ToolButtonTextBesideIcon);
	advanced_->setArrowType(Qt::RightArrow);
	advanced_->setCheckable(true);
	layout->addWidget(advanced_);
	advancedContent_ = new QWidget(&owner_);
	advancedContent_->setObjectName(QStringLiteral("profileAdvancedContent"));
	// advancedLayout удерживает все расширенные поля в том же виджете, даже когда раздел скрыт.
	auto *advancedLayout = new QVBoxLayout(advancedContent_);
	advancedLayout->setContentsMargins(0, 0, 0, 0);
	layout->addWidget(advancedContent_);
	advancedContent_->hide();
	// expanded меняет только видимость; отключение полей нарушило бы чтение правок свёрнутого раздела.
	QObject::connect(advanced_, &QToolButton::toggled, &owner_, [this](bool expanded) {
		advancedContent_->setVisible(expanded);
		advanced_->setArrowType(expanded ? Qt::DownArrow : Qt::RightArrow);
	});
	// video содержит весь ожидаемый видеоформат, включая цвет и точные ProfileText.
	auto *video = section(advancedLayout, "Profile.VideoFormat");
	add(video, "video.codec_profile", "Profile.CodecProfile", true, true, false);
	add(video, "video.level", "Profile.Level", true, true, false);
	add(video, "video.tier", "Profile.Tier", true, true, false);
	add(video, "video.pixel_format", "Profile.PixelFormat", true, true, false);
	add(video, "video.bit_depth", "Profile.BitDepth", true, false, false);
	add(video, "video.color.range", "Profile.ColorRange", true, true, false);
	add(video, "video.color.primaries", "Profile.ColorPrimaries", true, true, false);
	add(video, "video.color.transfer", "Profile.ColorTransfer", true, true, false);
	add(video, "video.color.matrix", "Profile.ColorMatrix", true, true, false);
	// audio содержит ожидаемый аудиоформат, а не измерения текущего LIVE.
	auto *audio = section(advancedLayout, "Profile.AudioFormat");
	add(audio, "audio.codec", "Profile.Codec", true, false, false);
	add(audio, "audio.codec_profile", "Profile.CodecProfile", true, true, false);
	add(audio, "audio.sample_rate", "Profile.SampleRate", true, false, false);
	add(audio, "audio.channels", "Profile.Channels", true, false, false);
	add(audio, "audio.channel_layout", "Profile.ChannelLayout", true, true, false);
	// rates содержит точные числа рецепта: биты/с, биты и количество кадров.
	auto *rates = section(advancedLayout, "Profile.Bitrates");
	add(rates, "video.maximum", "Profile.MaxBitrate", false, false, false);
	add(rates, "video.buffer", "Profile.Buffer", false, false, false);
	add(rates, "video.gop", "Profile.Gop", false, false, false);
	add(rates, "video.b_frames", "Profile.BFrames", true, false, false);
	add(rates, "video.closed_gop", "Profile.ClosedGop", true, false, true);
	// encoding содержит полный явный рецепт без выбора версии или присутствия.
	auto *encoding = section(advancedLayout, "Profile.Encoding");
	add(encoding, "encoding.video_encoder", "Profile.VideoEncoder", true, false, false);
	add(encoding, "encoding.audio_encoder", "Profile.AudioEncoder", true, false, false);
	add(encoding, "encoding.rate_control", "Profile.RateControl", true, false, false);
	add(encoding, "encoding.qp", "Profile.Qp");
	add(encoding, "encoding.crf", "Profile.Crf");
	add(encoding, "encoding.rescale", "Profile.Rescale", true, false, true);
	// nvenc содержит конечные параметры применимых кодировщиков NVENC.
	auto *nvenc = section(advancedLayout, "Profile.Nvenc");
	add(nvenc, "nvenc.preset", "Profile.Preset", true, false, false);
	add(nvenc, "nvenc.tuning", "Profile.Tuning", true, false, false);
	add(nvenc, "nvenc.multipass", "Profile.Multipass", true, false, false);
	add(nvenc, "nvenc.lookahead", "Profile.Lookahead", true, false, false);
	add(nvenc, "nvenc.spatial", "Profile.SpatialAq", true, false, true);
	add(nvenc, "nvenc.temporal", "Profile.TemporalAq", true, false, true);
	add(nvenc, "nvenc.strength", "Profile.AqStrength", true, false, false);
	// svt содержит настройки AV1-рецепта заглушки и не включает перекодирование основного copy-потока.
	auto *svt = section(advancedLayout, "Profile.Svt");
	add(svt, "svt.preset", "Profile.Preset", true, false, false);
	add(svt, "svt.tuning", "Profile.Tuning", true, false, false);
	add(svt, "svt.aq", "Profile.AqMode", true, false, false);
	add(svt, "svt.lookahead", "Profile.Lookahead", true, false, false);
	// custom содержит отдельные разрешённые API-пары вместо произвольной командной строки.
	auto *custom = section(advancedLayout, "Profile.Custom");
	// key адресует разрешённую API-пару; её каталог и применимость определяет общий модуль.
	for (const auto &key : {QStringLiteral("nonref_p"), QStringLiteral("strict_gop"), QStringLiteral("lp"),
				QStringLiteral("aac_coder")}) {
		// combo хранит только технические строки, без свободного CLI-ввода.
		auto *combo = new QComboBox(&owner_);
		combo->setObjectName(QStringLiteral("custom.") + key);
		combo->setEditable(false);
		custom_[key] = combo;
		custom->addRow(text_(key == QStringLiteral("nonref_p")     ? "Profile.CustomNonref"
				     : key == QStringLiteral("strict_gop") ? "Profile.CustomStrictGop"
				     : key == QStringLiteral("lp")         ? "Profile.CustomLp"
									   : "Profile.CustomAacCoder"),
			       combo);
		// Обработчик запоминает изменение одной пары, сохраняя незатронутые исходные опции.
		QObject::connect(combo, &QComboBox::activated, &owner_,
				 [this, key] { touch(QStringLiteral("custom.") + key); });
	}
	// track разделяет исходные дополнительные параметры видео и звука; новые строки здесь не вводятся.
	for (const auto &track : {QStringLiteral("video"), QStringLiteral("audio")}) {
		// combo предлагает только явное удаление неподдерживаемой части; caption подписывает исходную строку.
		auto *combo = new QComboBox(&owner_);
		auto *caption = new QLabel(text_(track == QStringLiteral("audio") ? "Profile.UnsupportedAudioOptions"
										  : "Profile.UnsupportedVideoOptions"),
					   &owner_);
		combo->setObjectName(QStringLiteral("unsupportedCustom.") + track);
		combo->setEditable(false);
		caption->setBuddy(combo);
		custom->addRow(caption, combo);
		sourceCustom_[track] = combo;
		sourceCaptions_[track] = caption;
		// Обработчик отмечает явное решение удалить неизвестные пары; простая смена encoder их не удаляет.
		QObject::connect(combo, &QComboBox::activated, &owner_,
				 [this, track] { touch(QStringLiteral("custom_source.") + track); });
	}
	updateBitrateHelp();
}

// Возвращает заимствованный ввод по стабильному внутреннему имени поля.
QWidget *ProfileEditor::Impl::input(const QString &path) const
{
	// field — очередная привязка известного поля структуры.
	for (const auto &field : fields_)
		if (field.path == path)
			return field.input;
	return nullptr;
}

// Отражает текущие строки основного и скрытого расширенного битрейта без нормализации значений.
void ProfileEditor::Impl::updateBitrateHelp()
{
	// target и maximum сохраняют пользовательский текст в кбит/с, включая временно невалидные строки.
	auto *target = qobject_cast<QLineEdit *>(input(QStringLiteral("video.target")));
	auto *maximum = qobject_cast<QLineEdit *>(input(QStringLiteral("video.maximum")));
	if (!bitrateHelp_ || !target || !maximum)
		return;
	// description остаётся обычным текстом для screen reader и локального всплывающего пояснения.
	const auto description = text_("Profile.BitrateHelpText").arg(target->text(), maximum->text());
	bitrateHelp_->setAccessibleDescription(description);
	bitrateHelp_->setToolTip(Qt::convertFromPlainText(description));
}

// Показывает подсказку у значка и не меняет содержимое или dirty-признаки профиля.
void ProfileEditor::Impl::showBitrateHelp()
{
	updateBitrateHelp();
	if (!bitrateHelp_)
		return;
	// point размещает подсказку под кнопкой; tooltip содержит только текущий локализованный текст.
	const auto point = bitrateHelp_->mapToGlobal(QPoint(0, bitrateHelp_->height()));
	QToolTip::showText(point, bitrateHelp_->toolTip(), bitrateHelp_);
}

// Отмечает одно пользовательское изменение и обновляет зависимости без подстановки значений.
void ProfileEditor::Impl::touch(const QString &path)
{
	if (loading_)
		return;
	dirty_.insert(path);
	updateApplicability();
	emit owner_.changed();
}

// Локализует значения конечных каталогов; ни перевод, ни пояснение не попадают в DTO.
QString ProfileEditor::Impl::optionLabel(const QString &path, const QString &value) const
{
	if (path == QStringLiteral("nvenc.preset"))
		return text_((QStringLiteral("Profile.Option.NvencP") + value.mid(1)).toUtf8().constData());
	if (path == QStringLiteral("audio.channels") && (value == QStringLiteral("1") || value == QStringLiteral("2")))
		return text_(value == QStringLiteral("1") ? "Profile.Option.Mono" : "Profile.Option.Stereo") +
		       QStringLiteral(" (%1)").arg(value);
	if (path == QStringLiteral("svt.lookahead") && value == QStringLiteral("-1"))
		return text_("Profile.Option.Auto");
	if (value == QStringLiteral("true") || value == QStringLiteral("false"))
		return text_(value == QStringLiteral("true") ? "Dialog.Yes" : "Dialog.No");
	if (path == QStringLiteral("mode"))
		return text_(value == QStringLiteral("copy") ? "Profile.CopyVideo" : "Profile.TranscodeVideo");
	if (path == QStringLiteral("live_audio_mode"))
		return text_(value == QStringLiteral("copy") ? "Profile.CopyAudio" : "Profile.TranscodeAudio");
	if (value == QStringLiteral("h264") || value == QStringLiteral("hevc") || value == QStringLiteral("vp9") ||
	    value == QStringLiteral("av1"))
		return value == QStringLiteral("h264") ? QStringLiteral("H.264") : value.toUpper();
	if (value == QStringLiteral("cqp") || value == QStringLiteral("crf"))
		return value.toUpper();
	if (value == QStringLiteral("h264_nvenc"))
		return QStringLiteral("NVIDIA NVENC H.264");
	if (value == QStringLiteral("libsvtav1"))
		return QStringLiteral("SVT-AV1");
	if (value == QStringLiteral("aac"))
		return QStringLiteral("AAC");
	if (value == QStringLiteral("LC"))
		return QStringLiteral("AAC LC");
	if (value == QStringLiteral("yuv420p"))
		return QStringLiteral("YUV 4:2:0");
	if (value == QStringLiteral("bt709"))
		return QStringLiteral("BT.709");
	// names сопоставляет API-значения ключам локализации только для понятных человеку подписей.
	const QMap<QString, const char *> names{{QStringLiteral("auto"), "Profile.Option.Auto"},
						{QStringLiteral("baseline"), "Profile.Option.Baseline"},
						{QStringLiteral("main"), "Profile.Option.Main"},
						{QStringLiteral("high"), "Profile.Option.High"},
						{QStringLiteral("limited"), "Profile.Option.Limited"},
						{QStringLiteral("full"), "Profile.Option.Full"},
						{QStringLiteral("tv"), "Profile.Option.Tv"},
						{QStringLiteral("pc"), "Profile.Option.Pc"},
						{QStringLiteral("cbr"), "Profile.Option.Cbr"},
						{QStringLiteral("vbr"), "Profile.Option.Vbr"},
						{QStringLiteral("hq"), "Profile.Option.Hq"},
						{QStringLiteral("ll"), "Profile.Option.Ll"},
						{QStringLiteral("ull"), "Profile.Option.Ull"},
						{QStringLiteral("disabled"), "Profile.Option.Disabled"},
						{QStringLiteral("qres"), "Profile.Option.Qres"},
						{QStringLiteral("fullres"), "Profile.Option.Fullres"},
						{QStringLiteral("mono"), "Profile.Option.Mono"},
						{QStringLiteral("stereo"), "Profile.Option.Stereo"},
						{QStringLiteral("psnr"), "Profile.Option.Psnr"},
						{QStringLiteral("vq"), "Profile.Option.Vq"}};
	return names.contains(value) ? text_(names.value(value)) : value;
}
// Перестраивает только каталог поля path; текущее значение сохраняется, даже если стало неприменимым.
void ProfileEditor::Impl::choices(const QString &path, const QStringList &options)
{
	// field задаёт конечный список и тип его технических данных.
	for (const auto &field : fields_) {
		if (field.path != path || !field.finite)
			continue;
		// combo и selected сохраняют текущий выбор во время обновления каталога.
		auto *combo = qobject_cast<QComboBox *>(field.input);
		const auto selected = combo->currentData();
		combo->clear();
		// option — одно разрешённое API-значение; пустота добавляется только самим каталогом.
		for (const auto &option : options)
			combo->addItem(option.isEmpty() ? text_("Profile.Unspecified") : optionLabel(path, option),
				       field.bytes ? QVariant(option.toUtf8()) : QVariant(option));
		// data сохраняет незаполненный выбор; display содержит исходную строку для недоступного пункта.
		const auto data = selected.isValid() ? selected
				  : field.bytes      ? QVariant(QByteArray{})
						     : QVariant(QString{});
		const auto display = field.bytes ? QString::fromUtf8(data.toByteArray()) : data.toString();
		selectValue(combo, data,
			    display.isEmpty() ? text_("Editor.Choose") : text_("Profile.UnknownValue").arg(display));
		return;
	}
}

// Получает актуальные каталоги из общей проекции, не исправляя пользовательский ввод.
void ProfileEditor::Impl::updateApplicability()
{
	// profile нужен только для зависимостей; ignored не превращает частичный черновик в допустимый профиль.
	api::GroupProfile profile;
	QString ignored;
	project(profile, ignored, false);
	// field получает каталог и применимость независимо от валидности остальных полей.
	for (const auto &field : fields_) {
		if (field.finite)
			choices(field.path, api::profileOptions(profile, wirePath(field.path)));
		field.input->setEnabled(api::profileFieldApplicable(profile, wirePath(field.path)));
		if (field.path == QStringLiteral("video.target"))
			field.caption->setText(text_(profile.mode == QStringLiteral("copy") ? "Profile.VideoStubBitrate"
											    : "Profile.TargetBitrate"));
		if (field.path == QStringLiteral("video.maximum"))
			field.caption->setText(text_(profile.mode == QStringLiteral("copy")
							     ? "Profile.VideoStubMaxBitrate"
							     : "Profile.MaxBitrate"));
		if (field.path == QStringLiteral("audio.bitrate"))
			field.caption->setText(text_(profile.liveAudioMode.value == QStringLiteral("transcode")
							     ? "Profile.AudioBitrate"
							     : "Profile.AudioStubBitrate"));
		field.input->setAccessibleName(field.caption->text());
	}
	// item адресует только именованные custom-пары, не свободную командную строку.
	for (auto item = custom_.cbegin(); item != custom_.cend(); ++item) {
		// path является публичным путём пары, selected сохраняет точный прежний выбор.
		const auto path = wirePath(QStringLiteral("custom.") + item.key());
		const auto selected = item.value()->currentData().toString();
		item.value()->clear();
		// option получен из того же каталога, что используется сериализатором.
		for (const auto &option : api::profileOptions(profile, path))
			item.value()->addItem(option.isEmpty() ? text_("Profile.OptionAbsent") : option, option);
		selectValue(item.value(), selected,
			    selected.isEmpty() ? text_("Profile.OptionAbsent")
					       : text_("Profile.UnknownValue").arg(selected));
		item.value()->setEnabled(api::profileFieldApplicable(profile, path));
	}
}

// Создаёт редактор внутри родительской страницы и передаёт имя в общую колонку формы.
ProfileEditor::ProfileEditor(TextLookup text, QWidget *parent, QWidget *nameInput)
	: QWidget(parent),
	  impl_(std::make_unique<Impl>(*this, std::move(text), nameInput))
{
}
// Освобождает описания привязок; Qt удаляет дочерние элементы формы.
ProfileEditor::~ProfileEditor() = default;

// Отражает profile без пользовательских изменений; creating оставляет обязательный новый формат незаполненным.
void ProfileEditor::setProfile(const api::GroupProfile &profile, bool creating)
{
	impl_->loading_ = true;
	impl_->original_ = profile;
	impl_->creating_ = creating;
	impl_->dirty_.clear();
	clearUnknownValues(impl_->fps_);
	impl_->preset_->setCurrentIndex(0);
	impl_->advanced_->setChecked(false);
	impl_->resolution_->setEditText(
		creating && profile.format.video.width == 0 && profile.format.video.height == 0
			? QString{}
			: QStringLiteral("%1 × %2").arg(profile.format.video.width).arg(profile.format.video.height));
	// rate сохраняет исходные числитель и знаменатель без сокращения, даже у нестандартной частоты.
	const auto rate = QStringLiteral("%1/%2")
				  .arg(profile.format.video.frameRate.numerator)
				  .arg(profile.format.video.frameRate.denominator);
	selectValue(impl_->fps_, creating && !profile.format.video.frameRate.numerator ? QString{} : rate,
		    impl_->text_("Profile.ExistingFps")
			    .arg(profile.format.video.frameRate.numerator)
			    .arg(profile.format.video.frameRate.denominator),
		    false);
	// display сохраняет исходные значения; неполный новый рецепт не наследует значения другой карточки.
	auto display = profile;
	if (!display.liveAudioMode.hasValue())
		display.liveAudioMode.value.clear();
	if (!display.encoding.hasValue()) {
		display.encoding.value = {};
		display.encoding.value.audioEncoder = QStringLiteral("aac");
		display.encoding.value.rescaleOutput = profile.mode == QStringLiteral("transcode");
	}
	// field получает точное представление своего типа; обработчик visit не изменяет исходный DTO.
	for (const auto &field : impl_->fields_) {
		// combo удаляет только временный пункт другой карточки перед установкой нового исходного значения.
		if (auto *combo = qobject_cast<QComboBox *>(field.input))
			clearUnknownValues(combo);
		std::visit(
			[this, &field](auto *value) {
				// Value обозначает фактический тип поля; text содержит только его отображение.
				using Value = std::remove_pointer_t<decltype(value)>;
				QString text;
				if constexpr (std::is_same_v<Value, bool>) {
					text = *value ? QStringLiteral("true") : QStringLiteral("false");
				} else if constexpr (std::is_same_v<Value, api::ProfileText>) {
					text = QString::fromUtf8(value->bytes);
					selectValue(qobject_cast<QComboBox *>(field.input), value->bytes,
						    text.toUtf8() == value->bytes
							    ? impl_->text_("Profile.UnknownValue").arg(text)
							    : impl_->text_("Profile.OpaqueValue"));
					return;
				} else if constexpr (std::is_same_v<Value, QString>)
					text = *value;
				else {
					text = QString::number(*value);
					if constexpr (std::is_same_v<Value, quint64>)
						if (!text.isEmpty() && isBitrate(field.path))
							text = bitrateText(*value);
				}
				if (field.path == QStringLiteral("encoding.qp") &&
				    !impl_->original_.encoding.value.qp.hasValue())
					text.clear();
				if (field.path == QStringLiteral("encoding.crf"))
					text = impl_->original_.encoding.value.crfHundredths.hasValue()
						       ? crfText(impl_->original_.encoding.value.crfHundredths.value)
						       : QString{};
				// line и combo выбирают фактическое представление текущего типизированного поля.
				if (auto *line = qobject_cast<QLineEdit *>(field.input))
					line->setText(text);
				else if (auto *combo = qobject_cast<QComboBox *>(field.input)) {
					selectValue(combo, text, impl_->text_("Profile.UnknownValue").arg(text));
				}
			},
			profileValue(display, field.path));
	}
	// item проходит разрешённые поля; combo показывает значение пары без пересборки исходной строки.
	for (auto item = impl_->custom_.cbegin(); item != impl_->custom_.cend(); ++item) {
		// key и combo обозначают отдельный ввод, source содержит исходную строку соответствующего кодировщика.
		const auto key = item.key();
		auto *combo = item.value();
		const auto source = key == QStringLiteral("aac_coder") ? display.encoding.value.audioCustom
								       : display.encoding.value.videoCustom;
		clearUnknownValues(combo);
		selectValue(combo, QString{}, impl_->text_("Profile.OptionAbsent"));
		// pair сохраняет отдельный токен API; пользователь не редактирует произвольный CLI.
		for (const auto &pair : source.split(QLatin1Char(' '), Qt::SkipEmptyParts))
			if (pair.startsWith(key + QLatin1Char('=')))
				selectValue(combo, pair.mid(key.size() + 1),
					    impl_->text_("Profile.UnknownValue").arg(pair.mid(key.size() + 1)));
	}
	// item показывает исходные неизвестные пары отдельно; известные значения остаются в собственных контролах.
	for (auto item = impl_->sourceCustom_.cbegin(); item != impl_->sourceCustom_.cend(); ++item) {
		// source содержит одну исходную строку, unsupported отмечает отличающуюся неподдерживаемую часть.
		const auto source = item.key() == QStringLiteral("audio") ? display.encoding.value.audioCustom
									  : display.encoding.value.videoCustom;
		const bool unsupported = source.split(QLatin1Char(' '), Qt::SkipEmptyParts).join(QLatin1Char(' ')) !=
					 knownCustomOptions(source, item.key() == QStringLiteral("audio"));
		item.value()->clear();
		item.value()->addItem(impl_->text_("Profile.RemoveUnsupportedOptions"), QString{});
		if (unsupported)
			selectValue(item.value(), source, impl_->text_("Profile.UnknownValue").arg(source));
		item.value()->setVisible(unsupported);
		impl_->sourceCaptions_[item.key()]->setVisible(unsupported);
	}
	impl_->updateApplicability();
	impl_->updateBitrateHelp();
	impl_->loading_ = false;
}

// Возвращает RAM-снимок известных привязок, исходного DTO и пользовательского ввода без валидации.
QVariantMap ProfileEditor::captureDraft() const
{
	// values содержит строки, точные данные пунктов и флаги каждого структурного поля.
	QVariantMap values;
	// field проходит только объявленные привязки, исключая внутренние служебные QObject.
	for (const auto &field : impl_->fields_) {
		// value хранит исходный текст пункта для неизвестного значения либо непосредственно строку/флаг.
		QVariant value;
		// line предоставляет сырой ввод, combo — данные и подпись выбранного пункта.
		if (auto *line = qobject_cast<QLineEdit *>(field.input))
			value = line->text();
		else if (auto *combo = qobject_cast<QComboBox *>(field.input))
			value = QVariantMap{{QStringLiteral("data"), combo->currentData()},
					    {QStringLiteral("text"), combo->currentText()}};
		values.insert(field.path, value);
	}
	// custom содержит отдельные разрешённые опции; item проходит их точные привязки.
	QVariantMap custom;
	for (auto item = impl_->custom_.cbegin(); item != impl_->custom_.cend(); ++item)
		custom.insert(item.key(), item.value()->currentData());
	// sourceCustom сохраняет отдельное явное решение о неизвестных исходных парах.
	QVariantMap sourceCustom;
	// item адресует одну дорожку и выбранное исходное значение либо его явное удаление.
	for (auto item = impl_->sourceCustom_.cbegin(); item != impl_->sourceCustom_.cend(); ++item)
		sourceCustom.insert(item.key(), item.value()->currentData());
	return {{QStringLiteral("original"), QVariant::fromValue(impl_->original_)},
		{QStringLiteral("creating"), impl_->creating_},
		{QStringLiteral("dirty"), QStringList(impl_->dirty_.values())},
		{QStringLiteral("fields"), values},
		{QStringLiteral("custom"), custom},
		{QStringLiteral("sourceCustom"), sourceCustom},
		{QStringLiteral("resolution"), impl_->resolution_->currentText()},
		{QStringLiteral("fps"), impl_->fps_->currentData()},
		{QStringLiteral("fpsText"), impl_->fps_->currentText()},
		{QStringLiteral("preset"), impl_->preset_->currentIndex()},
		{QStringLiteral("advanced"), impl_->advanced_->isChecked()}};
}

// Восстанавливает собственный RAM-снимок, не превращая отражённые сигналы в новое намерение пользователя.
void ProfileEditor::restoreDraft(const QVariantMap &draft)
{
	if (!draft.value(QStringLiteral("original")).canConvert<api::GroupProfile>())
		return;
	setProfile(draft.value(QStringLiteral("original")).value<api::GroupProfile>(),
		   draft.value(QStringLiteral("creating")).toBool());
	impl_->loading_ = true;
	// values содержит полный сырой ввод предыдущего показа этой карточки.
	const auto values = draft.value(QStringLiteral("fields")).toMap();
	// field получает строку, QByteArray/QString itemData либо флаг своего объявленного типа.
	for (const auto &field : impl_->fields_) {
		if (!values.contains(field.path))
			continue;
		// value содержит состояние только текущего поля, selection — данные и читаемое имя пункта.
		const auto value = values.value(field.path);
		// line восстанавливает даже невалидную строку, combo возвращает точный выбор без сигналов activated.
		if (auto *line = qobject_cast<QLineEdit *>(field.input))
			line->setText(value.toString());
		else if (auto *combo = qobject_cast<QComboBox *>(field.input)) {
			const auto selection = value.toMap();
			selectValue(combo, selection.value(QStringLiteral("data")),
				    selection.value(QStringLiteral("text")).toString());
		}
	}
	impl_->resolution_->setEditText(draft.value(QStringLiteral("resolution")).toString());
	selectValue(impl_->fps_, draft.value(QStringLiteral("fps")), draft.value(QStringLiteral("fpsText")).toString(),
		    false);
	impl_->preset_->setCurrentIndex(draft.value(QStringLiteral("preset")).toInt());
	// custom возвращает только разрешённые именованные опции; item указывает на существующий ввод.
	const auto custom = draft.value(QStringLiteral("custom")).toMap();
	for (auto item = impl_->custom_.cbegin(); item != impl_->custom_.cend(); ++item)
		selectValue(item.value(), custom.value(item.key()),
			    impl_->text_("Profile.UnknownValue").arg(custom.value(item.key()).toString()));
	// sourceCustom возвращает выбор удаления, не меняя сохранённый исходный документ.
	const auto sourceCustom = draft.value(QStringLiteral("sourceCustom")).toMap();
	// item восстанавливает только существующие закрытые контролы дорожек.
	for (auto item = impl_->sourceCustom_.cbegin(); item != impl_->sourceCustom_.cend(); ++item)
		if (sourceCustom.contains(item.key()))
			selectValue(
				item.value(), sourceCustom.value(item.key()),
				impl_->text_("Profile.UnknownValue").arg(sourceCustom.value(item.key()).toString()));
	impl_->dirty_.clear();
	// path восстанавливает намерение редактировать конкретное поле, включая ещё невалидную строку.
	for (const auto &path : draft.value(QStringLiteral("dirty")).toStringList())
		impl_->dirty_.insert(path);
	impl_->updateApplicability();
	impl_->advanced_->setChecked(draft.value(QStringLiteral("advanced")).toBool());
	impl_->updateBitrateHelp();
	impl_->loading_ = false;
}

// Собирает profile из исходного DTO и изменённых полей; error объясняет отказ без потери исходных байтов.
bool ProfileEditor::readProfile(api::GroupProfile &profile, QString &error) const
{
	if (!impl_->project(profile, error, true))
		return false;
	// validation содержит только локальные доверенные ключи общего валидатора.
	api::Error validation;
	if (api::validateProfile(profile, validation))
		return true;
	if (validation.problem && !validation.problem->errors.isEmpty()) {
		// issue — первая ошибка полного рецепта; подпись поля находится по wire-пути.
		const auto &issue = validation.problem->errors.front();
		// caption остаётся безопасным названием профиля при неизвестном локальном пути.
		QString caption = impl_->text_("Editor.Profile");
		// field связывает локальный wire-путь с уже переведённой подписью формы.
		for (const auto &field : impl_->fields_)
			if (issue.location == wirePath(field.path) ||
			    issue.location == QStringLiteral("profile.") + wirePath(field.path))
				caption = field.label;
		error = issue.messageKey.isEmpty()
				? impl_->text_("Profile.InvalidField").arg(caption)
				: caption + QStringLiteral(": ") + impl_->text_(issue.messageKey.toUtf8().constData());
	} else
		error = impl_->text_("Profile.RequiredFormat");
	return false;
}

// Собирает проекцию текущего ввода для каталогов либо полной проверки без изменения самих виджетов.
bool ProfileEditor::Impl::project(api::GroupProfile &profile, QString &error, bool strict) const
{
	profile = original_;
	error.clear();
	profile.encoding.state = api::FieldState::Value;
	profile.liveAudioMode.state = api::FieldState::Value;
	// invalid перечисляет строки, которые пока нельзя представить в точном типе DTO.
	QSet<QString> invalid;
	// field читается независимо от текущей доступности: скрытый ввод нужен для возврата к прежнему режиму.
	for (const auto &field : fields_) {
		// valid отражает точность преобразования; посетитель меняет только одно типизированное поле.
		const bool valid = std::visit(
			[&field](auto *value) {
				// Value задаёт физический диапазон; line и combo указывают на существующее представление поля.
				using Value = std::remove_pointer_t<decltype(value)>;
				auto *line = qobject_cast<QLineEdit *>(field.input);
				auto *combo = qobject_cast<QComboBox *>(field.input);
				// text сохраняет ввод без trim, включая незавершённые десятичные значения.
				const auto text = line ? line->text() : combo->currentData().toString();
				if constexpr (std::is_same_v<Value, QString>) {
					*value = text;
					return true;
				} else if constexpr (std::is_same_v<Value, api::ProfileText>) {
					value->bytes = combo->currentData().toByteArray();
					return true;
				} else if constexpr (std::is_same_v<Value, bool>) {
					if (text != QStringLiteral("false") && text != QStringLiteral("true"))
						return false;
					*value = text == QStringLiteral("true");
					return true;
				} else {
					if constexpr (std::is_same_v<Value, int>)
						if (field.path == QStringLiteral("encoding.crf"))
							return text.isEmpty() || parseCrf(text, *value);
					if (field.path == QStringLiteral("encoding.qp") && text.isEmpty())
						return true;
					if constexpr (std::is_same_v<Value, quint64>)
						if (isBitrate(field.path))
							return parseBitrate(text, *value);
					// ok подтверждает диапазон; parsed не проходит через плавающую точку.
					bool ok = false;
					if constexpr (std::is_signed_v<Value>) {
						const auto parsed = text.toLongLong(&ok);
						if (!ok ||
						    !QRegularExpression(QStringLiteral("^-?(0|[1-9][0-9]*)$"))
							     .match(text)
							     .hasMatch() ||
						    parsed < std::numeric_limits<Value>::min() ||
						    parsed > std::numeric_limits<Value>::max())
							return false;
						*value = static_cast<Value>(parsed);
					} else {
						const auto parsed = text.toULongLong(&ok);
						if (!ok ||
						    !QRegularExpression(QStringLiteral("^(0|[1-9][0-9]*)$"))
							     .match(text)
							     .hasMatch() ||
						    parsed > std::numeric_limits<Value>::max())
							return false;
						*value = static_cast<Value>(parsed);
					}
					return true;
				}
			},
			profileValue(profile, field.path));
		if (!valid)
			invalid.insert(field.path);
	}
	// dimensions проверяет целые стороны без округления; widthOk/heightOk подтверждают физический диапазон.
	const auto dimensions =
		QRegularExpression(QStringLiteral("^([0-9]+)\\s*[x×]\\s*([0-9]+)$")).match(resolution_->currentText());
	bool widthOk = false;
	bool heightOk = false;
	// width и height остаются широкими целыми до проверки границы uint32.
	const auto width = dimensions.captured(1).toULongLong(&widthOk);
	const auto height = dimensions.captured(2).toULongLong(&heightOk);
	if (dimensions.hasMatch() && widthOk && heightOk && width <= std::numeric_limits<quint32>::max() &&
	    height <= std::numeric_limits<quint32>::max()) {
		profile.format.video.width = quint32(width);
		profile.format.video.height = quint32(height);
	} else if (strict) {
		error = text_("Profile.InvalidField").arg(text_("Profile.Resolution"));
		return false;
	}
	// rate сохраняет выбранную дробь; numeratorOk/denominatorOk проверяют точные uint32.
	const auto rate = fps_->currentData().toString().split(QLatin1Char('/'));
	bool numeratorOk = false;
	bool denominatorOk = false;
	// numerator/denominator не сокращаются: исходная форма дроби остаётся в рецепте.
	const auto numerator = rate.value(0).toUInt(&numeratorOk);
	const auto denominator = rate.value(1).toUInt(&denominatorOk);
	if (rate.size() == 2 && numeratorOk && denominatorOk)
		profile.format.video.frameRate = {numerator, denominator};
	else if (strict) {
		error = text_("Profile.InvalidField").arg(text_("Profile.Fps"));
		return false;
	}
	profile.encoding.value.qp.state =
		qobject_cast<QLineEdit *>(input(QStringLiteral("encoding.qp")))->text().isEmpty()
			? api::FieldState::Missing
			: api::FieldState::Value;
	profile.encoding.value.crfHundredths.state =
		qobject_cast<QLineEdit *>(input(QStringLiteral("encoding.crf")))->text().isEmpty()
			? api::FieldState::Missing
			: api::FieldState::Value;
	profile.encoding.value.nvenc.state =
		api::profileFieldApplicable(profile, QStringLiteral("encoding.nvenc.preset"))
			? api::FieldState::Value
			: api::FieldState::Missing;
	profile.encoding.value.svtAv1.state =
		api::profileFieldApplicable(profile, QStringLiteral("encoding.svt_av1.preset"))
			? api::FieldState::Value
			: api::FieldState::Missing;
	// item применяет только явное решение удалить неизвестные пары; известный ввод сохраняется.
	for (auto item = sourceCustom_.cbegin(); item != sourceCustom_.cend(); ++item) {
		if (!dirty_.contains(QStringLiteral("custom_source.") + item.key()) ||
		    !item.value()->currentData().toString().isEmpty())
			continue;
		// source относится к одной дорожке полного рецепта.
		auto &source = item.key() == QStringLiteral("audio") ? profile.encoding.value.audioCustom
								     : profile.encoding.value.videoCustom;
		source = knownCustomOptions(source, item.key() == QStringLiteral("audio"));
	}
	// item проецирует только известные отдельные пары; неизвестные исходные токены сохраняются для проверки.
	for (auto item = custom_.cbegin(); item != custom_.cend(); ++item) {
		// source указывает на исходную строку нужного кодировщика.
		auto &source = item.key() == QStringLiteral("aac_coder") ? profile.encoding.value.audioCustom
									 : profile.encoding.value.videoCustom;
		if (dirty_.contains(QStringLiteral("custom.") + item.key()))
			source = replaceCustomOption(source, item.key(), item.value()->currentData().toString());
	}
	if (!strict)
		return true;
	// field проверяет конечные каталоги и точный ввод только применимых параметров.
	for (const auto &field : fields_) {
		if (!api::profileFieldApplicable(profile, wirePath(field.path)))
			continue;
		if (invalid.contains(field.path) ||
		    (field.finite &&
		     !api::profileOptions(profile, wirePath(field.path))
			      .contains(qobject_cast<QComboBox *>(field.input)->currentData().toString()))) {
			error = text_("Profile.InvalidField").arg(field.label);
			return false;
		}
	}
	// item проверяет выбранные значения разрешённых дополнительных опций до их отправки.
	for (auto item = custom_.cbegin(); item != custom_.cend(); ++item) {
		// path относится к одной известной API-паре, value хранит её точное значение.
		const auto path = wirePath(QStringLiteral("custom.") + item.key());
		const auto value = item.value()->currentData().toString();
		if (api::profileFieldApplicable(profile, path)) {
			if (!api::profileOptions(profile, path).contains(value)) {
				error = text_("Profile.InvalidField").arg(text_("Profile.Custom"));
				return false;
			}
		} else {
			// source содержит одну строку custom; удаление пары не меняет её RAM-ввод в виджете.
			auto &source = item.key() == QStringLiteral("aac_coder") ? profile.encoding.value.audioCustom
										 : profile.encoding.value.videoCustom;
			source = replaceCustomOption(source, item.key(), {});
		}
	}
	if (!api::profileFieldApplicable(profile, QStringLiteral("encoding.qp")))
		profile.encoding.value.qp.state = api::FieldState::Missing;
	if (!api::profileFieldApplicable(profile, QStringLiteral("encoding.crf")))
		profile.encoding.value.crfHundredths.state = api::FieldState::Missing;
	profile.encoding.value.nvenc.state =
		api::profileFieldApplicable(profile, QStringLiteral("encoding.nvenc.preset"))
			? api::FieldState::Value
			: api::FieldState::Missing;
	profile.encoding.value.svtAv1.state =
		api::profileFieldApplicable(profile, QStringLiteral("encoding.svt_av1.preset"))
			? api::FieldState::Value
			: api::FieldState::Missing;
	return true;
}
} // namespace gost
