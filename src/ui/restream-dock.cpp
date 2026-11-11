#include "restream-dock.hpp"

#include "core/state-store.hpp"
#include "core/stats-baseline.hpp"
#include "ui/status-label.hpp"

#include <QFormLayout>
#include <QHBoxLayout>
#include <QLabel>
#include <QListView>
#include <QLocale>
#include <QPalette>
#include <QPushButton>
#include <QSet>
#include <QStringListModel>
#include <QTimer>
#include <QToolButton>
#include <QVBoxLayout>

#include <algorithm>
#include <cmath>
#include <limits>
#include <optional>
#include <utility>

namespace gost {
namespace {

bool fresh(RuntimeAvailability evidence)
{
	return evidence == RuntimeAvailability::Fresh;
}

QLabel *valueLabel(QWidget *parent, const char *name)
{
	auto *label = new QLabel(parent);
	label->setObjectName(QString::fromLatin1(name));
	label->setTextFormat(Qt::PlainText);
	label->setWordWrap(true);
	return label;
}

const char *sessionKey(const QString &state)
{
	if (state == QStringLiteral("STARTING"))
		return "Dock.SessionStarting";
	if (state == QStringLiteral("RUNNING"))
		return "Dock.SessionRunning";
	if (state == QStringLiteral("STOPPING"))
		return "Dock.SessionStopping";
	if (state == QStringLiteral("STOPPED"))
		return "Dock.SessionStopped";
	if (state == QStringLiteral("FAILED"))
		return "Dock.SessionFailed";
	if (state == QStringLiteral("INTERRUPTED"))
		return "Dock.SessionInterrupted";
	return state.isEmpty() ? "Observation.NoData" : "Dock.UnknownStatus";
}

const char *outputKey(const QString &state)
{
	if (state == QStringLiteral("CONNECTING"))
		return "Dock.OutputConnecting";
	if (state == QStringLiteral("PUBLISHING"))
		return "Dock.OutputStreaming";
	if (state == QStringLiteral("RETRYING"))
		return "Dock.OutputReconnecting";
	if (state == QStringLiteral("FAILED"))
		return "Dock.OutputFailed";
	if (state == QStringLiteral("STOPPED"))
		return "Dock.OutputStopped";
	return state.isEmpty() ? "Observation.NoData" : "Dock.UnknownStatus";
}

const char *periodKey(StatsBaseline::Period period)
{
	switch (period) {
	case StatsBaseline::Period::CurrentRun:
		return "Dock.PeriodRun";
	case StatsBaseline::Period::SinceReset:
		return "Dock.PeriodReset";
	case StatsBaseline::Period::SinceNewSample:
		return "Dock.PeriodNewSample";
	}
	return "Dock.PeriodRun";
}

QString bitrate(const QLocale &locale, const TextLookup &text, double bitsPerSecond)
{
	return text("Dock.Megabits").arg(locale.toString(bitsPerSecond / 1000000.0, 'f', 2));
}

QString counterText(const QLocale &locale, const TextLookup &text, const StatsBaseline &baseline)
{
	const auto reading = baseline.reading();
	if (reading.limited)
		return text("Dock.ChangeUnavailable");
	if (!reading.counters)
		return text("Observation.NoData");
	const auto count = locale.toString(reading.counters->count);
	if (!reading.counters->total)
		return text("Dock.CountPeriod").arg(count, text(periodKey(reading.period)));
	const auto percent = reading.percent ? locale.toString(*reading.percent, 'f', 1) + QLatin1Char('%')
					     : text("Observation.NoData");
	return text("Dock.PairPeriod")
		.arg(count, locale.toString(*reading.counters->total), percent, text(periodKey(reading.period)));
}

QString bytesText(const QLocale &locale, const TextLookup &text, const StatsBaseline &baseline)
{
	const auto reading = baseline.reading();
	if (reading.limited)
		return text("Dock.ChangeUnavailable");
	if (!reading.counters)
		return text("Observation.NoData");
	const auto mib = locale.toString(static_cast<double>(reading.counters->count) / (1024.0 * 1024.0), 'f', 2);
	return text("Dock.CountPeriod").arg(text("Dock.Mebibytes").arg(mib), text(periodKey(reading.period)));
}

std::optional<StatsBaseline::Counters> frames(const std::optional<FrameCounters> &value)
{
	if (!value)
		return std::nullopt;
	return StatsBaseline::Counters{value->skipped, value->total};
}

std::optional<StatsBaseline::Counters> count(std::optional<quint64> value)
{
	if (!value)
		return std::nullopt;
	return StatsBaseline::Counters{*value, std::nullopt};
}

std::optional<quint64> counter(const QMap<QString, quint64> &counters, const QString &key)
{
	const auto it = counters.constFind(key);
	return it == counters.cend() ? std::nullopt : std::optional{*it};
}

struct DropTotal {
	std::optional<quint64> value;
	bool limited = false;
};

DropTotal dropTotal(const QMap<QString, quint64> &counts)
{
	if (counts.isEmpty())
		return {};
	quint64 total = 0;
	for (const auto value : counts) {
		if (value > std::numeric_limits<quint64>::max() - total)
			return {std::nullopt, true};
		total += value;
	}
	return {total, false};
}

struct DisplayRate {
	std::optional<double> last;
	bool stale = false;

