#include "restream-dock.hpp"

#include "core/state-store.hpp"
#include "core/stats-baseline.hpp"
#include "core/restream-controller.hpp"
#include "core/connection-controller.hpp"
#include "ui/status-label.hpp"
#include "ui/confirmation-dialog.hpp"

#include <QFormLayout>
#include <QFontMetrics>
#include <QComboBox>
#include <QGridLayout>
#include <QHBoxLayout>
#include <QLabel>
#include <QListView>
#include <QLocale>
#include <QMenu>
#include <QMessageBox>
#include <QStyle>
#include <QAction>
#include <QPalette>
#include <QPainter>
#include <QPushButton>
#include <QPointer>
#include <QSet>
#include <QSignalBlocker>
#include <QStandardItemModel>
#include <QScrollArea>
#include <QScrollBar>
#include <QResizeEvent>
#include <QTimer>
#include <QTextBoundaryFinder>
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

// Добавляет карточку name с заголовком title в parent. При запросе headingLayout возвращает
// горизонтальную шапку для расположенных рядом с заголовком команд.
QVBoxLayout *card(QVBoxLayout *parent, const char *name, const QString &title, QHBoxLayout **headingLayout = nullptr)
{
	// widget рисует фон карточки текущей темой и передаётся в компоновку parent.
	auto *widget = new DockCard;
	widget->setObjectName(QString::fromLatin1(name));
	// layout располагает шапку и содержимое внутри widget.
	auto *layout = new QVBoxLayout(widget);
	layout->setContentsMargins(10, 10, 10, 10);
	layout->setSpacing(8);
	// heading показывает название карточки обычным полужирным текстом.
	auto *heading = valueLabel(widget, "cardTitle");
	heading->setText(title);
	bold(heading);
	if (headingLayout) {
		*headingLayout = new QHBoxLayout;
		(*headingLayout)->setSpacing(4);
		(*headingLayout)->addWidget(heading, 1);
		layout->addLayout(*headingLayout);
	} else {
		layout->addWidget(heading);
	}
	parent->addWidget(widget);
	return layout;
}

// Формирует экранированную строку: label подписывает value, emphasis выделяет значение.
// stacked переносит значение под подпись в узкой колонке, сохраняя полный текст.
QString tableRow(const QString &label, const QString &value, bool emphasis = false, bool stacked = false)
{
	// formatted содержит значение без возможности вставить серверную строку как HTML.
	const auto formatted = value.toHtmlEscaped();
	if (stacked)
		return QStringLiteral("<tr><td valign='top'>%1<br>%2</td></tr>\n")
			.arg(label.toHtmlEscaped(), emphasis ? QStringLiteral("<b>%1</b>").arg(formatted) : formatted);
	return QStringLiteral("<tr><td valign='top'>%1</td><td valign='top'>%2</td></tr>\n")
		.arg(label.toHtmlEscaped(), emphasis ? QStringLiteral("<b>%1</b>").arg(formatted) : formatted);
}

QString table(const QString &rows)
{
	return QStringLiteral("<table width='100%' cellspacing='0' cellpadding='2'>%1</table>").arg(rows);
}

// Экранирует value для колонки шириной columnWidth с метриками её шрифта metrics.
// Короткие слова сохраняют обычный перенос. В слишком широких фрагментах между пробелами
// добавляет возможности переноса U+200B только между целыми графемами: QLabel использует WordWrap
// и иначе расширяет таблицу до ширины непрерывного слова. Исходные данные не изменяются.
QString statisticText(const QString &value, const QFontMetrics &metrics, int columnWidth)
{
	// fragments сохраняет обычные пробелы, включая повторные, при обратном соединении.
	const auto fragments = value.split(QLatin1Char(' '), Qt::KeepEmptyParts);
	// escapedFragments содержит безопасный HTML каждого исходного фрагмента в прежнем порядке.
	QStringList escapedFragments;
	// fragment содержит очередное слово или пустой промежуток между исходными пробелами.
	for (const auto &fragment : fragments) {
		if (metrics.horizontalAdvance(fragment) <= columnWidth) {
			escapedFragments.append(fragment.toHtmlEscaped());
			continue;
		}
		// escaped собирает текст длинного фрагмента; только вставляемая сущность является разметкой.
		QString escaped;
		// boundaries находит целые графемы, не разделяя суррогатные пары и составные символы.
		QTextBoundaryFinder boundaries(QTextBoundaryFinder::Grapheme, fragment);
		// start указывает на начало ещё не добавленной графемы исходной строки.
		qsizetype start = 0;
		// end отмечает границу после очередной графемы в исходном UTF-16.
		for (qsizetype end = boundaries.toNextBoundary(); end >= 0; end = boundaries.toNextBoundary()) {
			escaped.append(fragment.mid(start, end - start).toHtmlEscaped());
			if (end < fragment.size())
				escaped.append(QStringLiteral("&#8203;"));
			start = end;
		}
		escapedFragments.append(escaped);
	}
	return escapedFragments.join(QLatin1Char(' '));
}

