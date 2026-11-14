#include "management-window.hpp"

#include "core/state-store.hpp"
#include "core/connection-controller.hpp"
#include "core/settings-store.hpp"
#include "core/configuration-controller.hpp"
#include "ui/configuration-editor.hpp"
#include "ui/confirmation-dialog.hpp"
#include "ui/status-label.hpp"

#include <QApplication>
#include <QCheckBox>
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
#include <QMessageBox>
#include <QPalette>
#include <QPixmap>
#include <QPushButton>
#include <QPointer>
#include <QShowEvent>
#include <QHideEvent>
#include <QSignalBlocker>
#include <QScrollArea>
#include <QSpinBox>
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

// Показывает локализованные стандартные Yes/No для parent с исходным выбором No.
// title и message выводятся обычным текстом; удаление родителя во время exec не оставляет висячий диалог.
int askConfirmation(QWidget *parent, const TextLookup &text, const QString &title, const QString &message)
{
	// dialog принадлежит parent; QPointer позволяет корректно завершить вложенный цикл при удалении окна.
	QPointer<QMessageBox> dialog =
		new QMessageBox(QMessageBox::Question, title, message, QMessageBox::Yes | QMessageBox::No, parent);
	dialog->setTextFormat(Qt::PlainText);
	dialog->setDefaultButton(QMessageBox::No);
	localizeConfirmation(*dialog, text);
	// choice сохраняет стандартный идентификатор ответа до удаления временного диалога.
	const int choice = dialog->exec();
	delete dialog.data();
	return choice;
}

} // namespace