	QString display(const api::Rate *rate, bool current, bool isBitrate, const QLocale &locale,
			const TextLookup &text)
	{
		stale = !current || (rate && rate->status == QStringLiteral("stale"));
		const auto format = [&](double value) {
			return isBitrate ? bitrate(locale, text, value)
					 : text("Dock.FpsValue").arg(locale.toString(value, 'f', 2));
		};
		if (stale && last)
			return text("Dock.LastValue").arg(format(*last));
		if (stale || !rate)
			return text("Observation.NoData");
		if (rate->status == QStringLiteral("available") && rate->value && std::isfinite(*rate->value) &&
		    *rate->value >= 0) {
			last = *rate->value;
			return format(*last);
		}
		return text(rate->status == QStringLiteral("warming_up") ? "Dock.Measuring" : "Observation.NoData");
	}
};

struct OutputStatistics {
	QString instance;
	QString name;
	DisplayRate bitrate;
	StatsBaseline reconnects;
	StatsBaseline queueDrops;
	StatsBaseline deliveryDrops;
	QString queue;

	bool canReset() const { return reconnects.canReset() || queueDrops.canReset() || deliveryDrops.canReset(); }
	void reset()
	{
		reconnects.reset();
		queueDrops.reset();
		deliveryDrops.reset();
	}
};

void updateRows(QStringListModel &model, const QStringList &rows)
{
	const int count = static_cast<int>(rows.size());
	const int previous = model.rowCount();
	if (previous > count)
		model.removeRows(count, previous - count);
	else if (previous < count)
		model.insertRows(previous, count - previous);
	for (int i = 0; i < count; ++i) {
		const auto index = model.index(i);
		if (model.data(index).toString() != rows.at(i))
			model.setData(index, rows.at(i));
	}
}

} // namespace

struct RestreamDock::Statistics {
	StatsBaseline render;
	StatsBaseline encode;
	StatsBaseline network;
	StatsBaseline sent;
	DockContext context;
	QString sessionId;
	QString sessionInstance;
	quint64 inputEpoch = 0;
	DisplayRate inputFps;
	DisplayRate inputBitrate;
	QMap<QString, OutputStatistics> outputs;