// Формирует одну из трёх колонок статистики: label находится над уже оформленным value.
// metrics и columnWidth задают перенос обеих экранированных строк по текущему шрифту и ширине.
QString statisticColumn(const QString &label, const QString &value, const QFontMetrics &metrics, int columnWidth)
{
	return QStringLiteral("<td width='33%' valign='top'>%1<br>%2</td>")
		.arg(statisticText(label, metrics, columnWidth), statisticText(value, metrics, columnWidth));
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

// Показывает имя, состояние и битрейт в одну строку. Полный текст остаётся у QLabel,
// в подсказках и средствах доступности; сокращается только рисуемая строка.
class OutputRow final : public QWidget {
public:
	// Создаёт однострочное представление; text переводит меню, parent получает владение виджетами.
	explicit OutputRow(const TextLookup &text, QWidget *parent) : QWidget(parent)
	{
		setAutoFillBackground(true);
		setBackgroundRole(QPalette::Window);
		// layout распределяет три поля с равными долями ширины и оставляет меню справа.
		auto *layout = new QHBoxLayout(this);
		layout->setContentsMargins(4, 4, 4, 4);
		layout->setSpacing(6);
		name = valueLabel(this, "outputName");
		bold(name);
		status = new StatusLabel(this);
		status->setObjectName(QStringLiteral("outputStatus"));
		rate = valueLabel(this, "outputBitrate");
		rate->setAlignment(Qt::AlignRight | Qt::AlignVCenter);
		// label проходит три поля, которые сжимаются без переноса и сохраняют исходный текст.
		for (auto *label : {name, static_cast<QLabel *>(status), rate}) {
			label->setWordWrap(false);
			label->setFixedHeight(label->fontMetrics().height());
			label->setMinimumWidth(0);
			label->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred);
			label->installEventFilter(this);
			layout->addWidget(label, 1);
		}
		status->setAlignment(Qt::AlignLeft | Qt::AlignVCenter);
		action = new QToolButton(this);
		action->setObjectName(QStringLiteral("outputActionsButton"));
		action->setText(QString::fromUtf8("…"));
		action->setEnabled(false);
		action->setToolTip(text("Dock.OutputActions"));
		action->setAccessibleName(text("Dock.OutputActions"));
		layout->addWidget(action);
	}

	// name содержит имя снимка Output либо назначения каталога; имя не адресует команды.
	QLabel *name;
	// status хранит полное наблюдаемое состояние и его смысловой цвет.
	StatusLabel *status;
	// rate содержит текущий либо явно устаревший битрейт без выдуманных нулей.
	QLabel *rate;
	// action открывает меню точного Output либо выбранного назначения каталога.
	QToolButton *action;
	// outputId адресует точный Output; пустая строка обозначает назначение без текущего Output.
	QString outputId;
	// destinationId адресует назначение отдельно от Output; неизвестная runtime-связь остаётся пустой.
	QString destinationId;
	// context сохраняет сервер, UUID профиля и Stream, которым принадлежит показанная строка.
	DockContext context;
	// sessionId фиксирует Session строки независимо от последующего обновления состояния.
	QString sessionId;

protected:
	// При Paint рисует полное значение watched с сокращением по доступной ширине.
	// FontChange сохраняет высоту одной строки, даже если имя содержит переносы. Остальные event
	// передаются QWidget. Точка StatusLabel сохраняет цвет и место рядом с текстом.
	bool eventFilter(QObject *watched, QEvent *event) override
	{
		// label указывает на одно из трёх полей с установленным этим объектом фильтром.
		auto *label = qobject_cast<QLabel *>(watched);
		if (!label)
			return QWidget::eventFilter(watched, event);
		if (event->type() == QEvent::FontChange)
			label->setFixedHeight(label->fontMetrics().height());
		if (event->type() != QEvent::Paint)
			return QWidget::eventFilter(watched, event);
		// painter рисует только поле; полный QLabel::text остаётся доступным без сокращения.
		QPainter painter(label);
		painter.setFont(label->font());
		// bounds учитывает отступ точки StatusLabel и внутренние поля QLabel.
		const auto bounds = label->contentsRect().adjusted(label->margin(), label->margin(), -label->margin(),
								   -label->margin());
		// visibleText заменяет переносы пробелами и сокращает только рисуемое представление.
		const auto visibleText = label->fontMetrics().elidedText(label->text().simplified(), Qt::ElideRight,
									 qMax(0, bounds.width()));
		label->style()->drawItemText(&painter, bounds,
					     static_cast<int>(label->alignment()) | Qt::TextSingleLine,
					     label->palette(), label->isEnabled(), visibleText, QPalette::WindowText);
		if (label == status) {
			painter.setRenderHint(QPainter::Antialiasing);
			painter.setPen(Qt::NoPen);
			painter.setBrush(label->palette().color(QPalette::WindowText));
			painter.drawEllipse(QRectF(1, (label->height() - 6) / 2.0, 6, 6));
		}
		return true;
	}
};

// Сохраняет модель, виджеты и прокрутку при телеметрии, ограничивая высоту списка выходов.
class OutputList final : public QListView {
public:
	// Использует стандартные конструкторы QListView с обычным владением родительским QWidget.
	using QListView::QListView;

	// Обновляет только изменившиеся высоты строк; общий предел оставляет длинный список прокручиваемым.
	void updateHeights()
	{
		// total суммирует высоты строк для списка, который ещё не достиг предела.
		int total = 0;
		// row адресует очередной виджет модели, не идентичность Output.
		for (int row = 0; row < model()->rowCount(); ++row) {
			// index выбирает размер и виджет одной существующей строки.
			const auto index = model()->index(row, 0);
			// widget заимствован у viewport; отсутствие означает обычную строку «Нет данных».
			auto *widget = indexWidget(index);
			// height учитывает высоту меню и однострочных полей без зависимости от длины текста.
			const int height = widget ? widget->sizeHint().height() : fontMetrics().height() + 4;
			// hint закрепляет текущую ширину viewport и достаточную высоту одной строки.
			const QSize hint(qMax(1, viewport()->width()), qMax(fontMetrics().height() + 4, height));
			if (index.data(Qt::SizeHintRole).toSize() != hint)
				model()->setData(index, hint, Qt::SizeHintRole);
			total += hint.height();
		}
		setFixedHeight(qMin(total + 2, fontMetrics().height() * 11));
	}

protected:
	// После штатного resize event согласует размеры строк с новой шириной viewport.
	void resizeEvent(QResizeEvent *event) override
	{
		QListView::resizeEvent(event);
		updateHeights();
	}
};

// Экранирует полный text для подсказки Qt, сохраняя переносы. Серверное имя с '<...>'
// показывается буквально, а не становится разметкой подсказки.
QString outputToolTip(const QString &text)
{
	return QStringLiteral("<qt>%1</qt>")
		.arg(text.toHtmlEscaped().replace(QLatin1Char('\n'), QStringLiteral("<br>")));
}

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

// Форматирует operations с текущим и прерванным результатом, используя переводы text.
// Квитанции, части строк и наблюдаемое состояние не смешиваются; ID и сырой текст отказа не выводятся.
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
		case OperationAction::StartDestination:
			return text("Operation.StartDestination");
		case OperationAction::StartOutput:
			return text("Operation.StartOutput");
		case OperationAction::StopOutput:
			return text("Operation.StopOutput");
		case OperationAction::RestartOutput:
			return text("Operation.RestartOutput");
		case OperationAction::DetachOutput:
			return text("Operation.DetachOutput");
		case OperationAction::StartStopped:
		case OperationAction::StartAllOutputs:
			return text("Dock.StartOutputs");
		case OperationAction::StopAllOutputs:
			return text("Dock.StopOutputs");
		case OperationAction::RestartAll:
		case OperationAction::RestartAllOutputs:
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
	// describe составляет одно представление workflow; вложенные обработчики вызываются синхронно.
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
		// output — элемент исходного фиксированного пакета; имя показывается только обычным текстом.
		for (const auto &output : workflow.outputs) {
			// name обозначает исходную строку; ни имя, ни оформленный итог не адресуют команды.
			const auto name = output.name.isEmpty() ? text("Dock.UnnamedOutput") : output.name;
			lines.append(text("Operation.OutputResult").arg(name, outcomeText(output.outcome)));
			if (output.stopOutcome != OperationOutcome::None)
				lines.append(
					text("Operation.OutputStopPart").arg(name, outcomeText(output.stopOutcome)));
			if (output.startOutcome != OperationOutcome::None)
				lines.append(
					text("Operation.OutputStartPart").arg(name, outcomeText(output.startOutcome)));
			if (output.noOp)
				lines.append(text("Operation.OutputNoOp").arg(name));
			if (!output.messageKey.isEmpty())
				lines.append(text("Operation.OutputReason")
						     .arg(name, text(output.messageKey.toUtf8().constData())));
		}
		if (workflow.retryAvailable && !workflow.retryTargetName.isEmpty())
			lines.append(text("Operation.RetryTarget").arg(workflow.retryTargetName));
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

