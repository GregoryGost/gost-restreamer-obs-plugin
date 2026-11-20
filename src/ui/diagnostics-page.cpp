#include "diagnostics-page.hpp"

#include "status-label.hpp"

#include <QComboBox>
#include <QDateTime>
#include <QFormLayout>
#include <QHeaderView>
#include <QKeyEvent>
#include <QLabel>
#include <QPointer>
#include <QScrollArea>
#include <QScrollBar>
#include <QSet>
#include <QSignalBlocker>
#include <QStringList>
#include <QTimer>
#include <QTreeWidget>
#include <QVBoxLayout>

#include <utility>

namespace gost {
namespace {

// Дополняет штатную навигацию дерева прокруткой внешней страницы до текущей строки.
class DiagnosticsTreeWidget final : public QTreeWidget {
public:
	// Передаёт владение деревом странице, в которой расположена его секция.
	explicit DiagnosticsTreeWidget(QWidget *parent) : QTreeWidget(parent) {}

protected:
	// Сначала оставляет Qt выбор и внутреннюю прокрутку, затем показывает выбранную строку на странице.
	void keyPressEvent(QKeyEvent *event) override
	{
		// key сохраняет исходную клавишу до штатной обработки события.
		const int key = event->key();
		// guard позволяет не обращаться к дереву, если обработчик сигнала удалит его во время базового вызова.
		const QPointer<DiagnosticsTreeWidget> guard(this);
		QTreeWidget::keyPressEvent(event);
		if (guard && guard->hasFocus() && isNavigationKey(key))
			guard->keepCurrentRowVisible();
	}

private:
	// Проверяет клавиши, которыми пользователь перемещает текущую строку дерева.
	static bool isNavigationKey(int key)
	{
		switch (key) {
		case Qt::Key_Up:
		case Qt::Key_Down:
		case Qt::Key_Left:
		case Qt::Key_Right:
		case Qt::Key_Home:
		case Qt::Key_End:
		case Qt::Key_PageUp:
		case Qt::Key_PageDown:
			return true;
		default:
			return false;
		}
	}