	void clearServer()
	{
		sessionInstance.clear();
		inputEpoch = 0;
		inputFps = {};
		inputBitrate = {};
		outputs.clear();
	}
};

RestreamDock::RestreamDock(StateStore &stateStore, TextLookup text, QWidget *parent, QString version)
	: QWidget(parent),
	  stateStore_(stateStore),
	  text_(std::move(text)),
	  statistics_(std::make_unique<Statistics>())
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
	server_ = new StatusLabel(this);
	server_->setObjectName(QStringLiteral("serverObservation"));
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
	streaming_ = new StatusLabel(this);
	streaming_->setObjectName(QStringLiteral("obsStreaming"));
	observations->addRow(text_("Dock.Obs"), streaming_);
	layout->addLayout(observations);
	notice_ = new QLabel(this);
	notice_->setTextFormat(Qt::PlainText);
	notice_->setWordWrap(true);
	layout->addWidget(notice_);
	session_ = valueLabel(this, "sessionObservation");
	groups_ = valueLabel(this, "groupObservation");
	fallback_ = valueLabel(this, "fallbackObservation");
	serverDataNotice_ = valueLabel(this, "serverDataNotice");
	layout->addWidget(session_);
	layout->addWidget(groups_);
	layout->addWidget(fallback_);
	layout->addWidget(serverDataNotice_);
	localStats_ = valueLabel(this, "obsStatistics");
	ingest_ = valueLabel(this, "ingestStatistics");
	layout->addWidget(localStats_);
	layout->addWidget(ingest_);
	outputSummary_ = valueLabel(this, "outputSummary");
	layout->addWidget(outputSummary_);
	auto *outputList = new QListView(this);
	outputList->setObjectName(QStringLiteral("outputList"));
	outputList->setAccessibleName(text_("Dock.Outputs"));
	outputList->setEditTriggers(QAbstractItemView::NoEditTriggers);
	outputList->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
	outputList->setVerticalScrollMode(QAbstractItemView::ScrollPerPixel);
	outputList->setResizeMode(QListView::Adjust);
	outputList->setWordWrap(true);
	outputList->setMinimumHeight(fontMetrics().height() * 2);
	outputs_ = new QStringListModel(outputList);
	outputList->setModel(outputs_);
	layout->addWidget(outputList, 1);
	reset_ = new QPushButton(text_("Dock.ResetCounters"), this);
	reset_->setObjectName(QStringLiteral("statsResetButton"));
	reset_->setToolTip(text_("Dock.ResetHint"));
	layout->addWidget(reset_);
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
	connect(reset_, &QPushButton::clicked, this, &RestreamDock::resetStatistics);
	connect(&stateStore_, &StateStore::obsStateChanged, this, &RestreamDock::refreshObservations);
	connect(&stateStore_, &StateStore::serverStateChanged, this, [this] {
		refreshObservations();
		scheduleRefresh();
	});
	connect(&stateStore_, &StateStore::runtimeStateChanged, this, &RestreamDock::scheduleRefresh);
	connect(&stateStore_, &StateStore::runtimeAboutToBecomeStale, this, &RestreamDock::refreshServer);
	connect(&stateStore_, &StateStore::obsStatisticsChanged, this, &RestreamDock::scheduleRefresh);
	refresh();
}

RestreamDock::~RestreamDock() = default;

void RestreamDock::scheduleRefresh()
{
	if (refreshScheduled_)
		return;
	refreshScheduled_ = true;
	QTimer::singleShot(0, this, [this] {
		refreshScheduled_ = false;
		refresh();
	});
}

void RestreamDock::refresh()
{
	refreshObservations();
	const auto &sample = stateStore_.obsStatistics();
	const StatsBaseline::Identity video{QStringLiteral("video"), sample.videoEpoch};
	const StatsBaseline::Identity output{QStringLiteral("output"), sample.outputEpoch};
	auto &stats = *statistics_;
	const auto limitedPair = [](const std::optional<FrameCounters> &pair) {
		constexpr auto limit = std::numeric_limits<quint64>::max();
		return pair && (pair->skipped == limit || pair->total == limit);
	};
	stats.render.observe(video, frames(sample.render), true, sample.renderLimited || limitedPair(sample.render));
	stats.encode.observe(video, frames(sample.encode), true, sample.encodeLimited || limitedPair(sample.encode));
	stats.network.observe(output, frames(sample.network), true,
			      sample.networkLimited || limitedPair(sample.network));
	stats.sent.observe(output, count(sample.sentBytes), true,
			   sample.sentBytes == std::numeric_limits<quint64>::max());
	const QLocale locale(text_("Dock.NumberLocale"));
	const auto noData = text_("Observation.NoData");
	const auto fps = sample.fps ? locale.toString(*sample.fps, 'f', 2) : noData;
	const auto renderTime =
		sample.renderTimeNs
			? text_("Dock.Milliseconds")
				  .arg(locale.toString(static_cast<double>(*sample.renderTimeNs) / 1e6, 'f', 2))
			: noData;
	const char *bitrateStatus = "Observation.NoData";
	if (sample.bitrateWarming)
		bitrateStatus = "Dock.Measuring";
	else if (sample.phase == ObsStreamingPhase::Stopped)
		bitrateStatus = "Dock.ObsNotStreaming";
	const auto bitRate = sample.bitrate ? bitrate(locale, text_, *sample.bitrate) : text_(bitrateStatus);
	localStats_->setText(
		text_("Dock.LocalRates").arg(fps, renderTime, bitRate) + QLatin1Char('\n') +
		text_("Dock.LocalCounters")
			.arg(counterText(locale, text_, stats.render), counterText(locale, text_, stats.encode),
			     counterText(locale, text_, stats.network), bytesText(locale, text_, stats.sent)));
	localStats_->setToolTip(text_("Dock.CounterHint"));
	refreshServer();
	const bool serverCanReset = std::any_of(stats.outputs.cbegin(), stats.outputs.cend(),
						[](const auto &entry) { return entry.canReset(); });
	reset_->setEnabled(stats.render.canReset() || stats.encode.canReset() || stats.network.canReset() ||
			   stats.sent.canReset() || serverCanReset);
}