// Создаёт представление stateStore в GUI-потоке с переводами text и версией version внизу.
// parent получает Dock; restream принимает команды эфира, connection — явный выбор рабочего Stream.
// Оба контроллера необязательны и заимствованы; их отсутствие отключает соответствующие действия.
RestreamDock::RestreamDock(StateStore &stateStore, TextLookup text, QWidget *parent, QString version,
			   RestreamController *restream, ConnectionController *connection)
	: QWidget(parent),
	  stateStore_(stateStore),
	  text_(std::move(text)),
	  restream_(restream),
	  connection_(connection),
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
	// header располагает рабочий Stream, подключение и единственную кнопку открытия управления.
	auto *header = new QHBoxLayout;
	streams_ = new QComboBox(this);
	streams_->setObjectName(QStringLiteral("streamSelector"));
	streams_->setAccessibleName(text_("Settings.Stream"));
	streams_->setMinimumWidth(0);
	streams_->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Fixed);
	bold(streams_);
	header->addWidget(streams_, 1);
	server_ = new StatusLabel(this, false);
	server_->setObjectName(QStringLiteral("serverObservation"));
	server_->setMinimumWidth(0);
	server_->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred);
	header->addWidget(server_, 1);
	// gear открывает существующее расширенное окно и доступна с клавиатуры.
	auto *gear = new QToolButton(this);
	gear->setObjectName(QStringLiteral("managementButton"));
	gear->setText(QString::fromUtf8("⚙"));
	gear->setToolTip(text_("Management.Open"));
	gear->setAccessibleName(text_("Management.Open"));
	header->addWidget(gear);
	layout->addLayout(header);

	// state содержит независимые оси OBS, входа, Session, групп и заглушки.
	auto *state = card(layout, "stateCard", text_("Dock.State"));
	// stateColumns даёт обеим группам состояний равную долю доступной ширины.
	auto *stateColumns = new QHBoxLayout;
	stateColumns->setSpacing(8);
	// localState выравнивает OBS, вход и Session; serverState — группы и заглушку.
	auto *localState = new QFormLayout;
	auto *serverState = new QFormLayout;
	// observations обозначает очередную форму; перенос сохраняет читаемость при узком Dock.
	for (auto *observations : {localState, serverState}) {
		observations->setContentsMargins(0, 0, 0, 0);
		observations->setSpacing(4);
		observations->setFieldGrowthPolicy(QFormLayout::AllNonFixedFieldsGrow);
		observations->setRowWrapPolicy(QFormLayout::WrapLongRows);
	}
	// axis добавляет в column подпись key и отдельный статус name, не объединяя смысл осей.
	const auto axis = [&](QFormLayout *column, const char *name, const char *key) {
		// label принадлежит доку и отражает одну независимую ось состояния.
		auto *label = new StatusLabel(this);
		label->setObjectName(QString::fromLatin1(name));
		// policy позволяет индикатору сжиматься при небольшой ширине дока.
		auto policy = label->sizePolicy();
		policy.setHorizontalPolicy(QSizePolicy::Preferred);
		label->setSizePolicy(policy);
		// caption переносит переведённую подпись, не задавая жёсткую минимальную ширину колонки.
		auto *caption = valueLabel(this, "stateCaption");
		caption->setText(text_(key));
		caption->setSizePolicy(QSizePolicy::Preferred, QSizePolicy::Preferred);
		column->addRow(caption, label);
		return label;
	};
	streaming_ = axis(localState, "obsStreaming", "Dock.Obs");
	input_ = axis(localState, "inputObservation", "Dock.InputLabel");
	session_ = axis(localState, "sessionObservation", "Dock.SessionLabel");
	groups_ = axis(serverState, "groupObservation", "Dock.GroupsLabel");
	fallback_ = axis(serverState, "fallbackObservation", "Dock.FallbackLabel");
	stateColumns->addLayout(localState, 1);
	stateColumns->addLayout(serverState, 1);
	state->addLayout(stateColumns);

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
	// localColumns размещает текущие измерения и накопления OBS рядом с равными долями ширины.
	auto *localColumns = new QHBoxLayout;
	localColumns->setSpacing(8);
	localStats_ = valueLabel(content, "obsStatistics");
	localStats_->setTextFormat(Qt::RichText);
	localStats_->setAlignment(Qt::AlignTop | Qt::AlignLeft);
	localColumns->addWidget(localStats_, 1);
	localDrops_ = valueLabel(content, "obsDropStatistics");
	localDrops_->setTextFormat(Qt::RichText);
	localDrops_->setAlignment(Qt::AlignTop | Qt::AlignLeft);
	localColumns->addWidget(localDrops_, 1);
	localCard->addLayout(localColumns);
	localPeriod_ = valueLabel(content, "obsStatisticsPeriod");
	localCard->addWidget(localPeriod_);
	// inputCard содержит формат и скорости серверного входа.
	auto *inputCard = card(details, "inputCard", text_("Dock.InputTitle"));
	ingest_ = valueLabel(content, "ingestStatistics");
	ingest_->setTextFormat(Qt::RichText);
	inputCard->addWidget(ingest_);
	// outputHeading располагает пакетные команды сразу справа от заголовка карточки.
	QHBoxLayout *outputHeading = nullptr;
	// outputCard содержит текущие Outputs и ещё не представленные назначения выбранного Stream.
	auto *outputCard = card(details, "outputsCard", text_("Dock.OutputsTitle"), &outputHeading);
	// outputCommand создаёт только иконку icon; key задаёт локализованные подсказку и доступное имя,
	// name сохраняет адрес виджета. Родитель карточки владеет кнопкой, обработчик подключается ниже.
	const auto outputCommand = [&](const char *key, const char *name, QStyle::StandardPixmap icon) {
		// button принадлежит карточке и не выполняет команд без доступного обработчика.
		auto *button = new QToolButton(outputHeading->parentWidget());
		button->setObjectName(QString::fromLatin1(name));
		button->setIcon(style()->standardIcon(icon));
		button->setToolButtonStyle(Qt::ToolButtonIconOnly);
		button->setToolTip(text_(key));
		button->setAccessibleName(text_(key));
		button->setEnabled(false);
		outputHeading->addWidget(button);
		return button;
	};
	startOutputs_ = outputCommand("Dock.StartOutputs", "outputsStartButton", QStyle::SP_MediaPlay);
	stopOutputs_ = outputCommand("Dock.StopOutputs", "outputsStopButton", QStyle::SP_MediaStop);
	restartOutputs_ = outputCommand("Dock.RestartOutputs", "outputsRestartButton", QStyle::SP_BrowserReload);
	outputSummary_ = valueLabel(content, "outputSummary");
	outputCard->addWidget(outputSummary_);
	// outputList сохраняет строки виджетов и их положение при обновлении телеметрии.
	auto *outputList = new OutputList(content);
	outputList_ = outputList;
	outputList->setObjectName(QStringLiteral("outputList"));
	outputList->setAccessibleName(text_("Dock.OutputsTitle"));
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
	// activated передаёт только пользовательский выбор; index адресует точный itemData, а не имя Stream.
	connect(streams_, &QComboBox::activated, this, [this](int index) {
		if (connection_)
			connection_->selectStream(streams_->itemData(index).toString());
	});
	// Обработчик Pause передаёт намерение паузы точной Session без остановки OBS.
	connect(pause_, &QPushButton::clicked, this, [this] { dispatchSessionAction(OperationAction::Pause); });
	// Обработчик Resume снимает паузу исходной Session.
	connect(resume_, &QPushButton::clicked, this, [this] { dispatchSessionAction(OperationAction::Resume); });
	// Обработчик завершения эфира отдельно подтверждает остановку OBS и исходной Session.
	connect(stop_, &QPushButton::clicked, this, [this] { dispatchSessionAction(OperationAction::StopAll); });
	// Нажатие Start копирует показанный состав без дополнительного диалога.
	connect(startOutputs_, &QToolButton::clicked, this,
		[this] { dispatchOutputAction(OperationAction::StartAllOutputs); });
	// Нажатие Stop подтверждает остановку только показанных выходов.
	connect(stopOutputs_, &QToolButton::clicked, this,
		[this] { dispatchOutputAction(OperationAction::StopAllOutputs); });
	// Нажатие Restart подтверждает прерывание и запуск всех исходных строк.
	connect(restartOutputs_, &QToolButton::clicked, this,
		[this] { dispatchOutputAction(OperationAction::RestartAllOutputs); });
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
	connect(&stateStore_, &StateStore::streamsChanged, this, &RestreamDock::refreshStreams);
	refreshStreams();
	refresh();
}

