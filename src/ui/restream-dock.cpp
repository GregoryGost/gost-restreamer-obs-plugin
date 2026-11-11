#include "restream-dock.hpp"

#include "core/state-store.hpp"
#include "core/stats-baseline.hpp"
#include "ui/status-label.hpp"

#include <QFormLayout>
#include <QGridLayout>
#include <QHBoxLayout>
#include <QLabel>
#include <QListView>
#include <QLocale>
#include <QPalette>
#include <QPainter>
#include <QPushButton>
#include <QSet>
#include <QStandardItemModel>
#include <QScrollArea>
#include <QScrollBar>
#include <QResizeEvent>
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
	// По умолчанию серверные имена показываются обычным текстом. Табличные значения ниже отдельно
	// экранируются при сборке HTML, поэтому '<...>' из серверного ответа не становится разметкой.
	auto *label = new QLabel(parent);
	label->setObjectName(QString::fromLatin1(name));
	label->setTextFormat(Qt::PlainText);
	label->setWordWrap(true);
	label->setMinimumWidth(0);
	auto policy = label->sizePolicy();
	policy.setHorizontalPolicy(QSizePolicy::Ignored);
	label->setSizePolicy(policy);
	return label;
}

void bold(QWidget *widget)
{
	auto font = widget->font();
	font.setBold(true);
	widget->setFont(font);
}

// Цвета карточки вычисляются из текущей палитры при рисовании. Смена темы не сохраняет старый фон
// и не требует stylesheet, который мог бы повторно инициировать StyleChange у всего дерева.
class DockCard final : public QWidget {
public:
	using QWidget::QWidget;

protected:
	void paintEvent(QPaintEvent *) override
	{
		QPainter painter(this);
		painter.setRenderHint(QPainter::Antialiasing);
		const auto base = palette().color(QPalette::Window);
		const auto text = palette().color(QPalette::WindowText);
		const auto blend = [&](int weight) {
			return QColor((base.red() * (100 - weight) + text.red() * weight) / 100,
				      (base.green() * (100 - weight) + text.green() * weight) / 100,
				      (base.blue() * (100 - weight) + text.blue() * weight) / 100);
		};
		painter.setBrush(blend(4));
		painter.setPen(blend(22));
		painter.drawRoundedRect(QRectF(rect()).adjusted(0.5, 0.5, -0.5, -0.5), 6, 6);
	}
};

QVBoxLayout *card(QVBoxLayout *parent, const char *name, const QString &title)
{
	auto *widget = new DockCard;
	widget->setObjectName(QString::fromLatin1(name));
	auto *layout = new QVBoxLayout(widget);
	layout->setContentsMargins(10, 10, 10, 10);
	layout->setSpacing(8);
	auto *heading = valueLabel(widget, "cardTitle");
	heading->setText(title);
	bold(heading);
	layout->addWidget(heading);
	parent->addWidget(widget);
	return layout;
}

QString tableRow(const QString &label, const QString &value, bool emphasis = false)
{
	const auto formatted = value.toHtmlEscaped();
	return QStringLiteral("<tr><td valign='top'>%1</td><td valign='top'>%2</td></tr>\n")
		.arg(label.toHtmlEscaped(), emphasis ? QStringLiteral("<b>%1</b>").arg(formatted) : formatted);
}

QString table(const QString &rows)
{
	return QStringLiteral("<table width='100%' cellspacing='0' cellpadding='2'>%1</table>").arg(rows);
}

QPushButton *futureButton(QWidget *parent, const TextLookup &text, const char *key, const char *name)
{
	auto *button = new QPushButton(text(key), parent);
	button->setObjectName(QString::fromLatin1(name));
	button->setEnabled(false);
	button->setToolTip(text("Dock.ControlsUnavailable"));
	button->setAccessibleDescription(text("Dock.ControlsUnavailable"));
	button->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Fixed);
	return button;
}

