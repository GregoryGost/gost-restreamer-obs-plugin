#include "management-window.hpp"

#include "core/state-store.hpp"
#include "core/connection-controller.hpp"
#include "core/settings-store.hpp"
#include "ui/connection-text.hpp"

#include <QCheckBox>
#include <QComboBox>
#include <QFormLayout>
#include <QHBoxLayout>
#include <QLabel>
#include <QListWidget>
#include <QLineEdit>
#include <QMessageBox>
#include <QPushButton>
#include <QSignalBlocker>
#include <QStackedWidget>
#include <QTabWidget>
#include <QVBoxLayout>

#include <utility>

namespace gost {
namespace {

QWidget *makePage(const QString &title, const QString &message, QWidget *parent)
{
	auto *page = new QWidget(parent);
	auto *layout = new QVBoxLayout(page);
	auto *heading = new QLabel(title, page);
	QFont font = heading->font();
	font.setBold(true);
	heading->setFont(font);
	layout->addWidget(heading);
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
	auto *navigation = new QListWidget(this);
	navigation->setObjectName(QStringLiteral("managementNavigation"));
	navigation->setAccessibleName(text_("Management.Sections"));
	navigation->setSizePolicy(QSizePolicy::Maximum, QSizePolicy::Expanding);
	auto *pages = new QStackedWidget(this);
	pages->setObjectName(QStringLiteral("managementPages"));
	layout->addWidget(navigation);
	layout->addWidget(pages, 1);

	const auto addPage = [&](const char *titleKey, QWidget *page) {
		navigation->addItem(text_(titleKey));
		pages->addWidget(page);
	};
	addPage("Page.Streams", makePage(text_("Page.Streams"), text_("Page.Streams.NotAvailable"), pages));
	addPage("Page.Outputs", makePage(text_("Page.Outputs"), text_("Page.Outputs.NotAvailable"), pages));
	auto *groups = new QWidget(pages);
	auto *groupsLayout = new QVBoxLayout(groups);
	auto *tabs = new QTabWidget(groups);
	tabs->addTab(makePage(text_("Page.Groups"), text_("Page.Groups.NotAvailable"), tabs), text_("Page.Groups"));
	tabs->addTab(makePage(text_("Page.Profiles"), text_("Page.Profiles.NotAvailable"), tabs),
		     text_("Page.Profiles"));
	groupsLayout->addWidget(tabs);
	addPage("Page.GroupsProfiles", groups);
	addPage("Page.Fallback", makePage(text_("Page.Fallback"), text_("Page.Fallback.NotAvailable"), pages));

	auto *diagnostics = new QWidget(pages);
	auto *diagnosticsLayout = new QVBoxLayout(diagnostics);
	auto *observations = new QFormLayout;
	observations->setRowWrapPolicy(QFormLayout::WrapLongRows);
	profile_ = new QLabel(diagnostics);
	profile_->setObjectName(QStringLiteral("obsProfile"));
	profile_->setTextFormat(Qt::PlainText);
	profile_->setWordWrap(true);
	observations->addRow(text_("OBS.Profile"), profile_);
	streaming_ = new QLabel(diagnostics);
	streaming_->setObjectName(QStringLiteral("obsStreaming"));
	streaming_->setWordWrap(true);
	observations->addRow(text_("OBS.Streaming"), streaming_);
	diagnosticsLayout->addLayout(observations);
	auto *notice = new QLabel(text_("Page.Diagnostics.NotAvailable"), diagnostics);
	notice->setWordWrap(true);
	diagnosticsLayout->addWidget(notice);
	diagnosticsLayout->addStretch();
	addPage("Page.Diagnostics", diagnostics);
	auto *settingsPage = new QWidget(pages);
	auto *settingsLayout = new QVBoxLayout(settingsPage);
	auto *heading = new QLabel(text_("Page.Settings"), settingsPage);
	auto headingFont = heading->font();
	headingFont.setBold(true);
	heading->setFont(headingFont);
	settingsLayout->addWidget(heading);
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
	connectionStatus_ = new QLabel(settingsPage);
	connectionStatus_->setObjectName(QStringLiteral("connectionStatus"));
	connectionStatus_->setTextFormat(Qt::PlainText);
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
	addPage("Page.Settings", settingsPage);
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
	const auto active = stateStore_.streamingActive();
	streaming_->setText(active ? text_(*active ? "OBS.StreamingActive" : "OBS.StreamingInactive")
				   : text_("Observation.NoData"));
	const QSignalBlocker blockAuto(autoConnect_);
	const QSignalBlocker blockReconnect(reconnect_);
	autoConnect_->setChecked(stateStore_.autoConnect());
	reconnect_->setChecked(stateStore_.reconnect());
	autoConnect_->setEnabled(connection_ != nullptr);
	reconnect_->setEnabled(connection_ != nullptr);
	connectButton_->setEnabled(connection_ != nullptr);
	disconnectButton_->setEnabled(connection_ && stateStore_.connectionStatus() != ConnectionStatus::Disconnected);
	connectionStatus_->setText(text_(connectionTextKey(stateStore_.connectionStatus())));
	if (displayedSelectionId_ != stateStore_.selectedStreamId()) {
		refreshStreams();
	}
	streams_->setEnabled(connection_ && stateStore_.profileResolved() && stateStore_.streamsComplete() &&
			     stateStore_.connectionStatus() == ConnectionStatus::Connected);
	QStringList messages;
	if (!stateStore_.profileNoticeKey().isEmpty()) {
		messages.append(text_(stateStore_.profileNoticeKey().toUtf8().constData()));
	}
	if (!stateStore_.safeErrorKey().isEmpty()) {
		messages.append(text_(stateStore_.safeErrorKey().toUtf8().constData()));
	}
	if (stateStore_.settingsUnsaved()) {
		messages.append(text_("Settings.Unsaved"));
	}
	connectionMessage_->setText(messages.join(QLatin1Char('\n')));
}

} // namespace gost
