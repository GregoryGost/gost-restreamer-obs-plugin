#include "management-window.hpp"

#include "core/state-store.hpp"
#include "core/connection-controller.hpp"
#include "core/settings-store.hpp"
#include "core/restream-controller.hpp"
#include "core/configuration-controller.hpp"
#include "ui/configuration-editor.hpp"
#include "ui/confirmation-dialog.hpp"
#include "ui/restream-dock.hpp"
#include "ui/status-label.hpp"

#include <QApplication>
#include <QCheckBox>
#include <QComboBox>
#include <QEvent>
#include <QFile>
#include <QFormLayout>
#include <QGroupBox>
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

#include <algorithm>
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
				   ConnectionController *connection, RestreamController *restream,
				   ConfigurationController *configuration)
	: QWidget(parent, Qt::Window),
	  stateStore_(stateStore),
	  text_(std::move(text)),
	  connection_(connection),
	  restream_(restream),
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
	// plainLabel создаёт подпись с переносом строк; серверные имена отображаются обычным текстом, без HTML.
	const auto plainLabel = [](QWidget *parent, const char *name) {
		// label принадлежит странице и допускает длинные локализованные подписи.
		auto *label = new QLabel(parent);
		label->setObjectName(QString::fromLatin1(name));
		label->setTextFormat(Qt::PlainText);
		label->setWordWrap(true);
		return label;
	};
	// streamScroll сохраняет размер окна при длинном списке назначений и сообщениях редактора.
	auto *streamScroll = new QScrollArea(pages);
	streamScroll->setFrameShape(QFrame::NoFrame);
	streamScroll->setWidgetResizable(true);
	streamScroll->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
	streamScroll->setWidget(streamPage);
	addPage("Page.Streams", streamScroll, "settings/stream.svg", QStyle::SP_MediaPlay);
	// outputsPage и outputsLayout разделяют изменяемую конфигурацию и прикреплённые Output текущей Session.
	auto *outputsPage = new QWidget(pages);
	// outputsLayout размещает выбор выхода, независимые состояния и явные действия.
	auto *outputsLayout = new QVBoxLayout(outputsPage);
	addPageHeading(*outputsLayout, text_("Page.Outputs"));
	if (configuration_) {
		destinationEditor_ = new ConfigurationEditor(stateStore_, *configuration_, text_,
							     ConfigurationKind::Destination, outputsPage);
		outputsLayout->addWidget(destinationEditor_, 1);
		connect(destinationEditor_, &ConfigurationEditor::requestAttachDestination, this,
			&ManagementWindow::attachDestination);
		connect(destinationEditor_, &ConfigurationEditor::selectionChanged, this,
			&ManagementWindow::refreshOperations);
	}
	currentOutputs_ = new QComboBox(outputsPage);
	currentOutputs_->setObjectName(QStringLiteral("currentOutputs"));
	currentOutputs_->setAccessibleName(text_("Page.Outputs"));
	outputsLayout->addWidget(currentOutputs_);
	outputStatus_ = plainLabel(outputsPage, "selectedOutputStatus");
	outputsLayout->addWidget(outputStatus_);
	// outputButtons создаёт клавиатурно доступные команды с полной шириной переведённого текста.
	auto *outputButtons = new QVBoxLayout;
	outputStart_ = new QPushButton(text_("Operation.StartOutput"), outputsPage);
	outputStop_ = new QPushButton(text_("Operation.StopOutput"), outputsPage);
	outputRestart_ = new QPushButton(text_("Operation.RestartOutput"), outputsPage);
	outputDetach_ = new QPushButton(text_("Operation.DetachOutput"), outputsPage);
	outputStart_->setObjectName(QStringLiteral("outputStartButton"));
	outputStop_->setObjectName(QStringLiteral("outputStopButton"));
	outputRestart_->setObjectName(QStringLiteral("outputRestartButton"));
	outputDetach_->setObjectName(QStringLiteral("outputDetachButton"));
	outputButtons->addWidget(outputStart_);
	outputButtons->addWidget(outputStop_);
	outputButtons->addWidget(outputRestart_);
	outputButtons->addWidget(outputDetach_);
	outputsLayout->addLayout(outputButtons);
	// fallbackBox отделяет предварительный запуск всего выбранного потока от команд одного Output.
	auto *fallbackBox = new QGroupBox(text_("Fallback.OutputsHeading"), outputsPage);
	fallbackBox->setObjectName(QStringLiteral("outputFallbackControls"));
	// fallbackLayout содержит явные действия; результаты появляются только после проверки.
	auto *fallbackLayout = new QVBoxLayout(fallbackBox);
	selectedStream_ = plainLabel(fallbackBox, "selectedStream");
	destinations_ = plainLabel(fallbackBox, "fallbackDestinations");
	fallbackStatus_ = plainLabel(fallbackBox, "fallbackPrecheckStatus");
	fallbackLayout->addWidget(selectedStream_);
	fallbackLayout->addWidget(destinations_);
	fallbackLayout->addWidget(fallbackStatus_);
	checkFallback_ = new QPushButton(text_("Operation.CheckFallback"), fallbackBox);
	checkFallback_->setObjectName(QStringLiteral("fallbackCheckButton"));
	checkFallback_->setToolTip(text_("Fallback.PrecheckHint"));
	startFallback_ = new QPushButton(text_("Operation.StartFallback"), fallbackBox);
	startFallback_->setObjectName(QStringLiteral("fallbackStartButton"));
	startFallback_->setToolTip(text_("Fallback.StartHint") + QLatin1Char('\n') + text_("Fallback.RebuildWarning"));
	startFallback_->setAccessibleDescription(startFallback_->toolTip());
	fallbackLayout->addWidget(checkFallback_);
	fallbackLayout->addWidget(startFallback_);
	outputsLayout->addWidget(fallbackBox);
	outputOperation_ = plainLabel(outputsPage, "outputOperationStatus");
	outputsLayout->addWidget(outputOperation_);
	outputsLayout->addStretch();
	// outputsScroll оставляет все результаты доступными без роста минимальной высоты главного окна.
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
	// Проверка не запускает эфир; обработчик передаёт снимок логического контекста контроллеру.
	connect(checkFallback_, &QPushButton::clicked, this, [this] {
		if (restream_)
			restream_->checkFallback(stateStore_.dockContext());
	});
	connect(startFallback_, &QPushButton::clicked, this, &ManagementWindow::startFallback);
	// Каждая кнопка задаёт отдельное намерение; dispatchOutputAction фиксирует цель до подтверждения.
	connect(outputStart_, &QPushButton::clicked, this,
		[this] { dispatchOutputAction(OperationAction::StartOutput); });
	connect(outputStop_, &QPushButton::clicked, this,
		[this] { dispatchOutputAction(OperationAction::StopOutput); });
	connect(outputRestart_, &QPushButton::clicked, this,
		[this] { dispatchOutputAction(OperationAction::RestartOutput); });
	connect(outputDetach_, &QPushButton::clicked, this,
		[this] { dispatchOutputAction(OperationAction::DetachOutput); });
	connect(currentOutputs_, &QComboBox::currentIndexChanged, this, &ManagementWindow::refreshOperations);
	connect(&stateStore_, &StateStore::operationsChanged, this, &ManagementWindow::refreshOperations);
	connect(&stateStore_, &StateStore::runtimeStateChanged, this, &ManagementWindow::refreshOperations);
	connect(&stateStore_, &StateStore::configurationEditorChanged, this, &ManagementWindow::refreshOperations);
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