// Создаёт страницы единого окна и соединения с общими сервисами; закрытие окна только скрывает его.
ManagementWindow::ManagementWindow(StateStore &stateStore, TextLookup text, QWidget *parent,
				   ConnectionController *connection, RestreamController *,
				   ConfigurationController *configuration)
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
							streamPage);
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
	// profiles и profilesLayout представляют полный профиль самостоятельным разделом навигации.
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
	addPage("Page.Fallback", makePage(text_("Page.Fallback"), text_("Page.Fallback.NotAvailable"), pages),
		"sources/image.svg", QStyle::SP_FileIcon);

	// diagnostics содержит локальные наблюдения OBS и сообщение о границах диагностики.
	auto *diagnostics = new QWidget(pages);
	// diagnosticsLayout размещает наблюдения и существующее пояснение диагностики.
	auto *diagnosticsLayout = new QVBoxLayout(diagnostics);
	// observations выравнивает профиль и состояние OBS с переносом длинных подписей.
	auto *observations = new QFormLayout;
	observations->setRowWrapPolicy(QFormLayout::WrapLongRows);
	profile_ = new QLabel(diagnostics);
	profile_->setObjectName(QStringLiteral("obsProfile"));
	profile_->setTextFormat(Qt::PlainText);
	profile_->setWordWrap(true);
	observations->addRow(text_("OBS.Profile"), profile_);
	streaming_ = new StatusLabel(diagnostics);
	streaming_->setObjectName(QStringLiteral("obsStreaming"));
	observations->addRow(text_("OBS.Streaming"), streaming_);
	diagnosticsLayout->addLayout(observations);
	// notice поясняет доступные сейчас сведения диагностики.
	auto *notice = new QLabel(text_("Page.Diagnostics.NotAvailable"), diagnostics);
	notice->setWordWrap(true);
	diagnosticsLayout->addWidget(notice);
	diagnosticsLayout->addStretch();
	addPage("Page.Diagnostics", diagnostics, "sources/window.svg", QStyle::SP_FileDialogDetailedView);
	// settingsPage содержит только настройки подключения и выбора Stream.
	auto *settingsPage = new QWidget(pages);
	// settingsLayout размещает форму, команды подключения и безопасные сообщения.
	auto *settingsLayout = new QVBoxLayout(settingsPage);
	addPageHeading(*settingsLayout, text_("Page.Settings"));
	// form выравнивает поля сетевых предпочтений и текущий Stream.
	auto *form = new QFormLayout;
	form->setRowWrapPolicy(QFormLayout::WrapLongRows);
	apiUrl_ = new QLineEdit(stateStore_.serverOrigin(), settingsPage);
	apiUrl_->setObjectName(QStringLiteral("apiUrl"));
	apiUrl_->setAccessibleName(text_("Settings.ServerUrl"));
	apiUrl_->setPlaceholderText(QStringLiteral("http://host:8080"));
	apiUrl_->setMaxLength(4096);
	form->addRow(text_("Settings.ServerUrl"), apiUrl_);
	autoConnect_ = new QCheckBox(text_("Settings.AutoConnect"), settingsPage);
	autoConnect_->setObjectName(QStringLiteral("autoConnect"));
	form->addRow(autoConnect_);
	reconnect_ = new QCheckBox(text_("Settings.Reconnect"), settingsPage);
	reconnect_->setObjectName(QStringLiteral("reconnect"));
	form->addRow(reconnect_);
	streams_ = new QComboBox(settingsPage);
	streams_->setObjectName(QStringLiteral("streamSelector"));
	streams_->setAccessibleName(text_("Settings.Stream"));
	form->addRow(text_("Settings.Stream"), streams_);
	connectionStatus_ = new StatusLabel(settingsPage);
	connectionStatus_->setObjectName(QStringLiteral("connectionStatus"));
	form->addRow(text_("Settings.ConnectionStatus"), connectionStatus_);
	settingsLayout->addLayout(form);
	// buttons группирует явные действия подключения и отключения.
	auto *buttons = new QHBoxLayout;
	connectButton_ = new QPushButton(text_("Settings.Connect"), settingsPage);
	connectButton_->setObjectName(QStringLiteral("connectButton"));
	disconnectButton_ = new QPushButton(text_("Settings.Disconnect"), settingsPage);
	disconnectButton_->setObjectName(QStringLiteral("disconnectButton"));
	buttons->addWidget(connectButton_);
	buttons->addWidget(disconnectButton_);
	buttons->addStretch();
	settingsLayout->addLayout(buttons);
	connectionMessage_ = new QLabel(settingsPage);
	connectionMessage_->setObjectName(QStringLiteral("connectionMessage"));
	connectionMessage_->setTextFormat(Qt::PlainText);
	connectionMessage_->setWordWrap(true);
	settingsLayout->addWidget(connectionMessage_);
	settingsLayout->addStretch();
	addPage("Page.Settings", settingsPage, "settings/general.svg", QStyle::SP_ComputerIcon);

	// advancedPage содержит существующий параметр повторов подписки SSE.
	auto *advancedPage = new QWidget(pages);
	// advancedLayout размещает дополнительные сетевые предпочтения и результат сохранения.
	auto *advancedLayout = new QVBoxLayout(advancedPage);
	addPageHeading(*advancedLayout, text_("Page.Advanced"));
	// advancedForm выравнивает существующий лимит повторов SSE.
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
	// logNotice поясняет единый журнал OBS без вывода каких-либо серверных значений.
	auto *logNotice = new QLabel(text_("Settings.LogNotice"), advancedPage);
	logNotice->setTextFormat(Qt::PlainText);
	logNotice->setWordWrap(true);
	advancedLayout->addWidget(logNotice);
	advancedSettingsMessage_ = new QLabel(advancedPage);
	advancedSettingsMessage_->setObjectName(QStringLiteral("advancedSettingsMessage"));
	advancedSettingsMessage_->setTextFormat(Qt::PlainText);
	advancedSettingsMessage_->setWordWrap(true);
	advancedLayout->addWidget(advancedSettingsMessage_);
	advancedLayout->addStretch();
	addPage("Page.Advanced", advancedPage, "settings/advanced.svg", QStyle::SP_FileDialogContentsView);

	// Обработчик подключения проверяет введённый origin и подтверждает смену сервера до передачи контроллеру.
	connect(connectButton_, &QPushButton::clicked, this, [this] {
		// Подтверждение требуется при смене уже выбранного origin. Неверный ввод передаётся контроллеру
		// для сообщения в существующем UI, а не заменяет адрес догадкой или исправлением по умолчанию.
		if (!connection_) {
			return;
		}
		// origin содержит проверенный канонический адрес, если пользователь ввёл допустимое значение.
		const auto origin = SettingsStore::canonicalOrigin(apiUrl_->text());
		if (origin && !stateStore_.serverOrigin().isEmpty() && *origin != stateStore_.serverOrigin()) {
			// guard запрещает продолжение после удаления окна во вложенном цикле подтверждения.
			const QPointer<ManagementWindow> guard = this;
			// choice подтверждает смену уже выбранного сервера, сохраняя явный пользовательский выбор.
			const auto choice = askConfirmation(this, text_, text_("Settings.ChangeServerTitle"),
							    text_("Settings.ChangeServerConfirm").arg(*origin));
			if (!guard || choice != QMessageBox::Yes) {
				return;
			}
		}
		connection_->connectToServer(apiUrl_->text());
	});
	// Обработчик отключения прекращает наблюдение, не завершая серверный эфир или штатную трансляцию OBS.
	connect(disconnectButton_, &QPushButton::clicked, this, [this] {
		if (connection_) {
			connection_->disconnectFromServer();
		}
	});
	// Обработчик сохраняет enabled как предпочтение автоподключения; эфир не запускается.
	connect(autoConnect_, &QCheckBox::toggled, this, [this](bool enabled) {
		if (connection_) {
			connection_->setAutoConnect(enabled);
		}
	});
	// Обработчик передаёт enabled в существующую настройку восстановления наблюдения.
	connect(reconnect_, &QCheckBox::toggled, this, [this](bool enabled) {
		if (connection_) {
			connection_->setReconnect(enabled);
		}
	});
	// Обработчик сохраняет retries как существующий лимит повторов отклонённой подписки SSE.
	connect(sseRejectionRetries_, &QSpinBox::valueChanged, this, [this](int retries) {
		if (connection_) {
			connection_->setSseRejectionRetries(retries);
		}
	});
	// seconds изменяет общий срок только будущих обычных REST-заданий.
	connect(httpTimeout_, &QSpinBox::valueChanged, this, [this](int seconds) {
		if (connection_)
			connection_->setHttpTimeoutSeconds(seconds);
	});
	// index указывает один из двух закрытых режимов; отображаемое имя не сериализуется.
	connect(logMode_, &QComboBox::currentIndexChanged, this, [this](int index) {
		if (connection_ && index >= 0)
			connection_->setDetailedLogging(logMode_->itemData(index).toBool());
	});
	// Обработчик использует точный itemData выбранного index, а не отображаемое имя Stream.
	connect(streams_, &QComboBox::currentIndexChanged, this, [this](int index) {
		if (connection_ && index >= 0) {
			connection_->selectStream(streams_->itemData(index).toString());
		}
	});
	connect(navigation, &QListWidget::currentRowChanged, this, &ManagementWindow::changePage);
	connect(&stateStore_, &StateStore::obsStateChanged, this, &ManagementWindow::refresh);
	connect(&stateStore_, &StateStore::serverStateChanged, this, &ManagementWindow::refresh);
	connect(&stateStore_, &StateStore::streamsChanged, this, &ManagementWindow::refreshStreams);
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
	refreshStreams();
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
	// editor принадлежит окну и получает данные только через общий контроллер.
	if (auto *editor = currentEditor())
		editor->activate();
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