class OutputRow final : public QWidget {
public:
	explicit OutputRow(const TextLookup &text, QWidget *parent) : QWidget(parent)
	{
		setAutoFillBackground(true);
		setBackgroundRole(QPalette::Window);
		auto *layout = new QGridLayout(this);
		layout->setContentsMargins(4, 8, 4, 8);
		layout->setHorizontalSpacing(8);
		layout->setVerticalSpacing(4);
		name = valueLabel(this, "outputName");
		bold(name);
		status = new StatusLabel(this);
		status->setObjectName(QStringLiteral("outputStatus"));
		auto statusPolicy = status->sizePolicy();
		statusPolicy.setHorizontalPolicy(QSizePolicy::Ignored);
		status->setSizePolicy(statusPolicy);
		rate = valueLabel(this, "outputBitrate");
		rate->setAlignment(Qt::AlignRight | Qt::AlignTop);
		details = valueLabel(this, "outputDetails");
		auto *action = new QToolButton(this);
		action->setObjectName(QStringLiteral("outputActionsButton"));
		action->setText(QString::fromUtf8("…"));
		action->setEnabled(false);
		action->setToolTip(text("Dock.ControlsUnavailable"));
		action->setAccessibleName(text("Dock.OutputActions"));
		layout->addWidget(name, 0, 0, 1, 2);
		layout->addWidget(action, 0, 2);
		layout->addWidget(status, 1, 0);
		layout->addWidget(rate, 1, 1, 1, 2);
		layout->addWidget(details, 2, 0, 1, 3);
		layout->setColumnStretch(0, 1);
		layout->setColumnStretch(1, 1);
	}

	QLabel *name;
	StatusLabel *status;
	QLabel *rate;
	QLabel *details;
};

class OutputList final : public QListView {
public:
	using QListView::QListView;

	void updateHeights()
	{
		// Сохраняем модель и виджеты при телеметрии. Меняется только высота действительно изменившейся
		// строки; положение прокрутки не сбрасывается при поступлении очередного измерения.
		int total = 0;
		for (int row = 0; row < model()->rowCount(); ++row) {
			const auto index = model()->index(row, 0);
			auto *widget = indexWidget(index);
			const int height = widget ? widget->heightForWidth(viewport()->width())
						  : fontMetrics().height() + 4;
			const QSize hint(qMax(1, viewport()->width()), qMax(fontMetrics().height() + 4, height));
			if (index.data(Qt::SizeHintRole).toSize() != hint)
				model()->setData(index, hint, Qt::SizeHintRole);
			total += hint.height();
		}
		setFixedHeight(qMin(total + 2, fontMetrics().height() * 11));
	}

protected:
	void resizeEvent(QResizeEvent *event) override
	{
		QListView::resizeEvent(event);
		updateHeights();
	}
};

const char *sessionKey(const QString &state)
{
	// Пустое наблюдение и неизвестный полученный код различаются; ни одно не означает STOPPED.
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
	// Мбит/с — десятичная единица; ниже байты выводятся в двоичных MiB. Переводы содержат сами подписи единиц.
	return text("Dock.Megabits").arg(locale.toString(bitsPerSecond / 1000000.0, 'f', 2));
}

QString counterText(const QLocale &locale, const TextLookup &text, const StatsBaseline &baseline,
		    bool includePeriod = true)
{
	// Предел имеет собственное представление. Процент берётся из той же пары дельт baseline,
	// не вычисляется от полного total; каждый счётчик сохраняет свой период после разрыва.
	const auto reading = baseline.reading();
	if (reading.limited)
		return text("Dock.ChangeUnavailable");
	if (!reading.counters)
		return text("Observation.NoData");
	const auto count = locale.toString(reading.counters->count);
	if (!reading.counters->total)
		return includePeriod ? text("Dock.CountPeriod").arg(count, text(periodKey(reading.period))) : count;
	const auto percent = reading.percent ? locale.toString(*reading.percent, 'f', 1) + QLatin1Char('%')
					     : text("Observation.NoData");
	const auto pair = text("Dock.Pair").arg(count, locale.toString(*reading.counters->total), percent);
	return includePeriod ? text("Dock.CountPeriod").arg(pair, text(periodKey(reading.period))) : pair;
}

