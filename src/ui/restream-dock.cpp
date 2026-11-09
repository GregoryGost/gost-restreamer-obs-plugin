#include "restream-dock.hpp"

#include "core/state-store.hpp"

#include <QFormLayout>
#include <QHBoxLayout>
#include <QLabel>
#include <QToolButton>
#include <QVBoxLayout>

#include <utility>

namespace gost {

RestreamDock::RestreamDock(StateStore &stateStore, TextLookup text, QWidget *parent)
	: QWidget(parent),
	  stateStore_(stateStore),
	  text_(std::move(text))
{
	setObjectName(QStringLiteral("restreamDock"));
	auto *layout = new QVBoxLayout(this);
	auto *header = new QHBoxLayout;
	auto *title = new QLabel(text_("Plugin.Name"), this);
	header->addWidget(title, 1);
	auto *gear = new QToolButton(this);
	gear->setObjectName(QStringLiteral("managementButton"));
	gear->setText(QString::fromUtf8("⚙"));
	gear->setToolTip(text_("Management.Open"));
	gear->setAccessibleName(text_("Management.Open"));
	header->addWidget(gear);
	layout->addLayout(header);

	auto *observations = new QFormLayout;
	observations->setRowWrapPolicy(QFormLayout::WrapLongRows);
	auto *unknownServer = new QLabel(text_("Observation.NoData"), this);
	unknownServer->setObjectName(QStringLiteral("serverObservation"));
	observations->addRow(text_("Server.Label"), unknownServer);
	auto *unknownStream = new QLabel(text_("Observation.NoData"), this);
	unknownStream->setObjectName(QStringLiteral("streamObservation"));
	observations->addRow(text_("Stream.Label"), unknownStream);
	profile_ = new QLabel(this);
	profile_->setObjectName(QStringLiteral("obsProfile"));
	profile_->setWordWrap(true);
	profile_->setTextFormat(Qt::PlainText);
	observations->addRow(text_("OBS.Profile"), profile_);
	streaming_ = new QLabel(this);
	streaming_->setObjectName(QStringLiteral("obsStreaming"));
	streaming_->setWordWrap(true);
	observations->addRow(text_("OBS.Streaming"), streaming_);
	layout->addLayout(observations);
	auto *notice = new QLabel(text_("Server.NotConnected"), this);
	notice->setWordWrap(true);
	layout->addWidget(notice);
	layout->addStretch();
	connect(gear, &QToolButton::clicked, this, &RestreamDock::managementRequested);
	connect(&stateStore_, &StateStore::obsStateChanged, this, &RestreamDock::refresh);
	refresh();
}

void RestreamDock::refresh()
{
	const auto &profile = stateStore_.profileName();
	profile_->setText(profile && !profile->isEmpty() ? *profile : text_("Observation.NoData"));
	const auto active = stateStore_.streamingActive();
	streaming_->setText(active ? text_(*active ? "OBS.StreamingActive" : "OBS.StreamingInactive")
				   : text_("Observation.NoData"));
}

} // namespace gost
