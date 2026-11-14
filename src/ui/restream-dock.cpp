#include "restream-dock.hpp"

#include "core/state-store.hpp"
#include "core/stats-baseline.hpp"
#include "core/restream-controller.hpp"
#include "ui/status-label.hpp"
#include "ui/confirmation-dialog.hpp"

#include <QFormLayout>
#include <QGridLayout>
#include <QHBoxLayout>
#include <QLabel>
#include <QListView>
#include <QLocale>
#include <QMenu>
#include <QMessageBox>
#include <QCursor>
#include <QAction>
#include <QPalette>
#include <QPainter>
#include <QPushButton>
#include <QPointer>
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

// Создаёт компактную кнопку намерения; её доступность обновляет представление по общему состоянию.
QPushButton *commandButton(QWidget *parent, const TextLookup &text, const char *key, const char *name)
{
	// button принадлежит parent и не выполняет команд до подключения обработчика представлением.
	auto *button = new QPushButton(text(key), parent);
	button->setObjectName(QString::fromLatin1(name));
	button->setEnabled(false);
	button->setAccessibleName(text(key));
	button->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Fixed);
	return button;
}

// Отражает одну строку телеметрии и сообщает об открытии её меню без хранения индекса модели.
class OutputRow final : public QWidget {
public:
	// Создаёт строку; text переводит подписи, parent получает владение виджетами и соединениями Qt.
	explicit OutputRow(const TextLookup &text, QWidget *parent) : QWidget(parent)
	{
		setAutoFillBackground(true);
		setBackgroundRole(QPalette::Window);
		// layout раскладывает имя, независимые наблюдения и доступное с клавиатуры меню строки.
		auto *layout = new QGridLayout(this);
		layout->setContentsMargins(4, 8, 4, 8);
		layout->setHorizontalSpacing(8);
		layout->setVerticalSpacing(4);
		name = valueLabel(this, "outputName");
		bold(name);
		status = new StatusLabel(this);
		status->setObjectName(QStringLiteral("outputStatus"));
		// statusPolicy допускает сжатие длинного переведённого статуса в узком доке.
		auto statusPolicy = status->sizePolicy();
		statusPolicy.setHorizontalPolicy(QSizePolicy::Ignored);
		status->setSizePolicy(statusPolicy);
		rate = valueLabel(this, "outputBitrate");
		rate->setAlignment(Qt::AlignRight | Qt::AlignTop);
		details = valueLabel(this, "outputDetails");
		action = new QToolButton(this);
		action->setObjectName(QStringLiteral("outputActionsButton"));
		action->setText(QString::fromUtf8("…"));
		action->setEnabled(false);
		action->setToolTip(text("Dock.OutputActions"));
		action->setAccessibleName(text("Dock.OutputActions"));
		layout->addWidget(name, 0, 0, 1, 2);
		layout->addWidget(action, 0, 2);
		layout->addWidget(status, 1, 0);
		layout->addWidget(rate, 1, 1, 1, 2);
		layout->addWidget(details, 2, 0, 1, 3);
		layout->setColumnStretch(0, 1);
		layout->setColumnStretch(1, 1);
	}

