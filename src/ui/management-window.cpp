#include "management-window.hpp"

#include "core/state-store.hpp"
#include "core/connection-controller.hpp"
#include "core/settings-store.hpp"
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
#include <QSpinBox>
#include <QStackedWidget>
#include <QStyle>
#include <QTabWidget>
#include <QVBoxLayout>

#include <limits>
#include <utility>

namespace gost {
namespace {

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

ManagementWindow::ManagementWindow(StateStore &stateStore, TextLookup text, QWidget *parent,
				   ConnectionController *connection)
	: QWidget(parent, Qt::Window),
	  stateStore_(stateStore),
	  text_(std::move(text)),
	  connection_(connection)
{
	setObjectName(QStringLiteral("managementWindow"));
	setWindowTitle(text_("Management.Title"));
	setAttribute(Qt::WA_DeleteOnClose, false);
	setAttribute(Qt::WA_QuitOnClose, false);
	resize(900, 600);
	auto *layout = new QHBoxLayout(this);
	auto *navigation = new NavigationList(this);
	navigation->setObjectName(QStringLiteral("managementNavigation"));
	navigation->setAccessibleName(text_("Management.Sections"));
	navigation->setSizePolicy(QSizePolicy::Maximum, QSizePolicy::Expanding);
	auto *pages = new QStackedWidget(this);
	pages->setObjectName(QStringLiteral("managementPages"));
	layout->addWidget(navigation);
	layout->addWidget(pages, 1);

	const auto addPage = [&](const char *titleKey, QWidget *page, const char *icon,
				 QStyle::StandardPixmap fallback) {
		navigation->addSection(text_(titleKey), icon, fallback);
		pages->addWidget(page);
	};
	addPage("Page.Streams", makePage(text_("Page.Streams"), text_("Page.Streams.NotAvailable"), pages),
		"settings/stream.svg", QStyle::SP_MediaPlay);
	addPage("Page.Outputs", makePage(text_("Page.Outputs"), text_("Page.Outputs.NotAvailable"), pages),
		"settings/output.svg", QStyle::SP_DriveNetIcon);
	auto *groups = new QWidget(pages);
	auto *groupsLayout = new QVBoxLayout(groups);
	auto *tabs = new QTabWidget(groups);
	tabs->addTab(makePage(text_("Page.Groups"), text_("Page.Groups.NotAvailable"), tabs), text_("Page.Groups"));
	tabs->addTab(makePage(text_("Page.Profiles"), text_("Page.Profiles.NotAvailable"), tabs),
		     text_("Page.Profiles"));
	groupsLayout->addWidget(tabs);
	addPage("Page.GroupsProfiles", groups, "sources/group.svg", QStyle::SP_DirIcon);
	addPage("Page.Fallback", makePage(text_("Page.Fallback"), text_("Page.Fallback.NotAvailable"), pages),
		"sources/image.svg", QStyle::SP_FileIcon);

	auto *diagnostics = new QWidget(pages);
	auto *diagnosticsLayout = new QVBoxLayout(diagnostics);
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
	auto *notice = new QLabel(text_("Page.Diagnostics.NotAvailable"), diagnostics);
	notice->setWordWrap(true);
	diagnosticsLayout->addWidget(notice);
	diagnosticsLayout->addStretch();
	addPage("Page.Diagnostics", diagnostics, "sources/window.svg", QStyle::SP_FileDialogDetailedView);
	auto *settingsPage = new QWidget(pages);
	auto *settingsLayout = new QVBoxLayout(settingsPage);
	addPageHeading(*settingsLayout, text_("Page.Settings"));
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
	auto *streamingNotice = new QLabel(text_("Settings.ObsServiceNotice"), settingsPage);
	streamingNotice->setWordWrap(true);
	settingsLayout->addWidget(streamingNotice);
	settingsLayout->addStretch();
	addPage("Page.Settings", settingsPage, "settings/general.svg", QStyle::SP_ComputerIcon);

	auto *advancedPage = new QWidget(pages);
	auto *advancedLayout = new QVBoxLayout(advancedPage);
	addPageHeading(*advancedLayout, text_("Page.Advanced"));
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

	connect(connectButton_, &QPushButton::clicked, this, [this] {
		if (!connection_) {
			return;
		}
		const auto origin = SettingsStore::canonicalOrigin(apiUrl_->text());
		if (origin && !stateStore_.serverOrigin().isEmpty() && *origin != stateStore_.serverOrigin()) {
			const auto choice = QMessageBox::question(this, text_("Settings.ChangeServerTitle"),
								  text_("Settings.ChangeServerConfirm").arg(*origin),
								  QMessageBox::Yes | QMessageBox::No, QMessageBox::No);
			if (choice != QMessageBox::Yes) {
				return;
			}
		}
		connection_->connectToServer(apiUrl_->text());
	});
	connect(disconnectButton_, &QPushButton::clicked, this, [this] {
		if (connection_) {
			connection_->disconnectFromServer();
		}
	});
	connect(autoConnect_, &QCheckBox::toggled, this, [this](bool enabled) {
		if (connection_) {
			connection_->setAutoConnect(enabled);
		}
	});
	connect(reconnect_, &QCheckBox::toggled, this, [this](bool enabled) {
		if (connection_) {
			connection_->setReconnect(enabled);
		}
	});
	connect(sseRejectionRetries_, &QSpinBox::valueChanged, this, [this](int retries) {
		if (connection_) {
			connection_->setSseRejectionRetries(retries);
		}
	});
	connect(streams_, &QComboBox::currentIndexChanged, this, [this](int index) {
		if (connection_ && index >= 0) {
			connection_->selectStream(streams_->itemData(index).toString());
		}
	});
	connect(navigation, &QListWidget::currentRowChanged, pages, &QStackedWidget::setCurrentIndex);
	connect(&stateStore_, &StateStore::obsStateChanged, this, &ManagementWindow::refresh);
	connect(&stateStore_, &StateStore::serverStateChanged, this, &ManagementWindow::refresh);
	connect(&stateStore_, &StateStore::streamsChanged, this, &ManagementWindow::refreshStreams);
	navigation->setCurrentRow(0);
	refreshStreams();
	refresh();
}

void ManagementWindow::refreshStreams()
{
	const QSignalBlocker blockStreams(streams_);
	streams_->clear();
	streams_->addItem(text_("Stream.Choose"), QString{});
	for (const auto &stream : stateStore_.streams()) {
		streams_->addItem(stream.name, stream.id);
	}
	displayedSelectionId_ = stateStore_.selectedStreamId();
	int selected = streams_->findData(displayedSelectionId_);
	if (selected < 0 && !displayedSelectionId_.isEmpty()) {
		streams_->addItem(text_(stateStore_.streamsComplete() ? "Stream.Unavailable" : "Stream.Unconfirmed")
					  .arg(displayedSelectionId_),
				  displayedSelectionId_);
		selected = streams_->count() - 1;
	}
	streams_->setCurrentIndex(selected < 0 ? 0 : selected);
}

void ManagementWindow::refresh()
{
	const auto &profile = stateStore_.profileName();
	profile_->setText(profile && !profile->isEmpty() ? *profile : text_("Observation.NoData"));
	streaming_->setStreamingStatus(text_, stateStore_.streamingActive());
	const QSignalBlocker blockAuto(autoConnect_);
	const QSignalBlocker blockReconnect(reconnect_);
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
	if (displayedSelectionId_ != stateStore_.selectedStreamId()) {
		refreshStreams();
	}
	const bool streamsComplete = stateStore_.streamsComplete();
	const bool canChooseStream = streamsComplete ? stateStore_.connectionStatus() == ConnectionStatus::Connected
						     : !stateStore_.streams().isEmpty();
	streams_->setEnabled(connection_ && stateStore_.profileResolved() && canChooseStream);
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
	const auto unsavedMessage = stateStore_.settingsUnsaved() ? text_("Settings.Unsaved") : QString{};
	if (!unsavedMessage.isEmpty())
		messages.append(unsavedMessage);
	connectionMessage_->setText(messages.join(QLatin1Char('\n')));
	advancedSettingsMessage_->setText(unsavedMessage);
	advancedSettingsMessage_->setVisible(!unsavedMessage.isEmpty());
}

} // namespace gost