void RestreamDock::refreshObservations()
{
	const auto connection = stateStore_.connectionStatus();
	const auto observedConnection = stateStore_.serverOrigin().isEmpty() ? std::nullopt : std::optional{connection};
	server_->setConnectionStatus(text_, observedConnection);
	const auto streamName = stateStore_.selectedStreamName();
	stream_->setText(streamName.isEmpty() ? text_("Observation.NoData") : streamName);
	QStringList notices;
	if (connection != ConnectionStatus::Connected) {
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
	const auto &statistics = stateStore_.obsStatistics();
	if (statistics.observed)
		streaming_->setStreamingPhase(text_, statistics.phase);
	else
		streaming_->setStreamingStatus(text_, stateStore_.streamingActive());
}

void RestreamDock::refreshServer()
{
	auto &stats = *statistics_;
	const auto &context = stateStore_.dockContext();
	const auto &server = stateStore_.dockServer();
	if (stats.context != context) {
		stats.context = context;
		stats.sessionId.clear();
		stats.clearServer();
	}
	if (server.session && stats.sessionId != server.session->id) {
		stats.sessionId = server.session->id;
		stats.clearServer();
	}
	const bool connected = stateStore_.connectionStatus() == ConnectionStatus::Connected;
	const bool sessionCurrent = connected && server.session && fresh(server.sessionStatus.availability);
	const QLocale locale(text_("Dock.NumberLocale"));
	const auto noData = text_("Observation.NoData");
	const auto &runtime = stateStore_.runtime();
	const bool runtimeCurrent = connected && context.valid() && !stats.sessionId.isEmpty() &&
				    !runtime.cursor.isEmpty();
	const auto observed = runtime.sessions.constFind(stats.sessionId);
	const auto *session = runtimeCurrent && observed != runtime.sessions.cend() ? &*observed : nullptr;
	const auto sessionInstance = session && session->observation ? session->observation->instanceId : QString{};
	if ((!sessionInstance.isEmpty() && stats.sessionInstance != sessionInstance) || (runtimeCurrent && !session)) {
		stats.clearServer();
		stats.sessionInstance = sessionInstance;
	}
	const auto sessionEvidence = stateStore_.runtimeEvidence(sessionInstance);
	const auto status = sessionCurrent ? text_(sessionKey(server.session->state)) : noData;
	session_->setText(!sessionCurrent                 ? text_("Dock.SessionUnavailable").arg(noData)
			  : server.session->desiredPaused ? text_("Dock.SessionPaused").arg(status)
							  : status);

	int live = 0;
	int fallback = 0;
	int noSource = 0;
	bool unknownSource = false;
	if (session) {
		for (const auto &group : session->groups) {
			if (!fresh(stateStore_.runtimeEvidence(group.instanceId).state))
				continue;
			if (group.source == QStringLiteral("LIVE"))
				++live;
			else if (group.source == QStringLiteral("FALLBACK"))
				++fallback;
			else if (group.source == QStringLiteral("NONE"))
				++noSource;
			else if (!group.source.isEmpty())
				unknownSource = true;
		}
	}
	QStringList sources;
	if (live > 0)
		sources.append(text_("Dock.LiveCount").arg(locale.toString(live)));
	if (fallback > 0)
		sources.append(text_("Dock.FallbackCount").arg(locale.toString(fallback)));
	if (noSource > 0)
		sources.append(text_("Dock.NoSourceCount").arg(locale.toString(noSource)));
	if (unknownSource)
		sources.append(text_("Dock.UnknownStatus"));
	groups_->setText(text_("Dock.Groups").arg(sources.isEmpty() ? noData : sources.join(QStringLiteral(" · "))));

	QString fallbackStatus = noData;
	if (connected && fresh(server.fallback.availability) && server.fallback.assigned) {
		if (!*server.fallback.assigned)
			fallbackStatus = text_("Dock.FallbackUnassigned");
		else if (server.fallback.currentStatus == QStringLiteral("ready"))
			fallbackStatus = text_("Dock.FallbackPrepared");
		else if (server.fallback.assetKnown && server.fallback.currentGenerationId.isEmpty())
			fallbackStatus = text_("Dock.FallbackNoCurrent");
		if (!server.fallback.pendingGenerationId.isEmpty())
			fallbackStatus = text_("Dock.FallbackWithPending").arg(fallbackStatus);
	}
	fallback_->setText(text_("Dock.FallbackStatus").arg(fallbackStatus));
	fallback_->setToolTip(text_("Dock.FallbackHint"));

	const auto *input = session && session->input ? &*session->input : nullptr;
	const auto *inputMetrics = input && input->metrics ? &*input->metrics : nullptr;
	const bool inputCurrent = session && fresh(sessionEvidence.input);
	const bool inputMetricsCurrent = session && fresh(sessionEvidence.metrics);
	if ((inputMetricsCurrent && inputMetrics && stats.inputEpoch != inputMetrics->sourceEpoch) ||
	    (inputCurrent && !inputMetrics)) {
		stats.inputEpoch = inputMetrics ? inputMetrics->sourceEpoch : 0;
		stats.inputFps = {};
		stats.inputBitrate = {};
	}
	const auto inputStatus = inputCurrent && input
					 ? text_(input->connected ? "Dock.InputConnected" : "Dock.InputDisconnected")
					 : noData;
	QStringList inputFormat;
	if (inputMetricsCurrent && inputMetrics) {
		const auto &format = inputMetrics->media.format;
		if (!format.videoCodec.isEmpty())
			inputFormat.append(format.videoCodec.toUpper());
		if (format.width > 0 && format.height > 0)
			inputFormat.append(QStringLiteral("%1 × %2").arg(format.width).arg(format.height));
	}
	const auto ingestFps = stats.inputFps.display(inputMetrics ? &inputMetrics->media.rates.fps : nullptr,
						      inputMetricsCurrent, false, locale, text_);
	const auto ingestBitrate =
		stats.inputBitrate.display(inputMetrics ? &inputMetrics->media.rates.payloadBitrate : nullptr,
					   inputMetricsCurrent, true, locale, text_);
	ingest_->setText(text_("Dock.Input")
				 .arg(inputStatus, inputFormat.isEmpty() ? noData : inputFormat.join(QLatin1Char(' ')),
				      ingestFps, ingestBitrate));
	bool retained = (stats.inputFps.stale && stats.inputFps.last) ||
			(stats.inputBitrate.stale && stats.inputBitrate.last);

	QMap<QString, const api::RuntimeOutput *> outputObservations;
	if (session) {
		for (const auto &output : session->outputs)
			outputObservations.insert(output.outputId, &output);
	}
	QSet<QString> rows;
	for (auto it = server.outputs.cbegin(); it != server.outputs.cend(); ++it) {
		if (it->attached)
			rows.insert(it.key());
	}
	for (auto it = outputObservations.cbegin(); it != outputObservations.cend(); ++it) {
		const auto metadata = server.outputs.constFind(it.key());
		if (metadata == server.outputs.cend() || metadata->attached)
			rows.insert(it.key());
	}
	if (!runtimeCurrent) {
		for (auto it = stats.outputs.cbegin(); it != stats.outputs.cend(); ++it)
			rows.insert(it.key());
	}
	for (auto it = stats.outputs.begin(); it != stats.outputs.end();) {
		if (!rows.contains(it.key()))
			it = stats.outputs.erase(it);
		else
			++it;
	}
	QStringList outputRows;
	int streaming = 0;
	bool complete = sessionCurrent && fresh(server.outputsStatus.availability) && server.outputsStatus.complete;
	for (const auto &id : std::as_const(rows))
		stats.outputs[id];
	for (auto it = stats.outputs.begin(); it != stats.outputs.end(); ++it) {
		const auto &id = it.key();
		auto &cache = it.value();
		const auto *output = outputObservations.value(id);
		const auto metadata = server.outputs.constFind(id);
		if ((output && output->instanceId != cache.instance) || (runtimeCurrent && !output)) {
			cache = {};
			if (output)
				cache.instance = output->instanceId;
		}
		if (metadata != server.outputs.cend())
			cache.name = metadata->name;
		const auto evidence = stateStore_.runtimeEvidence(cache.instance);
		const bool statusCurrent = output && fresh(evidence.state);
		const bool metricsCurrent = output && fresh(evidence.metrics);
		const auto *metrics = output && output->metrics ? &*output->metrics : nullptr;
		const auto label = statusCurrent ? text_(outputKey(output->state)) : noData;
		const bool knownState = statusCurrent && label != noData && label != text_("Dock.UnknownStatus");
		complete = complete && knownState && metadata != server.outputs.cend();
		streaming += statusCurrent && output->state == QStringLiteral("PUBLISHING");
		const StatsBaseline::Identity identity{cache.instance};
		const auto queueDrops = metrics ? dropTotal(metrics->queueDrops) : DropTotal{};
		const auto deliveryDrops = metrics ? dropTotal(metrics->deliveryDrops) : DropTotal{};
		const bool saturated = metrics && metrics->media.saturated;
		cache.reconnects.observe(identity,
					 metrics ? count(counter(metrics->counters, QStringLiteral("reconnects")))
						 : std::nullopt,
					 metricsCurrent, saturated);
		cache.queueDrops.observe(identity, count(queueDrops.value), metricsCurrent,
					 saturated || queueDrops.limited);
		cache.deliveryDrops.observe(identity, count(deliveryDrops.value), metricsCurrent,
					    saturated || deliveryDrops.limited);
		if (metricsCurrent) {
			const auto queued = metrics ? counter(metrics->counters, QStringLiteral("queue_packets"))
						    : std::nullopt;
			cache.queue = queued ? locale.toString(*queued) : QString{};
		}
		const auto currentBitrate = cache.bitrate.display(
			metrics ? &metrics->media.rates.writtenBitrate : nullptr, metricsCurrent, true, locale, text_);
		const auto last = [&](const StatsBaseline &baseline) {
			const auto value = counterText(locale, text_, baseline);
			if (metricsCurrent || !baseline.reading().counters)
				return value;
			retained = true;
			return text_("Dock.LastValue").arg(value);
		};
		retained = retained || (cache.bitrate.stale && cache.bitrate.last) ||
			   (!metricsCurrent && !cache.queue.isEmpty());
		const auto queue = cache.queue.isEmpty() ? noData
				   : metricsCurrent      ? cache.queue
							 : text_("Dock.LastValue").arg(cache.queue);
		const auto name = cache.name.isEmpty() ? text_("Dock.UnnamedOutput") : cache.name.simplified();
		outputRows.append(text_("Dock.OutputRow")
					  .arg(name, label, currentBitrate, last(cache.reconnects), queue,
					       last(cache.queueDrops), last(cache.deliveryDrops)));
	}
	if (outputRows.isEmpty())
		outputRows.append(noData);
	updateRows(*outputs_, outputRows);
	outputSummary_->setText(
		complete && !rows.isEmpty()
			? text_("Dock.OutputCount").arg(locale.toString(streaming), locale.toString(rows.size()))
			: text_("Dock.Outputs"));
	QString dataNotice;
	if (context.valid() && (!connected || retained)) {
		dataNotice = text_("Dock.ServerDataMissing");
		if (retained)
			dataNotice += QLatin1Char('\n') + text_("Dock.LastValues");
	}
	serverDataNotice_->setText(dataNotice);
	serverDataNotice_->setVisible(!dataNotice.isEmpty());
}

void RestreamDock::resetStatistics()
{
	refresh();
	auto &stats = *statistics_;
	stats.render.reset();
	stats.encode.reset();
	stats.network.reset();
	stats.sent.reset();
	for (auto &output : stats.outputs)
		output.reset();
	refresh();
}

} // namespace gost