QString bytesText(const QLocale &locale, const TextLookup &text, const StatsBaseline &baseline,
		  bool includePeriod = true)
{
	const auto reading = baseline.reading();
	if (reading.limited)
		return text("Dock.ChangeUnavailable");
	if (!reading.counters)
		return text("Observation.NoData");
	const auto mib = locale.toString(static_cast<double>(reading.counters->count) / (1024.0 * 1024.0), 'f', 2);
	const auto value = text("Dock.Mebibytes").arg(mib);
	return includePeriod ? text("Dock.CountPeriod").arg(value, text(periodKey(reading.period))) : value;
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
	// Для отображения пустая карта считается недоступным измерением и не превращается в ноль.
	// Значения категорий одного вида потерь суммируются с проверкой переполнения.
	// queueDrops и deliveryDrops считаются отдельно.
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
	// Хранит последнюю доступную скорость одного источника. available обновляет last, warming_up
	// показывает, что замер ещё идёт. При stale прежнее значение выводится только с явной подписью
	// и только пока не сменился контекст источника; новая Session или исполнение не наследуют его.
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
	// Один конфигурационный Output может иметь новые исполнения. instance сменяется вместе со всем
	// кешем строки, чтобы дельты переподключений и потерь и последняя скорость не пересекли границу запуска.
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

void updateRows(QStandardItemModel &model, const QStringList &rows)
{
	// Изменяем только различающиеся строки, сохраняя существующую модель и положение списка при телеметрии.
	const int count = static_cast<int>(rows.size());
	const int previous = model.rowCount();
	if (previous > count)
		model.removeRows(count, previous - count);
	else if (previous < count)
		model.insertRows(previous, count - previous);
	for (int i = 0; i < count; ++i) {
		const auto index = model.index(i, 0);
		if (model.data(index).toString() != rows.at(i))
			model.setData(index, rows.at(i));
	}
}

} // namespace

struct RestreamDock::Statistics {
	// Четыре локальные точки отсчёта OBS не зависят от выбранного сервера. Смена DockContext, Session ID
	// или известного исполнения Session очищает весь серверный кеш; подтверждённое отсутствие Session — тоже.
	// Внутри той же Session смена подтверждённого sourceEpoch очищает только входные FPS/bitrate.
	// Смена или исчезновение исполнения Output очищает только его строку, включая точки отсчёта
	// переподключений и потерь.
	// clearServer оставляет context/sessionId для следующих сравнений и не трогает локальную статистику OBS.
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
	// Сигналы подключаются с контекстом виджета: его удаление OBS отменяет отложенные обновления.
	// runtimeAboutToBecomeStale обрабатывается отдельно и сразу, до отложенного общего refresh().
	setObjectName(QStringLiteral("restreamDock"));
	setMinimumWidth(0);
	auto *layout = new QVBoxLayout(this);
	layout->setContentsMargins(10, 10, 10, 10);
	layout->setSpacing(8);
	auto *header = new QHBoxLayout;
	auto *title = valueLabel(this, "dockTitle");
	title->setText(text_("Plugin.Name"));
	bold(title);
	header->addWidget(title, 1);
	auto *gear = new QToolButton(this);
	gear->setObjectName(QStringLiteral("managementButton"));
	gear->setText(QString::fromUtf8("⚙"));
	gear->setToolTip(text_("Management.Open"));
	gear->setAccessibleName(text_("Management.Open"));
	header->addWidget(gear);
	layout->addLayout(header);
	server_ = new StatusLabel(this);
	server_->setObjectName(QStringLiteral("serverObservation"));
	layout->addWidget(server_);
	stream_ = valueLabel(this, "streamObservation");
	bold(stream_);
	layout->addWidget(stream_);
	profile_ = valueLabel(this, "obsProfile");
	layout->addWidget(profile_);

	auto *state = card(layout, "stateCard", text_("Dock.State"));
	auto *observations = new QFormLayout;
	observations->setContentsMargins(0, 0, 0, 0);
	observations->setSpacing(4);
	observations->setFieldGrowthPolicy(QFormLayout::AllNonFixedFieldsGrow);
	observations->setRowWrapPolicy(QFormLayout::WrapLongRows);
	const auto axis = [&](const char *name, const char *key) {
		auto *label = new StatusLabel(this);
		label->setObjectName(QString::fromLatin1(name));
		auto policy = label->sizePolicy();
		policy.setHorizontalPolicy(QSizePolicy::Ignored);
		label->setSizePolicy(policy);
		observations->addRow(text_(key), label);
		return label;
	};
	streaming_ = axis("obsStreaming", "Dock.Obs");
	input_ = axis("inputObservation", "Dock.InputLabel");
	session_ = axis("sessionObservation", "Dock.SessionLabel");
	groups_ = axis("groupObservation", "Dock.GroupsLabel");
	fallback_ = axis("fallbackObservation", "Dock.FallbackLabel");
	state->addLayout(observations);