RestreamDock::~RestreamDock() = default;

// Пересчитывает переносы колонок при смене ширины event; изменение высоты не запускает новый цикл.
void RestreamDock::resizeEvent(QResizeEvent *event)
{
	QWidget::resizeEvent(event);
	if (event->size().width() != event->oldSize().width())
		scheduleRefresh();
}

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

// Принимает последнюю статистику OBS, независимо обновляет накопления и форматирует обе колонки.
void RestreamDock::refresh()
{
	refreshObservations();
	// sample содержит одну согласованную выборку локальных измерений OBS.
	const auto &sample = stateStore_.obsStatistics();
	// video привязывает render/encode к исполнению видео; output — network/sent к штатному output.
	const StatsBaseline::Identity video{QStringLiteral("video"), sample.videoEpoch};
	const StatsBaseline::Identity output{QStringLiteral("output"), sample.outputEpoch};
	// stats сохраняет независимые точки отсчёта и серверный кеш этого представления.
	auto &stats = *statistics_;
	// limitedPair определяет насыщение любого счётчика пары pair, при котором точный прирост недоступен.
	const auto limitedPair = [](const std::optional<FrameCounters> &pair) {
		// limit совпадает с предельным значением нативных накопительных счётчиков.
		constexpr auto limit = std::numeric_limits<quint64>::max();
		return pair && (pair->skipped == limit || pair->total == limit);
	};
	stats.render.observe(video, frames(sample.render), true, sample.renderLimited || limitedPair(sample.render));
	stats.encode.observe(video, frames(sample.encode), true, sample.encodeLimited || limitedPair(sample.encode));
	stats.network.observe(output, frames(sample.network), true,
			      sample.networkLimited || limitedPair(sample.network));
	stats.sent.observe(output, count(sample.sentBytes), true,
			   sample.sentBytes == std::numeric_limits<quint64>::max());
	// locale задаёт десятичный разделитель и формат чисел для текущей локали OBS.
	const QLocale locale(text_("Dock.NumberLocale"));
	// noData сохраняет отсутствие наблюдения отдельно от измеренного нуля.
	const auto noData = text_("Observation.NoData");
	// fps представляет текущую локальную частоту кадров без изменения baseline.
	const auto fps = sample.fps ? locale.toString(*sample.fps, 'f', 2) : noData;
	// renderTime переводит наблюдаемое среднее время рендера из наносекунд в миллисекунды.
	const auto renderTime =
		sample.renderTimeNs
			? text_("Dock.Milliseconds")
				  .arg(locale.toString(static_cast<double>(*sample.renderTimeNs) / 1e6, 'f', 2))
			: noData;
	// bitrateStatus объясняет отсутствие скорости: замер, остановленный output либо отсутствие данных.
	const char *bitrateStatus = "Observation.NoData";
	if (sample.bitrateWarming)
		bitrateStatus = "Dock.Measuring";
	else if (sample.phase == ObsStreamingPhase::Stopped)
		bitrateStatus = "Dock.ObsNotStreaming";
	// bitRate показывает текущий локальный битрейт либо точную причину его недоступности.
	const auto bitRate = sample.bitrate ? bitrate(locale, text_, *sample.bitrate) : text_(bitrateStatus);
	// commonPeriod запоминает период доступного накопления, differentPeriods отмечает несовпадение периодов.
	std::optional<StatsBaseline::Period> commonPeriod;
	bool differentPeriods = false;
	// baseline проходит четыре независимых накопления OBS; недоступное чтение не задаёт общий период.
	for (const auto *baseline : {&stats.render, &stats.encode, &stats.network, &stats.sent}) {
		// reading содержит уже вычисленную дельту и период одной точки отсчёта.
		const auto reading = baseline->reading();
		if (!reading.counters)
			continue;
		if (commonPeriod && *commonPeriod != reading.period)
			differentPeriods = true;
		commonPeriod = reading.period;
	}
	// perCounterPeriod оставляет собственный период у каждого значения при расхождении источников.
	const bool perCounterPeriod = differentPeriods;
	// stackedStatistics оставляет подписи и значения на отдельных строках в двух узких колонках Dock.
	const bool stackedStatistics = width() < 600;
	// localRows содержит только текущие измерения; dropRows — накопления с прежним форматированием.
	const auto localRows = tableRow(text_("Dock.FpsLabel"), fps, true, stackedStatistics) +
			       tableRow(text_("Dock.BitrateLabel"), bitRate, true, stackedStatistics) +
			       tableRow(text_("Dock.RenderLabel"), renderTime, false, stackedStatistics);
	const auto dropRows =
		tableRow(text_("Dock.RenderDropsLabel"), counterText(locale, text_, stats.render, perCounterPeriod),
			 false, stackedStatistics) +
		tableRow(text_("Dock.EncodeDropsLabel"), counterText(locale, text_, stats.encode, perCounterPeriod),
			 false, stackedStatistics) +
		tableRow(text_("Dock.NetworkDropsLabel"), counterText(locale, text_, stats.network, perCounterPeriod),
			 false, stackedStatistics) +
		tableRow(text_("Dock.SentLabel"), bytesText(locale, text_, stats.sent, perCounterPeriod), false,
			 stackedStatistics);
	localStats_->setText(table(localRows));
	localDrops_->setText(table(dropRows));
	localPeriod_->setText(commonPeriod && !differentPeriods ? text_(periodKey(*commonPeriod)) : QString{});
	localPeriod_->setVisible(!localPeriod_->text().isEmpty());
	localStats_->setToolTip(text_("Dock.CounterHint"));
	localDrops_->setToolTip(text_("Dock.CounterHint"));
	refreshServer();
	// serverCanReset разрешает локальный сброс при наличии пригодного накопления хотя бы одного Output.
	// Предикат entry проверяет только доступность baseline, не выполняя команд или изменения счётчиков.
	const bool serverCanReset = std::any_of(stats.outputs.cbegin(), stats.outputs.cend(),
						[](const auto &entry) { return entry.canReset(); });
	reset_->setEnabled(stats.render.canReset() || stats.encode.canReset() || stats.network.canReset() ||
			   stats.sent.canReset() || serverCanReset);
}

