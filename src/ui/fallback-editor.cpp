#include "fallback-editor.hpp"
#include "fallback-policy-editor.hpp"
#include "profile-editor.hpp"
#include "confirmation-dialog.hpp"
#include "core/fallback-controller.hpp"
#include "core/state-store.hpp"
#include "status-label.hpp"
#include <QComboBox>
#include <QFileDialog>
#include <QFormLayout>
#include <QGroupBox>
#include <QHeaderView>
#include <QHBoxLayout>
#include <QLabel>
#include <QIcon>
#include <QStyle>
#include <QLineEdit>
#include <QListWidget>
#include <QMessageBox>
#include <QPointer>
#include <QProgressBar>
#include <QPushButton>
#include <QScrollArea>
#include <QSignalBlocker>
#include <QStringList>
#include <QTableWidget>
#include <QVBoxLayout>
#include <algorithm>
#include <map>
#include <tuple>
#include <utility>

namespace gost {
namespace {
// Переводит только известные состояния поколения и варианта; неизвестный код не становится Ready.
const char *generationStatusKey(const QString &status)
{
	if (status == QStringLiteral("ready") || status == QStringLiteral("READY"))
		return "Fallback.Prepared";
	if (status == QStringLiteral("queued"))
		return "Fallback.Queued";
	if (status == QStringLiteral("preparing") || status == QStringLiteral("PREPARING"))
		return "Fallback.Preparing";
	if (status == QStringLiteral("error") || status == QStringLiteral("FAILED"))
		return "Fallback.PreparationError";
	return "Fallback.UnknownStatus";
}
// Возвращает итог записи независимо от наблюдаемого состояния серверного задания.
const char *outcomeKey(FallbackWriteOutcome outcome)
{
	switch (outcome) {
	case FallbackWriteOutcome::None:
		return "Fallback.NoWrite";
	case FallbackWriteOutcome::Pending:
		return "Fallback.Sending";
	case FallbackWriteOutcome::Saved:
		return "Configuration.Saved";
	case FallbackWriteOutcome::Deleted:
		return "Configuration.Deleted";
	case FallbackWriteOutcome::Accepted:
		return "Fallback.Accepted";
	case FallbackWriteOutcome::Rejected:
		return "Configuration.Rejected";
	case FallbackWriteOutcome::Conflict:
		return "Configuration.Conflict";
	case FallbackWriteOutcome::Unknown:
		return "Configuration.Unknown";
	case FallbackWriteOutcome::NotSent:
		return "Fallback.NotSent";
	}
	return "Fallback.NoWrite";
}
// Описывает фактически измеренный формат без объявления совместимости с рецептом.
QString formatDescription(const api::Format &format, const TextLookup &text)
{
	return text("Fallback.Format")
		.arg(format.video.codec, QString::number(format.video.width), QString::number(format.video.height),
		     QString::number(format.video.frameRate.numerator),
		     QString::number(format.video.frameRate.denominator), format.audio.codec,
		     QString::number(format.audio.sampleRateHz), QString::number(format.audio.channels));
}
// Показывает дополнительные измеренные параметры отдельно от рецепта, без нормализации строк.
QString detailedFormatDescription(const api::Format &format, const TextLookup &text)
{
	return formatDescription(format, text) + QLatin1Char('\n') +
	       text("Fallback.VideoFormatDetails")
		       .arg(QString::fromUtf8(format.video.codecProfile.bytes),
			    QString::fromUtf8(format.video.level.bytes), QString::fromUtf8(format.video.tier.bytes),
			    QString::fromUtf8(format.video.pixelFormat.bytes), QString::number(format.video.bitDepth)) +
	       QLatin1Char('\n') +
	       text("Fallback.ColorDetails")
		       .arg(QString::fromUtf8(format.video.color.range.bytes),
			    QString::fromUtf8(format.video.color.primaries.bytes),
			    QString::fromUtf8(format.video.color.transfer.bytes),
			    QString::fromUtf8(format.video.color.matrix.bytes)) +
	       QLatin1Char('\n') +
	       text("Fallback.AudioFormatDetails")
		       .arg(QString::fromUtf8(format.audio.codecProfile.bytes),
			    QString::fromUtf8(format.audio.channelLayout.bytes));
}
} // namespace

/** Отражает каталог и три независимые пары поколений; все сетевые действия передаёт контроллеру.
 * Элементы принадлежат owner_, а state_/controller_ заимствованы до уничтожения страницы. */
class FallbackEditor::Impl {
public:
	// Строит страницу owner с переводами text и подписками на заимствованное состояние.
	Impl(FallbackEditor &owner, StateStore &state, FallbackController &controller, TextLookup text);
	// Отражает RAM-ввод и наблюдения, не запуская HTTP и не меняя цель политики.
	void refresh();
	// Передаёт полный текущий ввод контроллеру без чтения файла.
	void edited();
	// Открывает системный выбор файла и проверяет исходный контекст после вложенного цикла.
	void chooseFile();
	// Подтверждает удаление точного Asset и ревизии; новая карточка не наследует согласие.
	void removeAsset();
	// Формирует PlainText сводку одной точной пары Asset/Generation с полнотой вариантов.
	QString describeGeneration(const FallbackGenerationState &generation) const;
	// Перестраивает таблицу лишь при смене наблюдений и сохраняет точный выбор варианта.
	void refreshVariants();
	// Отражает полный рецепт выбранного варианта и отдельно его измеренный формат.
	void showVariant();
	// owner_ задаёт время жизни подписок, state_/controller_ переживают страницу; text_ переводит подписи.
	FallbackEditor &owner_;
	StateStore &state_;
	FallbackController &controller_;
	TextLookup text_;
	// Ключ выбора политики разделяет сервер, профиль OBS и рабочий Stream.
	using PolicyBrowseKey = std::tuple<QString, QString, QString>;
	// browsingContext_ адресует показанный выбор; contextTargets_ сохраняет его в RAM при смене контекста.
	std::optional<PolicyBrowseKey> browsingContext_;
	std::map<PolicyBrowseKey, QString> contextTargets_;
	// active_ разрешает передачу цели policy; updating_ подавляет обратное редактирование при отражении.
	bool active_ = false;
	bool updating_ = false;
	// confirming_ исключает второе подтверждение удаления во вложенном цикле.
	bool confirming_ = false;
	// assets_ хранит имена и точные ID; catalogStatus_ показывает полноту и свежесть списка.
	QListWidget *assets_ = nullptr;
	QLabel *catalogStatus_ = nullptr;
	// name_ содержит имя нового Asset, kind_ и path_ — RAM-ввод исходного файла.
	QLineEdit *name_ = nullptr;
	QComboBox *kind_ = nullptr;
	QLineEdit *path_ = nullptr;
	// create_/remove_ управляют каталогом; choose_/upload_/rebuild_ передают намерения подготовки.
	QPushButton *create_ = nullptr;
	QPushButton *remove_ = nullptr;
	QPushButton *choose_ = nullptr;
	QPushButton *upload_ = nullptr;
	QPushButton *rebuild_ = nullptr;
	// cancel_ прекращает только местное ожидание; refresh_ явно перечитывает текущую информацию.
	QPushButton *cancel_ = nullptr;
	QPushButton *refresh_ = nullptr;
	// progress_ и progressText_ описывают только известные переданные байты, не серверное кодирование.
	QProgressBar *progress_ = nullptr;
	QLabel *progressText_ = nullptr;
	// result_ отображает локальный исход записи и Unknown отдельно от состояния поколений.
	StatusLabel *result_ = nullptr;
	// current_/pending_/accepted_ сохраняют раздельный смысл опубликованной, готовящейся и собственной пары.
	QLabel *current_ = nullptr;
	QLabel *pending_ = nullptr;
	QLabel *accepted_ = nullptr;
	// variants_ показывает точные пары; variantRows_ адресует DTO каждой строки без повторного сопоставления профилей.
	QTableWidget *variants_ = nullptr;
	QVector<api::Variant> variantRows_;
	// variantsIdentity_ сохраняет отображение при несвязанных событиях, selectedVariant_ — адрес строки.
	QString variantsIdentity_;
	QString selectedVariant_;
	// variantProfile_ показывает полный структурный рецепт; variantFormat_ — отдельный измеренный формат.
	ProfileEditor *variantProfile_ = nullptr;
	QLabel *variantFormat_ = nullptr;
	// stream_ выбирает явную цель policy без изменения Dock; policy_ является переиспользуемой карточкой.
	QComboBox *stream_ = nullptr;
	FallbackPolicyEditor *policy_ = nullptr;
	// check_ запускает лишь существующую предварительную проверку рабочего Stream, readiness_ показывает предварительный результат.
	QPushButton *check_ = nullptr;
	StatusLabel *readiness_ = nullptr;
};

// Создаёт только элементы представления и подписки, не выполняя сетевых действий при конструировании.
FallbackEditor::Impl::Impl(FallbackEditor &owner, StateStore &state, FallbackController &controller, TextLookup text)
	: owner_(owner),
	  state_(state),
	  controller_(controller),
	  text_(std::move(text))
{
	owner_.setObjectName(QStringLiteral("fallbackEditor"));
	// layout размещает каталог рядом с прокручиваемой карточкой подготовки.
	auto *layout = new QHBoxLayout(&owner_);
	// left содержит каталог и создание нового Asset; создавать связи автоматически здесь нельзя.
	auto *left = new QVBoxLayout;
	assets_ = new QListWidget(&owner_);
	assets_->setObjectName(QStringLiteral("fallbackAssets"));
	assets_->setAccessibleName(text_("Fallback.Assets"));
	assets_->setMinimumWidth(150);
	assets_->setMaximumWidth(260);
	left->addWidget(assets_, 1);
	catalogStatus_ = new QLabel(&owner_);
	catalogStatus_->setTextFormat(Qt::PlainText);
	catalogStatus_->setWordWrap(true);
	left->addWidget(catalogStatus_);
	name_ = new QLineEdit(&owner_);
	name_->setObjectName(QStringLiteral("fallbackCreateName"));
	name_->setAccessibleName(text_("Fallback.NewName"));
	name_->setPlaceholderText(text_("Fallback.NewName"));
	left->addWidget(name_);
	create_ = new QPushButton(text_("Editor.Create"), &owner_);
	create_->setObjectName(QStringLiteral("fallbackCreate"));
	left->addWidget(create_);
	refresh_ = new QPushButton(text_("Editor.Reload"), &owner_);
	refresh_->setObjectName(QStringLiteral("fallbackRefresh"));
	refresh_->setIcon(QIcon::fromTheme(QStringLiteral("view-refresh"),
					   owner_.style()->standardIcon(QStyle::SP_BrowserReload)));
	left->addWidget(refresh_);
	layout->addLayout(left);
	// scroll прокручивает длинную форму; card содержит её элементы, cardLayout располагает их по вертикали.
	auto *scroll = new QScrollArea(&owner_);
	auto *card = new QWidget(scroll);
	auto *cardLayout = new QVBoxLayout(card);
	scroll->setWidgetResizable(true);
	scroll->setWidget(card);
	layout->addWidget(scroll, 1);
	// instructions объясняет фактическую зависимость вариантов от назначений и сохранённых групп.
	auto *instructions = new QLabel(text_("Fallback.PreparationOrder"), card);
	instructions->setTextFormat(Qt::PlainText);
	instructions->setWordWrap(true);
	cardLayout->addWidget(instructions);
	// policyBox объединяет политику Stream; policyLayout располагает её карточку, policyFields — выбор потока.
	auto *policyBox = new QGroupBox(text_("Fallback.PolicyTitle"), card);
	auto *policyLayout = new QVBoxLayout(policyBox);
	auto *policyFields = new QFormLayout;
	stream_ = new QComboBox(policyBox);
	stream_->setObjectName(QStringLiteral("fallbackPolicyStream"));
	stream_->setAccessibleName(text_("Stream.Label"));
	policyFields->addRow(text_("Stream.Label"), stream_);
	policyLayout->addLayout(policyFields);
	policy_ = new FallbackPolicyEditor(state_, controller_, text_, policyBox);
	policyLayout->addWidget(policy_);
	cardLayout->addWidget(policyBox);
	// sourceFields выравнивает тип и путь оригинала; path_ не выдаёт локальный файл за загруженный.
	auto *sourceFields = new QFormLayout;
	sourceFields->setRowWrapPolicy(QFormLayout::WrapLongRows);
	kind_ = new QComboBox(card);
	kind_->setObjectName(QStringLiteral("fallbackFileKind"));
	kind_->setAccessibleName(text_("Fallback.Kind"));
	kind_->addItem(text_("Fallback.Image"), QStringLiteral("image"));
	kind_->addItem(text_("Fallback.Video"), QStringLiteral("video"));
	sourceFields->addRow(text_("Fallback.Kind"), kind_);
	path_ = new QLineEdit(card);
	path_->setReadOnly(true);
	path_->setObjectName(QStringLiteral("fallbackFilePath"));
	path_->setAccessibleName(text_("Fallback.SourceFile"));
	sourceFields->addRow(text_("Fallback.SourceFile"), path_);
	cardLayout->addLayout(sourceFields);
	// sourceButtons объединяет явный выбор/передачу/пересборку и безопасную остановку ожидания.
	auto *sourceButtons = new QHBoxLayout;
	choose_ = new QPushButton(text_("Fallback.ChooseFile"), card);
	choose_->setObjectName(QStringLiteral("fallbackChooseFile"));
	upload_ = new QPushButton(text_("Fallback.Upload"), card);
	upload_->setObjectName(QStringLiteral("fallbackUpload"));
	rebuild_ = new QPushButton(text_("Fallback.Rebuild"), card);
	rebuild_->setObjectName(QStringLiteral("fallbackRebuild"));
	cancel_ = new QPushButton(text_("Fallback.CancelWait"), card);
	cancel_->setObjectName(QStringLiteral("fallbackCancel"));
	sourceButtons->addWidget(choose_);
	sourceButtons->addWidget(upload_);
	sourceButtons->addWidget(rebuild_);
	sourceButtons->addWidget(cancel_);
	cardLayout->addLayout(sourceButtons);
	// restriction объясняет серверный запрет при активном и приостановленном эфире.
	auto *restriction = new QLabel(text_("Fallback.PreparationRestriction"), card);
	restriction->setTextFormat(Qt::PlainText);
	restriction->setWordWrap(true);
	cardLayout->addWidget(restriction);
	progress_ = new QProgressBar(card);
	progress_->setObjectName(QStringLiteral("fallbackUploadProgress"));
	progress_->setAccessibleName(text_("Fallback.Transferred"));
	progress_->setRange(0, 1000);
	progress_->setTextVisible(false);
	cardLayout->addWidget(progress_);
	progressText_ = new QLabel(card);
	progressText_->setObjectName(QStringLiteral("fallbackUploadBytes"));
	progressText_->setTextFormat(Qt::PlainText);
	progressText_->setWordWrap(true);
	cardLayout->addWidget(progressText_);
	result_ = new StatusLabel(card, false);
	result_->setObjectName(QStringLiteral("fallbackResult"));
	result_->setTextFormat(Qt::PlainText);
	result_->setWordWrap(true);
	cardLayout->addWidget(result_);
	// current_, pending_ и accepted_ получают отдельные текстовые строки даже при одинаковых IDs.
	current_ = new QLabel(card);
	pending_ = new QLabel(card);
	accepted_ = new QLabel(card);
	current_->setObjectName(QStringLiteral("fallbackCurrent"));
	pending_->setObjectName(QStringLiteral("fallbackPending"));
	accepted_->setObjectName(QStringLiteral("fallbackAccepted"));
	// label настраивает одинаковый безопасный режим трёх независимых наблюдений.
	for (auto *label : {current_, pending_, accepted_}) {
		label->setTextFormat(Qt::PlainText);
		label->setWordWrap(true);
		cardLayout->addWidget(label);
	}
	variants_ = new QTableWidget(0, 6, card);
	variants_->setObjectName(QStringLiteral("fallbackVariants"));
	variants_->setAccessibleName(text_("Fallback.Variants"));
	variants_->setHorizontalHeaderLabels({text_("Fallback.GenerationRole"), text_("Fallback.Generation"),
					      text_("Fallback.Variant"), text_("Fallback.Status"),
					      text_("Fallback.MeasuredFormat"), text_("Fallback.Size")});
	variants_->horizontalHeader()->setSectionResizeMode(QHeaderView::ResizeToContents);
	variants_->horizontalHeader()->setStretchLastSection(true);
	variants_->setEditTriggers(QAbstractItemView::NoEditTriggers);
	variants_->setSelectionBehavior(QAbstractItemView::SelectRows);
	variants_->setSelectionMode(QAbstractItemView::SingleSelection);
	variants_->setMinimumHeight(140);
	cardLayout->addWidget(variants_);
	variantFormat_ = new QLabel(card);
	variantFormat_->setTextFormat(Qt::PlainText);
	variantFormat_->setWordWrap(true);
	cardLayout->addWidget(variantFormat_);
	variantProfile_ = new ProfileEditor(text_, card);
	variantProfile_->setObjectName(QStringLiteral("fallbackVariantProfile"));
	variantProfile_->setReadOnly(true);
	variantProfile_->hide();
	cardLayout->addWidget(variantProfile_);
	// silence и generator отражают ограничения медиа, которые нельзя доказать одним статусом READY.
	auto *silence = new QLabel(text_("Fallback.Silence"), card);
	auto *generator = new QLabel(text_("Fallback.GeneratorWarning"), card);
	silence->setTextFormat(Qt::PlainText);
	generator->setTextFormat(Qt::PlainText);
	silence->setWordWrap(true);
	generator->setWordWrap(true);
	cardLayout->addWidget(silence);
	cardLayout->addWidget(generator);
	check_ = new QPushButton(text_("Fallback.Check"), card);
	check_->setObjectName(QStringLiteral("fallbackCheck"));
	cardLayout->addWidget(check_);
	readiness_ = new StatusLabel(card, false);
	readiness_->setObjectName(QStringLiteral("fallbackReadiness"));
	readiness_->setTextFormat(Qt::PlainText);
	readiness_->setWordWrap(true);
	cardLayout->addWidget(readiness_);
	// preliminary ограничивает смысл API-проверки и указывает существующее место запуска.
	auto *preliminary = new QLabel(text_("Fallback.Preliminary"), card);
	preliminary->setTextFormat(Qt::PlainText);
	preliminary->setWordWrap(true);
	cardLayout->addWidget(preliminary);
	remove_ = new QPushButton(text_("Editor.Delete"), card);
	remove_->setObjectName(QStringLiteral("fallbackDelete"));
	cardLayout->addWidget(remove_, 0, Qt::AlignLeft);
	cardLayout->addStretch();
	// Переключение Asset передаёт точный ID, сохраняя ввод и собственную принятую пару в контроллере.
	QObject::connect(assets_, &QListWidget::currentItemChanged, &owner_, [this](QListWidgetItem *item) {
		if (!updating_ && item)
			controller_.selectAsset(item->data(Qt::UserRole).toString());
	});
	// Изменение имени сохраняет RAM-черновик без создания Asset.
	QObject::connect(name_, &QLineEdit::textEdited, &owner_, [this] { edited(); });
	// Выбор типа меняет только kind следующей передачи.
	QObject::connect(kind_, &QComboBox::activated, &owner_, [this] { edited(); });
	// Create отправляет одно явное намерение создания по сохранённому имени.
	QObject::connect(create_, &QPushButton::clicked, &owner_, [this] { controller_.createAsset(); });
	// Refresh перечитывает сервер и сверяет прежний Unknown, не повторяя запись.
	QObject::connect(refresh_, &QPushButton::clicked, &owner_, [this] { controller_.refresh(); });
	// Выбор файла открывает диалог без чтения медиа в GUI-потоке.
	QObject::connect(choose_, &QPushButton::clicked, &owner_, [this] { chooseFile(); });
	// Upload передаёт сохранённый путь контроллеру, который проверяет допуск и revision.
	QObject::connect(upload_, &QPushButton::clicked, &owner_, [this] { controller_.upload(); });
	// Rebuild использует сохранённый на сервере оригинал выбранного Asset.
	QObject::connect(rebuild_, &QPushButton::clicked, &owner_, [this] { controller_.rebuild(); });
	// Cancel прекращает местное ожидание и не обещает отмену уже принятой подготовки.
	QObject::connect(cancel_, &QPushButton::clicked, &owner_, [this] { controller_.cancel(); });
	// Delete сначала запрашивает отдельное подтверждение точного Asset.
	QObject::connect(remove_, &QPushButton::clicked, &owner_, [this] { removeAsset(); });
	// Политика адресуется только по явному выбору активной страницы, не по selectedStreamId Dock.
	QObject::connect(stream_, &QComboBox::activated, &owner_, [this] {
		if (active_)
			policy_->setStreamId(stream_->currentData().toString());
		refresh();
	});
	// Check использует только прежнюю предварительную проверку рабочего Stream без запуска.
	QObject::connect(check_, &QPushButton::clicked, &owner_, [this] {
		if (active_ && !state_.selectedStreamId().isEmpty() &&
		    stream_->currentData().toString() == state_.selectedStreamId())
			controller_.checkReadiness();
	});
	// Выбор строки открывает полный рецепт именно её пары Asset/Generation.
	QObject::connect(variants_, &QTableWidget::itemSelectionChanged, &owner_, [this] { showVariant(); });
	// Новые наблюдения fallback только отражаются в элементах страницы.
	QObject::connect(&state_, &StateStore::fallbackChanged, &owner_, [this] { refresh(); });
	// Смена подключения обновляет доступность, не создавая фоновых записей.
	QObject::connect(&state_, &StateStore::serverStateChanged, &owner_, [this] { refresh(); });
	// Обновление каталога Streams сохраняет явную цель этой страницы.
	QObject::connect(&state_, &StateStore::streamsChanged, &owner_, [this] { refresh(); });
	// Результат общей предварительной проверки отображается независимо от подготовки Asset.
	QObject::connect(&state_, &StateStore::operationsChanged, &owner_, [this] { refresh(); });
	refresh();
}

// Хранит ввод в контроллере; сетевую передачу запускает только отдельная кнопка Upload.
void FallbackEditor::Impl::edited()
{
	if (!updating_)
		controller_.setDraft(name_->text(), path_->text(), kind_->currentData().toString());
}

// Не переносит выбранный файл на новый сервер или карточку, появившиеся во время системного диалога.
void FallbackEditor::Impl::chooseFile()
{
	// before фиксирует контекст, выбранный Asset и поколение до вложенного диалога.
	const auto before = state_.fallback();
	// alive замечает уничтожение страницы во время выбора файла.
	const QPointer<FallbackEditor> alive(&owner_);
	// file содержит пользовательский путь; пустая строка означает отмену без изменения RAM-ввода.
	const auto file = QFileDialog::getOpenFileName(&owner_, text_("Fallback.ChooseFile"), before.draftFilePath,
						       text_("Fallback.FileFilter"));
	if (!alive || file.isEmpty() || state_.fallback().context != before.context ||
	    state_.fallback().selectedAssetId != before.selectedAssetId ||
	    state_.fallback().generation != before.generation)
		return;
	path_->setText(file);
	edited();
}

// Удаление подтверждается для точного ID и ревизии; новый выбор после диалога получает новое подтверждение.
void FallbackEditor::Impl::removeAsset()
{
	if (confirming_ || !controller_.canWrite() || !state_.fallback().assetFresh || !state_.fallback().asset)
		return;
	// before сохраняет адрес и исходную серверную revision до вложенного цикла событий.
	const auto before = state_.fallback();
	confirming_ = true;
	// alive защищает доступ к Impl после удаления страницы.
	const QPointer<FallbackEditor> alive(&owner_);
	// dialog принадлежит странице и показывает серверное имя только как PlainText.
	QPointer<QMessageBox> dialog = new QMessageBox(QMessageBox::Question, text_("Operation.ConfirmTitle"),
						       text_("Fallback.ConfirmDelete").arg(before.asset->name),
						       QMessageBox::Yes | QMessageBox::No, &owner_);
	dialog->setTextFormat(Qt::PlainText);
	dialog->setDefaultButton(QMessageBox::No);
	localizeConfirmation(*dialog, text_);
	// answer фиксирует выбор до удаления временного диалога.
	const int answer = dialog->exec();
	if (dialog)
		delete dialog.data();
	if (!alive)
		return;
	confirming_ = false;
	// after фиксирует состояние после событий диалога, которые могли сменить цель или ревизию.
	const auto after = state_.fallback();
	if (answer != QMessageBox::Yes || after.context != before.context || after.generation != before.generation ||
	    after.selectedAssetId != before.selectedAssetId || !after.asset || !after.assetFresh ||
	    after.asset->id != before.asset->id || after.asset->revision != before.asset->revision)
		return;
	controller_.deleteAsset();
}

// Состояние поколения и полнота его Variants остаются разными наблюдениями.
QString FallbackEditor::Impl::describeGeneration(const FallbackGenerationState &generation) const
{
	if (generation.generationId.isEmpty())
		return text_("Fallback.NoGeneration");
	return text_("Fallback.GenerationDescription")
		.arg(generation.generationId,
		     generation.generation ? text_(generationStatusKey(generation.generation->status))
					   : text_("Observation.NoData"),
		     text_(generation.variantsStatus.availability == RuntimeAvailability::Fresh &&
					   generation.variantsStatus.complete
				   ? "Fallback.VariantsComplete"
				   : "Fallback.VariantsUnconfirmed"));
}

// Таблица использует точные Asset/Generation/Variant и не смешивает Ready разных поколений.
void FallbackEditor::Impl::refreshVariants()
{
	// fallback фиксирует все три независимые пары и их текущие серверные данные.
	const auto fallback = state_.fallback();
	// identity содержит происхождение, ревизии и статусы, чтобы байтовый прогресс не сбрасывал таблицу.
	QString identity;
	// generation перебирает current, pending и accepted, не подменяя один источник другим.
	for (const auto *generation : {&fallback.current, &fallback.pending, &fallback.accepted}) {
		identity += generation->assetId + QLatin1Char('/') + generation->generationId + QLatin1Char(':') +
			    QString::number(static_cast<int>(generation->variantsStatus.availability)) +
			    QString::number(generation->variantsStatus.complete);
		// variant добавляет адрес и ревизию каждого фактически полученного варианта.
		for (const auto &variant : generation->variants)
			identity += QLatin1Char('|') + variant.id + QLatin1Char(':') +
				    QString::number(variant.revision) + variant.state;
		identity += QLatin1Char('\n');
	}
	if (identity == variantsIdentity_)
		return;
	variantsIdentity_ = identity;
	// previousRow и previous сохраняют идентичность выбранной строки до перестройки, а не её позицию.
	const int previousRow = variants_->currentRow();
	const auto previous = previousRow >= 0 ? variants_->item(previousRow, 0)->data(Qt::UserRole).toString()
					       : selectedVariant_;
	// blocked исключает чтение частично перестроенных строк обработчиком выбора.
	const QSignalBlocker blocked(variants_);
	variants_->setRowCount(0);
	variantRows_.clear();
	// role соответствует положению current/pending/accepted; selectedRow восстанавливает прежнюю точную строку.
	int role = 0;
	int selectedRow = -1;
	// generation содержит только варианты своей проверенной пары.
	for (const auto *generation : {&fallback.current, &fallback.pending, &fallback.accepted}) {
		// variant содержит полный рецепт и измеренный формат одной строки.
		for (const auto &variant : generation->variants) {
			// row адресует новую строку; key сохраняет происхождение и все непрозрачные IDs.
			const int row = variants_->rowCount();
			const auto key = QString::number(role) + QLatin1Char('/') + variant.assetId + QLatin1Char('/') +
					 variant.generationId + QLatin1Char('/') + variant.id;
			variants_->insertRow(row);
			variants_->setItem(row, 0,
					   new QTableWidgetItem(text_(role == 0   ? "Fallback.Current"
								      : role == 1 ? "Fallback.Pending"
										  : "Fallback.AcceptedPair")));
			variants_->item(row, 0)->setData(Qt::UserRole, key);
			variants_->setItem(row, 1, new QTableWidgetItem(variant.generationId));
			variants_->setItem(row, 2, new QTableWidgetItem(variant.id));
			variants_->setItem(row, 3, new QTableWidgetItem(text_(generationStatusKey(variant.state))));
			variants_->setItem(row, 4, new QTableWidgetItem(formatDescription(variant.format, text_)));
			variants_->setItem(row, 5, new QTableWidgetItem(QString::number(variant.sizeBytes)));
			variantRows_.append(variant);
			if (key == previous)
				selectedRow = row;
		}
		++role;
	}
	if (selectedRow >= 0)
		variants_->selectRow(selectedRow);
	showVariant();
}

// Отдельный структурный профиль показывает рецепт варианта, а не профили нынешней конфигурации Stream.
void FallbackEditor::Impl::showVariant()
{
	// row указывает единственную выбранную строку; отсутствие выбора скрывает прежний рецепт.
	const int row = variants_->currentRow();
	if (row < 0 || row >= variantRows_.size()) {
		selectedVariant_.clear();
		variantProfile_->hide();
		variantFormat_->clear();
		return;
	}
	selectedVariant_ = variants_->item(row, 0)->data(Qt::UserRole).toString();
	variantProfile_->setProfile(variantRows_.at(row).profile);
	variantProfile_->show();
	variantFormat_->setText(detailedFormatDescription(variantRows_.at(row).format, text_));
}

// Обновляет наблюдения и доступность; только активная страница передаёт контроллеру явную цель политики.
void FallbackEditor::Impl::refresh()
{
	if (updating_)
		return;
	updating_ = true;
	// fallback является копией модели, сохраняющей раздельный исход записи и наблюдения.
	const auto fallback = state_.fallback();
	// contextKey разделяет сохранённые цели разных серверов, профилей OBS и рабочего выбора.
	const PolicyBrowseKey contextKey{fallback.context.origin, fallback.context.profileUuid,
					 fallback.context.workingStreamId};
	// selectedStream содержит явный выбор этой страницы, а не привязку Dock.
	auto selectedStream = stream_->currentData().toString();
	if (!browsingContext_ || *browsingContext_ != contextKey) {
		if (browsingContext_)
			contextTargets_.insert_or_assign(*browsingContext_, selectedStream);
		browsingContext_ = contextKey;
		// previous возвращает выбор только этого контекста; новый контекст начинает без цели.
		const auto previous = contextTargets_.find(contextKey);
		selectedStream = previous == contextTargets_.end() ? QString{} : previous->second;
	}
	// blockAssets, blockName и blockKind не превращают отражение StateStore в новый пользовательский ввод.
	const QSignalBlocker blockAssets(assets_);
	const QSignalBlocker blockName(name_);
	const QSignalBlocker blockKind(kind_);
	name_->setText(fallback.draftName);
	path_->setText(fallback.draftFilePath);
	kind_->setCurrentIndex(qMax(0, kind_->findData(fallback.draftKind)));
	// changed проверяет полный состав и подписи каталога, сохраняя фокус и прокрутку при прогрессе.
	bool changed = assets_->count() != fallback.assets.size();
	// row и asset сопоставляют видимые имена и устойчивые ID записей.
	int row = 0;
	for (auto asset = fallback.assets.cbegin(); !changed && asset != fallback.assets.cend(); ++asset, ++row)
		changed = assets_->item(row)->text() != asset->name ||
			  assets_->item(row)->data(Qt::UserRole).toString() != asset.key();
	if (changed) {
		assets_->clear();
		// asset содержит полученную запись; entry принадлежит списку и адресует только её ID.
		for (const auto &asset : fallback.assets) {
			auto *entry = new QListWidgetItem(asset.name, assets_);
			entry->setData(Qt::UserRole, asset.id);
		}
	}
	// selectedRow восстанавливает лишь точный выбранный Asset, не выбирая соседнюю строку после удаления.
	int selectedRow = -1;
	// index ищет прежнюю идентичность, даже если обновились подписи.
	for (int index = 0; index < assets_->count(); ++index)
		if (assets_->item(index)->data(Qt::UserRole).toString() == fallback.selectedAssetId)
			selectedRow = index;
	assets_->setCurrentRow(selectedRow);
	catalogStatus_->setText(
		text_(fallback.assetsStatus.complete && fallback.assetsStatus.availability == RuntimeAvailability::Fresh
			      ? "Editor.ListComplete"
			      : "Editor.ListIncomplete")
			.arg(fallback.assets.size()));
	create_->setEnabled(controller_.canWrite() && !fallback.draftName.trimmed().isEmpty());
	remove_->setEnabled(controller_.canWrite() && fallback.assetFresh && fallback.asset.has_value() &&
			    !confirming_);
	upload_->setEnabled(controller_.canPrepare() && !fallback.draftFilePath.isEmpty());
	rebuild_->setEnabled(controller_.canPrepare());
	choose_->setEnabled(!fallback.writing);
	name_->setEnabled(!fallback.writing);
	kind_->setEnabled(!fallback.writing);
	cancel_->setEnabled(fallback.writing || fallback.observation == FallbackObservation::Observing);
	refresh_->setEnabled(!fallback.loading);
	progress_->setVisible(fallback.totalBytes > 0);
	progressText_->setVisible(fallback.totalBytes > 0);
	// ratio ограничен видимой шкалой и вычисляется в long double, не изменяя точные счётчики.
	const long double ratio =
		fallback.totalBytes > 0 ? static_cast<long double>(fallback.bytesSent) / fallback.totalBytes : 0;
	progress_->setValue(static_cast<int>(qBound(0.0L, ratio, 1.0L) * 1000));
	progressText_->setText(text_("Fallback.BytesTransferred")
				       .arg(QString::number(fallback.bytesSent), QString::number(fallback.totalBytes)));
	// messages содержит только доверенные локальные ключи, без полученных серверных причин и действий.
	QStringList messages;
	messages.append(text_(outcomeKey(fallback.outcome)));
	if (fallback.reconciliationRequired)
		messages.append(text_("Configuration.Unknown"));
	if (fallback.observation == FallbackObservation::Unknown)
		messages.append(text_("Fallback.ObservationUnknown"));
	if (fallback.observation == FallbackObservation::Cancelled)
		messages.append(text_("Fallback.WaitStopped"));
	if (!fallback.messageKey.isEmpty())
		messages.append(text_(fallback.messageKey.toUtf8().constData()));
	result_->setStatus(messages.join(QLatin1Char('\n')),
			   fallback.outcome == FallbackWriteOutcome::Unknown ||
					   fallback.outcome == FallbackWriteOutcome::Rejected ||
					   fallback.outcome == FallbackWriteOutcome::Conflict
				   ? StatusLabel::Tone::Error
			   : fallback.writing || fallback.observation == FallbackObservation::Observing
				   ? StatusLabel::Tone::Pending
				   : StatusLabel::Tone::Neutral);
	current_->setText(
		text_("Fallback.GenerationLine").arg(text_("Fallback.Current"), describeGeneration(fallback.current)));
	pending_->setText(
		text_("Fallback.GenerationLine").arg(text_("Fallback.Pending"), describeGeneration(fallback.pending)));
	accepted_->setText(text_("Fallback.AcceptedDescription")
				   .arg(fallback.acceptedAssetId.isEmpty() ? text_("Observation.NoData")
									   : fallback.acceptedAssetId,
					fallback.acceptedGenerationId.isEmpty() ? text_("Observation.NoData")
										: fallback.acceptedGenerationId,
					describeGeneration(fallback.accepted)));
	refreshVariants();
	// blockStream исключает новое назначение политики во время обновления списка имён.
	const QSignalBlocker blockStream(stream_);
	// entries сохраняет полный известный список с неподтверждённой прежней целью при необходимости.
	QList<QPair<QString, QString>> entries;
	entries.append({text_("Fallback.ChooseStream"), QString{}});
	// stream содержит одну полученную серверную запись.
	for (const auto &stream : state_.streams())
		entries.append({stream.name, stream.id});
	if (!selectedStream.isEmpty() &&
	    std::none_of(entries.cbegin(), entries.cend(),
			 [&selectedStream](const auto &entry) { return entry.second == selectedStream; }))
		entries.append({text_("Editor.ReferenceUnknown").arg(selectedStream), selectedStream});
	// streamsChanged предотвращает пересоздание открытого селектора при каждом прогрессе upload.
	bool streamsChanged = stream_->count() != entries.size();
	// index сравнивает подписи и адреса списка Streams.
	for (int index = 0; !streamsChanged && index < entries.size(); ++index)
		streamsChanged = stream_->itemText(index) != entries.at(index).first ||
				 stream_->itemData(index).toString() != entries.at(index).second;
	if (streamsChanged) {
		stream_->clear();
		// entry содержит имя и непрозрачный ID без переназначения рабочего Stream.
		for (const auto &entry : entries)
			stream_->addItem(entry.first, entry.second);
	}
	stream_->setCurrentIndex(qMax(0, stream_->findData(selectedStream)));
	// alive защищает отражение страницы после синхронной адресации новой цели.
	const QPointer<FallbackEditor> alive(&owner_);
	policy_->setStreamId(selectedStream);
	if (!alive)
		return;
	// precheck остаётся предварительной оценкой именно нынешнего рабочего Stream и эпохи конфигурации.
	const auto precheck = state_.operations().fallback;
	// sameWorking разрешает только проверку адреса, поддерживаемого существующим RestreamController.
	const bool sameWorking = !selectedStream.isEmpty() && selectedStream == state_.selectedStreamId();
	check_->setEnabled(active_ && sameWorking && state_.connectionStatus() == ConnectionStatus::Connected &&
			   state_.configurationSettled(state_.serverOrigin()) &&
			   precheck.readiness != FallbackReadiness::Checking);
	readiness_->setStatus(!sameWorking ? text_("Fallback.CheckWorkingOnly")
			      : precheck.context == state_.dockContext() &&
					      precheck.configurationGeneration == state_.configurationGeneration() &&
					      !precheck.messageKey.isEmpty()
				      ? text_(precheck.messageKey.toUtf8().constData())
				      : text_("Fallback.ReadinessUnknown"),
			      StatusLabel::Tone::Neutral);
	updating_ = false;
}

// Создаёт пассивную страницу; сетевое обновление выполняется при её активации.
FallbackEditor::FallbackEditor(StateStore &state, FallbackController &controller, TextLookup text, QWidget *parent)
	: QWidget(parent),
	  impl_(std::make_unique<Impl>(*this, state, controller, std::move(text)))
{
}
// Общая операция хранится вне страницы и не отменяется её удалением.
FallbackEditor::~FallbackEditor() = default;

// Адресует политику только видимой страницы, затем запрашивает ограниченные чтения контроллера.
void FallbackEditor::activate()
{
	// alive защищает продолжение после синхронной публикации новой цели политики.
	const QPointer<FallbackEditor> alive(this);
	setActive(true);
	if (alive)
		impl_->controller_.refresh();
}

// Скрытие страницы не трогает выбранный файл, RAM-черновик, принятое поколение или его наблюдение.
void FallbackEditor::setActive(bool active)
{
	impl_->active_ = active;
	// alive защищает продолжение после переключения цели общей политики.
	const QPointer<FallbackEditor> alive(this);
	impl_->policy_->setActive(false);
	if (active) {
		impl_->policy_->setStreamId(impl_->stream_->currentData().toString());
		if (!alive)
			return;
		impl_->policy_->setActive(true);
	}
	if (alive)
		impl_->refresh();
}
} // namespace gost