	// Шапка, независимые состояния и основные действия остаются снаружи прокрутки.
	// На небольшой высоте подробные метрики доступны без увеличения минимальной высоты Dock.
	auto *scroll = new QScrollArea(this);
	scroll->setObjectName(QStringLiteral("dockDetailsScroll"));
	scroll->setFrameShape(QFrame::NoFrame);
	scroll->setWidgetResizable(true);
	scroll->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
	scroll->setMinimumHeight(fontMetrics().height() * 3);
	auto *content = new QWidget;
	auto *details = new QVBoxLayout(content);
	details->setContentsMargins(0, 0, 0, 0);
	details->setSpacing(8);
	notice_ = valueLabel(content, "dockNotice");
	details->addWidget(notice_);
	serverDataNotice_ = valueLabel(content, "serverDataNotice");
	details->addWidget(serverDataNotice_);
	auto *localCard = card(details, "obsStatisticsCard", text_("Dock.StatisticsTitle"));
	localStats_ = valueLabel(content, "obsStatistics");
	localStats_->setTextFormat(Qt::RichText);
	localCard->addWidget(localStats_);
	auto *inputCard = card(details, "inputCard", text_("Dock.InputTitle"));
	ingest_ = valueLabel(content, "ingestStatistics");
	ingest_->setTextFormat(Qt::RichText);
	inputCard->addWidget(ingest_);
	auto *outputCard = card(details, "outputsCard", text_("Dock.OutputsTitle"));
	outputSummary_ = valueLabel(content, "outputSummary");
	outputCard->addWidget(outputSummary_);
	auto *outputList = new OutputList(content);
	outputList_ = outputList;
	outputList->setObjectName(QStringLiteral("outputList"));
	outputList->setAccessibleName(text_("Dock.Outputs"));
	outputList->setEditTriggers(QAbstractItemView::NoEditTriggers);
	outputList->setFrameShape(QFrame::NoFrame);
	outputList->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
	outputList->setVerticalScrollMode(QAbstractItemView::ScrollPerPixel);
	outputList->setResizeMode(QListView::Adjust);
	outputList->setWordWrap(true);
	outputList->setMinimumWidth(0);
	outputs_ = new QStandardItemModel(0, 1, outputList);
	outputList->setModel(outputs_);
	outputCard->addWidget(outputList);
	auto *batch = new QVBoxLayout;
	batch->setSpacing(4);
	batch->addWidget(futureButton(content, text_, "Dock.StartOutputs", "outputsStartButton"));
	batch->addWidget(futureButton(content, text_, "Dock.RestartOutputs", "outputsRestartButton"));
	outputCard->addLayout(batch);
	details->addStretch();
	scroll->setWidget(content);
	layout->addWidget(scroll, 1);