void ManagementWindow::refreshStreams()
{
	// Синтетический пункт старого ID хранит точное itemData. Он сохраняет принадлежность выбора,
	// но не подтверждает наличие Stream на сервере; контроллер разрешает новый выбор только из полученных.
	// Пересоздание пунктов не является пользовательским выбором и не должно менять сохранённую привязку.
	const QSignalBlocker blockStreams(streams_);
	streams_->clear();
	streams_->addItem(text_("Stream.Choose"), QString{});
	for (const auto &stream : stateStore_.streams()) {
		streams_->addItem(stream.name, stream.id);
	}
	displayedSelectionId_ = stateStore_.selectedStreamId();
	int selected = streams_->findData(displayedSelectionId_);
	if (selected < 0 && !displayedSelectionId_.isEmpty()) {
		// Неполный список не доказывает удаления сохранённого Stream; привязка остаётся до явного выбора.
		streams_->addItem(text_(stateStore_.streamsComplete() ? "Stream.Unavailable" : "Stream.Unconfirmed")
					  .arg(displayedSelectionId_),
				  displayedSelectionId_);
		selected = streams_->count() - 1;
	}
	streams_->setCurrentIndex(selected < 0 ? 0 : selected);
}

void ManagementWindow::refresh()
{
	// Отражение состояния не должно вызвать методы изменения или сохранение обратно через toggled/valueChanged.
	// Список потоков обновляется отдельно: частая SSE-телеметрия иначе сбрасывала бы раскрытый список.
	// profile содержит последнее доступное имя профиля OBS.
	const auto &profile = stateStore_.profileName();
	profile_->setText(profile && !profile->isEmpty() ? *profile : text_("Observation.NoData"));
	streaming_->setStreamingStatus(text_, stateStore_.streamingActive());
	// blockAuto не допускает сохранения отражённого значения автоподключения как нового ввода.
	const QSignalBlocker blockAuto(autoConnect_);
	// blockReconnect не допускает обратного вызова изменения reconnect при отражении состояния.
	const QSignalBlocker blockReconnect(reconnect_);
	// blockSseRetries не допускает обратного сохранения отражённого лимита повторов.
	const QSignalBlocker blockSseRetries(sseRejectionRetries_);
	// blockTimeout и blockLog исключают обратное сохранение отражённых предпочтений.
	const QSignalBlocker blockTimeout(httpTimeout_);
	const QSignalBlocker blockLog(logMode_);
	autoConnect_->setChecked(stateStore_.autoConnect());
	reconnect_->setChecked(stateStore_.reconnect());
	if (sseRejectionRetries_->value() != stateStore_.sseRejectionRetries()) {
		sseRejectionRetries_->setValue(stateStore_.sseRejectionRetries());
	}
	autoConnect_->setEnabled(connection_ != nullptr);
	reconnect_->setEnabled(connection_ != nullptr);
	sseRejectionRetries_->setEnabled(connection_ && stateStore_.reconnect());
	httpTimeout_->setValue(stateStore_.httpTimeoutSeconds());
	httpTimeout_->setEnabled(connection_ != nullptr);
	logMode_->setCurrentIndex(stateStore_.detailedLogging() ? 1 : 0);
	logMode_->setEnabled(connection_ != nullptr);
	connectButton_->setEnabled(connection_ != nullptr);
	disconnectButton_->setEnabled(connection_ && stateStore_.connectionStatus() != ConnectionStatus::Disconnected);
	connectionStatus_->setConnectionStatus(text_, stateStore_.connectionStatus());
	// Обычная телеметрия не пересоздаёт список и не закрывает открытый пользователем выбор потока.
	if (displayedSelectionId_ != stateStore_.selectedStreamId()) {
		refreshStreams();
	}
	// streamsComplete отделяет полный список серверных Stream от частичного чтения.
	const bool streamsComplete = stateStore_.streamsComplete();
	// canChooseStream разрешает выбор только из доступных подтверждённых вариантов.
	const bool canChooseStream = streamsComplete ? stateStore_.connectionStatus() == ConnectionStatus::Connected
						     : !stateStore_.streams().isEmpty();
	streams_->setEnabled(connection_ && stateStore_.profileResolved() && canChooseStream);
	// messages собирает безопасные локализованные пояснения без сетевых тел и секретов.
	QStringList messages;
	if (!streamsComplete &&
	    stateStore_.configuration().streamsStatus.availability != RuntimeAvailability::Unknown) {
		messages.append(text_("Stream.ListIncomplete"));
	}
	if (!stateStore_.profileNoticeKey().isEmpty()) {
		messages.append(text_(stateStore_.profileNoticeKey().toUtf8().constData()));
	}
	if (!stateStore_.safeErrorKey().isEmpty()) {
		messages.append(text_(stateStore_.safeErrorKey().toUtf8().constData()));
	}
	// unsavedMessage отделяет ошибку сохранения предпочтений от состояния серверного эфира.
	const auto unsavedMessage = stateStore_.settingsUnsaved() ? text_("Settings.Unsaved") : QString{};
	if (!unsavedMessage.isEmpty())
		messages.append(unsavedMessage);
	connectionMessage_->setText(messages.join(QLatin1Char('\n')));
	advancedSettingsMessage_->setText(unsavedMessage);
	advancedSettingsMessage_->setVisible(!unsavedMessage.isEmpty());
}

