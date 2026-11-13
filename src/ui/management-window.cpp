#include "management-window.hpp"

#include "core/state-store.hpp"
#include "core/connection-controller.hpp"
#include "core/settings-store.hpp"
#include "core/restream-controller.hpp"
#include "ui/restream-dock.hpp"
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
#include <QSignalBlocker>
#include <QScrollArea>
#include <QSpinBox>
#include <QStackedWidget>
#include <QStyle>
#include <QTabWidget>
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

// Создаёт страницы единого окна и соединения с общими сервисами; закрытие окна только скрывает его.
ManagementWindow::ManagementWindow(StateStore &stateStore, TextLookup text, QWidget *parent,
				   ConnectionController *connection, RestreamController *restream)
	: QWidget(parent, Qt::Window),
	  stateStore_(stateStore),
	  text_(std::move(text)),
	  connection_(connection),
	  restream_(restream)
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
	pages->setObjectName(QStringLiteral("managementPages"));
	layout->addWidget(navigation);
	layout->addWidget(pages, 1);

	// addPage добавляет page с переводом titleKey и иконкой icon либо fallback; callback сохраняет порядок разделов.
	const auto addPage = [&](const char *titleKey, QWidget *page, const char *icon,
				 QStyle::StandardPixmap fallback) {
		navigation->addSection(text_(titleKey), icon, fallback);
		pages->addWidget(page);
	};
	// streamPage и streamLayout содержат только текущий Stream, проверку и предварительный запуск.
	auto *streamPage = new QWidget(pages);
	// streamLayout располагает имена назначений, предварительное заключение и команды.
	auto *streamLayout = new QVBoxLayout(streamPage);
	addPageHeading(*streamLayout, text_("Page.Streams"));
	// plainLabel создаёт подпись с переносом строк; серверные имена отображаются обычным текстом, без HTML.
	const auto plainLabel = [](QWidget *parent, const char *name) {
		// label принадлежит странице и допускает длинные локализованные подписи.
		auto *label = new QLabel(parent);
		label->setObjectName(QString::fromLatin1(name));
		label->setTextFormat(Qt::PlainText);
		label->setWordWrap(true);
		return label;
	};
	selectedStream_ = plainLabel(streamPage, "selectedStream");
	destinations_ = plainLabel(streamPage, "fallbackDestinations");
	fallbackStatus_ = plainLabel(streamPage, "fallbackPrecheckStatus");
	streamLayout->addWidget(selectedStream_);
	streamLayout->addWidget(destinations_);
	streamLayout->addWidget(fallbackStatus_);
	// warning объясняет предел клиентской проверки и необходимость обновления старой заглушки.
	auto *warning = plainLabel(streamPage, "fallbackRebuildWarning");
	warning->setText(text_("Fallback.RebuildWarning"));
	streamLayout->addWidget(warning);
	checkFallback_ = new QPushButton(text_("Operation.CheckFallback"), streamPage);
	checkFallback_->setObjectName(QStringLiteral("fallbackCheckButton"));
	startFallback_ = new QPushButton(text_("Operation.StartFallback"), streamPage);
	startFallback_->setObjectName(QStringLiteral("fallbackStartButton"));
	startFallback_->setToolTip(text_("Fallback.StartHint"));
	startFallback_->setAccessibleDescription(text_("Fallback.StartHint"));
	streamLayout->addWidget(checkFallback_);
	streamLayout->addWidget(startFallback_);
	streamOperation_ = plainLabel(streamPage, "streamOperationStatus");
	streamLayout->addWidget(streamOperation_);
	streamLayout->addStretch();
	// streamScroll сохраняет размер окна при длинном списке назначений и подробном результате пакета.
	auto *streamScroll = new QScrollArea(pages);
	streamScroll->setFrameShape(QFrame::NoFrame);
	streamScroll->setWidgetResizable(true);
	streamScroll->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
	streamScroll->setWidget(streamPage);
	addPage("Page.Streams", streamScroll, "settings/stream.svg", QStyle::SP_MediaPlay);
	// outputsPage и outputsLayout показывают лишь существующие выходы нынешней Session, без редактора назначений.
	auto *outputsPage = new QWidget(pages);
	// outputsLayout размещает выбор выхода, независимые состояния и явные действия.
	auto *outputsLayout = new QVBoxLayout(outputsPage);
	addPageHeading(*outputsLayout, text_("Page.Outputs"));
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
	// groups содержит существующие информационные вкладки групп и профилей.
	auto *groups = new QWidget(pages);
	// groupsLayout размещает внутренние вкладки без дополнительного главного окна.
	auto *groupsLayout = new QVBoxLayout(groups);
	// tabs сохраняет внутреннюю навигацию групп и профилей.
	auto *tabs = new QTabWidget(groups);
	tabs->addTab(makePage(text_("Page.Groups"), text_("Page.Groups.NotAvailable"), tabs), text_("Page.Groups"));
	tabs->addTab(makePage(text_("Page.Profiles"), text_("Page.Profiles.NotAvailable"), tabs),
		     text_("Page.Profiles"));
	groupsLayout->addWidget(tabs);
	addPage("Page.GroupsProfiles", groups, "sources/group.svg", QStyle::SP_DirIcon);
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
	// streamingNotice напоминает о штатной настройке сервиса трансляции в OBS.
	auto *streamingNotice = new QLabel(text_("Settings.ObsServiceNotice"), settingsPage);
	streamingNotice->setWordWrap(true);
	settingsLayout->addWidget(streamingNotice);
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
	advancedLayout->addLayout(advancedForm);
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
			// choice подтверждает смену уже выбранного сервера, сохраняя явный пользовательский выбор.
			const auto choice = QMessageBox::question(this, text_("Settings.ChangeServerTitle"),
								  text_("Settings.ChangeServerConfirm").arg(*origin),
								  QMessageBox::Yes | QMessageBox::No, QMessageBox::No);
			if (choice != QMessageBox::Yes) {
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
	// Обработчик использует точный itemData выбранного index, а не отображаемое имя Stream.
	connect(streams_, &QComboBox::currentIndexChanged, this, [this](int index) {
		if (connection_ && index >= 0) {
			connection_->selectStream(streams_->itemData(index).toString());
		}
	});
	connect(navigation, &QListWidget::currentRowChanged, pages, &QStackedWidget::setCurrentIndex);
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
	navigation->setCurrentRow(0);
	refreshStreams();
	refresh();
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
	autoConnect_->setChecked(stateStore_.autoConnect());
	reconnect_->setChecked(stateStore_.reconnect());
	if (sseRejectionRetries_->value() != stateStore_.sseRejectionRetries()) {
		sseRejectionRetries_->setValue(stateStore_.sseRejectionRetries());
	}
	autoConnect_->setEnabled(connection_ != nullptr);
	reconnect_->setEnabled(connection_ != nullptr);
	sseRejectionRetries_->setEnabled(connection_ && stateStore_.reconnect());
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
	destinations_->setText(
		names.isEmpty()
			? text_(matching && fallback.messageKey == QStringLiteral("Fallback.NoEnabledDestinations")
					? "Fallback.NoEnabledDestinations"
					: "Fallback.DestinationsUnknown")
			: text_("Fallback.Destinations").arg(names.join(QLatin1Char('\n'))));
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
	checkFallback_->setEnabled(connected && !busy);
	// noActive требует полного свежего поиска, а не отсутствия Session в частично прочитанном ответе.
	const bool noActive = server.sessionStatus.availability == RuntimeAvailability::Fresh &&
			      server.sessionStatus.complete &&
			      (!server.session || server.session->state == QStringLiteral("STOPPED") ||
			       server.session->state == QStringLiteral("FAILED") ||
			       server.session->state == QStringLiteral("INTERRUPTED"));
	startFallback_->setEnabled(connected && !busy && noActive && matching &&
				   fallback.readiness == FallbackReadiness::Ready && !fallback.destinations.isEmpty());
	streamOperation_->setText(formatOperations(operations, text_));
	outputOperation_->setText(streamOperation_->text());
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
		// choice подтверждает прерывание именно исходного Output; новое состояние проверяет фасад.
		const auto choice = QMessageBox::question(this, text_("Operation.ConfirmTitle"),
							  text_(action == OperationAction::DetachOutput
									? "Operation.ConfirmDetach"
									: "Operation.ConfirmRestart"),
							  QMessageBox::Yes | QMessageBox::No, QMessageBox::No);
		if (choice != QMessageBox::Yes)
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
	// confirmation выводит имена назначений только обычным текстом, даже если серверное имя похоже на HTML.
	QMessageBox confirmation(QMessageBox::Question, text_("Operation.StartFallback"),
				 text_("Fallback.ConfirmStart").arg(names.join(QLatin1Char('\n'))),
				 QMessageBox::Yes | QMessageBox::No, this);
	confirmation.setTextFormat(Qt::PlainText);
	confirmation.setDefaultButton(QMessageBox::No);
	if (confirmation.exec() != QMessageBox::Yes)
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