// Пересоздаёт варианты только после изменения списка или выбранного ID, не сохраняя отражение обратно.
void RestreamDock::refreshStreams()
{
	// blockStreams исключает сигналы выбора при отражении; запись разрешена только из activated.
	const QSignalBlocker blockStreams(streams_);
	streams_->clear();
	streams_->addItem(text_("Stream.Choose"), QString{});
	// stream содержит полученное имя для показа и точный ID для адресации выбора.
	for (const auto &stream : stateStore_.streams())
		streams_->addItem(stream.name, stream.id);
	displayedSelectionId_ = stateStore_.selectedStreamId();
	// selected адресует сохранённый ID; синтетический пункт не подтверждает его наличие на сервере.
	int selected = streams_->findData(displayedSelectionId_);
	if (selected < 0 && !displayedSelectionId_.isEmpty()) {
		// Неполный список не доказывает удаления: привязка сохраняется до явного выбора пользователя.
		streams_->addItem(text_(stateStore_.streamsComplete() ? "Stream.Unavailable" : "Stream.Unconfirmed")
					  .arg(displayedSelectionId_),
				  displayedSelectionId_);
		selected = streams_->count() - 1;
	}
	streams_->setCurrentIndex(selected < 0 ? 0 : selected);
}

// Обновляет подключение, разрешение выбора и безопасные сообщения; телеметрия не пересоздаёт список.
void RestreamDock::refreshObservations()
{
	// connection содержит нынешнее состояние общего канала наблюдения.
	const auto connection = stateStore_.connectionStatus();
	// observedConnection оставляет отсутствие сервера отдельным от состояния отключения.
	const auto observedConnection = stateStore_.serverOrigin().isEmpty() ? std::nullopt : std::optional{connection};
	server_->setConnectionStatus(text_, observedConnection);
	if (displayedSelectionId_ != stateStore_.selectedStreamId())
		refreshStreams();
	// streamsComplete различает полное чтение и полученную часть конфигурационного списка.
	const bool streamsComplete = stateStore_.streamsComplete();
	// canChooseStream сохраняет доступность полученных вариантов до подтверждения SSE при неполном списке.
	const bool canChooseStream = streamsComplete ? connection == ConnectionStatus::Connected
						     : !stateStore_.streams().isEmpty();
	streams_->setEnabled(connection_ && stateStore_.profileResolved() && canChooseStream);
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
	if (!streamsComplete &&
	    stateStore_.configuration().streamsStatus.availability != RuntimeAvailability::Unknown) {
		notices.append(text_("Stream.ListIncomplete"));
	}
	if (!stateStore_.profileNoticeKey().isEmpty())
		notices.append(text_(stateStore_.profileNoticeKey().toUtf8().constData()));
	if (stateStore_.settingsUnsaved())
		notices.append(text_("Settings.Unsaved"));
	notice_->setText(notices.join(QLatin1Char('\n')));
	notice_->setVisible(!notices.isEmpty());
	// statistics даёт подтверждённую фазу штатной трансляции независимо от сервера.
	const auto &statistics = stateStore_.obsStatistics();
	if (statistics.observed)
		streaming_->setStreamingPhase(text_, statistics.phase);
	else
		streaming_->setStreamingStatus(text_, stateStore_.streamingActive());
	refreshOperations();
}

