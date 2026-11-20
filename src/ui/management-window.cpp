#include "management-window.hpp"

#include "core/state-store.hpp"
#include "core/connection-controller.hpp"
#include "core/settings-store.hpp"
#include "core/configuration-controller.hpp"
#include "ui/configuration-editor.hpp"
#include "ui/fallback-editor.hpp"
#include "ui/diagnostics-page.hpp"
#include "ui/status-label.hpp"

#include <QApplication>
#include <QComboBox>
#include <QEvent>
#include <QFile>
#include <QFormLayout>
#include <QHBoxLayout>
#include <QIcon>
#include <QLabel>
#include <QList>
#include <QListWidget>
#include <QLineEdit>
#include <QPalette>
#include <QPixmap>
#include <QPushButton>
#include <QPointer>
#include <QShowEvent>
#include <QHideEvent>
#include <QSignalBlocker>
#include <QScrollArea>
#include <QSpinBox>
#include <QStringList>
#include <QStackedWidget>
#include <QStyle>
#include <QTimer>
#include <QVBoxLayout>

#include <limits>
#include <utility>

namespace gost {
namespace {

// Иконки хранят исходное имя ресурса и запасной QStyle ID: смена палитры OBS требует повторного
// выбора Light/Dark, а не перекрашивания уже загруженного изображения. Список живёт в GUI-потоке;
// фильтр qApp удаляется Qt вместе с объектом, элементы списка принадлежат QListWidget.
class NavigationList final : public QListWidget {
public:
	explicit NavigationList(QWidget *parent) : QListWidget(parent)
	{
		setIconSize(QSize(16, 16));
		setSpacing(1);
		qApp->installEventFilter(this);
	}

	void addSection(const QString &title, const char *asset, QStyle::StandardPixmap fallback)
	{
		// asset заимствован до удаления списка; вызывающий код передаёт строковые литералы.
		// Порядок icons_ совпадает с порядком элементов и QStackedWidget.
		auto *section = new QListWidgetItem(title, this);
		icons_.append({asset, fallback});
		setSectionIcon(*section, icons_.back());
	}

protected:
	bool eventFilter(QObject *watched, QEvent *event) override
	{
		if (event->type() == QEvent::ApplicationPaletteChange && watched == qApp) {
			for (int row = 0; row < count(); ++row)
				setSectionIcon(*item(row), icons_.at(row));
		}
		return QListWidget::eventFilter(watched, event);
	}

private:
	struct SectionIcon {
		const char *asset;
		QStyle::StandardPixmap fallback;
	};

	void setSectionIcon(QListWidgetItem &item, const SectionIcon &spec)
	{
		// Ресурс берётся из текущей темы OBS; при его отсутствии остаётся штатная иконка стиля Qt.
		const bool dark = QApplication::palette().color(QPalette::Window).lightness() < 128;
		const auto path = QStringLiteral("theme:%1/%2")
					  .arg(dark ? QStringLiteral("Dark") : QStringLiteral("Light"),
					       QString::fromLatin1(spec.asset));
		const auto icon = QFile::exists(path) ? QIcon(path) : QIcon{};
		item.setIcon(icon.pixmap(iconSize()).isNull() ? style()->standardIcon(spec.fallback, nullptr, this)
							      : icon);
	}