	auto *commands = new QGridLayout;
	commands->setSpacing(4);
	commands->addWidget(futureButton(this, text_, "Dock.Start", "broadcastStartButton"), 0, 0);
	commands->addWidget(futureButton(this, text_, "Dock.Stop", "broadcastStopButton"), 0, 1);
	commands->addWidget(futureButton(this, text_, "Dock.Pause", "broadcastPauseButton"), 1, 0);
	commands->addWidget(futureButton(this, text_, "Dock.Resume", "broadcastResumeButton"), 1, 1);
	layout->addLayout(commands);
	auto *footer = new QHBoxLayout;
	reset_ = new QPushButton(text_("Dock.ResetShort"), this);
	reset_->setObjectName(QStringLiteral("statsResetButton"));
	reset_->setFlat(true);
	reset_->setToolTip(text_("Dock.ResetHint"));
	reset_->setAccessibleName(text_("Dock.ResetCounters"));
	footer->addWidget(reset_);
	footer->addStretch();
	if (!version.isEmpty()) {
		auto *versionLabel = valueLabel(this, "pluginVersion");
		versionLabel->setText(text_("Plugin.Version").arg(version));
		versionLabel->setSizePolicy(QSizePolicy::Preferred, QSizePolicy::Preferred);
		footer->addWidget(versionLabel);
	}
	layout->addLayout(footer);
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
	// Несколько сигналов одного изменения StateStore объединяются в одно обновление следующего оборота Qt.
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
	std::optional<StatsBaseline::Period> commonPeriod;
	bool differentPeriods = false;
	for (const auto *baseline : {&stats.render, &stats.encode, &stats.network, &stats.sent}) {
		const auto reading = baseline->reading();
		if (!reading.counters)
			continue;
		if (commonPeriod && *commonPeriod != reading.period)
			differentPeriods = true;
		commonPeriod = reading.period;
	}
	const bool perCounterPeriod = differentPeriods;
	const auto localRows =
		tableRow(text_("Dock.FpsLabel"), fps, true) + tableRow(text_("Dock.BitrateLabel"), bitRate, true) +
		tableRow(text_("Dock.RenderLabel"), renderTime) +
		tableRow(text_("Dock.RenderDropsLabel"), counterText(locale, text_, stats.render, perCounterPeriod)) +
		tableRow(text_("Dock.EncodeDropsLabel"), counterText(locale, text_, stats.encode, perCounterPeriod)) +
		tableRow(text_("Dock.NetworkDropsLabel"), counterText(locale, text_, stats.network, perCounterPeriod)) +
		tableRow(text_("Dock.SentLabel"), bytesText(locale, text_, stats.sent, perCounterPeriod));
	localStats_->setText(table(localRows) +
			     (commonPeriod && !differentPeriods
				      ? QStringLiteral("<p>%1</p>").arg(text_(periodKey(*commonPeriod)).toHtmlEscaped())
				      : QString{}));
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
	stream_->setText(
		text_("Dock.SelectedStream").arg(streamName.isEmpty() ? text_("Observation.NoData") : streamName));
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
	profile_->setText(
		text_("Dock.ProfileValue").arg(profile && !profile->isEmpty() ? *profile : text_("Observation.NoData")));
	const auto &statistics = stateStore_.obsStatistics();
	if (statistics.observed)
		streaming_->setStreamingPhase(text_, statistics.phase);
	else
		streaming_->setStreamingStatus(text_, stateStore_.streamingActive());
}