// Подтверждение фиксирует точную Session, а контроллер повторно проверяет её перед POST.
void ManagementWindow::attachDestination(const QString &destinationId)
{
	if (!restream_ || !stateStore_.dockServer().session)
		return;
	// context/sessionId фиксируют цель до диалога; destination хранит подтверждённую карточку.
	const auto context = stateStore_.dockContext();
	const auto sessionId = stateStore_.dockServer().session->id;
	const auto destination = stateStore_.configuration().destinations.value(destinationId);
	if (destination.id != destinationId || destination.streamId != context.streamId)
		return;
	// guard отслеживает удаление окна; confirmation принадлежит Qt и безопасно исчезает вместе с родителем.
	const QPointer<ManagementWindow> guard = this;
	const QPointer<QMessageBox> confirmation = new QMessageBox(
		QMessageBox::Question, text_("Operation.AddOutput"),
		text_("Operation.ConfirmAddOutput").arg(destination.name), QMessageBox::Yes | QMessageBox::No, this);
	confirmation->setTextFormat(Qt::PlainText);
	confirmation->setDefaultButton(QMessageBox::No);
	localizeConfirmation(*confirmation, text_);
	// choice не используется до проверки, что окно пережило вложенный цикл событий.
	const auto choice = confirmation->exec();
	delete confirmation.data();
	if (guard && choice == QMessageBox::Yes)
		restream_->addOutput(context, sessionId, destinationId);
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
	refreshOperations();
}