	QList<SectionIcon> icons_;
};

void addPageHeading(QVBoxLayout &layout, const QString &title)
{
	auto *heading = new QLabel(title, layout.parentWidget());
	auto font = heading->font();
	font.setBold(true);
	heading->setFont(font);
	layout.addWidget(heading);
}

QWidget *makePage(const QString &title, const QString &message, QWidget *parent)
{
	// Информационная страница обозначает ещё недоступное действие; её создание не подключает API
	// и не выдаёт каркас редактора за реализованную серверную операцию.
	auto *page = new QWidget(parent);
	auto *layout = new QVBoxLayout(page);
	addPageHeading(*layout, title);
	auto *notice = new QLabel(message, page);
	notice->setWordWrap(true);
	layout->addWidget(notice);
	layout->addStretch();
	return page;
}

} // namespace

// Создаёт страницы для stateStore с переводами text, связывая окно с parent и заимствованными
// connection/configuration/fallback. Неименованный RestreamController не используется этим представлением.
// Каждая страница настроек применяет свой черновик только явным сохранением;
// отмена восстанавливает её текущие рабочие значения без записи и сетевых действий.
// Соединения Qt привязаны к этому окну и прекращаются при его удалении; закрытие только скрывает окно.
ManagementWindow::ManagementWindow(StateStore &stateStore, TextLookup text, QWidget *parent,
				   ConnectionController *connection, RestreamController *,
				   ConfigurationController *configuration, FallbackController *fallback)
	: QWidget(parent, Qt::Window),
	  stateStore_(stateStore),
	  text_(std::move(text)),
	  connection_(connection),
	  configuration_(configuration)
{
	// Qt::Window сохраняет самостоятельное окно при родителе OBS. WA_DeleteOnClose=false обеспечивает
	// повторное открытие того же экземпляра; WA_QuitOnClose=false не позволяет ему завершить приложение.
	setObjectName(QStringLiteral("managementWindow"));
	setWindowTitle(text_("Management.Title"));
	setAttribute(Qt::WA_DeleteOnClose, false);
	setAttribute(Qt::WA_QuitOnClose, false);
	resize(900, 600);
	// layout размещает навигацию и существующие страницы в едином окне.
	auto *layout = new QHBoxLayout(this);
	// navigation меняет страницу без создания новых окон и принадлежит этому окну.
	auto *navigation = new NavigationList(this);
	navigation_ = navigation;
	navigation->setObjectName(QStringLiteral("managementNavigation"));
	navigation->setAccessibleName(text_("Management.Sections"));
	navigation->setSizePolicy(QSizePolicy::Maximum, QSizePolicy::Expanding);
	// pages хранит все разделы единого расширенного окна.
	auto *pages = new QStackedWidget(this);
	pages_ = pages;
	pages->setObjectName(QStringLiteral("managementPages"));
	layout->addWidget(navigation);
	layout->addWidget(pages, 1);

	// addPage добавляет page с переводом titleKey и иконкой icon либо fallback; callback сохраняет порядок разделов.
	const auto addPage = [&](const char *titleKey, QWidget *page, const char *icon,
				 QStyle::StandardPixmap fallback) {
		navigation->addSection(text_(titleKey), icon, fallback);
		pages->addWidget(page);
	};
	// streamPage содержит только редактирование конфигурации потока и его назначений.
	auto *streamPage = new QWidget(pages);
	// streamLayout содержит только заголовок и редактор конфигурации потока.
	auto *streamLayout = new QVBoxLayout(streamPage);
	addPageHeading(*streamLayout, text_("Page.Streams"));
	if (configuration_) {
		streamEditor_ = new ConfigurationEditor(stateStore_, *configuration_, text_, ConfigurationKind::Stream,
							streamPage, fallback);
		streamLayout->addWidget(streamEditor_, 1);
	}
	// streamScroll сохраняет размер окна при длинном списке назначений и сообщениях редактора.
	auto *streamScroll = new QScrollArea(pages);
	streamScroll->setFrameShape(QFrame::NoFrame);
	streamScroll->setWidgetResizable(true);
	streamScroll->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
	streamScroll->setWidget(streamPage);
	addPage("Page.Streams", streamScroll, "settings/stream.svg", QStyle::SP_MediaPlay);
	// outputsPage содержит только список и карточку сохранённых назначений.
	auto *outputsPage = new QWidget(pages);
	// outputsLayout отдаёт свободное место редактору конфигурации выходов.
	auto *outputsLayout = new QVBoxLayout(outputsPage);
	addPageHeading(*outputsLayout, text_("Page.Outputs"));
	if (configuration_) {
		destinationEditor_ = new ConfigurationEditor(stateStore_, *configuration_, text_,
							     ConfigurationKind::Destination, outputsPage);
		outputsLayout->addWidget(destinationEditor_, 1);
	}
	// outputsScroll позволяет редактировать длинную карточку без роста минимальной высоты окна.
	auto *outputsScroll = new QScrollArea(pages);
	outputsScroll->setFrameShape(QFrame::NoFrame);
	outputsScroll->setWidgetResizable(true);
	outputsScroll->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
	outputsScroll->setWidget(outputsPage);
	addPage("Page.Outputs", outputsScroll, "settings/output.svg", QStyle::SP_DriveNetIcon);
	// groups содержит отдельную страницу групп кодирования в едином окне.
	auto *groups = new QWidget(pages);
	// groupsLayout располагает заголовок и редактор связей группы.
	auto *groupsLayout = new QVBoxLayout(groups);
	addPageHeading(*groupsLayout, text_("Page.Groups"));
	// profiles содержит самостоятельную страницу профилей; profilesLayout размещает её заголовок и редактор.
	auto *profiles = new QWidget(pages);
	auto *profilesLayout = new QVBoxLayout(profiles);
	addPageHeading(*profilesLayout, text_("Page.Profiles"));
	if (configuration_) {
		groupEditor_ =
			new ConfigurationEditor(stateStore_, *configuration_, text_, ConfigurationKind::Group, groups);
		profileEditor_ = new ConfigurationEditor(stateStore_, *configuration_, text_,
							 ConfigurationKind::Profile, profiles);
		groupsLayout->addWidget(groupEditor_, 1);
		profilesLayout->addWidget(profileEditor_, 1);
	} else {
		groupsLayout->addWidget(makePage({}, text_("Page.Groups.NotAvailable"), groups));
		profilesLayout->addWidget(makePage({}, text_("Page.Profiles.NotAvailable"), profiles));
	}
	addPage("Page.Groups", groups, "sources/group.svg", QStyle::SP_DirIcon);
	addPage("Page.Profiles", profiles, "settings/advanced.svg", QStyle::SP_FileDialogContentsView);
	// fallbackPage содержит подготовку и назначение без кнопок управления текущим эфиром.
	QWidget *fallbackPage = nullptr;
	if (fallback) {
		fallbackEditor_ = new FallbackEditor(stateStore_, *fallback, text_, pages);
		fallbackPage = fallbackEditor_;
	} else {
		fallbackPage = makePage(text_("Page.Fallback"), text_("Page.Fallback.NotAvailable"), pages);
	}
	addPage("Page.Fallback", fallbackPage, "sources/image.svg", QStyle::SP_FileIcon);

	// diagnostics показывает только текущий эфир из общего состояния без дополнительных запросов.
	auto *diagnostics = new DiagnosticsPage(stateStore_, text_, pages);
	// diagnosticsScroll сохраняет доступ ко всем источникам наблюдений при небольшом размере окна.
	auto *diagnosticsScroll = new QScrollArea(pages);
	diagnosticsScroll->setObjectName(QStringLiteral("diagnosticsScroll"));
	diagnosticsScroll->setFrameShape(QFrame::NoFrame);
	diagnosticsScroll->setWidgetResizable(true);
	diagnosticsScroll->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
	diagnosticsScroll->setWidget(diagnostics);
	addPage("Page.Diagnostics", diagnosticsScroll, "sources/window.svg", QStyle::SP_FileDialogDetailedView);
	// settingsPage содержит черновик адреса API и явные действия сохранения/отмены.
	auto *settingsPage = new QWidget(pages);
	// settingsLayout размещает форму адреса, кнопки и локальные сообщения о её сохранении.
	auto *settingsLayout = new QVBoxLayout(settingsPage);
	addPageHeading(*settingsLayout, text_("Page.Settings"));
	// form выравнивает подпись и поле адреса API.
	auto *form = new QFormLayout;
	form->setRowWrapPolicy(QFormLayout::WrapLongRows);
	apiUrl_ = new QLineEdit(stateStore_.serverOrigin(), settingsPage);
	apiUrl_->setObjectName(QStringLiteral("apiUrl"));
	apiUrl_->setAccessibleName(text_("Settings.ServerUrl"));
	apiUrl_->setPlaceholderText(QStringLiteral("http://host:8080"));
	apiUrl_->setMaxLength(4096);
	form->addRow(text_("Settings.ServerUrl"), apiUrl_);
	settingsLayout->addLayout(form);
	// buttons группирует сохранение адреса и отмену локального ввода.
	auto *buttons = new QHBoxLayout;
	settingsSaveButton_ = new QPushButton(text_("Settings.Save"), settingsPage);
	settingsSaveButton_->setObjectName(QStringLiteral("settingsSaveButton"));
	settingsCancelButton_ = new QPushButton(text_("Settings.Cancel"), settingsPage);
	settingsCancelButton_->setObjectName(QStringLiteral("settingsCancelButton"));
	buttons->addWidget(settingsSaveButton_);
	buttons->addWidget(settingsCancelButton_);
	buttons->addStretch();
	settingsLayout->addLayout(buttons);
	settingsMessage_ = new QLabel(settingsPage);
	settingsMessage_->setObjectName(QStringLiteral("settingsMessage"));
	settingsMessage_->setTextFormat(Qt::PlainText);
	settingsMessage_->setWordWrap(true);
	settingsLayout->addWidget(settingsMessage_);
	settingsLayout->addStretch();
	addPage("Page.Settings", settingsPage, "settings/general.svg", QStyle::SP_ComputerIcon);

	// advancedPage содержит черновики предпочтений SSE, REST и журнала OBS.
	auto *advancedPage = new QWidget(pages);
	// advancedLayout размещает дополнительные сетевые предпочтения и результат сохранения.
	auto *advancedLayout = new QVBoxLayout(advancedPage);
	addPageHeading(*advancedLayout, text_("Page.Advanced"));
	// advancedForm выравнивает поля лимита SSE, срока REST и режима журнала.
	auto *advancedForm = new QFormLayout;
	advancedForm->setRowWrapPolicy(QFormLayout::WrapLongRows);
	sseRejectionRetries_ = new QSpinBox(advancedPage);
	sseRejectionRetries_->setObjectName(QStringLiteral("sseRejectionRetries"));
	sseRejectionRetries_->setRange(SettingsStore::UnlimitedSseRejectionRetries, std::numeric_limits<int>::max());
	sseRejectionRetries_->setSpecialValueText(text_("Settings.UnlimitedRetries"));
	sseRejectionRetries_->setKeyboardTracking(false);
	sseRejectionRetries_->setAccessibleName(text_("Settings.SseRejectionRetries"));
	advancedForm->addRow(text_("Settings.SseRejectionRetries"), sseRejectionRetries_);
	httpTimeout_ = new QSpinBox(advancedPage);
	httpTimeout_->setObjectName(QStringLiteral("httpTimeoutSeconds"));
	httpTimeout_->setRange(SettingsStore::MinHttpTimeoutSeconds, SettingsStore::MaxHttpTimeoutSeconds);
	httpTimeout_->setKeyboardTracking(false);
	httpTimeout_->setAccessibleName(text_("Settings.HttpTimeout"));
	httpTimeout_->setToolTip(text_("Settings.HttpTimeoutHint"));
	advancedForm->addRow(text_("Settings.HttpTimeout"), httpTimeout_);
	logMode_ = new QComboBox(advancedPage);
	logMode_->setObjectName(QStringLiteral("clientLogMode"));
	logMode_->setAccessibleName(text_("Settings.LogMode"));
	logMode_->addItem(text_("Settings.LogNormal"), false);
	logMode_->addItem(text_("Settings.LogDetailed"), true);
	advancedForm->addRow(text_("Settings.LogMode"), logMode_);
	advancedLayout->addLayout(advancedForm);
	// advancedButtons группирует явное применение всех трёх предпочтений и отмену их черновика.
	auto *advancedButtons = new QHBoxLayout;
	advancedSettingsSaveButton_ = new QPushButton(text_("Settings.Save"), advancedPage);
	advancedSettingsSaveButton_->setObjectName(QStringLiteral("advancedSettingsSaveButton"));
	advancedSettingsCancelButton_ = new QPushButton(text_("Settings.Cancel"), advancedPage);
	advancedSettingsCancelButton_->setObjectName(QStringLiteral("advancedSettingsCancelButton"));
	advancedButtons->addWidget(advancedSettingsSaveButton_);
	advancedButtons->addWidget(advancedSettingsCancelButton_);
	advancedButtons->addStretch();
	advancedLayout->addLayout(advancedButtons);
	advancedSettingsMessage_ = new StatusLabel(advancedPage, false);
	advancedSettingsMessage_->setObjectName(QStringLiteral("advancedSettingsMessage"));
	advancedSettingsMessage_->setTextFormat(Qt::PlainText);
	advancedSettingsMessage_->setWordWrap(true);
	advancedLayout->addWidget(advancedSettingsMessage_);
	advancedLayout->addStretch();
	addPage("Page.Advanced", advancedPage, "settings/advanced.svg", QStyle::SP_FileDialogContentsView);

	// Изменение текста сбрасывает прежнюю локальную ошибку и обновляет доступность Save/Cancel.
	connect(apiUrl_, &QLineEdit::textChanged, this, [this] {
		apiUrlInvalid_ = false;
		refresh();
	});
	// Save проверяет черновик до вызова контроллера. Нажатие само подтверждает выбранный сервер;
	// эквивалентный origin не прерывает связь, но позволяет повторить подключение после Error/Disconnected.
	connect(settingsSaveButton_, &QPushButton::clicked, this, [this] {
		if (!connection_) {
			return;
		}
		// origin содержит проверенный канонический адрес, если пользователь ввёл допустимое значение.
		const auto origin = SettingsStore::canonicalOrigin(apiUrl_->text());
		if (!origin) {
			apiUrlInvalid_ = true;
			refresh();
			return;
		}
		apiUrlInvalid_ = false;
		apiUrl_->setText(*origin);
		// canRetry разрешает повторное подключение к тому же адресу лишь после остановки наблюдения.
		const bool canRetry = stateStore_.connectionStatus() == ConnectionStatus::Error ||
				      stateStore_.connectionStatus() == ConnectionStatus::Disconnected;
		if (*origin == stateStore_.serverOrigin() && !canRetry) {
			refresh();
			return;
		}
		connection_->connectToServer(*origin);
	});
	// Cancel восстанавливает текущий рабочий origin и очищает только локальную ошибку формы.
	// Черновик дополнительных предпочтений не затрагивается; сетевые действия здесь не выполняются.
	connect(settingsCancelButton_, &QPushButton::clicked, this, [this] {
		apiUrlInvalid_ = false;
		apiUrl_->setText(stateStore_.serverOrigin());
		refresh();
	});
	connect(sseRejectionRetries_, &QSpinBox::valueChanged, this, &ManagementWindow::editAdvancedSettings);
	connect(httpTimeout_, &QSpinBox::valueChanged, this, &ManagementWindow::editAdvancedSettings);
	connect(logMode_, &QComboBox::currentIndexChanged, this, &ManagementWindow::editAdvancedSettings);
	// sseEditor и timeoutEditor принадлежат своим spinbox. textEdited отмечает ввод до valueChanged,
	// поскольку keyboardTracking=false откладывает разбор до потери фокуса или interpretText().
	auto *sseEditor = sseRejectionRetries_->findChild<QLineEdit *>();
	auto *timeoutEditor = httpTimeout_->findChild<QLineEdit *>();
	connect(sseEditor, &QLineEdit::textEdited, this, &ManagementWindow::editAdvancedSettings);
	connect(timeoutEditor, &QLineEdit::textEdited, this, &ManagementWindow::editAdvancedSettings);
	// Save разбирает ожидающий текст обоих spinbox и фиксирует полный набор до синхронной публикации.
	// Пока контроллер сохраняет настройки, dirty защищает оставшиеся поля от промежуточного refresh.
	connect(advancedSettingsSaveButton_, &QPushButton::clicked, this, [this] {
		if (!connection_)
			return;
		sseRejectionRetries_->interpretText();
		httpTimeout_->interpretText();
		// retries задаёт новый лимит SSE; seconds — срок REST; detailed — выбранный режим журнала.
		const int retries = sseRejectionRetries_->value();
		const int seconds = httpTimeout_->value();
		const bool detailed = logMode_->currentData().toBool();
		advancedSettingsDirty_ = true;
		// guard запрещает обращаться к виджетам после удаления окна синхронным обработчиком состояния.
		const QPointer<ManagementWindow> guard(this);
		// saved подтверждает именно commit файла, а не одно принятие значений в рабочую RAM.
		const bool saved = connection_->saveAdvancedPreferences(retries, seconds, detailed);
		if (!guard)
			return;
		advancedSettingsSaveResult_ = saved;
		advancedSettingsDirty_ = stateStore_.sseRejectionRetries() != retries ||
					 stateStore_.httpTimeoutSeconds() != seconds ||
					 stateStore_.detailedLogging() != detailed;
		refresh();
	});
	// Cancel возвращает текущие рабочие значения, включая RAM после отказа диска, без вызова контроллера.
	connect(advancedSettingsCancelButton_, &QPushButton::clicked, this, [this] {
		advancedSettingsDirty_ = false;
		advancedSettingsSaveResult_.reset();
		refresh();
	});
	connect(navigation, &QListWidget::currentRowChanged, this, &ManagementWindow::changePage);
	connect(&stateStore_, &StateStore::obsStateChanged, this, &ManagementWindow::refresh);
	connect(&stateStore_, &StateStore::serverStateChanged, this, &ManagementWindow::refresh);
	// initialPage восстанавливает вид существующего RAM-черновика при редком пересоздании самого окна.
	int initialPage = 0;
	if (configuration_ && stateStore_.configurationEditor().draft) {
		switch (stateStore_.configurationEditor().kind) {
		case ConfigurationKind::Stream:
			break;
		case ConfigurationKind::Destination:
			initialPage = 1;
			break;
		case ConfigurationKind::Group:
			initialPage = 2;
			break;
		case ConfigurationKind::Profile:
			initialPage = 3;
			break;
		}
	}
	currentPage_ = initialPage;
	navigation->setCurrentRow(initialPage);
	refresh();
}

// Возвращает карточку фактически показанного раздела, включая выбранную внутреннюю вкладку.
ConfigurationEditor *ManagementWindow::currentEditor() const
{
	switch (currentPage_) {
	case 0:
		return streamEditor_;
	case 1:
		return destinationEditor_;
	case 2:
		return groupEditor_;
	case 3:
		return profileEditor_;
	default:
		return nullptr;
	}
}

// Обновляет текущую карточку; контроллер сохраняет её несохранённый ввод в общей RAM-модели.
void ManagementWindow::activateCurrentEditor()
{
	deactivateEditors();
	if (currentPage_ == 4 && fallbackEditor_) {
		fallbackEditor_->activate();
		return;
	}
	// editor принадлежит окну и получает данные только через общий контроллер.
	if (auto *editor = currentEditor())
		editor->activate();
}

// Только активная страница может адресовать общую policy; выключение не отменяет workflow.
void ManagementWindow::deactivateEditors()
{
	// editor перебирает созданные карточки; каждая сохраняет RAM-ввод при выключении.
	for (auto *editor : {streamEditor_, destinationEditor_, groupEditor_, profileEditor_})
		if (editor)
			editor->setActive(false);
	if (fallbackEditor_)
		fallbackEditor_->setActive(false);
}

// Маскирует также невидимые карточки, чтобы переключение страницы не раскрыла старое значение.
void ManagementWindow::hideSecrets()
{
	// editor перебирает только созданные конфигурационные страницы этого окна.
	for (auto *editor : {streamEditor_, destinationEditor_, groupEditor_, profileEditor_})
		if (editor)
			editor->hideSecrets();
}

// Повторный показ возвращает маску; интерактивная активация ждёт окончания обработки показа Qt.
void ManagementWindow::showEvent(QShowEvent *event)
{
	QWidget::showEvent(event);
	hideSecrets();
	// Обработчик может открыть подтверждение. Вложенный EXIT удаляет окно, поэтому exec нельзя
	// начинать внутри QWidget::show(). Контекст this отменяет вызов при раннем уничтожении окна.
	QTimer::singleShot(0, this, [this] {
		if (isVisible())
			activateCurrentEditor();
	});
}

// Скрытие не уничтожает окно, черновик или принадлежащую Runtime операцию.
void ManagementWindow::hideEvent(QHideEvent *event)
{
	deactivateEditors();
	hideSecrets();
	QWidget::hideEvent(event);
}

// Переход выполняется только после решения о dirty-форме; отказ возвращает выделение навигации.
void ManagementWindow::changePage(int row)
{
	if (row < 0 || row >= pages_->count())
		return;
	if (navigationChanging_) {
		// blocked предотвращает повторную навигацию во вложенном цикле подтверждения.
		const QSignalBlocker blocked(navigation_);
		navigation_->setCurrentRow(currentPage_);
		return;
	}
	if (row == currentPage_) {
		pages_->setCurrentIndex(row);
		return;
	}
	navigationChanging_ = true;
	// guard обнаруживает уничтожение окна во время модального подтверждения.
	const QPointer<ManagementWindow> guard = this;
	// editor указывает прежнюю страницу, чья форма должна разрешить переход.
	auto *editor = currentEditor();
	// allowed сохраняет ответ до проверки жизни окна после вложенного event loop.
	const bool allowed = !editor || editor->canLeave(true);
	if (!guard)
		return;
	if (!allowed) {
		// blocked восстанавливает прежний ряд без нового вызова changePage.
		const QSignalBlocker blocked(navigation_);
		navigation_->setCurrentRow(currentPage_);
		navigationChanging_ = false;
		return;
	}
	hideSecrets();
	currentPage_ = row;
	pages_->setCurrentIndex(row);
	navigationChanging_ = false;
	activateCurrentEditor();
}

// Сохраняет даже ещё не разобранный ввод spinbox до явного решения пользователя; новых настроек не публикует.
void ManagementWindow::editAdvancedSettings()
{
	advancedSettingsDirty_ = true;
	advancedSettingsSaveResult_.reset();
	refresh();
}

// Отражает доступность форм без обратного сохранения через сигналы виджетов.
// Черновики адреса и дополнительных предпочтений сохраняются при любых обновлениях состояния.
void ManagementWindow::refresh()
{
	if (!advancedSettingsDirty_) {
		// blockSseRetries, blockTimeout и blockLog не превращают отражение рабочих значений в новый ввод.
		const QSignalBlocker blockSseRetries(sseRejectionRetries_);
		const QSignalBlocker blockTimeout(httpTimeout_);
		const QSignalBlocker blockLog(logMode_);
		sseRejectionRetries_->setValue(stateStore_.sseRejectionRetries());
		httpTimeout_->setValue(stateStore_.httpTimeoutSeconds());
		logMode_->setCurrentIndex(stateStore_.detailedLogging() ? 1 : 0);
	}
	sseRejectionRetries_->setEnabled(connection_ != nullptr);
	httpTimeout_->setEnabled(connection_ != nullptr);
	logMode_->setEnabled(connection_ != nullptr);
	advancedSettingsSaveButton_->setEnabled(connection_ &&
						(advancedSettingsDirty_ || stateStore_.settingsUnsaved()));
	advancedSettingsCancelButton_->setEnabled(connection_ && advancedSettingsDirty_);
	// hasDraft отличает фактический изменённый ввод или локальную ошибку от текущего рабочего адреса.
	const bool hasDraft = apiUrl_->text() != stateStore_.serverOrigin() || apiUrlInvalid_;
	// canRetry разрешает вручную повторить подключение после окончательного отказа или отключения.
	const bool canRetry = !stateStore_.serverOrigin().isEmpty() &&
			      (stateStore_.connectionStatus() == ConnectionStatus::Error ||
			       stateStore_.connectionStatus() == ConnectionStatus::Disconnected);
	settingsSaveButton_->setEnabled(connection_ && (hasDraft || canRetry));
	settingsCancelButton_->setEnabled(connection_ && hasDraft);
	// messages собирает локальные сообщения страницы без сетевых ошибок и предупреждений о выборе Stream.
	QStringList messages;
	if (apiUrlInvalid_)
		messages.append(text_("Error.InvalidServerUrl"));
	// unsavedMessage отделяет отказ записи предпочтений от состояния серверного эфира.
	const auto unsavedMessage = stateStore_.settingsUnsaved() ? text_("Settings.Unsaved") : QString{};
	if (!unsavedMessage.isEmpty())
		messages.append(unsavedMessage);
	settingsMessage_->setText(messages.join(QLatin1Char('\n')));
	settingsMessage_->setVisible(!messages.isEmpty());
	// advancedMessage отделяет подтверждённую запись от рабочего изменения только в RAM.
	QString advancedMessage;
	// advancedTone сохраняет смысл результата для светлой/тёмной палитры OBS без собственной цветовой схемы.
	auto advancedTone = StatusLabel::Tone::Neutral;
	if (stateStore_.settingsUnsaved()) {
		advancedMessage =
			text_(advancedSettingsSaveResult_ == false ? "Settings.WriteError" : "Settings.Unsaved");
		advancedTone = StatusLabel::Tone::Error;
	} else if (advancedSettingsSaveResult_) {
		advancedMessage =
			text_(*advancedSettingsSaveResult_ ? "Settings.AdvancedSaved" : "Settings.AdvancedSaveFailed");
		advancedTone = *advancedSettingsSaveResult_ ? StatusLabel::Tone::Active : StatusLabel::Tone::Error;
	}
	advancedSettingsMessage_->setStatus(advancedMessage, advancedTone);
	advancedSettingsMessage_->setVisible(!advancedMessage.isEmpty());
}

// Преобразует прежний адрес Output/Session в Destination ID и делегирует существующей карточке.
// context не допускает переход к другому Stream, а sessionId/outputId сохраняют смысл прежнего адаптера.
void ManagementWindow::selectOutput(const DockContext &context, const QString &sessionId, const QString &outputId)
{
	if (stateStore_.dockContext() != context || !stateStore_.dockServer().session ||
	    stateStore_.dockServer().session->id != sessionId)
		return;
	// output связывает исходный прикреплённый Output с сохранённым назначением.
	const auto output = stateStore_.dockServer().outputs.constFind(outputId);
	if (output == stateStore_.dockServer().outputs.cend() || !output->attached || output->destinationId.isEmpty())
		return;
	// destinationId копируется до публикации формы, которая может заменить метаданные Dock.
	const auto destinationId = output->destinationId;
	selectDestination(context, destinationId);
}

// Открывает сохранённое назначение без зависимости от Session, attached и runtime.
// context и destinationId задают исходную строку Dock. Чужой черновик паркуется в RAM,
// секреты маскируются; вложенные события не позволяют открыть карточку в новом контексте.
void ManagementWindow::selectDestination(const DockContext &context, const QString &destinationId)
{
	if (!configuration_ || navigationChanging_ || !context.valid() || destinationId.isEmpty() ||
	    stateStore_.dockContext() != context)
		return;
	// sourceContext и sourceDestination сохраняют значения, даже если вызывающий передал ссылки на StateStore.
	const auto sourceContext = context;
	const auto sourceDestination = destinationId;
	// guard замечает удаление окна; editor сохраняет ввод прежней страницы через canLeave(true).
	const QPointer<ManagementWindow> guard(this);
	auto *editor = currentEditor();
	if (editor && !editor->canLeave(true))
		return;
	if (!guard || stateStore_.dockContext() != sourceContext)
		return;
	hideSecrets();
	// opened восстанавливает RAM-черновик назначения либо запускает его точное адресное чтение.
	// Отсутствие записи обрабатывает прежний редактор, поэтому каталог/Session здесь не служат фильтром.
	const bool opened = configuration_->restoreDraft(ConfigurationKind::Destination, sourceDestination) ||
			    configuration_->open(ConfigurationKind::Destination, sourceDestination, true);
	if (!guard || !opened || stateStore_.dockContext() != sourceContext)
		return;
	deactivateEditors();
	// blocked исключает повторную активацию, которая могла бы заменить уже выбранную карточку.
	const QSignalBlocker blocked(navigation_);
	currentPage_ = 1;
	navigation_->setCurrentRow(currentPage_);
	pages_->setCurrentIndex(currentPage_);
}

} // namespace gost
