#include "management-window.hpp"

#include "core/state-store.hpp"

#include <QFormLayout>
#include <QHBoxLayout>
#include <QLabel>
#include <QListWidget>
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

ManagementWindow::ManagementWindow(StateStore &stateStore, TextLookup text, QWidget *parent)
	: QWidget(parent, Qt::Window),
	  stateStore_(stateStore),
	  text_(std::move(text))
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
	addPage("Page.Settings", makePage(text_("Page.Settings"), text_("Page.Settings.NotAvailable"), pages));
	connect(navigation, &QListWidget::currentRowChanged, pages, &QStackedWidget::setCurrentIndex);
	connect(&stateStore_, &StateStore::obsStateChanged, this, &ManagementWindow::refresh);
	navigation->setCurrentRow(0);
	refresh();
}

void ManagementWindow::refresh()
{
	const auto &profile = stateStore_.profileName();
	profile_->setText(profile && !profile->isEmpty() ? *profile : text_("Observation.NoData"));
	const auto active = stateStore_.streamingActive();
	streaming_->setText(active ? text_(*active ? "OBS.StreamingActive" : "OBS.StreamingInactive")
				   : text_("Observation.NoData"));
}

} // namespace gost