// Отражает независимые серверные наблюдения и объединяет Outputs с безопасным каталогом назначений.
// Сохраняет кеш только внутри прежнего контекста и исполнения; неполные сведения не становятся сводкой N/M.
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
	// Полное свежее отсутствие Session отзывает прежнее исполнение и его строки.
	// Неизвестный или устаревший поиск сохраняет кеш; каталог назначений хранится отдельно.
	if (connected && !server.session && fresh(server.sessionStatus.availability) && server.sessionStatus.complete &&
	    !stats.sessionId.isEmpty()) {
		stats.sessionId.clear();
		stats.clearServer();
	}
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
	// ingestMetrics измеряет подписи и значения тем же шрифтом, которым их рисует QLabel.
	const auto ingestMetrics = ingest_->fontMetrics();
	// ingestColumnWidth учитывает width=33% ячейки и cellpadding=2 с обеих сторон.
	// До первой компоновки малая ширина разрешает перенос между всеми графемами; resize обновит текст.
	const int ingestColumnWidth = qMax(1, (ingest_->contentsRect().width() - 2 * ingest_->margin()) * 33 / 100 - 4);
	ingest_->setText(
		table(QStringLiteral("<tr>%1%2%3</tr>")
			      .arg(statisticColumn(text_("Dock.FormatLabel"),
						   inputFormat.isEmpty() ? noData : inputFormat.join(QLatin1Char(' ')),
						   ingestMetrics, ingestColumnWidth),
				   statisticColumn(text_("Dock.FpsLabel"), ingestFps, ingestMetrics, ingestColumnWidth),
				   statisticColumn(text_("Dock.BitrateLabel"), ingestBitrate, ingestMetrics,
						   ingestColumnWidth))));
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
		for (auto it = stats.outputs.cbegin(); it != stats.outputs.cend(); ++it) {
			// metadata не позволяет вернуть из кеша явно отсоединённый Output при потере runtime.
			const auto metadata = server.outputs.constFind(it.key());
			if (metadata == server.outputs.cend() || metadata->attached)
				rows.insert(it.key());
		}
	}
	// it проходит текущую карту выходов; её ключи остаются точными ID при объединении и удалении строк.
	for (auto it = stats.outputs.begin(); it != stats.outputs.end();) {
		if (!rows.contains(it.key()))
			it = stats.outputs.erase(it);
		else
			++it;
	}
	// representedDestinations содержит только точные связи прикреплённых REST Outputs с каталогом.
	// Runtime без такой связи остаётся отдельной строкой, даже если его имя совпадает с назначением.
	QSet<QString> representedDestinations;
	// output проходит долговечные метаданные; отсоединённый Output больше не представляет назначение.
	for (const auto &output : server.outputs) {
		if (output.attached && !output.destinationId.isEmpty())
			representedDestinations.insert(output.destinationId);
	}
	// outputRows содержит доступное текстовое описание строк для модели и средств доступности.
	QStringList outputRows;
	// RowValues хранит отображение и раздельные IDs Output/назначения; имя не адресует команды.
	struct RowValues {
		// id адресует точный Output; каталоговая строка не выдаёт Destination ID за Output ID.
		QString id;
		// destinationId сохраняет точную связь с каталогом; runtime без метаданных её не выдумывает.
		QString destinationId;
		// name содержит безопасное отображаемое имя без влияния на адресацию команды.
		QString name;
		// status содержит переведённое runtime-состояние выхода или отсутствие данных.
		QString status;
		// rate содержит оформленный текущий или явно устаревший битрейт выхода.
		QString rate;
		// tone задаёт цвет только наблюдаемого состояния выхода.
		Tone tone;
	};
	// rowValues содержит значения виджетов в том же порядке, что outputRows.
	QVector<RowValues> rowValues;
	// streaming считает только свежие наблюдения PUBLISHING, независимо от квитанций.
	int streaming = 0;
	// complete разрешает сводку количества только при полном составе и известных состояниях.
	bool complete = sessionCurrent && server.sessionStatus.complete && fresh(server.outputsStatus.availability) &&
			server.outputsStatus.complete && fresh(server.destinationsStatus.availability) &&
			server.destinationsStatus.complete;
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
		// last форматирует baseline; обработчик помечает retained, когда показывает прежнее измерение.
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
		const auto name = cache.name.isEmpty() ? text_("Dock.UnnamedOutput") : cache.name;
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
		rowValues.append({id, metadata != server.outputs.cend() ? metadata->destinationId : QString{}, name,
				  label, currentBitrate, tone});
	}
	// destination проходит безопасный каталог выбранного Stream, включая назначения будущего эфира.
	for (auto destination = server.destinations.cbegin(); destination != server.destinations.cend();
	     ++destination) {
		if (representedDestinations.contains(destination.key()))
			continue;
		// name сохраняет полное имя назначения для подсказки и средств доступности.
		const auto name = destination->name.isEmpty() ? text_("Dock.UnnamedOutput") : destination->name;
		outputRows.append(text_("Dock.OutputRow").arg(name, noData, noData, noData, noData, noData, noData));
		rowValues.append({{}, destination.key(), name, noData, noData, Tone::Neutral});
		complete = false;
	}
	if (outputRows.isEmpty())
		outputRows.append(noData);
	updateRows(*outputs_, outputRows);
	// shown фиксирует адресацию вместе с установленной моделью, включая строки за прокруткой.
	OutputBatchRequest shown;
	shown.context = context;
	// sourceSessionId сохраняет также завершённую Session, не адресуя ею создание нового эфира.
	const auto sourceSessionId = server.session ? server.session->id : QString{};
	shown.noActiveSession = connected && fresh(server.sessionStatus.availability) &&
				server.sessionStatus.complete &&
				(!server.session || server.session->state == QStringLiteral("STOPPED") ||
				 server.session->state == QStringLiteral("FAILED") ||
				 server.session->state == QStringLiteral("INTERRUPTED"));
	shown.sessionId = shown.noActiveSession ? QString{} : sourceSessionId;
	shown.configurationGeneration = stateStore_.configurationGeneration();
	// values переносит только адресацию и имя показанной строки, без секретной конфигурации.
	for (const auto &values : rowValues)
		shown.targets.append({values.id, values.destinationId, values.name});
	shownOutputs_ = std::move(shown);
	shownSessionId_ = sourceSessionId;
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
			// Повторное использование indexWidget не оставляет в обработчике ID прежнего выхода.
			connect(widget->action, &QToolButton::clicked, this, [this, widget] {
				outputActions(widget->context, widget->sessionId, widget->outputId,
					      widget->destinationId, widget->action);
			});
		}
		// values содержит значения именно нынешней строки, включая точный ID.
		const auto &values = rowValues.at(row);
		widget->outputId = values.id;
		widget->destinationId = values.destinationId;
		widget->context = context;
		widget->sessionId = server.session ? server.session->id : QString{};
		// metadata даёт доступ к прежним Output-командам; известный Destination отдельно разрешает Details.
		const auto metadata = server.outputs.constFind(values.id);
		widget->action->setEnabled(
			context.valid() &&
			(!values.destinationId.isEmpty() ||
			 (restream_ && server.session && metadata != server.outputs.cend() && metadata->attached)));
		widget->name->setText(values.name);
		widget->status->setStatus(values.status, values.tone);
		widget->rate->setText(values.rate);
		widget->name->setToolTip(outputToolTip(outputRows.at(row)));
		widget->name->setAccessibleName(values.name);
		widget->status->setToolTip(outputToolTip(outputRows.at(row)));
		widget->status->setAccessibleName(values.status);
		widget->rate->setToolTip(outputToolTip(outputRows.at(row)));
		widget->rate->setAccessibleName(values.rate);
		widget->setToolTip(outputToolTip(outputRows.at(row)));
		widget->setAccessibleName(values.name);
		widget->setAccessibleDescription(outputRows.at(row));
		widget->action->setAccessibleDescription(outputRows.at(row));
	}
	static_cast<OutputList *>(outputList_)->updateHeights();
	outputSummary_->setText(
		complete && !rows.isEmpty()
			? text_("Dock.OutputCount").arg(locale.toString(streaming), locale.toString(rows.size()))
			: QString{});
	outputSummary_->setVisible(!outputSummary_->text().isEmpty());
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
	// sessionReadComplete требует полного свежего чтения для действующего контекста и подключения.
	const bool sessionReadComplete = stateStore_.connectionStatus() == ConnectionStatus::Connected &&
					 stateStore_.dockContext().valid() &&
					 fresh(server.sessionStatus.availability) && server.sessionStatus.complete;
	// knownNoActive требует полного свежего чтения: неизвестная или частичная выборка не доказывает отсутствие эфира.
	const bool knownNoActive = sessionReadComplete &&
				   (!server.session || server.session->state == QStringLiteral("STOPPED") ||
				    server.session->state == QStringLiteral("FAILED") ||
				    server.session->state == QStringLiteral("INTERRUPTED"));
	// shownCurrent связывает доступность с уже показанной проекцией, а не новым составом каталога.
	const bool shownCurrent = shownOutputs_.context == stateStore_.dockContext() &&
				  shownSessionId_ == (server.session ? server.session->id : QString{}) &&
				  shownOutputs_.noActiveSession == knownNoActive;
	// batchAvailable исключает переходную/неизвестную Session и несовместимый workflow.
	// Проверки отдельных строк и эпохи новых снимков остаются у контроллера: отказ каталога
	// не должен блокировать исторические прикреплённые выходы всего показанного пакета.
	const bool batchAvailable = restream_ && !busy && shownCurrent && sessionReadComplete &&
				    !shownOutputs_.targets.isEmpty() && shownOutputs_.targets.size() <= 10000 &&
				    (running || knownNoActive);
	startOutputs_->setEnabled(batchAvailable);
	stopOutputs_->setEnabled(batchAvailable);
	restartOutputs_->setEnabled(batchAvailable);
	// operationText совмещает штатный результат и отдельный риск следующей публикации.
	QString operationText = formatOperations(operations, text_);
	if (knownNoActive && obsActive)
		operationText.append(operationText.isEmpty() ? QString{} : QStringLiteral("\n\n"))
			.append(text_("Operation.ObsStillActive"));
	operationStatus_->setText(operationText);
	operationStatus_->setToolTip(outputToolTip(operationText));
	operationStatus_->setAccessibleDescription(operationText);
	operationStatus_->setVisible(!operationStatus_->text().isEmpty());
	// Подсказка штатного запуска нужна лишь при отсутствии серверного эфира и активной фазы OBS.
	nativeStart_->setVisible(knownNoActive && !obsActive);
	// retryHint указывает выбранную контроллером первую Unknown-цель, не меняя её по имени.
	const auto retryHint = operations.current && !operations.current->retryTargetName.isEmpty()
				       ? text_("Operation.RetryTarget").arg(operations.current->retryTargetName)
				       : text_("Operation.Retry");
	retry_->setToolTip(outputToolTip(retryHint));
	retry_->setAccessibleDescription(retryHint);
	retry_->setVisible(operations.current && operations.current->retryAvailable);
	retry_->setEnabled(restream_ && operations.current && operations.current->retryAvailable &&
			   operations.current->context == stateStore_.dockContext());
}