void RestreamDock::refreshServer()
{
	// Сначала очищаем кеш прежнего выбора или Session, чтобы их статистика не появилась у нового выбора.
	// При Connected можно читать runtime, но состояние, подключение входа и измерения подтверждаются
	// независимо через RuntimeEvidence. Полноту и свежесть состава выходов задаёт REST ReadStatus;
	// одно восстановленное соединение не делает все сохранённые значения снова актуальными.
	auto &stats = *statistics_;
	const auto &context = stateStore_.dockContext();
	const auto &server = stateStore_.dockServer();
	if (stats.context != context) {
		// Данные и точки отсчёта прежнего сервера, профиля или Stream не переносятся в новый выбор.
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
	using Tone = StatusLabel::Tone;
	const auto sessionTone = !sessionCurrent                                      ? Tone::Neutral
				 : server.session->state == QStringLiteral("RUNNING") ? Tone::Active
				 : server.session->state == QStringLiteral("STARTING") ||
						 server.session->state == QStringLiteral("STOPPING")
					 ? Tone::Pending
				 : server.session->state == QStringLiteral("FAILED") ||
						 server.session->state == QStringLiteral("INTERRUPTED")
					 ? Tone::Error
					 : Tone::Neutral;
	session_->setStatus(!sessionCurrent                 ? noData
			    : server.session->desiredPaused ? text_("Dock.SessionPaused").arg(status)
							    : status,
			    sessionTone);

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
	groups_->setStatus(sources.isEmpty() ? noData : sources.join(QStringLiteral(" · ")),
			   live > 0 || fallback > 0 ? Tone::Active : Tone::Neutral);

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
	// READY описывает подготовленный файл, поэтому остаётся нейтральным и не обещает готовую Session.
	fallback_->setStatus(fallbackStatus, Tone::Neutral);
	fallback_->setToolTip(text_("Dock.FallbackHint"));

	const auto *input = session && session->input ? &*session->input : nullptr;
	const auto *inputMetrics = input && input->metrics ? &*input->metrics : nullptr;
	// Состояние подключения входа и его измерения могут подтверждаться разными событиями.
	const bool inputCurrent = session && fresh(sessionEvidence.input);
	const bool inputMetricsCurrent = session && fresh(sessionEvidence.metrics);
	if ((inputMetricsCurrent && inputMetrics && stats.inputEpoch != inputMetrics->sourceEpoch) ||
	    (inputCurrent && !inputMetrics)) {
		// Новый источник входа начинает собственные измерения. Счётчики того же исполнения Output продолжаются.
		stats.inputEpoch = inputMetrics ? inputMetrics->sourceEpoch : 0;
		stats.inputFps = {};
		stats.inputBitrate = {};
	}
	const auto inputStatus = inputCurrent && input
					 ? text_(input->connected ? "Dock.InputConnected" : "Dock.InputDisconnected")
					 : noData;
	input_->setStatus(inputStatus, inputCurrent && input && input->connected ? Tone::Active : Tone::Neutral);
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
	ingest_->setText(table(tableRow(text_("Dock.FormatLabel"),
					inputFormat.isEmpty() ? noData : inputFormat.join(QLatin1Char(' '))) +
			       tableRow(text_("Dock.FpsLabel"), ingestFps) +
			       tableRow(text_("Dock.BitrateLabel"), ingestBitrate)));
	bool retained = (stats.inputFps.stale && stats.inputFps.last) ||
			(stats.inputBitrate.stale && stats.inputBitrate.last);

	QMap<QString, const api::RuntimeOutput *> outputObservations;
	if (session) {
		for (const auto &output : session->outputs)
			outputObservations.insert(output.outputId, &output);
	}
	QSet<QString> rows;
	// Показываем прикреплённые по REST выходы и наблюдаемые исполнения, для которых REST ещё не сообщил
	// об отсоединении. Отсутствие ID в неполном списке или отсутствие метрик само по себе не удаляет строку.
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
		// При потере связи сохраняем последние строки; прежние измерения будут подписаны как устаревшие.
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
	struct RowValues {
		QString name;
		QString status;
		QString rate;
		QString details;
		Tone tone;
	};
	QVector<RowValues> rowValues;
	int streaming = 0;
	bool complete = sessionCurrent && fresh(server.outputsStatus.availability) && server.outputsStatus.complete;
	// Сводка «N из M» допустима только при полном составе и известном состоянии каждой строки.
	// Наличие bitrate, единичного PUBLISHING или пустой карты не подтверждает общий итог.
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
		// Потери очереди и доставки суммируются по своим категориям, независимо от сетевых пропусков OBS.
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
		const auto tone = !statusCurrent                                  ? Tone::Neutral
				  : output->state == QStringLiteral("PUBLISHING") ? Tone::Active
				  : output->state == QStringLiteral("CONNECTING") ||
						  output->state == QStringLiteral("RETRYING")
					  ? Tone::Pending
				  : output->state == QStringLiteral("FAILED") ? Tone::Error
									      : Tone::Neutral;
		rowValues.append(
			{name, label, currentBitrate,
			 text_("Dock.OutputDetails")
				 .arg(last(cache.reconnects), queue, last(cache.queueDrops), last(cache.deliveryDrops)),
			 tone});
	}
	if (outputRows.isEmpty())
		outputRows.append(noData);
	updateRows(*outputs_, outputRows);
	for (int row = 0; row < outputs_->rowCount(); ++row) {
		const auto index = outputs_->index(row, 0);
		if (rowValues.isEmpty()) {
			if (outputList_->indexWidget(index))
				outputList_->setIndexWidget(index, nullptr);
			continue;
		}
		auto *widget = static_cast<OutputRow *>(outputList_->indexWidget(index));
		if (!widget) {
			widget = new OutputRow(text_, outputList_->viewport());
			outputList_->setIndexWidget(index, widget);
		}
		const auto &values = rowValues.at(row);
		widget->name->setText(values.name);
		widget->status->setStatus(values.status, values.tone);
		widget->rate->setText(values.rate);
		widget->details->setText(values.details);
		widget->setAccessibleName(outputRows.at(row));
	}
	static_cast<OutputList *>(outputList_)->updateHeights();
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
	// Любой недоступный/stale baseline сам отвергает reset. Никаких REST/OBS-команд не требуется;
	// FPS, bitrate, очередь и статусы не вычитаются и продолжают показывать свои текущие значения.
	// Сначала забираем последнее состояние, затем сдвигаем только локальные точки отсчёта накоплений.
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