// Открывает сохранённое назначение исходного Output, сохраняя ввод другой карточки в RAM.
// Ссылка из Dock не переносится в другой контекст или Session и не отправляет команды эфира.
void ManagementWindow::selectOutput(const DockContext &context, const QString &sessionId, const QString &outputId)
{
	if (!configuration_ || stateStore_.dockContext() != context || !stateStore_.dockServer().session ||
	    stateStore_.dockServer().session->id != sessionId)
		return;
	// output связывает исходный Output с постоянной записью назначения по точному ID.
	const auto output = stateStore_.dockServer().outputs.constFind(outputId);
	if (output == stateStore_.dockServer().outputs.cend() || !output->attached || output->destinationId.isEmpty())
		return;
	// destinationId сохраняет адрес до публикации формы, которая может обновить общее состояние.
	const auto destinationId = output->destinationId;
	// guard отслеживает удаление окна; editor паркует несохранённый ввод прежней страницы.
	const QPointer<ManagementWindow> guard(this);
	auto *editor = currentEditor();
	if (editor && !editor->canLeave(true))
		return;
	if (!guard)
		return;
	hideSecrets();
	// opened восстанавливает прежний ввод нужного назначения либо начинает его адресное чтение.
	const bool opened = configuration_->restoreDraft(ConfigurationKind::Destination, destinationId) ||
			    configuration_->open(ConfigurationKind::Destination, destinationId, true);
	if (!guard || !opened || stateStore_.dockContext() != context || !stateStore_.dockServer().session ||
	    stateStore_.dockServer().session->id != sessionId)
		return;
	// blocked не запускает вторую активацию, которая могла бы заменить уже открытую карточку.
	const QSignalBlocker blocked(navigation_);
	currentPage_ = 1;
	navigation_->setCurrentRow(currentPage_);
	pages_->setCurrentIndex(currentPage_);
}

} // namespace gost
