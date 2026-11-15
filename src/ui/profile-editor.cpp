#include "profile-editor.hpp"

#include <QCheckBox>
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

// Разбирает кбит/с без double; допускает точку или запятую и до трёх знаков, проверяет uint64 до умножения.
bool parseBitrate(const QString &text, quint64 &bits)
{
	// match отделяет целую часть от тысячных кбит/с и отвергает знаки, экспоненты и округление.
	const auto match = QRegularExpression(QStringLiteral("^(0|[1-9][0-9]*)(?:[.,]([0-9]{1,3}))?$")).match(text);
	if (!match.hasMatch())
		return false;
	// ok подтверждает представимость целой части; whole и fraction измеряются в кбит/с и бит/с соответственно.
	bool ok = false;
	const auto whole = match.captured(1).toULongLong(&ok);
	const auto fraction = match.captured(2).leftJustified(3, QLatin1Char('0')).toUInt();
	if (!ok || whole > (std::numeric_limits<quint64>::max() - fraction) / 1000)
		return false;
	bits = whole * 1000 + fraction;
	return true;
}

// Создаёт конечный целочисленный каталог first..last для документированных параметров кодировщика.
QStringList integerOptions(int first, int last)
{
	// result содержит точные строки API; value последовательно проходит разрешённые целые.
	QStringList result;
	for (int value = first; value <= last; ++value)
		result.append(QString::number(value));
	return result;
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
	profile.encoding.value = {2,
				  av1 ? QStringLiteral("libsvtav1") : QStringLiteral("h264_nvenc"),
				  QStringLiteral("aac"),
				  QStringLiteral("cbr"),
				  !av1,
				  {},
				  {},
				  {},
				  {}};
	if (av1)
		profile.encoding.value.svtAv1 = {api::FieldState::Value, {8, QStringLiteral("psnr"), 2, 0}};
	else
		profile.encoding.value.nvenc = {api::FieldState::Value,
						{QStringLiteral("p4"), QStringLiteral("hq"), QStringLiteral("disabled"),
						 0, false, false, 8}};
	return profile;
}