// Обновляет предварительную пригодность и текущие выходы, не меняя наблюдения или серверные намерения.
void ManagementWindow::refreshOperations()
{
	// context задаёт точный выбор, server содержит REST-метаданные, operations — независимые итоги команд.
	// Ссылки читаются в одном GUI-вызове и не удерживаются через диалог.
	const auto &context = stateStore_.dockContext();
	const auto &server = stateStore_.dockServer();
	const auto &operations = stateStore_.operations();
	// connected требует подтверждённого выбора и актуального управляющего подключения.
	const bool connected = restream_ && context.valid() && stateStore_.selectionAvailable() &&
			       stateStore_.connectionStatus() == ConnectionStatus::Connected;
	// busy исключает одновременные несовместимые команды общего исполнителя.
	const bool busy = operations.current && operations.current->phase != OperationPhase::Idle &&
			  operations.current->phase != OperationPhase::Finished;
	// settled запрещает разрешение нового fallback до завершения конфигурационной записи и её сверки.
	const bool settled = !stateStore_.configurationEditor().writing &&
			     !stateStore_.configurationEditor().reconciliationRequired;
	// fallback относится только к нынешнему контексту; старое заключение не разрешает новый запуск.
	const auto &fallback = operations.fallback;
	// matching означает, что имена и revision проверки принадлежат выбранному Stream.
	const bool matching = fallback.context == context;
	selectedStream_->setText(text_("Dock.SelectedStream")
					 .arg(stateStore_.selectedStreamName().isEmpty()
						      ? text_("Observation.NoData")
						      : stateStore_.selectedStreamName()));
	// names содержит именно назначения последней проверки, которые передаются в явный start.
	QStringList names;
	if (matching) {
		// destination содержит безопасное имя и точный ID проверенного назначения.
		for (const auto &destination : fallback.destinations)
			names.append(destination.name.isEmpty() ? text_("Dock.UnnamedOutput") : destination.name);
	}
	destinations_->setText(names.isEmpty() ? QString{}
					       : text_("Fallback.Destinations").arg(names.join(QLatin1Char('\n'))));
	destinations_->setVisible(!names.isEmpty());
	// readinessText отделяет предварительное заключение от действительной готовности медиатракта.
	const char *readinessText = "Fallback.Unknown";
	if (matching) {
		switch (fallback.readiness) {
		case FallbackReadiness::Unknown:
			break;
		case FallbackReadiness::Checking:
			readinessText = "Fallback.Checking";
			break;
		case FallbackReadiness::Ready:
			readinessText = "Fallback.Ready";
			break;
		case FallbackReadiness::Unavailable:
			readinessText = "Fallback.Unavailable";
			break;
		}
	}
	fallbackStatus_->setText(text_(readinessText) +
				 (matching && !fallback.messageKey.isEmpty()
					  ? QLatin1Char('\n') + text_(fallback.messageKey.toUtf8().constData())
					  : QString{}));
	fallbackStatus_->setVisible(
		matching && (fallback.readiness != FallbackReadiness::Unknown || !fallback.messageKey.isEmpty()));
	checkFallback_->setEnabled(connected && !busy && settled);
	// noActive требует полного свежего поиска, а не отсутствия Session в частично прочитанном ответе.
	const bool noActive = server.sessionStatus.availability == RuntimeAvailability::Fresh &&
			      server.sessionStatus.complete &&
			      (!server.session || server.session->state == QStringLiteral("STOPPED") ||
			       server.session->state == QStringLiteral("FAILED") ||
			       server.session->state == QStringLiteral("INTERRUPTED"));
	startFallback_->setEnabled(connected && !busy && settled && noActive && matching &&
				   fallback.readiness == FallbackReadiness::Ready && !fallback.destinations.isEmpty());
	outputOperation_->setText(formatOperations(operations, text_));
	// sessionId отделяет выбор выхода от прежней Session с похожими именами и ID.
	const auto sessionId = server.session ? server.session->id : QString{};
	// ids содержит нынешние прикреплённые выходы в стабильном порядке QMap.
	QStringList ids;
	// output проходит долговечные метаданные текущего состава.
	for (auto output = server.outputs.cbegin(); output != server.outputs.cend(); ++output)
		if (output->attached)
			ids.append(output.key());
	// changed выявляет реальные изменения списка; телеметрия не закрывает раскрытый QComboBox.
	bool changed = displayedOutputContext_ != context || displayedSessionId_ != sessionId ||
		       currentOutputs_->count() != ids.size();
	// row сравнивает точные itemData и подписи, не используя имя для адресации команды.
	for (int row = 0; !changed && row < ids.size(); ++row)
		changed = currentOutputs_->itemData(row).toString() != ids.at(row) ||
			  currentOutputs_->itemText(row) != (server.outputs.value(ids.at(row)).name.isEmpty()
								     ? text_("Dock.UnnamedOutput")
								     : server.outputs.value(ids.at(row)).name);
	if (changed) {
		// previous сохраняется только в том же контексте и той же Session.
		const auto previous = displayedOutputContext_ == context && displayedSessionId_ == sessionId
					      ? currentOutputs_->currentData().toString()
					      : QString{};
		// blocked не позволяет отражению списка вызвать вложенное обновление через currentIndexChanged.
		const QSignalBlocker blocked(currentOutputs_);
		currentOutputs_->clear();
		// id — точная идентичность существующего прикреплённого Output.
		for (const auto &id : ids)
			currentOutputs_->addItem(server.outputs.value(id).name.isEmpty()
							 ? text_("Dock.UnnamedOutput")
							 : server.outputs.value(id).name,
						 id);
		if (currentOutputs_->findData(previous) >= 0)
			currentOutputs_->setCurrentIndex(currentOutputs_->findData(previous));
		displayedOutputContext_ = context;
		displayedSessionId_ = sessionId;
	}
	// id обозначает точный Output, metadata содержит его долговечные намерения, независимо от индекса UI.
	const auto id = currentOutputs_->currentData().toString();
	const auto metadata = server.outputs.value(id);
	// eligible требует свежей Session и свежего полного списка выходов.
	const bool eligible = connected && !busy && server.session &&
			      server.session->state == QStringLiteral("RUNNING") &&
			      server.sessionStatus.availability == RuntimeAvailability::Fresh &&
			      server.outputsStatus.availability == RuntimeAvailability::Fresh &&
			      server.outputsStatus.complete && !id.isEmpty() && metadata.attached;
	outputStart_->setEnabled(eligible && !metadata.desiredRunning);
	outputStop_->setEnabled(eligible && metadata.desiredRunning);
	outputRestart_->setEnabled(eligible);
	outputDetach_->setEnabled(eligible);
	if (destinationEditor_) {
		// destinationId адресует карточку, destination — её подтверждённая конфигурация в текущем кеше.
		const auto destinationId = destinationEditor_->selectedDestinationId();
		const auto destination = stateStore_.configuration().destinations.constFind(destinationId);
		// attached выявляет существующее прикрепление по сохранённому ID назначения, а не имени.
		const bool attached = std::any_of(
			server.outputs.cbegin(), server.outputs.cend(), [&destinationId](const DockOutput &output) {
				return output.attached && output.destinationId == destinationId;
			});
		destinationEditor_->setAttachAvailable(
			connected && !busy && server.session && server.session->state == QStringLiteral("RUNNING") &&
			server.sessionStatus.availability == RuntimeAvailability::Fresh &&
			server.outputsStatus.availability == RuntimeAvailability::Fresh &&
			server.outputsStatus.complete &&
			destination != stateStore_.configuration().destinations.cend() &&
			destination->streamId == context.streamId && !attached);
	}
	// runtimeText остаётся неизвестным без отдельного свежего наблюдения исполнения выхода.
	QString runtimeText = text_("Observation.NoData");
	// runtimeSession связывает наблюдение с той же Session, а не с порядком строк UI.
	const auto runtimeSession = stateStore_.runtime().sessions.constFind(sessionId);
	if (stateStore_.connectionStatus() == ConnectionStatus::Connected &&
	    runtimeSession != stateStore_.runtime().sessions.cend()) {
		// output — отдельное исполнение, чья идентичность и свежесть проверяются перед отображением.
		for (const auto &output : runtimeSession->outputs) {
			if (output.outputId != id ||
			    stateStore_.runtimeEvidence(output.instanceId).state != RuntimeAvailability::Fresh)
				continue;
			if (output.state == QStringLiteral("PUBLISHING"))
				runtimeText = text_("Dock.OutputStreaming");
			else if (output.state == QStringLiteral("CONNECTING"))
				runtimeText = text_("Dock.OutputConnecting");
			else if (output.state == QStringLiteral("RETRYING"))
				runtimeText = text_("Dock.OutputReconnecting");
			else if (output.state == QStringLiteral("FAILED"))
				runtimeText = text_("Dock.OutputFailed");
			else if (output.state == QStringLiteral("STOPPED"))
				runtimeText = text_("Dock.OutputStopped");
			else
				runtimeText = text_("Dock.UnknownStatus");
		}
	}
	// outputKnown разрешает показывать желаемое состояние лишь для свежих метаданных текущей Session.
	const bool outputKnown = stateStore_.connectionStatus() == ConnectionStatus::Connected &&
				 server.sessionStatus.availability == RuntimeAvailability::Fresh &&
				 server.outputsStatus.availability == RuntimeAvailability::Fresh;
	outputStatus_->setText(id.isEmpty() ? text_(outputKnown && server.outputsStatus.complete ? "Outputs.NoCurrent"
												 : "Observation.NoData")
					    : text_("Outputs.Status")
						      .arg(text_(!outputKnown              ? "Observation.NoData"
								 : metadata.desiredRunning ? "Outputs.DesiredRunning"
											   : "Outputs.DesiredStopped"),
							   runtimeText));
}