	// name, status, rate и details представляют имя, состояние, скорость и отдельные счётчики выхода.
	QLabel *name;
	StatusLabel *status;
	QLabel *rate;
	QLabel *details;
	// action открывает меню; outputId обновляется при повторном использовании строки и копируется при нажатии.
	QToolButton *action;
	QString outputId;
	// context и sessionId принадлежат отображаемой строке; обновление StateStore не перенаправляет её нажатие.
	DockContext context;
	QString sessionId;
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

// Форматирует известные коды и локальные подписи; ID и текст отказа не выводятся, код причины показывается отдельно.
QString formatOperations(const OperationsState &operations, const TextLookup &text)
{
	// outcomeText переводит итог шага или части Stop All, сохраняя неизвестный результат явно.
	const auto outcomeText = [&text](OperationOutcome outcome) {
		switch (outcome) {
		case OperationOutcome::None:
			return text("Operation.NoResult");
		case OperationOutcome::Pending:
			return text("Operation.Pending");
		case OperationOutcome::Completed:
			return text("Operation.Completed");
		case OperationOutcome::Failed:
			return text("Operation.Failed");
		case OperationOutcome::NotSent:
			return text("Operation.NotSent");
		case OperationOutcome::Unknown:
			return text("Operation.Unknown");
		}
		return text("Operation.Unknown");
	};
	// actionText переводит явное намерение без раскрытия внутренних идентификаторов или маршрутов API.
	const auto actionText = [&text](OperationAction action) {
		switch (action) {
		case OperationAction::CheckFallback:
			return text("Operation.CheckFallback");
		case OperationAction::StartFallback:
			return text("Operation.StartFallback");
		case OperationAction::AddOutput:
			return text("Operation.AddOutput");
		case OperationAction::Pause:
			return text("Dock.Pause");
		case OperationAction::Resume:
			return text("Dock.Resume");
		case OperationAction::StopAll:
			return text("Dock.Stop");
		case OperationAction::StartOutput:
			return text("Operation.StartOutput");
		case OperationAction::StopOutput:
			return text("Operation.StopOutput");
		case OperationAction::RestartOutput:
			return text("Operation.RestartOutput");
		case OperationAction::DetachOutput:
			return text("Operation.DetachOutput");
		case OperationAction::StartStopped:
			return text("Dock.StartOutputs");
		case OperationAction::RestartAll:
			return text("Dock.RestartOutputs");
		case OperationAction::None:
			return text("Operation.NoResult");
		}
		return text("Operation.Unknown");
	};
	// phaseText объясняет ожидание отдельно от окончательного итога команды.
	const auto phaseText = [&text](OperationPhase phase) {
		switch (phase) {
		case OperationPhase::Prechecking:
			return text("Operation.Prechecking");
		case OperationPhase::Sending:
			return text("Operation.Sending");
		case OperationPhase::PollingReceipt:
			return text("Operation.PollingReceipt");
		case OperationPhase::ConfirmingState:
			return text("Operation.ConfirmingState");
		case OperationPhase::StoppingObs:
			return text("Operation.StoppingObs");
		case OperationPhase::Finished:
			return text("Operation.Finished");
		case OperationPhase::Idle:
			return text("Operation.NoResult");
		}
		return text("Operation.Unknown");
	};
	// describe составляет одно представление workflow; callbacks вызываются только синхронно в этом методе.
	const auto describe = [&](const WorkflowResult &workflow) {
		// lines сохраняет раздельные результаты квитанции, состояния и каждого выхода пакета.
		QStringList lines;
		lines.append(text("Operation.Summary")
				     .arg(actionText(workflow.action), outcomeText(workflow.outcome),
					  phaseText(workflow.phase)));
		if (workflow.receipt) {
			// status берётся из проверенной квитанции; неизвестный код не выводится дословно.
			const auto &status = workflow.receipt->status;
			// receiptText отдельно сообщает успех исполнения команды, а не состояние видеоплощадки.
			const auto receiptText =
				status == QStringLiteral("SUCCEEDED")     ? text("Operation.ReceiptSucceeded")
				: status == QStringLiteral("FAILED")      ? text("Operation.Failed")
				: status == QStringLiteral("INTERRUPTED") ? text("Operation.ReceiptInterrupted")
				: status == QStringLiteral("PENDING") || status == QStringLiteral("CLAIMED") ||
						status == QStringLiteral("ISSUED")
					? text("Operation.Pending")
					: text("Operation.Unknown");
			lines.append(text("Operation.Receipt").arg(receiptText));
			// Выводится только контрактный failure.code из проверенной квитанции, без failure.message.
			if (workflow.receipt->failure.hasValue() && !workflow.receipt->failure->code.isEmpty())
				lines.append(text("Operation.FailureCode").arg(workflow.receipt->failure->code));
		}
		if (workflow.action != OperationAction::CheckFallback)
			lines.append(text(workflow.stateConfirmed ? "Operation.StateConfirmed"
								  : "Operation.StateUnconfirmed"));
		if (workflow.action == OperationAction::StopAll) {
			lines.append(text("Operation.ObsResult").arg(outcomeText(workflow.obsOutcome)));
			lines.append(text("Operation.ServerResult").arg(outcomeText(workflow.serverOutcome)));
			// Неопределённая остановка OBS остаётся заметной даже после подтверждённого завершения Session.
			if (workflow.serverOutcome == OperationOutcome::Completed &&
			    workflow.obsOutcome != OperationOutcome::Completed)
				lines.append(text("Operation.ObsStopUnconfirmed"));
		}
		// output — элемент исходного фиксированного пакета; имя используется только в безопасном plain text.
		for (const auto &output : workflow.outputs)
			lines.append(text("Operation.OutputResult")
					     .arg(output.name.isEmpty() ? text("Dock.UnnamedOutput") : output.name,
						  outcomeText(output.outcome)));
		if (workflow.remainingStopped)
			lines.append(text("Operation.RemainingStopped"));
		if (workflow.phase == OperationPhase::Finished && workflow.outcome != OperationOutcome::Completed &&
		    !workflow.outputs.isEmpty())
			lines.append(text("Operation.Partial"));
		if (!workflow.messageKey.isEmpty())
			lines.append(text(workflow.messageKey.toUtf8().constData()));
		return lines.join(QLatin1Char('\n'));
	};
	// sections содержит текущую операцию и не более одного последнего прерванного намерения.
	QStringList sections;
	if (operations.current)
		sections.append(describe(*operations.current));
	if (operations.lastInterrupted)
		sections.append(text("Operation.Previous").arg(describe(*operations.lastInterrupted)));
	return sections.join(QStringLiteral("\n\n"));
}

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

// Создаёт представление общих наблюдений и передаёт пользовательские намерения restream в GUI-потоке.
RestreamDock::RestreamDock(StateStore &stateStore, TextLookup text, QWidget *parent, QString version,
			   RestreamController *restream)
	: QWidget(parent),
	  stateStore_(stateStore),
	  text_(std::move(text)),
	  restream_(restream),
	  statistics_(std::make_unique<Statistics>())
{
	// Сигналы подключаются с контекстом виджета: его удаление OBS отменяет отложенные обновления.
	// runtimeAboutToBecomeStale обрабатывается отдельно и сразу, до отложенного общего refresh().
	setObjectName(QStringLiteral("restreamDock"));
	setMinimumWidth(0);
	// layout задаёт вертикальную композицию дока и принадлежит этому виджету.
	auto *layout = new QVBoxLayout(this);
	layout->setContentsMargins(10, 10, 10, 10);
	layout->setSpacing(8);
	// header располагает название и единственную кнопку открытия управления.
	auto *header = new QHBoxLayout;
	// title показывает название продукта обычным текстом.
	auto *title = valueLabel(this, "dockTitle");
	title->setText(text_("Plugin.Name"));
	bold(title);
	header->addWidget(title, 1);
	// gear открывает существующее расширенное окно и доступна с клавиатуры.
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

	// state содержит независимые оси OBS, входа, Session, групп и заглушки.
	auto *state = card(layout, "stateCard", text_("Dock.State"));
	// observations выравнивает подписи состояний с переносом длинных строк.
	auto *observations = new QFormLayout;
	observations->setContentsMargins(0, 0, 0, 0);
	observations->setSpacing(4);
	observations->setFieldGrowthPolicy(QFormLayout::AllNonFixedFieldsGrow);
	observations->setRowWrapPolicy(QFormLayout::WrapLongRows);
	// axis создаёт индикатор для name и переведённой подписи key; callback возвращает дочерний виджет.
	const auto axis = [&](const char *name, const char *key) {
		// label принадлежит доку и отражает одну независимую ось состояния.
		auto *label = new StatusLabel(this);
		label->setObjectName(QString::fromLatin1(name));
		// policy позволяет индикатору сжиматься при небольшой ширине дока.
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
	// scroll оставляет основные состояния и команды видимыми при прокрутке деталей.
	auto *scroll = new QScrollArea(this);
	scroll->setObjectName(QStringLiteral("dockDetailsScroll"));
	scroll->setFrameShape(QFrame::NoFrame);
	scroll->setWidgetResizable(true);
	scroll->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
	scroll->setMinimumHeight(fontMetrics().height() * 3);
	// content принадлежит scroll после setWidget и содержит подробные наблюдения.
	auto *content = new QWidget;
	// details размещает сообщения, карточки метрик и результаты команд внутри прокрутки.
	auto *details = new QVBoxLayout(content);
	details->setContentsMargins(0, 0, 0, 0);
	details->setSpacing(8);
	notice_ = valueLabel(content, "dockNotice");
	details->addWidget(notice_);
	serverDataNotice_ = valueLabel(content, "serverDataNotice");
	details->addWidget(serverDataNotice_);
	// localCard отделяет локальные метрики OBS от серверного входа и выходов.
	auto *localCard = card(details, "obsStatisticsCard", text_("Dock.StatisticsTitle"));
	localStats_ = valueLabel(content, "obsStatistics");
	localStats_->setTextFormat(Qt::RichText);
	localCard->addWidget(localStats_);
	// inputCard содержит формат и скорости серверного входа.
	auto *inputCard = card(details, "inputCard", text_("Dock.InputTitle"));
	ingest_ = valueLabel(content, "ingestStatistics");
	ingest_->setTextFormat(Qt::RichText);
	inputCard->addWidget(ingest_);
	// outputCard содержит наблюдаемые выходы и фиксированные пакетные действия.
	auto *outputCard = card(details, "outputsCard", text_("Dock.OutputsTitle"));
	outputSummary_ = valueLabel(content, "outputSummary");
	outputCard->addWidget(outputSummary_);
	// outputList сохраняет строки виджетов и их положение при обновлении телеметрии.
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
	// batch располагает две команды для фиксированного состава выходов.
	auto *batch = new QVBoxLayout;
	batch->setSpacing(4);
	startOutputs_ = commandButton(content, text_, "Dock.StartOutputs", "outputsStartButton");
	restartOutputs_ = commandButton(content, text_, "Dock.RestartOutputs", "outputsRestartButton");
	batch->addWidget(startOutputs_);
	batch->addWidget(restartOutputs_);
	outputCard->addLayout(batch);
	operationStatus_ = valueLabel(content, "operationStatus");
	details->addWidget(operationStatus_);
	retry_ = commandButton(content, text_, "Operation.Retry", "operationRetryButton");
	details->addWidget(retry_);
	details->addStretch();
	scroll->setWidget(content);
	layout->addWidget(scroll, 1);

	// commands располагает pause/resume и Stop All за пределами прокрутки.
	auto *commands = new QGridLayout;
	commands->setSpacing(4);
	pause_ = commandButton(this, text_, "Dock.Pause", "broadcastPauseButton");
	resume_ = commandButton(this, text_, "Dock.Resume", "broadcastResumeButton");
	stop_ = commandButton(this, text_, "Dock.Stop", "broadcastStopButton");
	commands->addWidget(pause_, 0, 0);
	commands->addWidget(resume_, 0, 1);
	commands->addWidget(stop_, 1, 0, 1, 2);
	layout->addLayout(commands);
	nativeStart_ = valueLabel(this, "nativeStartGuidance");
	nativeStart_->setText(text_("Dock.NativeStart"));
	layout->addWidget(nativeStart_);
	// footer оставляет сброс локальных счётчиков рядом с версией плагина.
	auto *footer = new QHBoxLayout;
	reset_ = new QPushButton(text_("Dock.ResetShort"), this);
	reset_->setObjectName(QStringLiteral("statsResetButton"));
	reset_->setFlat(true);
	reset_->setToolTip(text_("Dock.ResetHint"));
	reset_->setAccessibleName(text_("Dock.ResetCounters"));
	footer->addWidget(reset_);
	footer->addStretch();
	if (!version.isEmpty()) {
		// versionLabel показывает переданную владельцем версию без интерпретации HTML.
		auto *versionLabel = valueLabel(this, "pluginVersion");
		versionLabel->setText(text_("Plugin.Version").arg(version));
		versionLabel->setSizePolicy(QSizePolicy::Preferred, QSizePolicy::Preferred);
		footer->addWidget(versionLabel);
	}
	layout->addLayout(footer);
	connect(gear, &QToolButton::clicked, this, &RestreamDock::managementRequested);
	connect(reset_, &QPushButton::clicked, this, &RestreamDock::resetStatistics);
	// Обработчики фиксируют точные цели до подтверждения и передают только намерения фасаду.
	connect(pause_, &QPushButton::clicked, this, [this] { dispatchSessionAction(OperationAction::Pause); });
	connect(resume_, &QPushButton::clicked, this, [this] { dispatchSessionAction(OperationAction::Resume); });
	connect(stop_, &QPushButton::clicked, this, [this] { dispatchSessionAction(OperationAction::StopAll); });
	connect(startOutputs_, &QPushButton::clicked, this,
		[this] { dispatchSessionAction(OperationAction::StartStopped); });
	connect(restartOutputs_, &QPushButton::clicked, this,
		[this] { dispatchSessionAction(OperationAction::RestartAll); });
	// Повтор использует сохранённое намерение контроллера; UI не создаёт новый идентификатор команды.
	connect(retry_, &QPushButton::clicked, this, [this] {
		if (restream_)
			restream_->retryUnknown(stateStore_.dockContext());
	});
	connect(&stateStore_, &StateStore::operationsChanged, this, &RestreamDock::refreshOperations);
	connect(&stateStore_, &StateStore::obsStateChanged, this, &RestreamDock::refreshObservations);
	// Обработчик serverStateChanged сразу обновляет контекст и доступность команд, затем объединяет метрики.
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
	// connection содержит нынешнее состояние общего канала наблюдения.
	const auto connection = stateStore_.connectionStatus();
	// observedConnection оставляет отсутствие сервера отдельным от состояния отключения.
	const auto observedConnection = stateStore_.serverOrigin().isEmpty() ? std::nullopt : std::optional{connection};
	server_->setConnectionStatus(text_, observedConnection);
	// streamName содержит отображаемое имя выбранного Stream, если оно доступно.
	const auto streamName = stateStore_.selectedStreamName();
	stream_->setText(
		text_("Dock.SelectedStream").arg(streamName.isEmpty() ? text_("Observation.NoData") : streamName));
	// notices собирает только безопасные локализованные предупреждения.
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
	// profile содержит последнее доступное имя профиля OBS.
	const auto &profile = stateStore_.profileName();
	profile_->setText(
		text_("Dock.ProfileValue").arg(profile && !profile->isEmpty() ? *profile : text_("Observation.NoData")));
	// statistics даёт подтверждённую фазу штатной трансляции независимо от сервера.
	const auto &statistics = stateStore_.obsStatistics();
	if (statistics.observed)
		streaming_->setStreamingPhase(text_, statistics.phase);
	else
		streaming_->setStreamingStatus(text_, stateStore_.streamingActive());
	refreshOperations();
}

void RestreamDock::refreshServer()
{
	// Сначала очищаем кеш прежнего выбора или Session, чтобы их статистика не появилась у нового выбора.
	// При Connected можно читать runtime, но состояние, подключение входа и измерения подтверждаются
	// независимо через RuntimeEvidence. Полноту и свежесть состава выходов задаёт REST ReadStatus;
	// одно восстановленное соединение не делает все сохранённые значения снова актуальными.
	// stats содержит кеш и точки отсчёта текущего представления.
	auto &stats = *statistics_;
	// context определяет сервер, профиль OBS и выбранный Stream для показанных данных.
	const auto &context = stateStore_.dockContext();
	// server содержит долговечные метаданные и независимую свежесть REST-чтений.
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
	// connected показывает подтверждённую доступность общего канала наблюдения.
	const bool connected = stateStore_.connectionStatus() == ConnectionStatus::Connected;
	// sessionCurrent разрешает показывать сохранённое состояние Session как актуальное.
	const bool sessionCurrent = connected && server.session && fresh(server.sessionStatus.availability);
	// locale форматирует числа выбранной локали интерфейса.
	const QLocale locale(text_("Dock.NumberLocale"));
	// noData обозначает отсутствие подтверждённого наблюдения.
	const auto noData = text_("Observation.NoData");
	// runtime содержит принятый снимок и последовательные события SSE.
	const auto &runtime = stateStore_.runtime();
	// runtimeCurrent разрешает искать исполнения лишь в текущем подтверждённом контексте.
	const bool runtimeCurrent = connected && context.valid() && !stats.sessionId.isEmpty() &&
				    !runtime.cursor.isEmpty();
	// observed указывает на оперативное исполнение выбранной Session в runtime.
	const auto observed = runtime.sessions.constFind(stats.sessionId);
	// session заимствован из runtime на этот вызов и может отсутствовать.
	const auto *session = runtimeCurrent && observed != runtime.sessions.cend() ? &*observed : nullptr;
	// sessionInstance различает исполнения одной долговечной Session при очистке кеша.
	const auto sessionInstance = session && session->observation ? session->observation->instanceId : QString{};
	if ((!sessionInstance.isEmpty() && stats.sessionInstance != sessionInstance) || (runtimeCurrent && !session)) {
		stats.clearServer();
		stats.sessionInstance = sessionInstance;
	}
	// sessionEvidence независимо подтверждает состояние, подключение входа и измерения исполнения.
	const auto sessionEvidence = stateStore_.runtimeEvidence(sessionInstance);
	// status содержит переведённое состояние Session или отсутствие данных.
	const auto status = sessionCurrent ? text_(sessionKey(server.session->state)) : noData;
	// Tone обозначает цвета независимых наблюдаемых состояний, не результат отправки команды.
	using Tone = StatusLabel::Tone;
	// sessionTone окрашивает лишь подтверждённое состояние Session.
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

	// live считает группы со свежим фактическим источником LIVE.
	int live = 0;
	// fallback считает группы со свежим фактическим источником FALLBACK.
	int fallback = 0;
	// noSource считает группы со свежим подтверждённым отсутствием источника.
	int noSource = 0;
	// unknownSource сохраняет неизвестный код источника отдельно от отсутствия наблюдения.
	bool unknownSource = false;
	if (session) {
		// group обозначает одно исполнение группы, чья свежесть проверяется по instanceId.
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
	// sources собирает независимые количества фактических источников групп.
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

	// fallbackStatus описывает метаданные текущего поколения, не готовность Session.
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

	// input заимствован из выбранного исполнения Session и может отсутствовать.
	const auto *input = session && session->input ? &*session->input : nullptr;
	// inputMetrics указывает на отдельно доступные измерения серверного входа.
	const auto *inputMetrics = input && input->metrics ? &*input->metrics : nullptr;
	// Состояние подключения входа и его измерения могут подтверждаться разными событиями.
	// inputCurrent подтверждает свежесть подключения входа независимо от его метрик.
	const bool inputCurrent = session && fresh(sessionEvidence.input);
	// inputMetricsCurrent подтверждает свежесть измерений входа независимо от подключения.
	const bool inputMetricsCurrent = session && fresh(sessionEvidence.metrics);
	if ((inputMetricsCurrent && inputMetrics && stats.inputEpoch != inputMetrics->sourceEpoch) ||
	    (inputCurrent && !inputMetrics)) {
		// Новый источник входа начинает собственные измерения. Счётчики того же исполнения Output продолжаются.
		stats.inputEpoch = inputMetrics ? inputMetrics->sourceEpoch : 0;
		stats.inputFps = {};
		stats.inputBitrate = {};
	}
	// inputStatus показывает подключение только при отдельном подтверждении его свежести.
	const auto inputStatus = inputCurrent && input
					 ? text_(input->connected ? "Dock.InputConnected" : "Dock.InputDisconnected")
					 : noData;
	input_->setStatus(inputStatus, inputCurrent && input && input->connected ? Tone::Active : Tone::Neutral);
	// inputFormat собирает доступные поля формата входа без выдуманных значений.
	QStringList inputFormat;
	if (inputMetricsCurrent && inputMetrics) {
		// format содержит наблюдаемые кодек и размеры серверного входа.
		const auto &format = inputMetrics->media.format;
		if (!format.videoCodec.isEmpty())
			inputFormat.append(format.videoCodec.toUpper());
		if (format.width > 0 && format.height > 0)
			inputFormat.append(QStringLiteral("%1 × %2").arg(format.width).arg(format.height));
	}
	// ingestFps форматирует частоту входа, сохраняя явную подпись последнего значения.
	const auto ingestFps = stats.inputFps.display(inputMetrics ? &inputMetrics->media.rates.fps : nullptr,
						      inputMetricsCurrent, false, locale, text_);
	// ingestBitrate форматирует битрейт входа отдельно от локального OBS.
	const auto ingestBitrate =
		stats.inputBitrate.display(inputMetrics ? &inputMetrics->media.rates.payloadBitrate : nullptr,
					   inputMetricsCurrent, true, locale, text_);
	ingest_->setText(table(tableRow(text_("Dock.FormatLabel"),
					inputFormat.isEmpty() ? noData : inputFormat.join(QLatin1Char(' '))) +
			       tableRow(text_("Dock.FpsLabel"), ingestFps) +
			       tableRow(text_("Dock.BitrateLabel"), ingestBitrate)));
	// retained отмечает наличие показанных старых измерений для общей подписи.
	bool retained = (stats.inputFps.stale && stats.inputFps.last) ||
			(stats.inputBitrate.stale && stats.inputBitrate.last);

	// outputObservations индексирует исполнения Output точными долговечными outputId.
	QMap<QString, const api::RuntimeOutput *> outputObservations;
	if (session) {
		// output заимствован из наблюдения текущей Session; отсутствие не означает остановку.
		for (const auto &output : session->outputs)
			outputObservations.insert(output.outputId, &output);
	}
	// rows объединяет прикреплённые и ещё не опровергнутые наблюдаемые выходы.
	QSet<QString> rows;
	// Показываем прикреплённые по REST выходы и наблюдаемые исполнения, для которых REST ещё не сообщил
	// об отсоединении. Отсутствие ID в неполном списке или отсутствие метрик само по себе не удаляет строку.
	// it проходит текущую карту выходов; её ключи остаются точными ID при объединении и удалении строк.
	for (auto it = server.outputs.cbegin(); it != server.outputs.cend(); ++it) {
		if (it->attached)
			rows.insert(it.key());
	}
	// it проходит текущую карту выходов; её ключи остаются точными ID при объединении и удалении строк.
	for (auto it = outputObservations.cbegin(); it != outputObservations.cend(); ++it) {
		// metadata указывает на сохранённое REST-состояние точного Output, если оно уже прочитано.
		const auto metadata = server.outputs.constFind(it.key());
		if (metadata == server.outputs.cend() || metadata->attached)
			rows.insert(it.key());
	}
	if (!runtimeCurrent) {
		// При потере связи сохраняем последние строки; прежние измерения будут подписаны как устаревшие.
		// it проходит текущую карту выходов; её ключи остаются точными ID при объединении и удалении строк.
		for (auto it = stats.outputs.cbegin(); it != stats.outputs.cend(); ++it)
			rows.insert(it.key());
	}
	// it проходит текущую карту выходов; её ключи остаются точными ID при объединении и удалении строк.
	for (auto it = stats.outputs.begin(); it != stats.outputs.end();) {
		if (!rows.contains(it.key()))
			it = stats.outputs.erase(it);
		else
			++it;
	}
	// outputRows содержит доступное текстовое описание строк для модели и средств доступности.
	QStringList outputRows;
	// RowValues содержит отображение строки и точный ID; имя не используется как адрес команды.
	struct RowValues {
		// id адресует точный Output при открытии меню.
		QString id;
		// name содержит безопасное отображаемое имя без влияния на адресацию команды.
		QString name;
		// status содержит переведённое runtime-состояние выхода или отсутствие данных.
		QString status;
		// rate содержит оформленный текущий или явно устаревший битрейт выхода.
		QString rate;
		// details содержит отдельные счётчики переподключений, очереди и доставки.
		QString details;
		// tone задаёт цвет только наблюдаемого состояния выхода.
		Tone tone;
	};
	// rowValues содержит значения виджетов в том же порядке, что outputRows.
	QVector<RowValues> rowValues;
	// streaming считает только свежие наблюдения PUBLISHING, независимо от квитанций.
	int streaming = 0;
	// complete разрешает сводку количества только при полном составе и известных состояниях.
	bool complete = sessionCurrent && fresh(server.outputsStatus.availability) && server.outputsStatus.complete;
	// Сводка «N из M» допустима только при полном составе и известном состоянии каждой строки.
	// Наличие bitrate, единичного PUBLISHING или пустой карты не подтверждает общий итог.
	// id обозначает точный Output в наборе строк или кеше, независимо от имени.
	for (const auto &id : std::as_const(rows))
		stats.outputs[id];
	// it проходит текущую карту выходов; её ключи остаются точными ID при объединении и удалении строк.
	for (auto it = stats.outputs.begin(); it != stats.outputs.end(); ++it) {
		// id обозначает точный Output в наборе строк или кеше, независимо от имени.
		const auto &id = it.key();
		// cache сохраняет измерения и точки отсчёта одного исполнения Output.
		auto &cache = it.value();
		// output заимствован из наблюдения текущей Session; отсутствие не означает остановку.
		const auto *output = outputObservations.value(id);
		// metadata указывает на сохранённое REST-состояние точного Output, если оно уже прочитано.
		const auto metadata = server.outputs.constFind(id);
		if ((output && output->instanceId != cache.instance) || (runtimeCurrent && !output)) {
			cache = {};
			if (output)
				cache.instance = output->instanceId;
		}
		if (metadata != server.outputs.cend())
			cache.name = metadata->name;
		// evidence разделяет свежесть состояния выхода и свежесть измерений.
		const auto evidence = stateStore_.runtimeEvidence(cache.instance);
		// statusCurrent подтверждает актуальность runtime-состояния выбранного исполнения.
		const bool statusCurrent = output && fresh(evidence.state);
		// metricsCurrent подтверждает актуальность скоростей и счётчиков выбранного исполнения.
		const bool metricsCurrent = output && fresh(evidence.metrics);
		// metrics заимствован из исполнения Output и может отсутствовать при известном состоянии.
		const auto *metrics = output && output->metrics ? &*output->metrics : nullptr;
		// label содержит переведённое наблюдаемое состояние выхода.
		const auto label = statusCurrent ? text_(outputKey(output->state)) : noData;
		// knownState отличает известный код runtime от отсутствующего или неизвестного.
		const bool knownState = statusCurrent && label != noData && label != text_("Dock.UnknownStatus");
		complete = complete && knownState && metadata != server.outputs.cend();
		streaming += statusCurrent && output->state == QStringLiteral("PUBLISHING");
		// identity привязывает накопления только к текущему исполнению Output.
		const StatsBaseline::Identity identity{cache.instance};
		// Потери очереди и доставки суммируются по своим категориям, независимо от сетевых пропусков OBS.
		// queueDrops суммирует категории потерь очереди с проверкой переполнения.
		const auto queueDrops = metrics ? dropTotal(metrics->queueDrops) : DropTotal{};
		// deliveryDrops суммирует категории потерь доставки отдельно от очереди.
		const auto deliveryDrops = metrics ? dropTotal(metrics->deliveryDrops) : DropTotal{};
		// saturated показывает насыщение накопительных значений в серверном измерении.
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
			// queued содержит наблюдаемую длину очереди пакетов, если сервер передал её.
			const auto queued = metrics ? counter(metrics->counters, QStringLiteral("queue_packets"))
						    : std::nullopt;
			cache.queue = queued ? locale.toString(*queued) : QString{};
		}
		// currentBitrate отображает скорость выбранного исполнения с учётом свежести.
		const auto currentBitrate = cache.bitrate.display(
			metrics ? &metrics->media.rates.writtenBitrate : nullptr, metricsCurrent, true, locale, text_);
		// last форматирует baseline; callback помечает retained, когда показывает прежнее измерение.
		const auto last = [&](const StatsBaseline &baseline) {
			// value содержит оформленное значение одного baseline до добавления признака устаревания.
			const auto value = counterText(locale, text_, baseline);
			if (metricsCurrent || !baseline.reading().counters)
				return value;
			retained = true;
			return text_("Dock.LastValue").arg(value);
		};
		retained = retained || (cache.bitrate.stale && cache.bitrate.last) ||
			   (!metricsCurrent && !cache.queue.isEmpty());
		// queue отображает текущую или явно устаревшую длину очереди.
		const auto queue = cache.queue.isEmpty() ? noData
				   : metricsCurrent      ? cache.queue
							 : text_("Dock.LastValue").arg(cache.queue);
		// name содержит безопасное отображаемое имя без влияния на адресацию команды.
		const auto name = cache.name.isEmpty() ? text_("Dock.UnnamedOutput") : cache.name.simplified();
		outputRows.append(text_("Dock.OutputRow")
					  .arg(name, label, currentBitrate, last(cache.reconnects), queue,
					       last(cache.queueDrops), last(cache.deliveryDrops)));
		// tone окрашивает только наблюдаемый статус, не результат команды.
		const auto tone = !statusCurrent                                  ? Tone::Neutral
				  : output->state == QStringLiteral("PUBLISHING") ? Tone::Active
				  : output->state == QStringLiteral("CONNECTING") ||
						  output->state == QStringLiteral("RETRYING")
					  ? Tone::Pending
				  : output->state == QStringLiteral("FAILED") ? Tone::Error
									      : Tone::Neutral;
		rowValues.append(
			{id, name, label, currentBitrate,
			 text_("Dock.OutputDetails")
				 .arg(last(cache.reconnects), queue, last(cache.queueDrops), last(cache.deliveryDrops)),
			 tone});
	}
	if (outputRows.isEmpty())
		outputRows.append(noData);
	updateRows(*outputs_, outputRows);
	// row обозначает позицию виджета в существующей модели, но не идентичность цели.
	for (int row = 0; row < outputs_->rowCount(); ++row) {
		// index выбирает текущий виджет строки в QListView.
		const auto index = outputs_->index(row, 0);
		if (rowValues.isEmpty()) {
			if (outputList_->indexWidget(index))
				outputList_->setIndexWidget(index, nullptr);
			continue;
		}
		// widget принадлежит viewport и может использоваться для другого Output после обновления.
		auto *widget = static_cast<OutputRow *>(outputList_->indexWidget(index));
		if (!widget) {
			widget = new OutputRow(text_, outputList_->viewport());
			outputList_->setIndexWidget(index, widget);
			// При активации читается ID нынешней строки, затем outputActions фиксирует весь контекст.
			// Повторное использование indexWidget не оставляет в callback ID прежнего выхода.
			connect(widget->action, &QToolButton::clicked, this, [this, widget] {
				outputActions(widget->context, widget->sessionId, widget->outputId);
			});
		}
		// values содержит значения именно нынешней строки, включая точный ID.
		const auto &values = rowValues.at(row);
		widget->outputId = values.id;
		widget->context = context;
		widget->sessionId = server.session ? server.session->id : QString{};
		widget->action->setEnabled(restream_ && sessionCurrent && fresh(server.outputsStatus.availability));
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
	// dataNotice объясняет недоступность сервера и сохранённые прежние измерения.
	QString dataNotice;
	if (context.valid() && (!connected || retained)) {
		dataNotice = text_("Dock.ServerDataMissing");
		if (retained)
			dataNotice += QLatin1Char('\n') + text_("Dock.LastValues");
	}
	serverDataNotice_->setText(dataNotice);
	serverDataNotice_->setVisible(!dataNotice.isEmpty());
	refreshOperations();
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

// Обновляет доступность по свежим REST-данным; окончательное решение и повторную проверку делает контроллер.
void RestreamDock::refreshOperations()
{
	// server содержит свежесть точной Session и полного состава выходов.
	const auto &server = stateStore_.dockServer();
	// operations не подменяет runtime и показывает результат независимо от последующей телеметрии.
	const auto &operations = stateStore_.operations();
	// busy блокирует новые несовместимые действия; Stop All может прервать ещё не отправленный пакет.
	const bool busy = operations.current && operations.current->phase != OperationPhase::Idle &&
			  operations.current->phase != OperationPhase::Finished;
	// current требует подтверждённой связи и свежей выбранной Session.
	const bool current = restream_ && stateStore_.connectionStatus() == ConnectionStatus::Connected &&
			     stateStore_.dockContext().valid() && fresh(server.sessionStatus.availability) &&
			     server.session;
	// running разрешает pause/resume только для RUNNING, а stop допустим и в переходных состояниях.
	const bool running = current && server.session->state == QStringLiteral("RUNNING");
	// active не трактует terminal Session как продолжающийся эфир.
	const bool active = current && (running || server.session->state == QStringLiteral("STARTING") ||
					server.session->state == QStringLiteral("STOPPING"));
	// obsStatistics содержит последнюю локальную фазу OBS, независимо от простого флага активности.
	const auto &obsStatistics = stateStore_.obsStatistics();
	// obsActive сохраняет наблюдаемую фазу перехода, даже если простой булевый статус ещё не обновился.
	const bool obsActive = stateStore_.streamingActive().value_or(false) ||
			       (obsStatistics.observed && obsStatistics.phase != ObsStreamingPhase::Unknown &&
				obsStatistics.phase != ObsStreamingPhase::Stopped);
	pause_->setEnabled(running && !busy && !server.session->desiredPaused);
	resume_->setEnabled(running && !busy && server.session->desiredPaused);
	stop_->setEnabled(active &&
			  (!operations.current || operations.current->action != OperationAction::StopAll || !busy));
	// outputsKnown предотвращает выбор пакета из частичного или устаревшего состава.
	const bool outputsKnown = running && !busy && fresh(server.outputsStatus.availability) &&
				  server.outputsStatus.complete;
	// attached и stopped обозначают наличие подходящих прикреплённых выходов, а не runtime RETRYING.
	bool attached = false;
	bool stopped = false;
	// output — текущие долговечные намерения выхода.
	for (const auto &output : server.outputs) {
		attached = attached || output.attached;
		stopped = stopped || (output.attached && !output.desiredRunning);
	}
	startOutputs_->setEnabled(outputsKnown && stopped);
	restartOutputs_->setEnabled(outputsKnown && attached);
	// sessionReadComplete требует полного свежего чтения для действующего контекста и подключения.
	const bool sessionReadComplete = stateStore_.connectionStatus() == ConnectionStatus::Connected &&
					 stateStore_.dockContext().valid() &&
					 fresh(server.sessionStatus.availability) && server.sessionStatus.complete;
	// knownNoActive требует полного свежего чтения: неизвестная или частичная выборка не доказывает отсутствие эфира.
	const bool knownNoActive = sessionReadComplete &&
				   (!server.session || server.session->state == QStringLiteral("STOPPED") ||
				    server.session->state == QStringLiteral("FAILED") ||
				    server.session->state == QStringLiteral("INTERRUPTED"));
	// operationText совмещает штатный результат и отдельный риск следующей публикации.
	QString operationText = formatOperations(operations, text_);
	if (knownNoActive && obsActive)
		operationText.append(operationText.isEmpty() ? QString{} : QStringLiteral("\n\n"))
			.append(text_("Operation.ObsStillActive"));
	operationStatus_->setText(operationText);
	operationStatus_->setVisible(!operationStatus_->text().isEmpty());
	// Подсказка штатного запуска нужна лишь при отсутствии серверного эфира и активной фазы OBS.
	nativeStart_->setVisible(knownNoActive && !obsActive);
	retry_->setVisible(operations.current && operations.current->retryAvailable);
	retry_->setEnabled(restream_ && operations.current && operations.current->retryAvailable &&
			   operations.current->context == stateStore_.dockContext());
}

// Фиксирует контекст, Session и весь состав пакета до модального подтверждения, затем передаёт исходную цель.
void RestreamDock::dispatchSessionAction(OperationAction action)
{
	if (!restream_ || !stateStore_.dockServer().session)
		return;
	// context и sessionId сохраняют цель до вложенного event loop диалога.
	const auto context = stateStore_.dockContext();
	const auto sessionId = stateStore_.dockServer().session->id;
	// outputIds фиксирует состав только один раз; телеметрия во время подтверждения его не расширяет.
	QStringList outputIds;
	// output проходит исходные долговечные состояния, id обозначает точный Output.
	for (auto output = stateStore_.dockServer().outputs.cbegin(); output != stateStore_.dockServer().outputs.cend();
	     ++output) {
		if (output->attached && (action != OperationAction::StartStopped || !output->desiredRunning))
			outputIds.append(output.key());
	}
	if (action == OperationAction::StopAll || action == OperationAction::RestartAll) {
		// alive запрещает доступ к доку после завершения OBS во вложенном цикле диалога.
		const QPointer<RestreamDock> alive(this);
		// confirmation принадлежит доку; QPointer замечает удаление вместе с родителем.
		const QPointer<QMessageBox> confirmation =
			new QMessageBox(QMessageBox::Question, text_("Operation.ConfirmTitle"),
					text_(action == OperationAction::StopAll ? "Operation.ConfirmStopAll"
										 : "Operation.ConfirmRestartAll"),
					QMessageBox::Yes | QMessageBox::No, this);
		confirmation->setDefaultButton(QMessageBox::No);
		localizeConfirmation(*confirmation, text_);
		// choice относится только к исходной цели, которую контроллер повторно проверит после диалога.
		const auto choice = confirmation->exec();
		if (confirmation)
			delete confirmation;
		if (!alive || choice != QMessageBox::Yes)
			return;
	}
	switch (action) {
	case OperationAction::Pause:
		restream_->pause(context, sessionId);
		break;
	case OperationAction::Resume:
		restream_->resume(context, sessionId);
		break;
	case OperationAction::StopAll:
		restream_->stopAll(context, sessionId);
		break;
	case OperationAction::StartStopped:
		restream_->startStopped(context, sessionId, outputIds);
		break;
	case OperationAction::RestartAll:
		restream_->restartAll(context, sessionId, outputIds);
		break;
	default:
		break;
	}
}

// Открывает меню для значения outputId, копируя также context и Session до любого вложенного event loop.
void RestreamDock::outputActions(DockContext context, QString sessionId, QString outputId)
{
	if (!restream_ || !stateStore_.dockServer().session)
		return;
	if (context != stateStore_.dockContext() || stateStore_.dockServer().session->id != sessionId)
		return;
	// metadata даёт только начальную доступность меню; перед отправкой контроллер проверяет цель заново.
	const auto metadata = stateStore_.dockServer().outputs.value(outputId);
	// operations и busy отделяют доступность команд от просмотра подробностей уже начатого действия.
	const auto &operations = stateStore_.operations();
	const bool busy = operations.current && operations.current->phase != OperationPhase::Idle &&
			  operations.current->phase != OperationPhase::Finished;
	// allowed учитывает свежесть исходной цели; фасад повторяет проверку после закрытия меню и диалогов.
	const bool allowed = !busy && stateStore_.connectionStatus() == ConnectionStatus::Connected &&
			     stateStore_.dockServer().session->state == QStringLiteral("RUNNING") &&
			     fresh(stateStore_.dockServer().sessionStatus.availability) &&
			     fresh(stateStore_.dockServer().outputsStatus.availability);
	// menu принадлежит стеку GUI и возвращает выбранное намерение, не захватывая индекс строки.
	QMenu menu(this);
	// start, stop и restart обозначают три явные команды выбранного выхода.
	auto *start = menu.addAction(text_("Operation.StartOutput"));
	auto *stop = menu.addAction(text_("Operation.StopOutput"));
	auto *restart = menu.addAction(text_("Operation.RestartOutput"));
	// details переводит в существующее окно; оно также проверяет неизменность исходной цели.
	auto *details = menu.addAction(text_("Operation.OutputDetails"));
	start->setEnabled(allowed && metadata.attached && !metadata.desiredRunning);
	stop->setEnabled(allowed && metadata.attached && metadata.desiredRunning);
	restart->setEnabled(allowed && metadata.attached);
	// selected — действие исходного меню, даже если обновилась строка дока.
	const auto *selected = menu.exec(QCursor::pos());
	if (!selected)
		return;
	if (selected == details) {
		emit outputDetailsRequested(context, sessionId, outputId);
		return;
	}
	if (selected == restart) {
		// alive замечает завершение OBS, пока пользователь отвечает на подтверждение.
		const QPointer<RestreamDock> alive(this);
		// confirmation сохраняет стандартные ответы и их смысл после явного перевода.
		const QPointer<QMessageBox> confirmation =
			new QMessageBox(QMessageBox::Question, text_("Operation.ConfirmTitle"),
					text_("Operation.ConfirmRestart"), QMessageBox::Yes | QMessageBox::No, this);
		confirmation->setDefaultButton(QMessageBox::No);
		localizeConfirmation(*confirmation, text_);
		// choice фиксируется до удаления собственного диалога.
		const auto choice = confirmation->exec();
		if (confirmation)
			delete confirmation;
		if (!alive || choice != QMessageBox::Yes)
			return;
	}
	restream_->outputActions(context, sessionId, outputId,
				 selected == start  ? OperationAction::StartOutput
				 : selected == stop ? OperationAction::StopOutput
						    : OperationAction::RestartOutput);
}

} // namespace gost