// Выбирает data в закрытом списке; неизвестное значение остаётся отдельным пунктом с точными байтами/строкой.
void selectValue(QComboBox *combo, const QVariant &data, const QString &unknownLabel)
{
	// index адресует точное itemData, а не локализованную подпись.
	int index = combo->findData(data);
	if (index < 0) {
		combo->addItem(unknownLabel, data);
		index = combo->count() - 1;
		combo->setItemData(index, true, Qt::UserRole + 1);
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

// Заменяет одну разрешённую пару key=value в source, сохраняя остальные серверные токены.
QString replaceCustomOption(const QString &source, const QString &key, const QString &value)
{
	// pairs сохраняет все незатронутые опции; pair проходит токены серверного ASCII-space представления.
	QStringList pairs;
	for (const auto &pair : source.split(QLatin1Char(' '), Qt::SkipEmptyParts))
		if (!pair.startsWith(key + QLatin1Char('=')))
			pairs.append(pair);
	if (!value.isEmpty())
		pairs.append(key + QLatin1Char('=') + value);
	return pairs.join(QLatin1Char(' '));
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

/** Привязывает элементы формы к известным полям DTO. Исходный профиль копируется,
 * поэтому неизменённый текст не восстанавливается из QString и не теряет байты. */
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
		// caption показывает зависимую от copy/transcode подпись, не меняя самого значения.
		QLabel *caption = nullptr;
	};
	// Создаёт разделы и привязки; owner и text задают QObject-контекст и перевод подписей.
	// nameInput, если передан, размещается перед параметрами в общей колонке ввода.
	Impl(ProfileEditor &owner, TextLookup text, QWidget *nameInput);
	// Добавляет в form поле DTO path с переводом подписи label; options задаёт закрытый список.
	// bytes хранит ProfileText как QByteArray, boolean выбирает флажок вместо строки или списка.
	void add(QFormLayout *form, const char *path, const char *label, const QStringList &options = {},
		 bool bytes = false, bool boolean = false);
	// Добавляет локализованную группу формы к общему layout.
	QFormLayout *section(QVBoxLayout *layout, const char *title);
	// Регистрирует явное изменение path и уведомляет внешний виджет, кроме периода отражения DTO.
	void touch(const QString &path);
	// Устанавливает доступность применимых блоков, не меняя скрытую обработку LIVE AAC.
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
	// recipe_ различает Missing/null/v2; nvenc_ и svt_ включают явные необязательные подблоки.
	QComboBox *recipe_ = nullptr;
	QCheckBox *nvenc_ = nullptr;
	QCheckBox *svt_ = nullptr;
	// custom_ хранит только разрешённые отдельные параметры; исходные строки сохраняются до их изменения.
	QMap<QString, QComboBox *> custom_;
};

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

void ProfileEditor::Impl::add(QFormLayout *form, const char *path, const char *label, const QStringList &options,
			      bool bytes, bool boolean)
{
	// field содержит адрес DTO, подпись и созданный ввод этой строки.
	Field field{QString::fromLatin1(path), text_(label), nullptr, bytes};
	if (boolean) {
		// check отражает булево значение; обработчик отмечает только явное переключение пользователя.
		auto *check = new QCheckBox(&owner_);
		field.input = check;
		QObject::connect(check, &QCheckBox::toggled, &owner_,
				 [this, path] { touch(QString::fromLatin1(path)); });
	} else if (bytes || !options.isEmpty()) {
		// combo хранит точное API-значение в itemData; неизвестное исходное значение добавляется при загрузке.
		auto *combo = new QComboBox(&owner_);
		combo->addItem(text_(bytes ? "Profile.Unspecified" : "Editor.Choose"),
			       bytes ? QVariant(QByteArray{}) : QVariant(QString{}));
		// option содержит один документированный вариант, используемый без нормализации.
		for (const auto &option : options)
			if (!option.isEmpty())
				combo->addItem(optionLabel(field.path, option),
					       bytes ? QVariant(option.toUtf8()) : QVariant(option));
		field.input = combo;
		// activated возникает только после пользовательского выбора, а не setCurrentIndex.
		QObject::connect(combo, &QComboBox::activated, &owner_,
				 [this, path] { touch(QString::fromLatin1(path)); });
	} else {
		// line сохраняет полный текст, в том числе точные десятичные uint64; обработчик не обрабатывает setText.
		auto *line = new QLineEdit(&owner_);
		line->setMaxLength(std::numeric_limits<int>::max());
		field.input = line;
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
	add(basics, "mode", "Profile.VideoMode", {QStringLiteral("copy"), QStringLiteral("transcode")});
	add(basics, "video.codec", "Profile.Codec", {QStringLiteral("h264"), QStringLiteral("av1")});
	qobject_cast<QComboBox *>(input(QStringLiteral("video.codec")))->setItemText(1, QStringLiteral("H.264"));
	qobject_cast<QComboBox *>(input(QStringLiteral("video.codec")))->setItemText(2, QStringLiteral("AV1"));
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
	add(basics, "video.target", "Profile.TargetBitrate");
	add(basics, "live_audio_mode", "Profile.LiveAudio", {QStringLiteral("copy"), QStringLiteral("transcode")});
	add(basics, "audio.bitrate", "Profile.AudioBitrate");
	qobject_cast<QComboBox *>(input(QStringLiteral("mode")))->setItemText(1, text_("Profile.CopyVideo"));
	qobject_cast<QComboBox *>(input(QStringLiteral("mode")))->setItemText(2, text_("Profile.TranscodeVideo"));
	qobject_cast<QComboBox *>(input(QStringLiteral("live_audio_mode")))->setItemText(1, text_("Profile.CopyAudio"));
	qobject_cast<QComboBox *>(input(QStringLiteral("live_audio_mode")))
		->setItemText(2, text_("Profile.TranscodeAudio"));
	qobject_cast<QComboBox *>(input(QStringLiteral("live_audio_mode")))
		->setItemText(0, text_("Profile.LegacyAudio"));
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
	add(video, "video.codec_profile", "Profile.CodecProfile", {}, true);
	add(video, "video.level", "Profile.Level", {}, true);
	add(video, "video.tier", "Profile.Tier", {}, true);
	add(video, "video.pixel_format", "Profile.PixelFormat", {QStringLiteral("yuv420p")}, true);
	add(video, "video.bit_depth", "Profile.BitDepth", {QStringLiteral("8")});
	add(video, "video.color.range", "Profile.ColorRange",
	    {QStringLiteral("limited"), QStringLiteral("full"), QStringLiteral("tv"), QStringLiteral("pc")}, true);
	add(video, "video.color.primaries", "Profile.ColorPrimaries", {QStringLiteral("bt709")}, true);
	add(video, "video.color.transfer", "Profile.ColorTransfer", {QStringLiteral("bt709")}, true);
	add(video, "video.color.matrix", "Profile.ColorMatrix", {QStringLiteral("bt709")}, true);
	// audio содержит ожидаемые параметры AAC, а не измеренные характеристики текущего LIVE.
	auto *audio = section(advancedLayout, "Profile.AudioFormat");
	add(audio, "audio.codec", "Profile.Codec", {QStringLiteral("aac")});
	add(audio, "audio.codec_profile", "Profile.CodecProfile", {QStringLiteral("LC")}, true);
	add(audio, "audio.sample_rate", "Profile.SampleRate",
	    {QStringLiteral("7350"), QStringLiteral("8000"), QStringLiteral("11025"), QStringLiteral("12000"),
	     QStringLiteral("16000"), QStringLiteral("22050"), QStringLiteral("24000"), QStringLiteral("32000"),
	     QStringLiteral("44100"), QStringLiteral("48000"), QStringLiteral("64000")});
	add(audio, "audio.channels", "Profile.Channels", {QStringLiteral("1"), QStringLiteral("2")});
	add(audio, "audio.channel_layout", "Profile.ChannelLayout", {}, true);
	// rates содержит точные числа рецепта: биты/с, биты и количество кадров.
	auto *rates = section(advancedLayout, "Profile.Bitrates");
	add(rates, "video.maximum", "Profile.MaxBitrate");
	add(rates, "video.buffer", "Profile.Buffer");
	add(rates, "video.gop", "Profile.Gop");
	add(rates, "video.b_frames", "Profile.BFrames", integerOptions(0, 4));
	add(rates, "video.closed_gop", "Profile.ClosedGop", {}, false, true);
	// encoding содержит выбор прежнего рецепта или явного v2 и его общие поля.
	auto *encoding = section(advancedLayout, "Profile.Encoding");
	recipe_ = new QComboBox(&owner_);
	recipe_->setObjectName(QStringLiteral("encodingPresence"));
	recipe_->addItem(text_("Profile.EncodingMissing"), 0);
	recipe_->addItem(text_("Profile.EncodingNull"), 1);
	recipe_->addItem(text_("Profile.EncodingV2"), 2);
	encoding->addRow(text_("Profile.Recipe"), recipe_);
	// Переключение рецепта отмечается отдельно от полей; удаление сохранённого v2 отвергается при чтении.
	QObject::connect(recipe_, &QComboBox::activated, &owner_, [this] { touch(QStringLiteral("encoding")); });
	add(encoding, "encoding.video_encoder", "Profile.VideoEncoder",
	    {QStringLiteral("h264_nvenc"), QStringLiteral("libsvtav1")});
	add(encoding, "encoding.audio_encoder", "Profile.AudioEncoder", {QStringLiteral("aac")});
	add(encoding, "encoding.rate_control", "Profile.RateControl", {QStringLiteral("cbr"), QStringLiteral("vbr")});
	add(encoding, "encoding.rescale", "Profile.Rescale", {}, false, true);
	// nvenc содержит только документированные настройки h264_nvenc.
	auto *nvenc = section(advancedLayout, "Profile.Nvenc");
	nvenc_ = new QCheckBox(text_("Profile.IncludeNvenc"), &owner_);
	nvenc_->setObjectName(QStringLiteral("includeNvenc"));
	nvenc->addRow(nvenc_);
	// Пользователь явно включает/удаляет необязательный NVENC-блок.
	QObject::connect(nvenc_, &QCheckBox::clicked, &owner_, [this] { touch(QStringLiteral("nvenc")); });
	add(nvenc, "nvenc.preset", "Profile.Preset",
	    {QStringLiteral("p1"), QStringLiteral("p2"), QStringLiteral("p3"), QStringLiteral("p4"),
	     QStringLiteral("p5"), QStringLiteral("p6"), QStringLiteral("p7")});
	add(nvenc, "nvenc.tuning", "Profile.Tuning",
	    {QStringLiteral("hq"), QStringLiteral("ll"), QStringLiteral("ull")});
	add(nvenc, "nvenc.multipass", "Profile.Multipass",
	    {QStringLiteral("disabled"), QStringLiteral("qres"), QStringLiteral("fullres")});
	add(nvenc, "nvenc.lookahead", "Profile.Lookahead", integerOptions(0, 31));
	add(nvenc, "nvenc.spatial", "Profile.SpatialAq", {}, false, true);
	add(nvenc, "nvenc.temporal", "Profile.TemporalAq", {}, false, true);
	add(nvenc, "nvenc.strength", "Profile.AqStrength", integerOptions(1, 15));
	// svt содержит настройки AV1-рецепта заглушки и не включает перекодирование основного copy-потока.
	auto *svt = section(advancedLayout, "Profile.Svt");
	svt_ = new QCheckBox(text_("Profile.IncludeSvt"), &owner_);
	svt_->setObjectName(QStringLiteral("includeSvt"));
	svt->addRow(svt_);
	// Пользователь явно включает/удаляет необязательный блок SVT-AV1.
	QObject::connect(svt_, &QCheckBox::clicked, &owner_, [this] { touch(QStringLiteral("svt")); });
	add(svt, "svt.preset", "Profile.Preset", integerOptions(0, 13));
	add(svt, "svt.tuning", "Profile.Tuning", {QStringLiteral("psnr"), QStringLiteral("vq")});
	add(svt, "svt.aq", "Profile.AqMode", {QStringLiteral("2")});
	add(svt, "svt.lookahead", "Profile.Lookahead", {QStringLiteral("0")});
	// custom содержит отдельные разрешённые API-пары вместо произвольной командной строки.
	auto *custom = section(advancedLayout, "Profile.Custom");
	// key проходит точные имена разрешённых параметров; options содержит допустимые значения текущей опции.
	for (const auto &key : {QStringLiteral("nonref_p"), QStringLiteral("strict_gop"), QStringLiteral("lp"),
				QStringLiteral("aac_coder")}) {
		QStringList options = key == QStringLiteral("aac_coder")
					      ? QStringList{QStringLiteral("twoloop"), QStringLiteral("fast")}
				      : key == QStringLiteral("lp")
					      ? QStringList{QStringLiteral("0"), QStringLiteral("1"),
							    QStringLiteral("2"), QStringLiteral("3"),
							    QStringLiteral("4"), QStringLiteral("5"),
							    QStringLiteral("6")}
					      : QStringList{QStringLiteral("0"), QStringLiteral("1")};
		// combo хранит точное значение пары, пустой пункт означает отсутствие дополнительной опции.
		auto *combo = new QComboBox(&owner_);
		combo->setObjectName(QStringLiteral("custom.") + key);
		combo->addItem(text_("Profile.OptionAbsent"), QString{});
		// option задаёт одно разрешённое значение; для булевых опций подпись объясняет 0/1.
		for (const auto &option : options)
			combo->addItem((key == QStringLiteral("nonref_p") || key == QStringLiteral("strict_gop"))
					       ? text_(option == QStringLiteral("1") ? "Dialog.Yes" : "Dialog.No")
					       : option,
				       option);
		custom_[key] = combo;
		custom->addRow(text_(key == QStringLiteral("nonref_p")     ? "Profile.CustomNonref"
				     : key == QStringLiteral("strict_gop") ? "Profile.CustomStrictGop"
				     : key == QStringLiteral("lp")         ? "Profile.CustomLp"
									   : "Profile.CustomAacCoder"),
			       combo);
		// Обработчик сохраняет конкретное изменённое имя, чтобы незатронутую строку дополнительных параметров оставить побайтно прежней.
		QObject::connect(combo, &QComboBox::activated, &owner_,
				 [this, key] { touch(QStringLiteral("custom.") + key); });
	}
	updateBitrateHelp();
}

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
	if (path == QStringLiteral("audio.channels"))
		return text_(value == QStringLiteral("1") ? "Profile.Option.Mono" : "Profile.Option.Stereo") +
		       QStringLiteral(" (%1)").arg(value);
	if (path == QStringLiteral("svt.lookahead") && value == QStringLiteral("-1"))
		return text_("Profile.Option.Auto");
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
	// field определяет тип itemData и текущее значение единственного обновляемого списка.
	for (const auto &field : fields_) {
		if (field.path != path)
			continue;
		// combo содержит исходный выбор, selected сохраняет его на время перестройки.
		auto *combo = qobject_cast<QComboBox *>(field.input);
		const auto selected = combo->currentData();
		combo->clear();
		combo->addItem(text_(field.bytes ? "Profile.Unspecified" : "Editor.Choose"),
			       field.bytes ? QVariant(QByteArray{}) : QVariant(QString{}));
		// option обозначает один подтверждённый сервером пункт текущего каталога.
		for (const auto &option : options)
			if (!option.isEmpty())
				combo->addItem(optionLabel(path, option),
					       field.bytes ? QVariant(option.toUtf8()) : QVariant(option));
		// display содержит исходное значение для подписи; двоичные байты обозначаются без потерь в itemData.
		const auto display = field.bytes ? QString::fromUtf8(selected.toByteArray()) : selected.toString();
		selectValue(combo, selected,
			    field.bytes && display.toUtf8() != selected.toByteArray()
				    ? text_("Profile.OpaqueValue")
				    : text_("Profile.UnknownValue").arg(display));
		return;
	}
}

// Ограничивает доступные каталоги выбранными параметрами без подстановки новых значений в форму.
void ProfileEditor::Impl::updateApplicability()
{
	// v2 и encoder определяют применимость подблоков; явное audio не переключается вслед за видео.
	const bool v2 = recipe_->currentData().toInt() == 2;
	const auto encoder =
		qobject_cast<QComboBox *>(input(QStringLiteral("encoding.video_encoder")))->currentData().toString();
	// codec выбирает профиль/уровни/число B-кадров, rateControl — допустимый анализ SVT-AV1.
	const auto codec = qobject_cast<QComboBox *>(input(QStringLiteral("video.codec")))->currentData().toString();
	const auto rateControl =
		qobject_cast<QComboBox *>(input(QStringLiteral("encoding.rate_control")))->currentData().toString();
	choices(QStringLiteral("video.codec_profile"),
		codec == QStringLiteral("h264")
			? QStringList{QStringLiteral("baseline"), QStringLiteral("main"), QStringLiteral("high")}
		: codec == QStringLiteral("av1") ? QStringList{QStringLiteral("main")}
						 : QStringList{});
	// levels перечисляет только уровни выбранного кодека; major/minor образуют допустимые AV1 2.0..7.3.
	QStringList levels;
	if (codec == QStringLiteral("h264"))
		levels = {QStringLiteral("auto"), QStringLiteral("1"),   QStringLiteral("1.0"), QStringLiteral("1b"),
			  QStringLiteral("1.0b"), QStringLiteral("1.1"), QStringLiteral("1.2"), QStringLiteral("1.3"),
			  QStringLiteral("2"),    QStringLiteral("2.0"), QStringLiteral("2.1"), QStringLiteral("2.2"),
			  QStringLiteral("3"),    QStringLiteral("3.0"), QStringLiteral("3.1"), QStringLiteral("3.2"),
			  QStringLiteral("4"),    QStringLiteral("4.0"), QStringLiteral("4.1"), QStringLiteral("4.2"),
			  QStringLiteral("5"),    QStringLiteral("5.0"), QStringLiteral("5.1"), QStringLiteral("5.2")};
	else if (codec == QStringLiteral("av1")) {
		levels.append(QStringLiteral("auto"));
		for (int major = 2; major <= 7; ++major)
			for (int minor = 0; minor <= 3; ++minor)
				levels.append(QString::number(major) + QLatin1Char('.') + QString::number(minor));
	}
	choices(QStringLiteral("video.level"), levels);
	choices(QStringLiteral("video.tier"),
		codec == QStringLiteral("av1") ? QStringList{QStringLiteral("main")} : QStringList{});
	choices(QStringLiteral("encoding.video_encoder"),
		codec == QStringLiteral("h264")  ? QStringList{QStringLiteral("h264_nvenc")}
		: codec == QStringLiteral("av1") ? QStringList{QStringLiteral("libsvtav1")}
						 : QStringList{});
	choices(QStringLiteral("video.b_frames"),
		codec == QStringLiteral("av1") ||
				qobject_cast<QComboBox *>(input(QStringLiteral("video.codec_profile")))
						->currentData()
						.toByteArray() == QByteArrayLiteral("baseline")
			? QStringList{QStringLiteral("0")}
			: integerOptions(0, 4));
	// bFrames и bFramesOk задают верхнюю границу NVENC lookahead без изменения выбранного значения.
	bool bFramesOk = false;
	const auto bFrames = qobject_cast<QComboBox *>(input(QStringLiteral("video.b_frames")))
				     ->currentData()
				     .toString()
				     .toUInt(&bFramesOk);
	choices(QStringLiteral("nvenc.lookahead"),
		integerOptions(0, bFramesOk && bFrames <= 31 ? 31 - int(bFrames) : 31));
	choices(QStringLiteral("audio.channel_layout"),
		qobject_cast<QComboBox *>(input(QStringLiteral("audio.channels")))->currentData().toString() ==
				QStringLiteral("1")
			? QStringList{QStringLiteral("mono")}
		: qobject_cast<QComboBox *>(input(QStringLiteral("audio.channels")))->currentData().toString() ==
				QStringLiteral("2")
			? QStringList{QStringLiteral("stereo")}
			: QStringList{});
	choices(QStringLiteral("svt.tuning"), rateControl == QStringLiteral("cbr")
						      ? QStringList{QStringLiteral("psnr")}
						      : QStringList{QStringLiteral("psnr"), QStringLiteral("vq")});
	choices(QStringLiteral("svt.lookahead"), rateControl == QStringLiteral("cbr")
							 ? QStringList{QStringLiteral("0")}
							 : QStringList{QStringLiteral("-1")} + integerOptions(6, 120));
	choices(QStringLiteral("nvenc.strength"),
		qobject_cast<QCheckBox *>(input(QStringLiteral("nvenc.spatial")))->isChecked()
			? integerOptions(1, 15)
			: QStringList{QStringLiteral("8")});
	// audio и audioModel ограничивают явный выбор без скрытой смены наследуемой семантики.
	auto *audio = qobject_cast<QComboBox *>(input(QStringLiteral("live_audio_mode")));
	auto *audioModel = qobject_cast<QStandardItemModel *>(audio->model());
	if (audioModel) {
		audioModel->item(0)->setEnabled(!creating_ && !original_.liveAudioMode.hasValue());
		audioModel->item(2)->setEnabled(
			qobject_cast<QComboBox *>(input(QStringLiteral("mode")))->currentData().toString() ==
			QStringLiteral("transcode"));
	}
	// copyVideo/effectiveAudioTranscode описывают действие параметров, включая наследуемый режим старого профиля.
	const bool copyVideo = qobject_cast<QComboBox *>(input(QStringLiteral("mode")))->currentData().toString() ==
			       QStringLiteral("copy");
	const bool effectiveAudioTranscode = audio->currentData().toString() == QStringLiteral("transcode") ||
					     (audio->currentData().toString().isEmpty() && !copyVideo);
	// field обновляет видимые единицы и область действия битрейта; эти подписи не ограничивают LIVE copy.
	for (const auto &field : fields_) {
		if (field.path == QStringLiteral("video.target"))
			field.caption->setText(text_(copyVideo ? "Profile.VideoStubBitrate" : "Profile.TargetBitrate"));
		if (field.path == QStringLiteral("video.maximum"))
			field.caption->setText(text_(copyVideo ? "Profile.VideoStubMaxBitrate" : "Profile.MaxBitrate"));
		if (field.path == QStringLiteral("audio.bitrate"))
			field.caption->setText(
				text_(effectiveAudioTranscode ? "Profile.AudioBitrate" : "Profile.AudioStubBitrate"));
		field.input->setAccessibleName(field.caption->text());
	}
	// recipeModel блокирует возврат сохранённого современного профиля к прежним настройкам.
	if (auto *recipeModel = qobject_cast<QStandardItemModel *>(recipe_->model())) {
		recipeModel->item(0)->setEnabled(!original_.encoding.hasValue());
		recipeModel->item(1)->setEnabled(!original_.encoding.hasValue());
	}
	nvenc_->setEnabled(v2 && encoder == QStringLiteral("h264_nvenc"));
	svt_->setEnabled(v2 && encoder == QStringLiteral("libsvtav1"));
	// field получает доступность по реальному encoder и явному флагу необязательного подблока.
	for (const auto &field : fields_) {
		if (field.path.startsWith(QStringLiteral("encoding.")))
			field.input->setEnabled(v2);
		if (field.path.startsWith(QStringLiteral("nvenc.")))
			field.input->setEnabled(nvenc_->isEnabled() && nvenc_->isChecked());
		if (field.path.startsWith(QStringLiteral("svt.")))
			field.input->setEnabled(svt_->isEnabled() && svt_->isChecked());
	}
	custom_[QStringLiteral("nonref_p")]->setEnabled(v2 && encoder == QStringLiteral("h264_nvenc"));
	custom_[QStringLiteral("strict_gop")]->setEnabled(v2 && encoder == QStringLiteral("h264_nvenc"));
	custom_[QStringLiteral("lp")]->setEnabled(v2 && encoder == QStringLiteral("libsvtav1"));
	custom_[QStringLiteral("aac_coder")]->setEnabled(v2);
}

ProfileEditor::ProfileEditor(TextLookup text, QWidget *parent, QWidget *nameInput)
	: QWidget(parent),
	  impl_(std::make_unique<Impl>(*this, std::move(text), nameInput))
{
}
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
			    .arg(profile.format.video.frameRate.denominator));
	// display сохраняет исходные поля; значения необязательных блоков используются лишь после явного включения.
	auto display = profile;
	if (!display.liveAudioMode.hasValue())
		display.liveAudioMode.value.clear();
	if (!display.encoding.hasValue()) {
		display.encoding.value = {};
		display.encoding.value.audioEncoder = QStringLiteral("aac");
		display.encoding.value.rescaleOutput = profile.mode == QStringLiteral("transcode");
	}
	if (!display.encoding.value.nvenc.hasValue()) {
		display.encoding.value.nvenc.value = {
			QStringLiteral("p4"), QStringLiteral("hq"), QStringLiteral("disabled"), 0, false, false, 8};
	}
	if (!display.encoding.value.svtAv1.hasValue()) {
		display.encoding.value.svtAv1.value = {
			8, QStringLiteral("psnr"), 2,
			display.encoding.value.rateControl == QStringLiteral("cbr") ? 0 : -1};
	}
	impl_->recipe_->setCurrentIndex(profile.encoding.hasValue() ? 2 : profile.encoding.isNull() ? 1 : 0);
	impl_->nvenc_->setChecked(profile.encoding.hasValue() && profile.encoding->nvenc.hasValue());
	impl_->svt_->setChecked(profile.encoding.hasValue() && profile.encoding->svtAv1.hasValue());
	// field получает точное представление своего типа; обработчик visit не изменяет исходный DTO.
	for (const auto &field : impl_->fields_) {
		// combo удаляет только временный пункт другой карточки перед установкой нового исходного значения.
		if (auto *combo = qobject_cast<QComboBox *>(field.input))
			clearUnknownValues(combo);
		std::visit(
			[this, &field, creating](auto *value) {
				// Value обозначает фактический тип поля; text содержит только его отображение.
				using Value = std::remove_pointer_t<decltype(value)>;
				QString text;
				if constexpr (std::is_same_v<Value, bool>) {
					qobject_cast<QCheckBox *>(field.input)->setChecked(*value);
					return;
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
					text = creating && *value == 0 &&
							       (field.path == QStringLiteral("video.target") ||
								field.path == QStringLiteral("audio.bitrate") ||
								field.path == QStringLiteral("audio.sample_rate") ||
								field.path == QStringLiteral("audio.channels") ||
								field.path == QStringLiteral("video.gop"))
						       ? QString{}
						       : QString::number(*value);
					if constexpr (std::is_same_v<Value, quint64>)
						if (!text.isEmpty() && isBitrate(field.path))
							text = bitrateText(*value);
				}
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
		combo->setCurrentIndex(0);
		// pair сохраняет отдельный токен API; пользователь не редактирует произвольный CLI.
		for (const auto &pair : source.split(QLatin1Char(' '), Qt::SkipEmptyParts))
			if (pair.startsWith(key + QLatin1Char('=')))
				combo->setCurrentIndex(qMax(0, combo->findData(pair.mid(key.size() + 1))));
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
		else
			value = qobject_cast<QCheckBox *>(field.input)->isChecked();
		values.insert(field.path, value);
	}
	// custom содержит отдельные разрешённые опции; item проходит их точные привязки.
	QVariantMap custom;
	for (auto item = impl_->custom_.cbegin(); item != impl_->custom_.cend(); ++item)
		custom.insert(item.key(), item.value()->currentData());
	return {{QStringLiteral("original"), QVariant::fromValue(impl_->original_)},
		{QStringLiteral("creating"), impl_->creating_},
		{QStringLiteral("dirty"), QStringList(impl_->dirty_.values())},
		{QStringLiteral("fields"), values},
		{QStringLiteral("custom"), custom},
		{QStringLiteral("resolution"), impl_->resolution_->currentText()},
		{QStringLiteral("fps"), impl_->fps_->currentData()},
		{QStringLiteral("fpsText"), impl_->fps_->currentText()},
		{QStringLiteral("preset"), impl_->preset_->currentIndex()},
		{QStringLiteral("encoding"), impl_->recipe_->currentData()},
		{QStringLiteral("nvenc"), impl_->nvenc_->isChecked()},
		{QStringLiteral("svt"), impl_->svt_->isChecked()},
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
		} else
			qobject_cast<QCheckBox *>(field.input)->setChecked(value.toBool());
	}
	impl_->resolution_->setEditText(draft.value(QStringLiteral("resolution")).toString());
	selectValue(impl_->fps_, draft.value(QStringLiteral("fps")), draft.value(QStringLiteral("fpsText")).toString());
	impl_->preset_->setCurrentIndex(draft.value(QStringLiteral("preset")).toInt());
	impl_->recipe_->setCurrentIndex(impl_->recipe_->findData(draft.value(QStringLiteral("encoding"))));
	impl_->nvenc_->setChecked(draft.value(QStringLiteral("nvenc")).toBool());
	impl_->svt_->setChecked(draft.value(QStringLiteral("svt")).toBool());
	// custom возвращает только разрешённые именованные опции; item указывает на существующий ввод.
	const auto custom = draft.value(QStringLiteral("custom")).toMap();
	for (auto item = impl_->custom_.cbegin(); item != impl_->custom_.cend(); ++item)
		selectValue(item.value(), custom.value(item.key()),
			    impl_->text_("Profile.UnknownValue").arg(custom.value(item.key()).toString()));
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
	profile = impl_->original_;
	error.clear();
	// recipe различает Missing/null/v2; newRecipe требует собрать обязательные поля нового блока.
	const int recipe = impl_->recipe_->currentData().toInt();
	const bool newRecipe = recipe == 2 && !profile.encoding.hasValue();
	if (profile.encoding.hasValue() && recipe != 2) {
		error = impl_->text_("Profile.CannotRemoveV2");
		return false;
	}
	if (impl_->dirty_.contains(QStringLiteral("encoding")))
		profile.encoding.state = recipe == 2   ? api::FieldState::Value
					 : recipe == 1 ? api::FieldState::Null
						       : api::FieldState::Missing;
	if (recipe == 2) {
		profile.encoding.state = api::FieldState::Value;
		profile.encoding.value.version = 2;
		if (newRecipe || impl_->dirty_.contains(QStringLiteral("encoding.video_encoder")) ||
		    impl_->dirty_.contains(QStringLiteral("nvenc")))
			profile.encoding.value.nvenc.state = impl_->nvenc_->isEnabled() && impl_->nvenc_->isChecked()
								     ? api::FieldState::Value
								     : api::FieldState::Missing;
		if (newRecipe || impl_->dirty_.contains(QStringLiteral("encoding.video_encoder")) ||
		    impl_->dirty_.contains(QStringLiteral("svt")))
			profile.encoding.value.svtAv1.state = impl_->svt_->isEnabled() && impl_->svt_->isChecked()
								      ? api::FieldState::Value
								      : api::FieldState::Missing;
	}
	if (impl_->creating_ || impl_->dirty_.contains(QStringLiteral("video.resolution"))) {
		// dimensions отделяет две целые стороны; widthOk/heightOk подтверждают представимость без округления.
		const auto dimensions = QRegularExpression(QStringLiteral("^([0-9]+)\\s*[x×]\\s*([0-9]+)$"))
						.match(impl_->resolution_->currentText());
		bool widthOk = false;
		bool heightOk = false;
		// width/height остаются uint64 до проверки границы полей uint32.
		const auto width = dimensions.captured(1).toULongLong(&widthOk);
		const auto height = dimensions.captured(2).toULongLong(&heightOk);
		if (!dimensions.hasMatch() || !widthOk || !heightOk || !width || !height ||
		    width > std::numeric_limits<quint32>::max() || height > std::numeric_limits<quint32>::max()) {
			error = impl_->text_("Profile.InvalidField").arg(impl_->text_("Profile.Resolution"));
			return false;
		}
		profile.format.video.width = quint32(width);
		profile.format.video.height = quint32(height);
	}
	if (impl_->creating_ || impl_->dirty_.contains(QStringLiteral("video.fps"))) {
		// rate содержит выбранную точную пару; numeratorOk/denominatorOk проверяют uint32 без double.
		const auto rate = impl_->fps_->currentData().toString().split(QLatin1Char('/'));
		bool numeratorOk = false;
		bool denominatorOk = false;
		// numerator и denominator сохраняют исходное отношение, включая несокращённый нестандартный FPS.
		const auto numerator = rate.value(0).toUInt(&numeratorOk);
		const auto denominator = rate.value(1).toUInt(&denominatorOk);
		if (rate.size() != 2 || !numeratorOk || !denominatorOk || !numerator || !denominator) {
			error = impl_->text_("Profile.InvalidField").arg(impl_->text_("Profile.Fps"));
			return false;
		}
		profile.format.video.frameRate = {numerator, denominator};
	}
	// field применяется только после пользовательского изменения либо для нового обязательного блока.
	for (const auto &field : impl_->fields_) {
		if (!field.input->isEnabled())
			continue;
		// freshBlock требует заполнить поля только что включённого необязательного рецепта.
		const bool freshBlock = (field.path.startsWith(QStringLiteral("encoding.")) && newRecipe) ||
					(field.path.startsWith(QStringLiteral("nvenc.")) &&
					 !impl_->original_.encoding.value.nvenc.hasValue()) ||
					(field.path.startsWith(QStringLiteral("svt.")) &&
					 !impl_->original_.encoding.value.svtAv1.hasValue());
		if (!impl_->creating_ && !freshBlock && !impl_->dirty_.contains(field.path))
			continue;
		// valid отражает проверку фактического типа поля; обработчик visit сохраняет точность целого ввода.
		const bool valid = std::visit(
			[&field](auto *value) {
				// Value — тип конечного DTO-поля, line и combo предоставляют один из вариантов ввода.
				using Value = std::remove_pointer_t<decltype(value)>;
				auto *line = qobject_cast<QLineEdit *>(field.input);
				auto *combo = qobject_cast<QComboBox *>(field.input);
				// text содержит пользовательскую строку без trim или другой нормализации ProfileText.
				const auto text = line    ? line->text()
						  : combo ? combo->currentData().toString()
							  : QString{};
				if constexpr (std::is_same_v<Value, bool>) {
					*value = qobject_cast<QCheckBox *>(field.input)->isChecked();
					return true;
				} else if constexpr (std::is_same_v<Value, QString>) {
					*value = text;
					return !text.isEmpty() || field.path == QStringLiteral("live_audio_mode");
				} else if constexpr (std::is_same_v<Value, api::ProfileText>) {
					value->bytes = combo->currentData().toByteArray();
					return true;
				} else {
					if constexpr (std::is_same_v<Value, quint64>)
						if (isBitrate(field.path))
							return parseBitrate(text, *value);
					// ok и parsed подтверждают точное целое, не проходя через числа с плавающей точкой.
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
		if (!valid) {
			error = impl_->text_("Profile.InvalidField").arg(field.label);
			return false;
		}
	}
	if (impl_->creating_ || impl_->dirty_.contains(QStringLiteral("live_audio_mode"))) {
		profile.liveAudioMode.state = profile.liveAudioMode.value.isEmpty() ? api::FieldState::Missing
										    : api::FieldState::Value;
	}
	if (impl_->original_.liveAudioMode.hasValue() && !profile.liveAudioMode.hasValue()) {
		error = impl_->text_("Profile.CannotRemoveAudio");
		return false;
	}
	if ((profile.mode != QStringLiteral("copy") && profile.mode != QStringLiteral("transcode")) ||
	    (impl_->original_.liveAudioMode.isMissing() && profile.liveAudioMode.isMissing() &&
	     impl_->original_.mode != profile.mode) ||
	    (profile.mode == QStringLiteral("copy") && profile.liveAudioMode.hasValue() &&
	     *profile.liveAudioMode != QStringLiteral("copy"))) {
		error = impl_->text_("Profile.InvalidAudioMode");
		return false;
	}
	if (impl_->creating_ &&
	    (!profile.liveAudioMode.hasValue() || profile.format.video.width == 0 || profile.format.video.height == 0 ||
	     profile.format.video.frameRate.numerator == 0 || profile.format.video.frameRate.denominator == 0 ||
	     profile.format.audio.sampleRateHz == 0 || profile.format.audio.channels == 0 ||
	     profile.videoEncoding.targetBitrateBps == 0 || profile.audioEncoding.bitrateBps == 0)) {
		error = impl_->text_("Profile.RequiredFormat");
		return false;
	}
	if (recipe == 2) {
		// encoding является полным явным рецептом, videoChanged/audioChanged определяют необходимость пересборки custom.
		auto &encoding = profile.encoding.value;
		const bool videoChanged = newRecipe ||
					  impl_->dirty_.contains(QStringLiteral("encoding.video_encoder")) ||
					  impl_->dirty_.contains(QStringLiteral("custom.nonref_p")) ||
					  impl_->dirty_.contains(QStringLiteral("custom.strict_gop")) ||
					  impl_->dirty_.contains(QStringLiteral("custom.lp"));
		const bool audioChanged = newRecipe || impl_->dirty_.contains(QStringLiteral("custom.aac_coder"));
		if (videoChanged) {
			// encoderChanged отзывает custom старого кодировщика только после его явной смены.
			const bool encoderChanged = newRecipe ||
						    impl_->dirty_.contains(QStringLiteral("encoding.video_encoder"));
			if (encoderChanged)
				encoding.videoCustom.clear();
			// key проходит применимые разрешённые параметры выбранного видеокодировщика.
			for (const auto &key :
			     encoding.videoEncoder == QStringLiteral("h264_nvenc")
				     ? QStringList{QStringLiteral("nonref_p"), QStringLiteral("strict_gop")}
				     : QStringList{QStringLiteral("lp")}) {
				if (!encoderChanged && !impl_->dirty_.contains(QStringLiteral("custom.") + key))
					continue;
				// value — точное значение отдельной API-пары либо отсутствие опции.
				const auto value = impl_->custom_[key]->currentData().toString();
				encoding.videoCustom = replaceCustomOption(encoding.videoCustom, key, value);
			}
		}
		if (audioChanged) {
			// value содержит только разрешённый aac_coder, пустота удаляет эту дополнительную пару.
			const auto value = impl_->custom_[QStringLiteral("aac_coder")]->currentData().toString();
			encoding.audioCustom = replaceCustomOption(newRecipe ? QString{} : encoding.audioCustom,
								   QStringLiteral("aac_coder"), value);
		}
		if (encoding.nvenc.hasValue() &&
		    (encoding.nvenc->lookaheadFrames < 0 || encoding.nvenc->lookaheadFrames > 31 ||
		     encoding.nvenc->aqStrength < 1 || encoding.nvenc->aqStrength > 15 ||
		     (!encoding.nvenc->enableSpatialAq && encoding.nvenc->aqStrength != 8) ||
		     (encoding.nvenc->lookaheadFrames > 0 &&
		      quint64(encoding.nvenc->lookaheadFrames) + profile.videoEncoding.maxBFrames > 31))) {
			error = impl_->text_("Profile.InvalidNvenc");
			return false;
		}
		if (encoding.svtAv1.hasValue() &&
		    (encoding.svtAv1->preset < 0 || encoding.svtAv1->preset > 13 || encoding.svtAv1->aqMode != 2 ||
		     (encoding.rateControl == QStringLiteral("cbr") &&
		      (encoding.svtAv1->tuning != QStringLiteral("psnr") || encoding.svtAv1->lookaheadFrames != 0)) ||
		     (encoding.rateControl == QStringLiteral("vbr") && encoding.svtAv1->lookaheadFrames != -1 &&
		      (encoding.svtAv1->lookaheadFrames < 6 || encoding.svtAv1->lookaheadFrames > 120)))) {
			error = impl_->text_("Profile.InvalidSvt");
			return false;
		}
	}
	return true;
}
} // namespace gost
