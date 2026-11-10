#include "restream-dock.hpp"

#include "core/state-store.hpp"
#include "ui/connection-text.hpp"

#include <QFormLayout>
#include <QHBoxLayout>
#include <QLabel>
#include <QPalette>
#include <QToolButton>
#include <QVBoxLayout>

#include <utility>

namespace gost {

RestreamDock::RestreamDock(StateStore &stateStore, TextLookup text, QWidget *parent, QString version)
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
	server_ = new QLabel(this);
	server_->setObjectName(QStringLiteral("serverObservation"));
	server_->setTextFormat(Qt::PlainText);
	server_->setWordWrap(true);
	observations->addRow(text_("Server.Label"), server_);
	stream_ = new QLabel(this);
	stream_->setObjectName(QStringLiteral("streamObservation"));
	stream_->setTextFormat(Qt::PlainText);
	stream_->setWordWrap(true);
	observations->addRow(text_("Stream.Label"), stream_);
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
	notice_ = new QLabel(this);
	notice_->setTextFormat(Qt::PlainText);
	notice_->setWordWrap(true);
	layout->addWidget(notice_);
	layout->addStretch();
	if (!version.isEmpty()) {
		auto *versionLabel = new QLabel(text_("Plugin.Version").arg(version), this);
		versionLabel->setObjectName(QStringLiteral("pluginVersion"));
		versionLabel->setTextFormat(Qt::PlainText);
		auto font = versionLabel->font();
		font.setPointSizeF(qMax(8.0, font.pointSizeF() - 1.0));
		versionLabel->setFont(font);
		auto palette = versionLabel->palette();
		palette.setColor(QPalette::WindowText, palette.color(QPalette::Disabled, QPalette::WindowText));
		versionLabel->setPalette(palette);
		layout->addWidget(versionLabel, 0, Qt::AlignRight);
	}
	connect(gear, &QToolButton::clicked, this, &RestreamDock::managementRequested);
	connect(&stateStore_, &StateStore::obsStateChanged, this, &RestreamDock::refresh);
	connect(&stateStore_, &StateStore::serverStateChanged, this, &RestreamDock::refresh);
	refresh();
}

void RestreamDock::refresh()
{
	server_->setText(stateStore_.serverOrigin().isEmpty()
				 ? text_("Observation.NoData")
				 : text_(connectionTextKey(stateStore_.connectionStatus())));
	const auto streamName = stateStore_.selectedStreamName();
	stream_->setText(streamName.isEmpty() ? text_("Observation.NoData") : streamName);
	QStringList notices;
	if (stateStore_.connectionStatus() != ConnectionStatus::Connected) {
		notices.append(text_("Server.NotConnected"));
	}
	if (!stateStore_.safeErrorKey().isEmpty()) {
		notices.append(text_(stateStore_.safeErrorKey().toUtf8().constData()));
	}
	if (!stateStore_.selectedStreamId().isEmpty() && !stateStore_.selectionAvailable()) {
		notices.append(text_("Stream.SelectionUnavailable"));
	}
	notice_->setText(notices.join(QLatin1Char('\n')));
	notice_->setVisible(!notices.isEmpty());
	const auto &profile = stateStore_.profileName();
	profile_->setText(profile && !profile->isEmpty() ? *profile : text_("Observation.NoData"));
	const auto active = stateStore_.streamingActive();
	streaming_->setText(active ? text_(*active ? "OBS.StreamingActive" : "OBS.StreamingInactive")
				   : text_("Observation.NoData"));
}

} // namespace gost