// Передаёт action Pause/Resume/StopAll исходной Session. Завершение эфира требует подтверждения;
// состав выходов здесь не выбирается.
void RestreamDock::dispatchSessionAction(OperationAction action)
{
	if (!restream_ || !stateStore_.dockServer().session)
		return;
	// context и sessionId сохраняют цель до вложенного event loop диалога.
	const auto context = stateStore_.dockContext();
	const auto sessionId = stateStore_.dockServer().session->id;
	if (action == OperationAction::StopAll) {
		// alive запрещает доступ к доку после завершения OBS во вложенном цикле диалога.
		const QPointer<RestreamDock> alive(this);
		// confirmation принадлежит доку; QPointer замечает удаление вместе с родителем.
		const QPointer<QMessageBox> confirmation =
			new QMessageBox(QMessageBox::Question, text_("Operation.ConfirmTitle"),
					text_("Operation.ConfirmStopAll"), QMessageBox::Yes | QMessageBox::No, this);
		confirmation->setDefaultButton(QMessageBox::No);
		localizeConfirmation(*confirmation, text_);
		// choice относится только к исходной цели, которую контроллер проверяет повторно.
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
	default:
		break;
	}
}

// Передаёт один пакет ровно показанных строк. action задаёт Start/Stop/Restart выходов,
// а request копируется до диалога: обновление модели во вложенном цикле не добавляет новых целей.
// Контроллер повторно проверяет исходные IDs, Session, режим и эпоху; UI не выбирает им замену.
void RestreamDock::dispatchOutputAction(OperationAction action)
{
	if (!restream_ || (action != OperationAction::StartAllOutputs && action != OperationAction::StopAllOutputs &&
			   action != OperationAction::RestartAllOutputs))
		return;
	// request сохраняет безопасную value-проекцию вместе с порядком и именами исходных строк.
	const auto request = shownOutputs_;
	// sourceSessionId сохраняет идентичность показанного списка, в том числе завершённого эфира.
	const auto sourceSessionId = shownSessionId_;
	if (!request.context.valid() || request.context != stateStore_.dockContext() || request.targets.isEmpty())
		return;
	if (action == OperationAction::StopAllOutputs || action == OperationAction::RestartAllOutputs) {
		// alive замечает удаление Dock во вложенном цикле подтверждения.
		const QPointer<RestreamDock> alive(this);
		// confirmation подтверждает только пакет выходов; Session и OBS этим действием не завершаются.
		const QPointer<QMessageBox> confirmation = new QMessageBox(
			QMessageBox::Question, text_("Operation.ConfirmTitle"),
			text_(action == OperationAction::StopAllOutputs ? "Operation.ConfirmStopOutputs"
									: "Operation.ConfirmRestartAll"),
			QMessageBox::Yes | QMessageBox::No, this);
		confirmation->setTextFormat(Qt::PlainText);
		confirmation->setDefaultButton(QMessageBox::No);
		localizeConfirmation(*confirmation, text_);
		// choice фиксируется до удаления диалога, затем проверяются Dock и исходный context.
		const auto choice = confirmation->exec();
		if (confirmation)
			delete confirmation;
		if (!alive || choice != QMessageBox::Yes)
			return;
	}
	if (stateStore_.dockContext() != request.context ||
	    (stateStore_.dockServer().session ? stateStore_.dockServer().session->id : QString{}) != sourceSessionId)
		return;
	restream_->controlOutputs(request, action);
}