	// Сдвигает внешнюю полосу только тогда, когда текущая строка частично вышла из её viewport.
	void keepCurrentRowVisible()
	{
		// item остаётся выбранным после штатной обработки клавиши и принадлежит этому дереву.
		auto *item = currentItem();
		if (!item)
			return;
		// ancestor и scrollArea находят внешнюю область, содержащую всю страницу диагностики.
		QScrollArea *scrollArea = nullptr;
		for (QWidget *ancestor = parentWidget(); ancestor && !scrollArea; ancestor = ancestor->parentWidget())
			scrollArea = qobject_cast<QScrollArea *>(ancestor);
		if (!scrollArea)
			return;
		// rowRect задаёт строку в координатах внутреннего viewport дерева.
		const QRect rowRect = visualItemRect(item);
		if (!rowRect.isValid())
			return;
		// rowInScrollViewport переносит строку в координаты видимой части внешней страницы.
		const QRect rowInScrollViewport(viewport()->mapTo(scrollArea->viewport(), rowRect.topLeft()),
						rowRect.size());
		// visibleRect задаёт границы страницы, доступные без внешней прокрутки.
		const QRect visibleRect = scrollArea->viewport()->rect();
		// scrollBar изменяет положение страницы, не затрагивая внутренний выбор дерева.
		auto *scrollBar = scrollArea->verticalScrollBar();
		// adjustment переносит только ту часть строки, которая сейчас скрыта за верхней или нижней границей.
		int adjustment = 0;
		if (rowInScrollViewport.top() < visibleRect.top())
			adjustment = rowInScrollViewport.top() - visibleRect.top();
		else if (rowInScrollViewport.bottom() > visibleRect.bottom())
			adjustment = rowInScrollViewport.bottom() - visibleRect.bottom();
		if (adjustment != 0)
			scrollBar->setValue(scrollBar->value() + adjustment);
	}
};

// Сохраняет навигацию read-only дерева при замене значений: прокрутку, свёрнутые узлы и текущую строку.
class TreePosition final {
public:
	// Копирует состояние tree до очистки; виджет должен пережить этот синхронный объект.
	explicit TreePosition(QTreeWidget &tree)
		: tree_(tree),
		  vertical_(tree.verticalScrollBar()->value()),
		  horizontal_(tree.horizontalScrollBar()->value())
	{
		capture(nullptr, {});
	}
	// Восстанавливает навигацию по смысловому пути строк, затем возвращает позицию полос прокрутки.
	~TreePosition()
	{
		restore(nullptr, {});
		tree_.verticalScrollBar()->setValue(vertical_);
		tree_.horizontalScrollBar()->setValue(horizontal_);
	}

private:
	// Сохраняет свёрнутые узлы и текущий элемент потомков parent с исходным путём path.
	void capture(QTreeWidgetItem *parent, const QString &path)
	{
		// count и index задают обход только существующих прямых потомков parent.
		const int count = parent ? parent->childCount() : tree_.topLevelItemCount();
		for (int index = 0; index < count; ++index) {
			// item и next обозначают текущую строку и её локальный путь с позицией среди соседей.
			auto *item = parent ? parent->child(index) : tree_.topLevelItem(index);
			const auto next = path + QStringLiteral("/%1:%2").arg(index).arg(item->text(0));
			if (!item->isExpanded())
				collapsed_.insert(next);
			if (item == tree_.currentItem())
				current_ = next;
			capture(item, next);
		}
	}
	// Применяет сохранённое состояние только существующим строкам потомков parent по пути path.
	void restore(QTreeWidgetItem *parent, const QString &path)
	{
		// count и index задают обход нового дерева без удержания прежних указателей на элементы.
		const int count = parent ? parent->childCount() : tree_.topLevelItemCount();
		for (int index = 0; index < count; ++index) {
			// item и next обозначают новый элемент и путь, сопоставимый с исходной строкой.
			auto *item = parent ? parent->child(index) : tree_.topLevelItem(index);
			const auto next = path + QStringLiteral("/%1:%2").arg(index).arg(item->text(0));
			if (collapsed_.contains(next))
				item->setExpanded(false);
			if (current_ == next)
				tree_.setCurrentItem(item);
			restore(item, next);
		}
	}
	// tree_ заимствован до конца синхронного refresh, без вложенного event loop.
	QTreeWidget &tree_;
	// vertical_ и horizontal_ сохраняют пользовательское положение полос прокрутки.
	int vertical_;
	int horizontal_;
	// collapsed_ и current_ адресуют прежние узлы без ссылок на удаляемые QTreeWidgetItem.
	QSet<QString> collapsed_;
	QString current_;
};

} // namespace

// Создаёт независимые секции наблюдений и связывает обновления с временем жизни страницы.
DiagnosticsPage::DiagnosticsPage(StateStore &stateStore, TextLookup text, QWidget *parent)
	: QWidget(parent),
	  stateStore_(stateStore),
	  text_(std::move(text))
{
	setObjectName(QStringLiteral("diagnosticsPage"));
	// layout располагает секции внутри внешней прокрутки ManagementWindow.
	auto *layout = new QVBoxLayout(this);
	// summary выравнивает профиль, OBS, API и адресацию текущего эфира.
	auto *summary = new QFormLayout;
	summary->setRowWrapPolicy(QFormLayout::WrapLongRows);
	// label создаёт переносимый PlainText для name с доступным названием key; Qt владеет результатом.
	const auto label = [&](const char *name, const char *key) {
		// value отображает одно значение без интерпретации разметки.
		auto *value = new QLabel(this);
		value->setObjectName(QString::fromLatin1(name));
		value->setTextFormat(Qt::PlainText);
		value->setTextInteractionFlags(Qt::TextSelectableByMouse | Qt::TextSelectableByKeyboard);
		value->setWordWrap(true);
		value->setAccessibleName(text_(key));
		return value;
	};
	profile_ = label("obsProfile", "OBS.Profile");
	streaming_ = new StatusLabel(this, false);
	streaming_->setObjectName(QStringLiteral("obsStreaming"));
	connection_ = new StatusLabel(this, false);
	connection_->setObjectName(QStringLiteral("diagnosticsConnection"));
	contextLabel_ = label("diagnosticsContext", "Stream.Label");
	sessionLabel_ = label("diagnosticsSession", "Diagnostics.Session");
	summary->addRow(text_("OBS.Profile"), profile_);
	summary->addRow(text_("OBS.Streaming"), streaming_);
	summary->addRow(text_("Settings.ConnectionStatus"), connection_);
	summary->addRow(text_("Stream.Label"), contextLabel_);
	summary->addRow(text_("Diagnostics.Session"), sessionLabel_);
	layout->addLayout(summary);
	obs_ = makeSection(*layout, "Diagnostics.OBS", "diagnosticsOBS");
	input_ = makeSection(*layout, "Diagnostics.Input", "diagnosticsInput");
	groups_ = makeSection(*layout, "Diagnostics.Groups", "diagnosticsGroups");
	// outputs_ использует только текущие runtime-выходы точной Session, без каталога конфигурации.
	outputs_ = new QComboBox(this);
	outputs_->setObjectName(QStringLiteral("diagnosticsOutputSelector"));
	outputs_->setAccessibleName(text_("Diagnostics.SelectOutput"));
	outputs_->setSizeAdjustPolicy(QComboBox::AdjustToMinimumContentsLengthWithIcon);
	outputs_->setMinimumContentsLength(15);
	layout->addWidget(outputs_);
	output_ = makeSection(*layout, "Diagnostics.Output", "diagnosticsOutput");
	// ackForm не смешивает четыре поля RTMP ACK с потерями кадров, RTT или очередями.
	auto *ackForm = new QFormLayout;
	ackForm->setRowWrapPolicy(QFormLayout::WrapLongRows);
	ackTotal_ = label("ackAheadEventsTotal", "Diagnostics.AckTotal");
	ackLast_ = label("ackAheadBytesLast", "Diagnostics.AckLast");
	ackMax_ = label("ackAheadBytesMax", "Diagnostics.AckMax");
	ackTime_ = label("ackLastAnomalyAt", "Diagnostics.AckTime");
	ackAvailability_ = label("diagnosticsAckAvailability", "Diagnostics.Availability");
	ackForm->addRow(text_("Diagnostics.AckTotal"), ackTotal_);
	ackForm->addRow(text_("Diagnostics.AckLast"), ackLast_);
	ackForm->addRow(text_("Diagnostics.AckMax"), ackMax_);
	ackForm->addRow(text_("Diagnostics.AckTime"), ackTime_);
	ackForm->addRow(text_("Diagnostics.Availability"), ackAvailability_);
	layout->addLayout(ackForm);
	// ackNotice объясняет пределы интерпретации даже достоверного нулевого счётчика.
	auto *ackNotice = label("diagnosticsAckNotice", "Diagnostics.AckNotice");
	ackNotice->setText(text_("Diagnostics.AckNotice"));
	layout->addWidget(ackNotice);
	ffmpeg_ = makeSection(*layout, "Diagnostics.FFmpeg", "diagnosticsFFmpeg");
	gpu_ = makeSection(*layout, "Diagnostics.GPU", "diagnosticsGPU");
	layout->addStretch();
	connect(&stateStore_, &StateStore::obsStateChanged, this, &DiagnosticsPage::scheduleRefresh);
	connect(&stateStore_, &StateStore::obsStateChanged, this, &DiagnosticsPage::refreshSummary);
	connect(&stateStore_, &StateStore::obsStatisticsChanged, this, &DiagnosticsPage::scheduleRefresh);
	connect(&stateStore_, &StateStore::serverStateChanged, this, &DiagnosticsPage::scheduleRefresh);
	connect(&stateStore_, &StateStore::serverStateChanged, this, &DiagnosticsPage::refreshSummary);
	connect(&stateStore_, &StateStore::runtimeStateChanged, this, &DiagnosticsPage::scheduleRefresh);
	// Изменение выбора только перечитывает уже имеющееся состояние, не выполняя серверных действий.
	connect(outputs_, &QComboBox::currentIndexChanged, this, &DiagnosticsPage::refresh);
	// ageTimer обновляет возраст GPU при отсутствии событий; это локальный таймер, не серверный опрос.
	auto *ageTimer = new QTimer(this);
	ageTimer->setInterval(1000);
	// Скрытая страница не перерисовывается таймером; следующий показ получает текущее время.
	connect(ageTimer, &QTimer::timeout, this, [this] {
		if (isVisible())
			refreshGpu();
	});
	ageTimer->start();
	refresh();
}

// Краткие подписи меняются до возврата из сигнала наблюдения, включая скрытое окно управления.
void DiagnosticsPage::refreshSummary()
{
	profile_->setText(stateStore_.profileName() && !stateStore_.profileName()->isEmpty()
				  ? *stateStore_.profileName()
				  : text_("Observation.NoData"));
	streaming_->setStreamingStatus(text_, stateStore_.streamingActive());
	connection_->setConnectionStatus(text_, stateStore_.connectionStatus());
}

// Откладывает одно чтение до завершения синхронной публикации; удаление страницы отменяет callback Qt.
void DiagnosticsPage::scheduleRefresh()
{
	// metadata и runtime ограничивают пользовательский выбор до отложенного обновления больших таблиц.
	const auto &metadata = stateStore_.dockServer();
	const auto &runtime = stateStore_.runtime();
	// currentSession адресует только прежнюю выбранную Session и не перечисляет другие эфиры.
	const auto currentSession = runtime.sessions.constFind(sessionId_);
	// invalidated замечает также краткую смену контекста или исполнения между двумя сигналами.
	bool invalidated = context_ != stateStore_.dockContext() || !metadata.session ||
			   metadata.session->id != sessionId_ || currentSession == runtime.sessions.cend();
	if (!invalidated) {
		invalidated = (currentSession->observation ? currentSession->observation->instanceId : QString{}) !=
			      sessionInstance_;
		// selectedId и selectedInstance задают точную пару выбора; retained подтверждает её наличие в runtime.
		const auto selectedId = outputs_->currentData(Qt::UserRole).toString();
		const auto selectedInstance = outputs_->currentData(Qt::UserRole + 1).toString();
		bool retained = selectedId.isEmpty();
		// output проверяет только исполнения текущего эфира, не используя совпадения имён.
		for (const auto &output : currentSession->outputs)
			if (output.outputId == selectedId && output.instanceId == selectedInstance)
				retained = true;
		invalidated = invalidated || !retained;
	}
	if (invalidated) {
		// blocked не допускает синхронную перестройку из сигнала инвалидированного выбора.
		const QSignalBlocker blocked(outputs_);
		outputs_->setCurrentIndex(0);
	}
	if (refreshPending_)
		return;
	refreshPending_ = true;
	// Callback читает последнее состояние, не сохраняя заимствованных DTO между оборотами event loop.
	QTimer::singleShot(0, this, [this] {
		refreshPending_ = false;
		refresh();
	});
}

// Создаёт read-only таблицу: имена и значения остаются обычным текстом, оформление берётся из темы.
QTreeWidget *DiagnosticsPage::makeSection(QVBoxLayout &layout, const char *titleKey, const char *name)
{
	// heading подписывает независимый источник наблюдений.
	auto *heading = new QLabel(text_(titleKey), this);
	heading->setTextFormat(Qt::PlainText);
	layout.addWidget(heading);
	// tree принадлежит странице, имеет собственную клавиатурную навигацию и прокрутку длинных значений.
	auto *tree = new DiagnosticsTreeWidget(this);
	tree->setObjectName(QString::fromLatin1(name));
	tree->setAccessibleName(text_(titleKey));
	tree->setHeaderLabels(
		{text_("Diagnostics.Measurement"), text_("Diagnostics.Value"), text_("Diagnostics.Availability")});
	tree->setEditTriggers(QAbstractItemView::NoEditTriggers);
	tree->setUniformRowHeights(true);
	tree->setMinimumHeight(150);
	tree->setMaximumHeight(270);
	tree->header()->setSectionResizeMode(QHeaderView::Interactive);
	tree->header()->setStretchLastSection(true);
	tree->setColumnWidth(0, 220);
	tree->setColumnWidth(1, 230);
	layout.addWidget(tree);
	return tree;
}

// Добавляет строку с известным родителем; QTreeWidget управляет удалением элементов.
QTreeWidgetItem *DiagnosticsPage::addRow(QTreeWidget &tree, QTreeWidgetItem *parent, const QString &key,
					 const QString &value, const QString &availability)
{
	// item хранит только разрешённое отображение, не DTO с секретной конфигурацией или failure.
	auto *item = parent ? new QTreeWidgetItem(parent) : new QTreeWidgetItem(&tree);
	item->setText(0, key);
	item->setText(1, value);
	item->setText(2, availability);
	item->setExpanded(true);
	return item;
}

// Возвращает локализованную доступность одной оси; Fresh не означает успешного воспроизведения.
QString DiagnosticsPage::availabilityText(RuntimeAvailability availability) const
{
	switch (availability) {
	case RuntimeAvailability::Fresh:
		return text_("Diagnostics.Confirmed");
	case RuntimeAvailability::Stale:
		return text_("Diagnostics.Stale");
	case RuntimeAvailability::Unknown:
		return text_("Observation.NoData");
	}
	return text_("Observation.NoData");
}

// Использует закрытый список безопасных состояний; произвольные серверные строки не становятся объяснениями.
QString DiagnosticsPage::statusText(const QString &value) const
{
	// keys содержит доверенные коды наблюдения и соответствующие ключи локализации.
	static const QMap<QString, const char *> keys{{QStringLiteral("STARTING"), "Diagnostics.Starting"},
						      {QStringLiteral("RUNNING"), "Diagnostics.Running"},
						      {QStringLiteral("STOPPING"), "Diagnostics.Stopping"},
						      {QStringLiteral("STOPPED"), "Diagnostics.Stopped"},
						      {QStringLiteral("READY"), "Diagnostics.Ready"},
						      {QStringLiteral("FAILED"), "Diagnostics.Failed"},
						      {QStringLiteral("INTERRUPTED"), "Dock.SessionInterrupted"},
						      {QStringLiteral("CONNECTING"), "Dock.OutputConnecting"},
						      {QStringLiteral("PUBLISHING"), "Dock.OutputStreaming"},
						      {QStringLiteral("RETRYING"), "Dock.OutputReconnecting"},
						      {QStringLiteral("LIVE"), "Diagnostics.Live"},
						      {QStringLiteral("FALLBACK"), "Diagnostics.Fallback"},
						      {QStringLiteral("NONE"), "Diagnostics.None"},
						      {QStringLiteral("ok"), "Diagnostics.StatusOk"},
						      {QStringLiteral("available"), "Diagnostics.StatusOk"},
						      {QStringLiteral("unavailable"), "Diagnostics.Unavailable"},
						      {QStringLiteral("stale"), "Diagnostics.Stale"},
						      {QStringLiteral("warming_up"), "Dock.Measuring"},
						      {QStringLiteral("saturated"), "Diagnostics.Saturated"},
						      {QStringLiteral("counter_reset"), "Diagnostics.CounterReset"},
						      {QStringLiteral("unsupported"), "Diagnostics.Unsupported"},
						      {QStringLiteral("disabled"), "Diagnostics.Disabled"},
						      {QStringLiteral("error"), "Diagnostics.Error"}};
	// found адресует только известный ключ, а не текст причины сервера.
	const auto found = keys.constFind(value);
	return value.isEmpty()        ? text_("Observation.NoData")
	       : found == keys.cend() ? text_("Dock.UnknownStatus")
				      : text_(*found);
}

// Сохраняет точную временную строку сервера; отсутствие не заменяется временем получения UI.
QString DiagnosticsPage::timestamp(const api::JsonField<QString> &field) const
{
	return field && !field->isEmpty() ? *field : text_("Observation.NoData");
}

// Дробные скорости форматируются отдельно от точных целых счётчиков и времён FFmpeg.
QString DiagnosticsPage::decimal(const api::JsonField<double> &field) const
{
	return field ? QString::number(*field, 'g', 12) : text_("Observation.NoData");
}

// Показывает идентичность и состояние observation; свежесть берётся по точному instanceId.
void DiagnosticsPage::addObservation(QTreeWidget &tree, QTreeWidgetItem *parent, const api::Observation &observation)
{
	addRow(tree, parent, text_("Diagnostics.Instance"), observation.instanceId);
	addRow(tree, parent, text_("Diagnostics.State"), statusText(observation.state),
	       availabilityText(stateStore_.runtimeEvidence(observation.instanceId).state));
	addRow(tree, parent, text_("Diagnostics.ObservedAt"), timestamp(observation.observedAt));
}

// Показывает полные накопления текущего исполнения без локального baseline и без объединения разных источников.
void DiagnosticsPage::addMedia(QTreeWidget &tree, QTreeWidgetItem *parent, const api::MediaMetrics &media,
			       RuntimeAvailability availability)
{
	// evidence подписывает каждое значение; saturated запрещает вывод о достоверном приросте накоплений.
	const auto evidence = availabilityText(availability);
	addRow(tree, parent, text_("Diagnostics.SampledAt"), timestamp(media.sampledAt), evidence);
	addRow(tree, parent, text_("Diagnostics.VideoFrames"), QString::number(media.videoFrames), evidence);
	addRow(tree, parent, text_("Diagnostics.PayloadBytes"), QString::number(media.payloadBytes), evidence);
	addRow(tree, parent, text_("Diagnostics.WrittenBytes"), QString::number(media.writtenBytes), evidence);
	addRow(tree, parent, text_("Diagnostics.AudioUnits"), QString::number(media.audioUnits), evidence);
	addRow(tree, parent, text_("Diagnostics.LastFrame"), timestamp(media.lastFrame), evidence);
	addRow(tree, parent, text_("Diagnostics.CounterReliability"),
	       text_(media.saturated ? "Diagnostics.Saturated" : "Diagnostics.NotSaturated"), evidence);
	// rate добавляет одно окно value/status со временем и длительностью; key определяет единицы value.
	const auto rate = [&](const char *key, const api::Rate &value) {
		// row группирует окно конкретного источника; server status не заменяется статусом соединения.
		auto *row = addRow(tree, parent, text_(key), decimal(value.value),
				   evidence + QStringLiteral(" · ") + statusText(value.status));
		addRow(tree, row, text_("Diagnostics.WindowStart"), timestamp(value.windowStart));
		addRow(tree, row, text_("Diagnostics.WindowEnd"), timestamp(value.windowEnd));
		addRow(tree, row, text_("Diagnostics.DurationNs"), QString::number(value.durationNs));
	};
	rate("Diagnostics.FPS", media.rates.fps);
	rate("Diagnostics.PayloadBitrate", media.rates.payloadBitrate);
	rate("Diagnostics.WrittenBitrate", media.rates.writtenBitrate);
	addRow(tree, parent, text_("Diagnostics.VideoCodec"),
	       media.format.videoCodec.isEmpty() ? text_("Observation.NoData") : media.format.videoCodec, evidence);
	addRow(tree, parent, text_("Diagnostics.AudioCodec"),
	       media.format.audioCodec.isEmpty() ? text_("Observation.NoData") : media.format.audioCodec, evidence);
	addRow(tree, parent, text_("Diagnostics.Resolution"),
	       media.format.width && media.format.height
		       ? QStringLiteral("%1 × %2").arg(media.format.width).arg(media.format.height)
		       : text_("Observation.NoData"),
	       evidence);
	addRow(tree, parent, text_("Diagnostics.SampleRate"),
	       media.format.sampleRateHz ? QString::number(media.format.sampleRateHz) : text_("Observation.NoData"),
	       evidence);
	addRow(tree, parent, text_("Diagnostics.Channels"),
	       media.format.channels ? QString::number(media.format.channels) : text_("Observation.NoData"), evidence);
}

// Отражает независимые оси OBS/сервера, не используя history/details или текущую конфигурацию вместо снимка.
void DiagnosticsPage::refresh()
{
	// obsPosition, inputPosition, groupPosition, outputPosition и ffmpegPosition сохраняют навигацию секций.
	const TreePosition obsPosition(*obs_);
	const TreePosition inputPosition(*input_);
	const TreePosition groupPosition(*groups_);
	const TreePosition outputPosition(*output_);
	const TreePosition ffmpegPosition(*ffmpeg_);
	refreshSummary();
	contextLabel_->setText(stateStore_.dockContext().valid() ? stateStore_.dockContext().streamId
								 : text_("Stream.Choose"));
	// statistics содержит локальные наблюдения OBS; они не становятся устаревшими из-за потери SSE.
	const auto &statistics = stateStore_.obsStatistics();
	obs_->clear();
	// local добавляет необязательное числовое value в исходных единицах key, не подставляя ноль.
	const auto local = [&](const char *key, const auto &value) {
		addRow(*obs_, nullptr, text_(key), value ? QString::number(*value) : text_("Observation.NoData"));
	};
	local("Diagnostics.FPS", statistics.fps);
	local("Diagnostics.RenderNs", statistics.renderTimeNs);
	local("Diagnostics.WrittenBitrate", statistics.bitrate);
	local("Diagnostics.SentBytes", statistics.sentBytes);
	if (statistics.bitrateWarming)
		addRow(*obs_, nullptr, text_("Diagnostics.WrittenBitrate"), text_("Dock.Measuring"));
	// frames показывает skipped/total одного этапа key, а limited отдельно сообщает предел исходного API.
	const auto frames = [&](const char *key, const std::optional<FrameCounters> &value, bool limited) {
		addRow(*obs_, nullptr, text_(key),
		       value ? QStringLiteral("%1 / %2").arg(value->skipped).arg(value->total)
			     : text_("Observation.NoData"),
		       limited ? text_("Diagnostics.Saturated") : QString{});
	};
	frames("Diagnostics.RenderDrops", statistics.render, statistics.renderLimited);
	frames("Diagnostics.EncodeDrops", statistics.encode, statistics.encodeLimited);
	frames("Diagnostics.NetworkDrops", statistics.network, statistics.networkLimited);
	input_->clear();
	groups_->clear();
	output_->clear();
	ffmpeg_->clear();
	// metadata задаёт единственную текущую Session выбранного DockContext, а не список всех runtime-сессий.
	const auto &metadata = stateStore_.dockServer();
	// session заимствована только на текущий синхронный refresh, без публикации изменений StateStore.
	const api::RuntimeSession *session = nullptr;
	if (stateStore_.dockContext().valid() && metadata.session) {
		// found ищет только точный ID текущего эфира.
		const auto found = stateStore_.runtime().sessions.constFind(metadata.session->id);
		if (found != stateStore_.runtime().sessions.cend())
			session = &*found;
	}
	// nextSession и nextInstance ограничивают сохранение выбора текущим серверным эфиром/исполнением.
	const auto nextSession = metadata.session ? metadata.session->id : QString{};
	const auto nextInstance = session && session->observation ? session->observation->instanceId : QString{};
	// preserve разрешает сохранять точный выбор при метриках и reconnect того же исполнения.
	const bool preserve = context_ == stateStore_.dockContext() && sessionId_ == nextSession &&
			      sessionInstance_ == nextInstance;
	// selectedId и selectedInstance копируются до перестройки combo; строка без данных означает placeholder.
	const auto selectedId = preserve ? outputs_->currentData(Qt::UserRole).toString() : QString{};
	const auto selectedInstance = preserve ? outputs_->currentData(Qt::UserRole + 1).toString() : QString{};
	context_ = stateStore_.dockContext();
	sessionId_ = nextSession;
	sessionInstance_ = nextInstance;
	// wantedLabels, wantedIds и wantedInstances описывают подписи и две независимые идентичности строк.
	// Пустая первая строка соответствует предложению выбрать исполнение, а не серверному Output.
	QStringList wantedLabels{text_("Diagnostics.SelectOutput")};
	QStringList wantedIds{QString{}};
	QStringList wantedInstances{QString{}};
	if (session) {
		// output перебирает только Outputs выбранной Session в неизменном порядке этого refresh.
		for (const auto &output : session->outputs) {
			// saved хранит безопасное имя закреплённого снимка, без чтения сегодняшнего Destination.
			const auto saved = metadata.outputs.constFind(output.outputId);
			// name не объединяет выходы с одинаковыми именами: явные ID сохраняют различимость исполнений.
			const auto name = saved != metadata.outputs.cend() && !saved->name.isEmpty()
						  ? saved->name
						  : text_("Dock.UnnamedOutput");
			wantedLabels.append(
				QStringLiteral("%1 · %2 · %3").arg(name, output.outputId, output.instanceId));
			wantedIds.append(output.outputId);
			wantedInstances.append(output.instanceId);
		}
	}
	// rebuild отличает изменение набора/контекста от новых метрик: модель открытого popup остаётся прежней.
	bool rebuild = !preserve || outputs_->count() != wantedLabels.size();
	// index сравнивает всю строку, включая execution ID, без смешения одинаковых имён выходов.
	for (int index = 0; !rebuild && index < wantedLabels.size(); ++index)
		rebuild = outputs_->itemText(index) != wantedLabels.at(index) ||
			  outputs_->itemData(index, Qt::UserRole).toString() != wantedIds.at(index) ||
			  outputs_->itemData(index, Qt::UserRole + 1).toString() != wantedInstances.at(index);
	// block исключает повторный refresh только от программного изменения текущего выбора.
	const QSignalBlocker block(outputs_);
	if (rebuild) {
		outputs_->clear();
		outputs_->addItem(wantedLabels.first());
		// index восстанавливает только реальные строки; placeholder сохраняет отсутствие itemData.
		for (int index = 1; index < wantedLabels.size(); ++index) {
			outputs_->addItem(wantedLabels.at(index), wantedIds.at(index));
			outputs_->setItemData(index, wantedInstances.at(index), Qt::UserRole + 1);
		}
	}
	// selected адресует только подтверждённый выбор, независимо от подсветки незавершённого popup.
	const api::RuntimeOutput *selected = nullptr;
	// outputIndex связывает тот же порядок Session со строками после placeholder.
	int outputIndex = 1;
	if (session) {
		// output не заменяет прежние метрики другим исполнением с тем же Output ID.
		for (const auto &output : session->outputs) {
			if (output.outputId == selectedId && output.instanceId == selectedInstance) {
				if (rebuild)
					outputs_->setCurrentIndex(outputIndex);
				selected = &output;
			}
			++outputIndex;
		}
	}
	outputs_->setEnabled(session && !session->outputs.isEmpty());
	// reset ACK удаляет значения прежнего выбора даже при неизвестной/отсутствующей новой метрике.
	ackTotal_->setText(text_("Observation.NoData"));
	ackLast_->setText(text_("Observation.NoData"));
	ackMax_->setText(text_("Observation.NoData"));
	ackTime_->setText(text_("Observation.NoData"));
	ackAvailability_->setText(text_("Observation.NoData"));
	sessionLabel_->setText(!context_.valid() ? text_("Diagnostics.NoContext")
			       : !metadata.session
				       ? (metadata.sessionStatus.complete && metadata.sessionStatus.availability ==
										     RuntimeAvailability::Fresh
						  ? text_("Diagnostics.NoSession")
						  : text_("Observation.NoData"))
				       : QStringLiteral("%1 · %2 · %3")
						 .arg(metadata.session->id, statusText(metadata.session->state),
						      availabilityText(metadata.sessionStatus.availability)));
	if (!session) {
		addRow(*input_, nullptr, text_("Diagnostics.Runtime"), text_("Diagnostics.NoRuntime"));
		addRow(*groups_, nullptr, text_("Diagnostics.Runtime"), text_("Diagnostics.NoRuntime"));
		addRow(*ffmpeg_, nullptr, text_("Diagnostics.Runtime"), text_("Diagnostics.NoRuntime"));
		refreshGpu();
		return;
	}
	if (session->observation)
		addObservation(*input_, nullptr, *session->observation);
	if (metadata.session)
		addRow(*input_, nullptr, text_("Diagnostics.Paused"),
		       text_(metadata.session->desiredPaused ? "Diagnostics.Yes" : "Diagnostics.No"),
		       availabilityText(metadata.sessionStatus.availability));
	// evidence разделяет состояние Session, connected входа и измерения входа.
	const auto evidence = stateStore_.runtimeEvidence(nextInstance);
	addRow(*input_, nullptr, text_("Diagnostics.InputConnected"),
	       session->input ? text_(session->input->connected ? "Diagnostics.Yes" : "Diagnostics.No")
			      : text_("Observation.NoData"),
	       availabilityText(evidence.input));
	if (session->input) {
		addRow(*input_, nullptr, text_("Diagnostics.ChangedAt"), timestamp(session->input->changedAt),
		       availabilityText(evidence.input));
		if (session->input->metrics) {
			addRow(*input_, nullptr, text_("Diagnostics.SourceEpoch"),
			       QString::number(session->input->metrics->sourceEpoch),
			       availabilityText(evidence.metrics));
			addMedia(*input_, nullptr, session->input->metrics->media, evidence.metrics);
		} else {
			addRow(*input_, nullptr, text_("Diagnostics.Measurement"), text_("Observation.NoData"));
		}
	}
	// group содержит конкретное исполнение; повтор groupId не приводит к слиянию строк.
	for (const auto &group : session->groups) {
		// row объединяет только показатели этого group.instanceId.
		auto *row = addRow(*groups_, nullptr, group.groupId, group.instanceId);
		addObservation(*groups_, row, group);
		addRow(*groups_, row, text_("Diagnostics.Source"), statusText(group.source),
		       availabilityText(stateStore_.runtimeEvidence(group.instanceId).state));
		if (group.metrics) {
			// live, fallback и outgoing различают независимые направления метрик одной группы.
			auto *live = addRow(*groups_, row, text_("Diagnostics.LiveInput"), QString{});
			auto *fallback = addRow(*groups_, row, text_("Diagnostics.FallbackInput"), QString{});
			auto *outgoing = addRow(*groups_, row, text_("Diagnostics.GroupOutput"), QString{});
			addMedia(*groups_, live, group.metrics->liveInput,
				 stateStore_.runtimeEvidence(group.instanceId).metrics);
			addMedia(*groups_, fallback, group.metrics->fallbackInput,
				 stateStore_.runtimeEvidence(group.instanceId).metrics);
			addMedia(*groups_, outgoing, group.metrics->output,
				 stateStore_.runtimeEvidence(group.instanceId).metrics);
		} else {
			addRow(*groups_, row, text_("Diagnostics.Measurement"), text_("Observation.NoData"));
		}
	}
	if (session->groups.isEmpty())
		addRow(*groups_, nullptr, text_("Diagnostics.Groups"), text_("Observation.NoData"));
	if (selected) {
		addObservation(*output_, nullptr, *selected);
		if (selected->metrics) {
			// metrics и freshness относятся именно к selected->instanceId, включая reconnect той же попытки.
			const auto &metrics = *selected->metrics;
			const auto freshness = stateStore_.runtimeEvidence(selected->instanceId).metrics;
			addMedia(*output_, nullptr, metrics.media, freshness);
			// counters добавляет map по точным контрактным именам; key/value не являются reason/action.
			const auto counters = [&](const char *key, const QMap<QString, quint64> &values) {
				// row объединяет только указанную категорию серверных счётчиков.
				auto *row =
					addRow(*output_, nullptr, text_(key), QString{}, availabilityText(freshness));
				// it адресует счётчик без суммирования различных причин потерь.
				for (auto it = values.cbegin(); it != values.cend(); ++it)
					addRow(*output_, row, it.key(), QString::number(it.value()),
					       availabilityText(freshness));
			};
			counters("Diagnostics.Counters", metrics.counters);
			counters("Diagnostics.QueueDrops", metrics.queueDrops);
			counters("Diagnostics.DeliveryDrops", metrics.deliveryDrops);
			addRow(*output_, nullptr, text_("Diagnostics.Resync"),
			       text_(metrics.resync ? "Diagnostics.Yes" : "Diagnostics.No"),
			       availabilityText(freshness));
			addRow(*output_, nullptr, text_("Diagnostics.LastWrite"), timestamp(metrics.lastWrite),
			       availabilityText(freshness));
			addRow(*output_, nullptr, text_("Diagnostics.LastMedia"), timestamp(metrics.lastMedia),
			       availabilityText(freshness));
			ackTotal_->setText(QString::number(metrics.ackAheadEventsTotal));
			ackLast_->setText(QString::number(metrics.ackAheadBytesLast));
			ackMax_->setText(QString::number(metrics.ackAheadBytesMax));
			ackTime_->setText(metrics.ackLastAnomalyAt.isNull() ? text_("Diagnostics.NotObserved")
									    : timestamp(metrics.ackLastAnomalyAt));
			ackAvailability_->setText(availabilityText(freshness) +
						  (metrics.media.saturated
							   ? QStringLiteral(" · ") + text_("Diagnostics.AckSaturated")
							   : QString{}));
		} else {
			addRow(*output_, nullptr, text_("Diagnostics.Measurement"), text_("Observation.NoData"));
		}
	} else {
		addRow(*output_, nullptr, text_("Diagnostics.Output"), text_("Diagnostics.SelectOutput"));
	}
	// process представляет точную попытку FFmpeg, а не уникальную роль или процент завершения команды.
	for (const auto &process : session->ffmpeg) {
		// role переводит только известные роли сервера, не раскрывая произвольную полученную строку.
		const auto role = process.role == QStringLiteral("live_transcode") ? text_("Diagnostics.LiveTranscode")
				  : process.role == QStringLiteral("fallback_player")
					  ? text_("Diagnostics.FallbackPlayer")
					  : text_("Dock.UnknownStatus");
		// row и freshness ограничивают данные исполнением процесса.
		auto *row = addRow(*ffmpeg_, nullptr, process.instanceId, role);
		const auto freshness = availabilityText(stateStore_.runtimeEvidence(process.instanceId).metrics);
		addObservation(*ffmpeg_, row, process);
		addRow(*ffmpeg_, row, text_("Diagnostics.GroupInstance"), process.groupInstanceId);
		if (!process.metrics) {
			addRow(*ffmpeg_, row, text_("Diagnostics.Measurement"), text_("Observation.NoData"));
			continue;
		}
		// metrics сохраняет независимые доступность и время последнего progress.
		const auto &metrics = *process.metrics;
		addRow(*ffmpeg_, row, text_("Diagnostics.Availability"), statusText(metrics.status), freshness);
		addRow(*ffmpeg_, row, text_("Diagnostics.SampledAt"), timestamp(metrics.sampledAt), freshness);
		addRow(*ffmpeg_, row, text_("Diagnostics.LastProgress"), timestamp(metrics.lastProgressAt), freshness);
		addRow(*ffmpeg_, row, text_("Diagnostics.ProgressLosses"), QString::number(metrics.progressLosses),
		       freshness);
		addRow(*ffmpeg_, row, text_("Diagnostics.DiagnosticLosses"), QString::number(metrics.diagnosticLosses),
		       freshness);
		if (!metrics.progress) {
			addRow(*ffmpeg_, row, text_("Diagnostics.Progress"), text_("Observation.NoData"));
			continue;
		}
		// progress содержит отдельные nullable поля; outTimeUs никогда не проходит через double.
		const auto &progress = *metrics.progress;
		addRow(*ffmpeg_, row, text_("Diagnostics.VideoFrames"), integer(progress.frame), freshness);
		addRow(*ffmpeg_, row, text_("Diagnostics.TotalSize"), integer(progress.totalSizeBytes), freshness);
		addRow(*ffmpeg_, row, text_("Diagnostics.DupFrames"), integer(progress.dupFrames), freshness);
		addRow(*ffmpeg_, row, text_("Diagnostics.DropFrames"), integer(progress.dropFrames), freshness);
		addRow(*ffmpeg_, row, text_("Diagnostics.FPS"), decimal(progress.fps), freshness);
		addRow(*ffmpeg_, row, text_("Diagnostics.WrittenBitrate"), decimal(progress.bitrateBps), freshness);
		addRow(*ffmpeg_, row, text_("Diagnostics.Speed"), decimal(progress.speed), freshness);
		addRow(*ffmpeg_, row, text_("Diagnostics.OutTimeUs"), integer(progress.outTimeUs), freshness);
		addRow(*ffmpeg_, row, text_("Diagnostics.ProgressEnd"),
		       text_(progress.end ? "Diagnostics.Yes" : "Diagnostics.No"), freshness);
	}
	if (session->ffmpeg.isEmpty())
		addRow(*ffmpeg_, nullptr, text_("Diagnostics.FFmpeg"), text_("Observation.NoData"));
	refreshGpu();
}

// Возраст каждой GPU-величины вычисляется из её sampledAt; успешное SSE-подключение не меняет эту метку.
void DiagnosticsPage::refreshGpu()
{
	// position сохраняет прокрутку GPU также при ежесекундной проверке возраста измерений.
	const TreePosition position(*gpu_);
	gpu_->clear();
	// gpu содержит только серверные устройства; локальная видеокарта OBS здесь не опрашивается.
	const auto &gpu = stateStore_.runtime().gpu;
	if (!gpu) {
		addRow(*gpu_, nullptr, text_("Diagnostics.GPU"), text_("Observation.NoData"));
		return;
	}
	addRow(*gpu_, nullptr, text_("Diagnostics.State"), statusText(gpu->status),
	       availabilityText(stateStore_.gpuAvailability()));
	addRow(*gpu_, nullptr, text_("Diagnostics.SampledAt"), timestamp(gpu->sampledAt));
	addRow(*gpu_, nullptr, text_("Diagnostics.StaleAfterNs"), QString::number(gpu->staleAfterNs));
	// device группирует собственный UUID без переноса доступности одной величины на остальные.
	for (const auto &device : gpu->devices) {
		// row связывает отдельные поля только с текущим устройством сервера.
		auto *row = addRow(*gpu_, nullptr, text_("Diagnostics.Device"), device.uuid);
		// metric отображает поле value с единицами key и независимыми серверными временем и статусом.
		const auto metric = [&](const char *key, const api::GPUMetric &value) {
			// measured — время серверного измерения с миллисекундной точностью только для проверки возраста.
			const auto measured = value.sampledAt
						      ? QDateTime::fromString(*value.sampledAt, Qt::ISODateWithMs)
						      : QDateTime{};
			// ageMs не используется для отображения точных времён; будущая метка не подтверждает свежесть.
			const auto ageMs = measured.isValid() ? measured.msecsTo(QDateTime::currentDateTimeUtc())
							      : qint64(-1);
			// expired учитывает серверный порог без переполнения наносекунд при умножении возраста.
			const bool expired = ageMs >= 0 && gpu->staleAfterNs > 0 &&
					     quint64(ageMs) > gpu->staleAfterNs / 1000000;
			// freshness соединяет независимое подтверждение GPU и собственный статус поля, не освежаясь от reconnect.
			QString freshness = availabilityText(stateStore_.gpuAvailability()) + QStringLiteral(" · ") +
					    statusText(value.status);
			if ((expired || stateStore_.connectionStatus() != ConnectionStatus::Connected) &&
			    stateStore_.gpuAvailability() != RuntimeAvailability::Stale)
				freshness += QStringLiteral(" · ") + text_("Diagnostics.Stale");
			else if (ageMs < 0)
				freshness += QStringLiteral(" · ") + text_("Diagnostics.TimeUnconfirmed");
			// field объединяет только value и служебные метки этого одного GPUMetric.
			auto *field = addRow(*gpu_, row, text_(key), integer(value.value), freshness);
			addRow(*gpu_, field, text_("Diagnostics.SampledAt"), timestamp(value.sampledAt));
			addRow(*gpu_, field, text_("Diagnostics.LastSuccess"), timestamp(value.lastSuccessAt));
			addRow(*gpu_, field, text_("Diagnostics.SamplingPeriodUs"),
			       QString::number(value.samplingPeriodUs));
		};
		metric("Diagnostics.Utilization", device.utilization);
		metric("Diagnostics.MemoryUtilization", device.memoryUtilization);
		metric("Diagnostics.EncoderUtilization", device.encoderUtilization);
		metric("Diagnostics.DecoderUtilization", device.decoderUtilization);
		metric("Diagnostics.MemoryUsed", device.memoryUsedBytes);
		metric("Diagnostics.MemoryTotal", device.memoryTotalBytes);
		metric("Diagnostics.MemoryFree", device.memoryFreeBytes);
	}
}

} // namespace gost