// Подтверждение относится к копии исходной цели, даже если вложенный цикл диалога обновил выбор и состав.
void ManagementWindow::dispatchOutputAction(OperationAction action)
{
	if (!restream_ || !stateStore_.dockServer().session || currentOutputs_->currentData().toString().isEmpty())
		return;
	// context, sessionId и outputId фиксируют точную цель до модального подтверждения.
	const auto context = displayedOutputContext_;
	const auto sessionId = displayedSessionId_;
	const auto outputId = currentOutputs_->currentData().toString();
	if (action == OperationAction::DetachOutput || action == OperationAction::RestartOutput) {
		// guard запрещает обращение к контроллеру через уничтоженное окно после диалога.
		const QPointer<ManagementWindow> guard = this;
		// choice подтверждает прерывание именно исходного Output; новое состояние проверяет фасад.
		const auto choice =
			askConfirmation(this, text_, text_("Operation.ConfirmTitle"),
					text_(action == OperationAction::DetachOutput ? "Operation.ConfirmDetach"
										      : "Operation.ConfirmRestart"));
		if (!guard || choice != QMessageBox::Yes)
			return;
	}
	restream_->outputActions(context, sessionId, outputId, action);
}

// Фиксирует revision и имена показанного непустого набора; контроллер отвергает устаревшую проверку.
void ManagementWindow::startFallback()
{
	if (!restream_)
		return;
	// fallback копируется до диалога: callback обновления не может заменить его назначения или revision.
	const auto fallback = stateStore_.operations().fallback;
	if (fallback.destinations.isEmpty() || fallback.context != stateStore_.dockContext())
		return;
	// names показывает неизменяемый набор, которому будет адресован явный предварительный запуск.
	QStringList names;
	// destination — одно назначение из проверенного списка, без endpoint и ключей.
	for (const auto &destination : fallback.destinations)
		names.append(destination.name.isEmpty() ? text_("Dock.UnnamedOutput") : destination.name);
	// guard отслеживает окно, confirmation выводит имена обычным текстом и допускает удаление родителем.
	const QPointer<ManagementWindow> guard = this;
	const QPointer<QMessageBox> confirmation =
		new QMessageBox(QMessageBox::Question, text_("Operation.StartFallback"),
				text_("Fallback.ConfirmStart").arg(names.join(QLatin1Char('\n'))),
				QMessageBox::Yes | QMessageBox::No, this);
	confirmation->setTextFormat(Qt::PlainText);
	confirmation->setDefaultButton(QMessageBox::No);
	localizeConfirmation(*confirmation, text_);
	// choice содержит ответ только для прежнего набора назначений и проверяется после жизни окна.
	const auto choice = confirmation->exec();
	delete confirmation.data();
	if (!guard || choice != QMessageBox::Yes)
		return;
	restream_->startFallback(fallback.context, fallback.revision);
}

// Переходит в существующую страницу выходов, не перенаправляя устаревшую ссылку на новый эфир.
void ManagementWindow::selectOutput(const DockContext &context, const QString &sessionId, const QString &outputId)
{
	refreshOperations();
	if (stateStore_.dockContext() != context || !stateStore_.dockServer().session ||
	    stateStore_.dockServer().session->id != sessionId)
		return;
	// index обозначает точный itemData; одинаковые отображаемые имена не влияют на выбор.
	const int index = currentOutputs_->findData(outputId);
	if (index >= 0) {
		currentOutputs_->setCurrentIndex(index);
		navigation_->setCurrentRow(1);
	}
}

} // namespace gost