// Фиксирует ветвь запуска и раздельные IDs до меню. После вложенных циклов исходные context,
// Session и цель проверяются снова: появившийся эфир не превращает создание в add-output.
// anchor задаёт положение меню; неизвестная runtime-строка без связи с каталогом не адресует команды.
void RestreamDock::outputActions(DockContext context, QString sessionId, QString outputId, QString destinationId,
				 QToolButton *anchor)
{
	if (!anchor || !context.valid() || context != stateStore_.dockContext())
		return;
	// initial содержит долговечные наблюдения только до входа во вложенный цикл меню.
	const auto &initial = stateStore_.dockServer();
	// sameSession разрешает команды только для исходной Session; просмотр Destination от неё не зависит.
	const bool sameSession = (initial.session ? initial.session->id : QString{}) == sessionId;
	// found связывает точный Output с его историческим Destination, не используя имя строки.
	const auto found = initial.outputs.constFind(outputId);
	// attached фиксирует наличие исходного прикреплённого Output независимо от его runtime.
	const bool attached = sameSession && !outputId.isEmpty() && initial.session &&
			      found != initial.outputs.cend() && found->attached;
	// metadata сохраняет имя, назначение и намерение исходного Output до обновлений состояния.
	const auto metadata = attached ? *found : DockOutput{};
	// sameDestination запрещает команды изменившейся связи Output, но не просмотр исходного Destination.
	const bool sameDestination = !attached || metadata.destinationId == destinationId;
	if (!attached && destinationId.isEmpty())
		return;
	// StartMode отделяет три серверных действия. Выбранная ветвь не меняется после меню.
	enum class StartMode {
		// Unavailable запрещает запуск в переходном либо неизвестном состоянии Session.
		Unavailable,
		// AttachedOutput запускает прежний остановленный Output по его историческому снимку.
		AttachedOutput,
		// AddToSession прикрепляет назначение к исходной RUNNING Session с новым снимком.
		AddToSession,
		// CreateSession создаёт серверную Session только для выбранного назначения.
		CreateSession
	};
	// mode вычисляется обработчиком один раз до меню по исходной Session. Терминальный Output
	// не адресуется командой запуска: новое исполнение может использовать только Destination ID.
	const auto mode = [&] {
		if (!sameSession || !sameDestination)
			return StartMode::Unavailable;
		if (initial.session && initial.session->state == QStringLiteral("RUNNING"))
			return attached ? StartMode::AttachedOutput : StartMode::AddToSession;
		if ((!initial.session || initial.session->state == QStringLiteral("STOPPED") ||
		     initial.session->state == QStringLiteral("FAILED") ||
		     initial.session->state == QStringLiteral("INTERRUPTED")) &&
		    !destinationId.isEmpty())
			return StartMode::CreateSession;
		return StartMode::Unavailable;
	}();
	// canCommand проверяет исходные Output/Session по полным свежим данным. intent при start/stop
	// дополнительно сверяет desiredRunning; отсутствие runtime не мешает сохранённому намерению.
	const auto canCommand = [&](std::optional<OperationAction> intent = std::nullopt) {
		// server содержит нынешнюю Session и полный долговечный состав, а не снимок до меню.
		const auto &server = stateStore_.dockServer();
		// operations и busy отделяют возможность новой команды от просмотра подробностей.
		const auto &operations = stateStore_.operations();
		const bool busy = operations.current && operations.current->phase != OperationPhase::Idle &&
				  operations.current->phase != OperationPhase::Finished;
		// output заново находит исходный Output ID, даже если виджет строки уже использован для другого.
		const auto output = server.outputs.constFind(outputId);
		return attached && sameDestination && restream_ && !busy && stateStore_.dockContext() == context &&
		       stateStore_.connectionStatus() == ConnectionStatus::Connected && server.session &&
		       server.session->id == sessionId && server.session->state == QStringLiteral("RUNNING") &&
		       fresh(server.sessionStatus.availability) && server.sessionStatus.complete &&
		       fresh(server.outputsStatus.availability) && server.outputsStatus.complete &&
		       output != server.outputs.cend() && output->attached && output->destinationId == destinationId &&
		       (!intent || ((*intent != OperationAction::StartOutput || !output->desiredRunning) &&
				    (*intent != OperationAction::StopOutput || output->desiredRunning)));
	};
	// canStart повторяет допуск только зафиксированной ветви. Каталог нужен для нового снимка,
	// но запуск attached Output использует его старый снимок и не зависит от нынешнего Destination.
	const auto canStart = [&] {
		if (mode == StartMode::AttachedOutput)
			return canCommand(OperationAction::StartOutput);
		if (mode == StartMode::Unavailable)
			return false;
		// server предоставляет свежесть каталога и исходной Session при проверке после меню.
		const auto &server = stateStore_.dockServer();
		// operations и busy запрещают начать ещё один workflow, пока прежний не завершён.
		const auto &operations = stateStore_.operations();
		const bool busy = operations.current && operations.current->phase != OperationPhase::Idle &&
				  operations.current->phase != OperationPhase::Finished;
		if (!restream_ || busy || stateStore_.dockContext() != context ||
		    stateStore_.connectionStatus() != ConnectionStatus::Connected ||
		    !fresh(server.sessionStatus.availability) || !server.sessionStatus.complete ||
		    !fresh(server.destinationsStatus.availability) || !server.destinationsStatus.complete ||
		    destinationId.isEmpty() || !server.destinations.contains(destinationId) ||
		    (server.session ? server.session->id : QString{}) != sessionId)
			return false;
		if (mode == StartMode::CreateSession)
			return !server.session || server.session->state == QStringLiteral("STOPPED") ||
			       server.session->state == QStringLiteral("FAILED") ||
			       server.session->state == QStringLiteral("INTERRUPTED");
		if (!server.session || server.session->state != QStringLiteral("RUNNING") ||
		    !fresh(server.outputsStatus.availability) || !server.outputsStatus.complete)
			return false;
		// output проверяет отсутствие уже прикреплённого назначения; detached история не мешает add.
		for (const auto &output : server.outputs) {
			if (output.attached && output.destinationId == destinationId)
				return false;
		}
		return true;
	};
	// allowed задаёт начальную доступность прежних Output-команд; перед отправкой допуск повторяется.
	const bool allowed = canCommand();
	// alive замечает удаление Dock во вложенных циклах самого меню и подтверждения.
	const QPointer<RestreamDock> alive(this);
	// position сохраняет положение кнопки до возможного обновления или удаления строки.
	const auto position = anchor->mapToGlobal(QPoint(0, anchor->height()));
	// menu принадлежит Dock; QPointer запрещает доступ после удаления вместе с родителем.
	const QPointer<QMenu> menu = new QMenu(this);
	// start предлагает фиксированную ветвь выбранного назначения или остановленного Output.
	auto *start = menu->addAction(text_("Operation.StartOutput"));
	// stop останавливает только исходный прикреплённый Output с сохранённым намерением запуска.
	auto *stop = menu->addAction(text_("Operation.StopOutput"));
	// restart сохраняет прежнюю последовательность stop/start того же Output с подтверждением.
	auto *restart = menu->addAction(text_("Operation.RestartOutput"));
	// details открывает сохранённый Destination даже без Session или прикреплённого Output.
	auto *details = menu->addAction(text_("Operation.OutputDetails"));
	start->setEnabled(canStart());
	stop->setEnabled(allowed && metadata.desiredRunning);
	restart->setEnabled(allowed);
	details->setEnabled(!destinationId.isEmpty());
	// selected заимствован у меню; после его вложенного цикла сначала проверяются владельцы.
	const auto *selected = menu->exec(position);
	if (!alive || !menu)
		return;
	// openDetails сохраняет выбор до удаления menu и принадлежащих ему QAction.
	const bool openDetails = selected == details;
	// action сохраняет значение намерения; указатели действий после удаления menu не используются.
	const auto action = selected == start  ? OperationAction::StartOutput
			    : selected == stop ? OperationAction::StopOutput
					       : OperationAction::RestartOutput;
	// chosen отличает отменённое меню от явно выбранного действия.
	const bool chosen = selected != nullptr;
	delete menu;
	if (!chosen)
		return;
	if (openDetails) {
		if (stateStore_.dockContext() == context && !destinationId.isEmpty())
			emit destinationDetailsRequested(context, destinationId);
		return;
	}
	if (action == OperationAction::StartOutput) {
		if (!canStart())
			return;
		if (mode == StartMode::CreateSession)
			restream_->startDestination(context, destinationId);
		else if (mode == StartMode::AddToSession)
			restream_->addOutput(context, sessionId, destinationId);
		else if (mode == StartMode::AttachedOutput)
			restream_->outputActions(context, sessionId, outputId, action);
		return;
	}
	if (!canCommand(action))
		return;
	if (action == OperationAction::RestartOutput) {
		// name показывает имя исходного снимка, поэтому переименование не меняет цель подтверждения.
		const auto name = metadata.name.isEmpty() ? text_("Dock.UnnamedOutput") : metadata.name;
		// confirmation принадлежит Dock и сохраняет прежние переведённые ответы и безопасный ответ по умолчанию.
		const QPointer<QMessageBox> confirmation = new QMessageBox(QMessageBox::Question,
									   text_("Operation.ConfirmTitle"),
									   text_("Operation.ConfirmRestart").arg(name),
									   QMessageBox::Yes | QMessageBox::No, this);
		confirmation->setTextFormat(Qt::PlainText);
		confirmation->setDefaultButton(QMessageBox::No);
		localizeConfirmation(*confirmation, text_);
		// choice фиксирует ответ до освобождения диалога; alive проверяется до следующего доступа к Dock.
		const auto choice = confirmation->exec();
		if (confirmation)
			delete confirmation;
		if (!alive || choice != QMessageBox::Yes)
			return;
	}
	if (!canCommand(action))
		return;
	restream_->outputActions(context, sessionId, outputId, action);
}

} // namespace gost
