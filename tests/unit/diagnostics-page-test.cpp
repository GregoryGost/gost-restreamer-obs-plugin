#include "api/json-decoder.hpp"
#include "core/state-store.hpp"
#include "fixtures/api-json.hpp"
#include "ui/diagnostics-page.hpp"
#include "ui/management-window.hpp"

#include <QAbstractItemView>
#include <QComboBox>
#include <QCoreApplication>
#include <QDateTime>
#include <QEventLoop>
#include <QLabel>
#include <QListWidget>
#include <QNetworkAccessManager>
#include <QPointer>
#include <QPushButton>
#include <QScrollArea>
#include <QScrollBar>
#include <QStackedWidget>
#include <QTest>
#include <QTimer>
#include <QTreeWidget>
#include <QTreeWidgetItemIterator>

#include <limits>
#include <memory>
#include <utility>

using namespace gost;
using namespace gost::api;

namespace {
// Возвращает ключ key, чтобы проверки значений не зависели от выбранного языка OBS.
QString translatedKey(const char *key)
{
	return QString::fromUtf8(key);
}

// Создаёт полученное поле value; Missing и Null задаются сценариями отдельно.
template<class T> JsonField<T> known(T value)
{
	return {FieldState::Value, std::move(value)};
}

// Создаёт наблюдение instance со свежей ревизией 10 и точным серверным временем для состояния state.
Observation observed(const QString &instance, const QString &state)
{
	return {instance, 10, known(QStringLiteral("2026-11-10T01:02:03.123456789Z")), state, {FieldState::Null, {}}};
}

// Выбирает один подтверждённый контекст и его текущую Session; сеть и настройки на диске не используются.
void selectContext(StateStore &store)
{
	// stream подтверждает существование выбранного Stream независимо от оперативного снимка.
	Stream stream;
	stream.id = QStringLiteral("stream");
	stream.name = QStringLiteral("Current stream");
	store.setConnection(QStringLiteral("http://diagnostics.invalid:8080"), ConnectionStatus::Connected);
	store.setStreams({stream}, true);
	store.setSelection(stream.id, true, {}, {}, QStringLiteral("obs-profile"));
	// metadata указывает на точный текущий эфир, а не на первый элемент runtime или истории.
	DockServerState metadata;
	metadata.sessionStatus = {RuntimeAvailability::Fresh, true, {}};
	metadata.session = DockSession{QStringLiteral("session"), QStringLiteral("RUNNING"), false};
	metadata.outputsStatus = {RuntimeAvailability::Fresh, true, {}};
	metadata.outputs.insert(QStringLiteral("output"), {QStringLiteral("Same name"), true, true, {}});
	metadata.outputs.insert(QStringLiteral("output-two"), {QStringLiteral("Same name"), true, true, {}});
	store.setDockServer(metadata);
}

// Возвращает дерево одного эфира с двумя различимыми Outputs, группой и попыткой FFmpeg.
// Начальные накопления измерены, а скорости warming_up не имеют значения, как в независимой фикстуре fixture::media.
Anchor currentAnchor()
{
	// session содержит текущее исполнение, связанное с REST-метаданными selectContext.
	RuntimeSession session;
	session.sessionId = QStringLiteral("session");
	session.observation = known(observed(QStringLiteral("session-instance"), QStringLiteral("RUNNING")));
	// input подтверждает соединение, но не выдумывает скорости до первого окна измерения.
	Input input;
	input.connected = true;
	input.metrics = known(InputMetrics{});
	input.metrics->sourceEpoch = 7;
	input.metrics->media.rates.fps.status = QStringLiteral("warming_up");
	input.metrics->media.rates.fps.value.state = FieldState::Null;
	session.input = known(input);
	// group и ffmpeg задают точную связь исполнения процесса с исполнением группы.
	RuntimeGroup group;
	static_cast<Observation &>(group) = observed(QStringLiteral("group-instance"), QStringLiteral("READY"));
	group.groupId = QStringLiteral("group");
	group.profileId = QStringLiteral("profile");
	group.source = QStringLiteral("LIVE");
	group.metrics = known(GroupMetrics{});
	group.metrics->liveInput.videoFrames = 101;
	group.metrics->fallbackInput.videoFrames = 202;
	group.metrics->output.videoFrames = 303;
	session.groups.append(group);
	// output содержит ACK, очереди, доставку и переподключения раздельно, без общего процента потерь.
	RuntimeOutput output;
	static_cast<Observation &>(output) = observed(QStringLiteral("output-instance"), QStringLiteral("PUBLISHING"));
	output.outputId = QStringLiteral("output");
	output.metrics = known(OutputMetrics{});
	output.metrics->ackAheadEventsTotal = 17;
	output.metrics->ackAheadBytesLast = 300;
	output.metrics->ackAheadBytesMax = 900;
	output.metrics->ackLastAnomalyAt.state = FieldState::Null;
	output.metrics->queueDrops.insert(QStringLiteral("video"), 3);
	output.metrics->deliveryDrops.insert(QStringLiteral("video"), 7);
	output.metrics->counters.insert(QStringLiteral("reconnects"), 2);
	session.outputs.append(output);
	output.outputId = QStringLiteral("output-two");
	output.instanceId = QStringLiteral("output-instance-two");
	output.metrics->ackAheadEventsTotal = 28;
	session.outputs.append(output);
	// ffmpeg допускает частичный progress; каждое поле отсутствует независимо от остальных.
	RuntimeFFmpeg ffmpeg;
	static_cast<Observation &>(ffmpeg) = observed(QStringLiteral("ffmpeg-instance"), QStringLiteral("READY"));
	ffmpeg.groupInstanceId = group.instanceId;
	ffmpeg.role = QStringLiteral("live");
	ffmpeg.metrics = known(FFmpegMetrics{});
	ffmpeg.metrics->status = QStringLiteral("ok");
	ffmpeg.metrics->progress = known(FFmpegProgress{});
	session.ffmpeg.append(ffmpeg);
	return {QStringLiteral("opaque-anchor"), {session}, {FieldState::Null, {}}};
}

// Возвращает событие Output с независимой ревизией revision для текущего output-instance.
RuntimeEvent outputEvent(const QString &kind, quint64 revision)
{
	// event сохраняет точную адресацию выхода и непрозрачный, различимый для каждого шага cursor.
	RuntimeEvent event;
	event.cursor = kind + QString::number(revision);
	event.kind = kind;
	event.owner = QStringLiteral("output");
	event.sessionId = QStringLiteral("session");
	event.outputId = QStringLiteral("output");
	event.observation = observed(QStringLiteral("output-instance"), QStringLiteral("RETRYING"));
	event.observation.revision = revision;
	return event;
}

// Обрабатывает запланированные обновления перед проверкой неизменившегося значения.
// Бюджет в 1 с ограничивает обработку событий; фиксированная задержка не используется.
void dispatchPendingUpdates()
{
	QCoreApplication::processEvents(QEventLoop::AllEvents, 1000);
}

// Возвращает текст именованной метки name у page; отсутствие виджета даёт отличимый провал сравнения.
QString labelText(const QWidget &page, const char *name)
{
	// label заимствована у страницы только на время чтения.
	const auto *label = page.findChild<QLabel *>(QString::fromLatin1(name));
	return label ? label->text() : QStringLiteral("<missing label>");
}

// Ищет в tree единственную строку с ключом key; неоднозначность возвращает nullptr вместо случайного совпадения.
QTreeWidgetItem *row(QTreeWidget *tree, const char *key)
{
	if (!tree)
		return nullptr;
	// matches содержит все строки ключа, включая вложенные секции измерений.
	const auto matches = tree->findItems(QString::fromUtf8(key), Qt::MatchExactly | Qt::MatchRecursive, 0);
	return matches.size() == 1 ? matches.front() : nullptr;
}

// Возвращает строку key среди прямых детей parent для различения одноимённых показателей разных направлений.
QTreeWidgetItem *childRow(QTreeWidgetItem *parent, const char *key)
{
	if (!parent)
		return nullptr;
	// index адресует только детей указанного направления или отдельного GPU-поля.
	for (int index = 0; index < parent->childCount(); ++index)
		if (parent->child(index)->text(0) == QString::fromUtf8(key))
			return parent->child(index);
	return nullptr;
}

// Проверяет, что текущая строка tree целиком видна по высоте внутри дерева и внешнего viewport.
// По ширине достаточно пересечения: длинные значения имеют самостоятельную горизонтальную прокрутку.
bool currentRowIsVisible(QTreeWidget &tree, QWidget &viewport)
{
	if (!tree.currentItem() || !tree.currentIndex().isValid())
		return false;
	// local содержит геометрию выбранной строки в собственной области дерева.
	const auto local = tree.visualItemRect(tree.currentItem());
	// outer переводит ту же строку во внешнюю область, которая может обрезать дерево целиком.
	const QRect outer(tree.viewport()->mapTo(&viewport, local.topLeft()), local.size());
	return !local.isEmpty() && tree.viewport()->rect().intersects(local) && local.top() >= 0 &&
	       local.bottom() < tree.viewport()->height() && viewport.rect().intersects(outer) && outer.top() >= 0 &&
	       outer.bottom() < viewport.height();
}

// Собирает отображаемые значения и подсказки page, чтобы секреты не прошли в другую секцию незаметно.
QString visibleText(const QWidget &page)
{
	// result содержит текст только UI, без QObject properties и скрытых DTO.
	QString result;
	// label перебирает также QLabel внутри статусных виджетов.
	for (const auto *label : page.findChildren<QLabel *>())
		result += label->text() + label->toolTip() + QLatin1Char('\n');
	// combo проверяет подписи всех доступных для выбора Outputs.
	for (const auto *combo : page.findChildren<QComboBox *>())
		// index адресует пункт, который может быть вне видимой части раскрытого списка.
		for (int index = 0; index < combo->count(); ++index)
			result += combo->itemText(index) + QLatin1Char('\n');
	// tree содержит одну независимую секцию; item обходит все вложенные измерения.
	for (auto *tree : page.findChildren<QTreeWidget *>()) {
		for (QTreeWidgetItemIterator item(tree); *item; ++item)
			// column включает также отдельную доступность измерения.
			for (int column = 0; column < tree->columnCount(); ++column)
				result += (*item)->text(column) + (*item)->toolTip(column) + QLatin1Char('\n');
	}
	return result;
}
} // namespace

// Проверяет публичные Widgets по независимым контрактам текущего эфира, точности и доступности данных.
class DiagnosticsPageTest final : public QObject {
	Q_OBJECT
private slots:
	// Диагностика занимает прежнюю страницу 5 единственного окна и помещается в прокрутку.
	void integratesIntoTheExistingManagementPage();
	// Клавиши перемещают реальный выбор и сохраняют видимость строки при двух уровнях прокрутки.
	void keepsKeyboardSelectionVisibleInTheManagementViewport();
	// Фоновые наблюдения сохраняют ручное положение прокрутки даже у дерева с клавиатурным фокусом.
	void preservesManualScrollWhileObservationsRefresh();
	// Независимые OBS и runtime обновления не меняют состав доступных исполнений Output.
	void preservesPendingOutputSelectionWhileObservationsRefresh_data();
	// Подсветка в открытом списке переживает обновление метрик, а Enter выбирает точное исполнение и ACK.
	void preservesPendingOutputSelectionWhileObservationsRefresh();
	// Неизвестные OBS/runtime/GPU не становятся нулевыми измерениями или выбранным первым Output.
	void emptyStateRemainsUnknown();
	// Полный путь JSON → StateStore → UI сохраняет точные ACK U64 и nullable время.
	void rendersExactAckWithoutInferringWhichCounterSaturated();
	// Missing/null metrics отличимы от действительно измеренных нулевых ACK.
	void missingMetricsDoNotBecomeZero();
	// Выбор хранит обе идентичности, а reconnect и state-событие не освежают старые метрики.
	void preservesExactSelectionAndIndependentFreshness();
	// Повторный запуск того же Output и замена Session отзывают прежний выбор до новой телеметрии.
	void replacementExecutionsClearSelection();
	// Три координаты контекста независимо ограничивают прежние текущие данные.
	void contextChangesClearSelection_data();
	// Смена выбранного контекста не показывает прежнюю Session из оставшегося RAM runtime.
	void contextChangesClearSelection();
	// История, детальные ответы и чужие runtime-сессии не подменяют выбранный текущий эфир.
	void excludesForeignSessionsHistoryAndFailurePayloads();
	// Вход и направления группы сохраняют независимые показатели и отсутствие скорости warming_up.
	void separatesInputAndGroupMeasurements();
	// Progress допускает отрицательные I64 микросекунды и независимо отсутствующие поля.
	void preservesSignedFfmpegTimeAndOptionalProgress_data();
	// Точные границы I64 и отдельные nullable поля сохраняются в исходных единицах.
	void preservesSignedFfmpegTimeAndOptionalProgress();
	// Доступность каждого GPU-поля зависит от его собственного статуса и временной метки.
	void keepsGpuFieldsIndependentAcrossReconnect();
	// Новый снимок подтверждается только заголовками SSE, а сброс runtime удаляет его GPU и ACK.
	void waitsForSubscriptionBeforeConfirmingGpuAndClearsRuntime();

	// Локальный таймер устаревания работает без SSE и удаляется вместе со страницей.
	void agesGpuWithoutEventsAndReleasesItsTimer();
};

void DiagnosticsPageTest::integratesIntoTheExistingManagementPage()
{
	// store переживает окно; настоящий OBS и контроллеры не нужны для проверки навигации.
	StateStore store;
	// window представляет единственное расширенное окно со стандартной навигацией.
	ManagementWindow window(store, translatedKey);
	// navigation и pages адресуют существующее меню и стек, а не новый самостоятельный диалог.
	auto *navigation = window.findChild<QListWidget *>(QStringLiteral("managementNavigation"));
	auto *pages = window.findChild<QStackedWidget *>(QStringLiteral("managementPages"));
	QVERIFY(navigation);
	QVERIFY(pages);
	QCOMPARE(navigation->count(), 8);
	QCOMPARE(pages->count(), 8);
	navigation->setCurrentRow(5);
	QCOMPARE(pages->currentIndex(), 5);
	QCOMPARE(window.findChildren<DiagnosticsPage *>().size(), 1);
	// page и scroll доказывают интеграцию диагностики в прокрутку прежнего раздела.
	auto *page = window.findChild<DiagnosticsPage *>(QStringLiteral("diagnosticsPage"));
	auto *scroll = qobject_cast<QScrollArea *>(pages->widget(5));
	QVERIFY(page);
	QVERIFY(scroll);
	QVERIFY(scroll->widgetResizable());
	QVERIFY(scroll->isAncestorOf(page));
	QVERIFY(page->findChild<QLabel *>(QStringLiteral("obsProfile")));
	QVERIFY(page->findChild<QLabel *>(QStringLiteral("obsStreaming")));
}

// Проверяет последовательность Home, Down, PageDown, PageUp и Home в настоящей иерархии ManagementWindow.
// Частично видимое дерево воспроизводит доступную верхнюю строку при скрытых нижних строках секции.
void DiagnosticsPageTest::keepsKeyboardSelectionVisibleInTheManagementViewport()
{
	// store содержит одну реальную группу текущего эфира без сетевого или OBS runtime.
	StateStore store;
	selectContext(store);
	QVERIFY(store.installAnchor(currentAnchor()));
	// window создаёт штатную внешнюю прокрутку, которую самостоятельная страница не воспроизводит.
	ManagementWindow window(store, translatedKey);
	window.resize(916, 639);
	// navigation открывает диагностику; scroll и tree заимствованы у окна до завершения проверки.
	auto *navigation = window.findChild<QListWidget *>(QStringLiteral("managementNavigation"));
	auto *scroll = window.findChild<QScrollArea *>(QStringLiteral("diagnosticsScroll"));
	auto *tree = window.findChild<QTreeWidget *>(QStringLiteral("diagnosticsGroups"));
	QVERIFY(navigation);
	QVERIFY(scroll);
	QVERIFY(tree);
	navigation->setCurrentRow(5);
	window.show();
	window.activateWindow();
	tree->setFocus(Qt::TabFocusReason);
	QTRY_VERIFY_WITH_TIMEOUT(tree->hasFocus(), 1000);
	QTest::keyClick(tree, Qt::Key_Home);
	QTRY_VERIFY_WITH_TIMEOUT(tree->currentItem() == row(tree, "group"), 1000);
	dispatchPendingUpdates();
	// firstRow задаёт высоту строки, а firstTop — её положение во внешней области до ручной прокрутки.
	const auto firstRow = tree->visualItemRect(tree->currentItem());
	const int firstTop = tree->viewport()->mapTo(scroll->viewport(), firstRow.topLeft()).y();
	QVERIFY(firstRow.height() > 0);
	// targetTop оставляет две строки у нижнего края внешней области: Home и Down доступны, PageDown уходит ниже.
	const int targetTop = scroll->viewport()->height() - 2 * firstRow.height();
	scroll->verticalScrollBar()->setValue(scroll->verticalScrollBar()->value() + firstTop - targetTop);
	dispatchPendingUpdates();
	QVERIFY(currentRowIsVisible(*tree, *scroll->viewport()));
	QVERIFY(tree->viewport()->mapTo(scroll->viewport(), tree->viewport()->rect().bottomLeft()).y() >=
		scroll->viewport()->height());
	QVERIFY(tree->verticalScrollBar()->maximum() > 0);
	QTest::keyClick(tree, Qt::Key_Home);
	QTRY_VERIFY_WITH_TIMEOUT(tree->currentItem() == row(tree, "group"), 1000);
	QTRY_VERIFY_WITH_TIMEOUT(currentRowIsVisible(*tree, *scroll->viewport()), 1000);
	QTest::keyClick(tree, Qt::Key_Down);
	QTRY_VERIFY_WITH_TIMEOUT(tree->currentItem() == childRow(row(tree, "group"), "Diagnostics.Instance"), 1000);
	QTRY_VERIFY_WITH_TIMEOUT(currentRowIsVisible(*tree, *scroll->viewport()), 1000);
	// beforePageDown отличает реальное перемещение выбора от одного лишь сохранения клавиатурного фокуса.
	const auto beforePageDown = tree->currentIndex();
	QTest::keyClick(tree, Qt::Key_PageDown);
	QTRY_VERIFY_WITH_TIMEOUT(tree->currentIndex().isValid() && tree->currentIndex() != beforePageDown, 1000);
	QVERIFY(tree->currentItem()->isSelected());
	QTRY_VERIFY_WITH_TIMEOUT(currentRowIsVisible(*tree, *scroll->viewport()), 1000);
	// beforePageUp подтверждает обратное перемещение независимо от конкретного числа строк на странице Qt.
	const auto beforePageUp = tree->currentIndex();
	QTest::keyClick(tree, Qt::Key_PageUp);
	QTRY_VERIFY_WITH_TIMEOUT(tree->currentIndex().isValid() && tree->currentIndex() != beforePageUp, 1000);
	QTRY_VERIFY_WITH_TIMEOUT(currentRowIsVisible(*tree, *scroll->viewport()), 1000);
	QTest::keyClick(tree, Qt::Key_Home);
	QTRY_VERIFY_WITH_TIMEOUT(tree->currentItem() == row(tree, "group"), 1000);
	QTRY_VERIFY_WITH_TIMEOUT(currentRowIsVisible(*tree, *scroll->viewport()), 1000);
	QVERIFY(tree->hasFocus());
}

// Проверяет, что обновления OBS и runtime не возвращают внешнюю прокрутку к ранее выбранной строке.
void DiagnosticsPageTest::preservesManualScrollWhileObservationsRefresh()
{
	// anchor сохраняет тот же состав строк; меняется только измеренное значение группы.
	auto anchor = currentAnchor();
	// store переживает окно и синхронно публикует наблюдения для обычного отложенного refresh.
	StateStore store;
	selectContext(store);
	QVERIFY(store.installAnchor(anchor));
	// window содержит штатные две независимые прокрутки диагностики.
	ManagementWindow window(store, translatedKey);
	window.resize(916, 639);
	// navigation открывает страницу; scroll, tree и obs заимствованы у окна.
	auto *navigation = window.findChild<QListWidget *>(QStringLiteral("managementNavigation"));
	auto *scroll = window.findChild<QScrollArea *>(QStringLiteral("diagnosticsScroll"));
	auto *tree = window.findChild<QTreeWidget *>(QStringLiteral("diagnosticsGroups"));
	auto *obs = window.findChild<QTreeWidget *>(QStringLiteral("diagnosticsOBS"));
	QVERIFY(navigation);
	QVERIFY(scroll);
	QVERIFY(tree);
	QVERIFY(obs);
	navigation->setCurrentRow(5);
	window.show();
	window.activateWindow();
	tree->setFocus(Qt::TabFocusReason);
	QTRY_VERIFY_WITH_TIMEOUT(tree->hasFocus(), 1000);
	QTest::keyClick(tree, Qt::Key_End);
	QTRY_VERIFY_WITH_TIMEOUT(tree->currentIndex().isValid(), 1000);
	dispatchPendingUpdates();
	QVERIFY(tree->verticalScrollBar()->maximum() > 1);
	QVERIFY(scroll->verticalScrollBar()->maximum() > 0);
	tree->verticalScrollBar()->setValue(tree->verticalScrollBar()->maximum() / 2);
	scroll->verticalScrollBar()->setValue(scroll->verticalScrollBar()->maximum());
	dispatchPendingUpdates();
	// innerPosition и outerPosition фиксируют ручной просмотр, selectedText — смысловой выбор до перестройки.
	const int innerPosition = tree->verticalScrollBar()->value();
	const int outerPosition = scroll->verticalScrollBar()->value();
	const auto selectedText = tree->currentItem()->text(0);
	QVERIFY(!currentRowIsVisible(*tree, *scroll->viewport()));
	// statistics меняет различимое OBS-измерение, чтобы ожидание доказывало завершение refresh.
	ObsStatistics statistics;
	statistics.fps = 77.0;
	store.setObsStatistics(statistics);
	QTRY_COMPARE_WITH_TIMEOUT(row(obs, "Diagnostics.FPS")->text(1), QStringLiteral("77"), 1000);
	dispatchPendingUpdates();
	QCOMPARE(tree->verticalScrollBar()->value(), innerPosition);
	QCOMPARE(scroll->verticalScrollBar()->value(), outerPosition);
	QVERIFY(tree->currentItem());
	QCOMPARE(tree->currentItem()->text(0), selectedText);
	anchor.cursor = QStringLiteral("updated-group-measurement");
	anchor.sessions[0].groups[0].metrics->output.videoFrames = 304;
	QVERIFY(store.installAnchor(anchor));
	QTRY_COMPARE_WITH_TIMEOUT(childRow(row(tree, "Diagnostics.GroupOutput"), "Diagnostics.VideoFrames")->text(1),
				  QStringLiteral("304"), 1000);
	dispatchPendingUpdates();
	QCOMPARE(tree->verticalScrollBar()->value(), innerPosition);
	QCOMPARE(scroll->verticalScrollBar()->value(), outerPosition);
	QVERIFY(tree->currentItem());
	QCOMPARE(tree->currentItem()->text(0), selectedText);
	QVERIFY(tree->hasFocus());
}

// Задаёт два источника фоновых обновлений при неизменных ID, исполнениях и именах выходов.
void DiagnosticsPageTest::preservesPendingOutputSelectionWhileObservationsRefresh_data()
{
	QTest::addColumn<bool>("obsRefresh");
	QTest::newRow("obs-statistics") << true;
	QTest::newRow("runtime-measurement") << false;
}

// Воспроизводит Down без подтверждения в настоящем popup штатной страницы управления.
// Ожидание новой видимой метрики подтверждает обработку обновления до проверки подсветки.
void DiagnosticsPageTest::preservesPendingOutputSelectionWhileObservationsRefresh()
{
	// obsRefresh выбирает локальную статистику OBS либо обновление измерения серверной группы.
	QFETCH(bool, obsRefresh);
	// anchor сохраняет идентичности и подписи двух выходов при изменении только группового счётчика.
	auto anchor = currentAnchor();
	// store переживает окно; обновления публикуются без OBS, сети и изменений конфигурации.
	StateStore store;
	selectContext(store);
	QVERIFY(store.installAnchor(anchor));
	// window создаёт обычную страницу с внешней прокруткой и настоящим QComboBox.
	ManagementWindow window(store, translatedKey);
	window.resize(916, 639);
	// navigation открывает диагностику, scroll прокручивает страницу, selector адресует список текущих исполнений Output.
	auto *navigation = window.findChild<QListWidget *>(QStringLiteral("managementNavigation"));
	auto *scroll = window.findChild<QScrollArea *>(QStringLiteral("diagnosticsScroll"));
	auto *selector = window.findChild<QComboBox *>(QStringLiteral("diagnosticsOutputSelector"));
	// obs и groups дают наблюдаемое подтверждение соответствующего фонового обновления.
	auto *obs = window.findChild<QTreeWidget *>(QStringLiteral("diagnosticsOBS"));
	auto *groups = window.findChild<QTreeWidget *>(QStringLiteral("diagnosticsGroups"));
	QVERIFY(navigation);
	QVERIFY(scroll);
	QVERIFY(selector);
	QVERIFY(row(obs, "Diagnostics.FPS"));
	QVERIFY(childRow(row(groups, "Diagnostics.GroupOutput"), "Diagnostics.VideoFrames"));
	navigation->setCurrentRow(5);
	window.show();
	window.activateWindow();
	scroll->ensureWidgetVisible(selector);
	selector->setFocus(Qt::TabFocusReason);
	QTRY_VERIFY_WITH_TIMEOUT(selector->hasFocus(), 1000);
	QCOMPARE(selector->count(), 3);
	QCOMPARE(selector->currentIndex(), 0);
	QCOMPARE(labelText(window, "ackAheadEventsTotal"), QStringLiteral("Observation.NoData"));
	selector->showPopup();
	// popup заимствован у selector; его текущая строка ещё не является подтверждённым выбором.
	auto *popup = selector->view();
	QVERIFY(popup);
	QTRY_VERIFY_WITH_TIMEOUT(popup->isVisible(), 1000);
	QCOMPARE(popup->currentIndex().row(), 0);
	QTest::keyClick(popup, Qt::Key_Down);
	QTRY_COMPARE_WITH_TIMEOUT(popup->currentIndex().row(), 1, 1000);
	QCOMPARE(popup->currentIndex().data(Qt::UserRole).toString(), QStringLiteral("output"));
	QCOMPARE(popup->currentIndex().data(Qt::UserRole + 1).toString(), QStringLiteral("output-instance"));
	QCOMPARE(selector->currentIndex(), 0);
	QCOMPARE(labelText(window, "ackAheadEventsTotal"), QStringLiteral("Observation.NoData"));
	if (obsRefresh) {
		// statistics меняет только локальный FPS, сохраняя серверный список выходов.
		ObsStatistics statistics;
		statistics.fps = 77.0;
		store.setObsStatistics(statistics);
		QTRY_COMPARE_WITH_TIMEOUT(row(obs, "Diagnostics.FPS")->text(1), QStringLiteral("77"), 1000);
	} else {
		anchor.cursor = QStringLiteral("updated-group-measurement");
		anchor.sessions[0].groups[0].metrics->output.videoFrames = 304;
		QVERIFY(store.installAnchor(anchor));
		QTRY_COMPARE_WITH_TIMEOUT(
			childRow(row(groups, "Diagnostics.GroupOutput"), "Diagnostics.VideoFrames")->text(1),
			QStringLiteral("304"), 1000);
	}
	QVERIFY(popup->isVisible());
	QCOMPARE(popup->currentIndex().row(), 1);
	QCOMPARE(popup->currentIndex().data(Qt::UserRole).toString(), QStringLiteral("output"));
	QCOMPARE(popup->currentIndex().data(Qt::UserRole + 1).toString(), QStringLiteral("output-instance"));
	QCOMPARE(selector->currentIndex(), 0);
	QCOMPARE(labelText(window, "ackAheadEventsTotal"), QStringLiteral("Observation.NoData"));
	QTest::keyClick(popup, Qt::Key_Return);
	QTRY_VERIFY_WITH_TIMEOUT(!popup->isVisible(), 1000);
	QCOMPARE(selector->currentData(Qt::UserRole).toString(), QStringLiteral("output"));
	QCOMPARE(selector->currentData(Qt::UserRole + 1).toString(), QStringLiteral("output-instance"));
	QCOMPARE(labelText(window, "ackAheadEventsTotal"), QStringLiteral("17"));
	QCOMPARE(labelText(window, "ackAheadBytesLast"), QStringLiteral("300"));
	QCOMPARE(labelText(window, "ackAheadBytesMax"), QStringLiteral("900"));
	QCOMPARE(labelText(window, "ackLastAnomalyAt"), QStringLiteral("Diagnostics.NotObserved"));
}

void DiagnosticsPageTest::emptyStateRemainsUnknown()
{
	// store ещё не получил ни локальной выборки OBS, ни серверного snapshot.
	StateStore store;
	// page читает только исходное неизвестное состояние.
	DiagnosticsPage page(store, translatedKey);
	// selector начинает с явного placeholder без OutputId и instanceId.
	auto *selector = page.findChild<QComboBox *>(QStringLiteral("diagnosticsOutputSelector"));
	QVERIFY(selector);
	QCOMPARE(selector->count(), 1);
	QVERIFY(selector->currentData(Qt::UserRole).toString().isEmpty());
	QVERIFY(selector->currentData(Qt::UserRole + 1).toString().isEmpty());
	QCOMPARE(labelText(page, "obsProfile"), QStringLiteral("Observation.NoData"));
	QCOMPARE(labelText(page, "obsStreaming"), QStringLiteral("Observation.NoData"));
	QCOMPARE(labelText(page, "ackAheadEventsTotal"), QStringLiteral("Observation.NoData"));
	QCOMPARE(labelText(page, "ackAheadBytesLast"), QStringLiteral("Observation.NoData"));
	QCOMPARE(labelText(page, "ackAheadBytesMax"), QStringLiteral("Observation.NoData"));
	QCOMPARE(labelText(page, "ackLastAnomalyAt"), QStringLiteral("Observation.NoData"));
	QVERIFY(page.findChildren<QPushButton *>().isEmpty());
	QVERIFY(page.findChildren<QNetworkAccessManager *>().isEmpty());
	// gpu подтверждает отсутствие измерений сервера, а не фиктивные 0% локальной видеокарты.
	auto *gpu = page.findChild<QTreeWidget *>(QStringLiteral("diagnosticsGPU"));
	QVERIFY(row(gpu, "Diagnostics.GPU"));
	QCOMPARE(row(gpu, "Diagnostics.GPU")->text(1), QStringLiteral("Observation.NoData"));
	store.setObsState(QStringLiteral("Local OBS profile"), true);
	// statistics задаёт независимое локальное измерение даже при недоступном сервере.
	ObsStatistics statistics;
	statistics.fps = 60.0;
	statistics.sentBytes = 123456;
	store.setObsStatistics(statistics);
	store.setConnection(QStringLiteral("http://diagnostics.invalid:8080"), ConnectionStatus::Error);
	QCOMPARE(labelText(page, "obsProfile"), QStringLiteral("Local OBS profile"));
	QCOMPARE(labelText(page, "obsStreaming"), QStringLiteral("OBS.StreamingActive"));
	QCOMPARE(labelText(page, "diagnosticsConnection"), QStringLiteral("Server.Error"));
	// obs хранит исходные локальные показатели, которые отказ серверного API не делает устаревшими.
	auto *obs = page.findChild<QTreeWidget *>(QStringLiteral("diagnosticsOBS"));
	QVERIFY(row(obs, "Diagnostics.FPS"));
	QTRY_COMPARE_WITH_TIMEOUT(row(obs, "Diagnostics.FPS")->text(1), QStringLiteral("60"), 1000);
	QVERIFY(row(obs, "Diagnostics.SentBytes"));
	QCOMPARE(row(obs, "Diagnostics.SentBytes")->text(1), QStringLiteral("123456"));
	QCOMPARE(labelText(page, "ackAheadEventsTotal"), QStringLiteral("Observation.NoData"));
}

void DiagnosticsPageTest::rendersExactAckWithoutInferringWhichCounterSaturated()
{
	// document содержит независимо заданные API числа, включая U64 max и соседние выше 2^53.
	const auto document = fixture::bytes(fixture::outputMetrics());
	// json, metrics и error связывают реальный decoder с представлением без промежуточного double.
	JsonValue json;
	OutputMetrics metrics;
	Error error;
	QVERIFY(parseJson(document, json));
	QVERIFY(fromJson(json, metrics, error));
	metrics.media.saturated = true;
	// anchor помещает проверенную телеметрию только в первый точный Output.
	auto anchor = currentAnchor();
	anchor.sessions[0].outputs[0].metrics = known(metrics);
	// store и page обеспечивают обычный путь обновления публичного интерфейса.
	StateStore store;
	selectContext(store);
	QVERIFY(store.installAnchor(anchor));
	DiagnosticsPage page(store, translatedKey);
	// selector выбирает OutputId; instanceId отдельно проверяет правильное исполнение.
	auto *selector = page.findChild<QComboBox *>(QStringLiteral("diagnosticsOutputSelector"));
	QVERIFY(selector);
	QVERIFY(selector->findData(QStringLiteral("output")) > 0);
	selector->setCurrentIndex(selector->findData(QStringLiteral("output")));
	QCOMPARE(selector->currentData(Qt::UserRole + 1).toString(), QStringLiteral("output-instance"));
	QCOMPARE(labelText(page, "ackAheadEventsTotal"), QStringLiteral("18446744073709551615"));
	QCOMPARE(labelText(page, "ackAheadBytesLast"), QStringLiteral("9007199254740993"));
	QCOMPARE(labelText(page, "ackAheadBytesMax"), QStringLiteral("18446744073709551614"));
	QCOMPARE(labelText(page, "ackLastAnomalyAt"), QStringLiteral("Diagnostics.NotObserved"));
	QVERIFY(labelText(page, "diagnosticsAckAvailability").contains(QStringLiteral("Diagnostics.AckSaturated")));
	// update сохраняет накопления того же Publisher; last/max и наносекунды времени выводятся буквально.
	auto update = outputEvent(QStringLiteral("metrics"), 1);
	metrics.ackLastAnomalyAt = known(QStringLiteral("2026-11-10T01:02:03.123456789Z"));
	update.output = known(metrics);
	QVERIFY(store.applyEvent(update));
	QTRY_COMPARE_WITH_TIMEOUT(labelText(page, "ackLastAnomalyAt"), QStringLiteral("2026-11-10T01:02:03.123456789Z"),
				  1000);
	QCOMPARE(labelText(page, "ackAheadEventsTotal"), QStringLiteral("18446744073709551615"));
	QCOMPARE(labelText(page, "ackAheadBytesLast"), QStringLiteral("9007199254740993"));
	QCOMPARE(labelText(page, "ackAheadBytesMax"), QStringLiteral("18446744073709551614"));
	QCOMPARE(labelText(page, "ackLastAnomalyAt"), QStringLiteral("2026-11-10T01:02:03.123456789Z"));
}

void DiagnosticsPageTest::missingMetricsDoNotBecomeZero()
{
	// anchor хранит физически ненулевые старые значения в null metrics: JsonField.state остаётся истиной.
	auto anchor = currentAnchor();
	anchor.sessions[0].outputs[0].metrics.state = FieldState::Null;
	anchor.sessions[0].outputs[1].metrics = known(OutputMetrics{});
	// store и page показывают сначала отсутствие, затем измеренный ноль другого выхода.
	StateStore store;
	selectContext(store);
	QVERIFY(store.installAnchor(anchor));
	DiagnosticsPage page(store, translatedKey);
	// selector явно выбирает оба текущих исполнения.
	auto *selector = page.findChild<QComboBox *>(QStringLiteral("diagnosticsOutputSelector"));
	QVERIFY(selector);
	selector->setCurrentIndex(selector->findData(QStringLiteral("output")));
	QCOMPARE(labelText(page, "ackAheadEventsTotal"), QStringLiteral("Observation.NoData"));
	QCOMPARE(labelText(page, "ackAheadBytesLast"), QStringLiteral("Observation.NoData"));
	anchor.cursor = QStringLiteral("missing-output-metrics");
	anchor.sessions[0].outputs[0].metrics.state = FieldState::Missing;
	QVERIFY(store.installAnchor(anchor));
	dispatchPendingUpdates();
	QCOMPARE(labelText(page, "ackAheadEventsTotal"), QStringLiteral("Observation.NoData"));
	selector->setCurrentIndex(selector->findData(QStringLiteral("output-two")));
	QCOMPARE(labelText(page, "ackAheadEventsTotal"), QStringLiteral("0"));
	QCOMPARE(labelText(page, "ackAheadBytesLast"), QStringLiteral("0"));
}

void DiagnosticsPageTest::preservesExactSelectionAndIndependentFreshness()
{
	// store и page содержат два выхода с одинаковым отображаемым именем, но разными идентичностями.
	StateStore store;
	selectContext(store);
	QVERIFY(store.installAnchor(currentAnchor()));
	DiagnosticsPage page(store, translatedKey);
	// selector выбирает первый Output, не полагаясь на порядок или одинаковое имя.
	auto *selector = page.findChild<QComboBox *>(QStringLiteral("diagnosticsOutputSelector"));
	QVERIFY(selector);
	selector->setCurrentIndex(selector->findData(QStringLiteral("output")));
	// outputTree сохраняет очереди, доставку и reconnects раздельно, без общего показателя потерь кадров.
	auto *outputTree = page.findChild<QTreeWidget *>(QStringLiteral("diagnosticsOutput"));
	QVERIFY(childRow(row(outputTree, "Diagnostics.QueueDrops"), "video"));
	QVERIFY(childRow(row(outputTree, "Diagnostics.DeliveryDrops"), "video"));
	QVERIFY(childRow(row(outputTree, "Diagnostics.Counters"), "reconnects"));
	QCOMPARE(childRow(row(outputTree, "Diagnostics.QueueDrops"), "video")->text(1), QStringLiteral("3"));
	QCOMPARE(childRow(row(outputTree, "Diagnostics.DeliveryDrops"), "video")->text(1), QStringLiteral("7"));
	QCOMPARE(childRow(row(outputTree, "Diagnostics.Counters"), "reconnects")->text(1), QStringLiteral("2"));
	store.markRuntimeStale();
	store.setConnection(store.serverOrigin(), ConnectionStatus::Reconnecting);
	store.setConnection(store.serverOrigin(), ConnectionStatus::Connected);
	QTRY_VERIFY_WITH_TIMEOUT(
		labelText(page, "diagnosticsAckAvailability").contains(QStringLiteral("Diagnostics.Stale")), 1000);
	QCOMPARE(selector->currentData().toString(), QStringLiteral("output"));
	QCOMPARE(selector->currentData(Qt::UserRole + 1).toString(), QStringLiteral("output-instance"));
	QCOMPARE(labelText(page, "ackAheadEventsTotal"), QStringLiteral("17"));
	QVERIFY(labelText(page, "diagnosticsAckAvailability").contains(QStringLiteral("Diagnostics.Stale")));
	// update освежает состояние RETRYING, но не подтверждает прежние ACK.
	auto update = outputEvent(QStringLiteral("state"), 11);
	QVERIFY(store.applyEvent(update));
	QTRY_COMPARE_WITH_TIMEOUT(row(outputTree, "Diagnostics.State")->text(1),
				  QStringLiteral("Dock.OutputReconnecting"), 1000);
	QVERIFY(labelText(page, "diagnosticsAckAvailability").contains(QStringLiteral("Diagnostics.Stale")));
	update = outputEvent(QStringLiteral("metrics"), 1);
	update.output = known(OutputMetrics{});
	update.output->ackAheadEventsTotal = 18;
	QVERIFY(store.applyEvent(update));
	QTRY_COMPARE_WITH_TIMEOUT(labelText(page, "ackAheadEventsTotal"), QStringLiteral("18"), 1000);
	QCOMPARE(selector->currentData().toString(), QStringLiteral("output"));
	QCOMPARE(labelText(page, "ackAheadEventsTotal"), QStringLiteral("18"));
	QCOMPARE(labelText(page, "diagnosticsAckAvailability"), QStringLiteral("Diagnostics.Confirmed"));
}

void DiagnosticsPageTest::replacementExecutionsClearSelection()
{
	// store и page начинают с выбранного первого исполнения.
	StateStore store;
	selectContext(store);
	QVERIFY(store.installAnchor(currentAnchor()));
	DiagnosticsPage page(store, translatedKey);
	// selector не должен переносить выбор на новый Publisher с прежним OutputId.
	auto *selector = page.findChild<QComboBox *>(QStringLiteral("diagnosticsOutputSelector"));
	QVERIFY(selector);
	selector->setCurrentIndex(selector->findData(QStringLiteral("output")));
	// replacement заменяет исполнение тем же конфигурационным ID и известными нулевыми накоплениями.
	auto replacement = outputEvent(QStringLiteral("metrics"), 1);
	replacement.observation.instanceId = QStringLiteral("replacement-output");
	replacement.output = known(OutputMetrics{});
	QVERIFY(store.applyEvent(replacement));
	QVERIFY(selector->currentData().toString().isEmpty());
	QTRY_COMPARE_WITH_TIMEOUT(labelText(page, "ackAheadEventsTotal"), QStringLiteral("Observation.NoData"), 1000);
	selector->setCurrentIndex(selector->findData(QStringLiteral("output")));
	QCOMPARE(selector->currentData(Qt::UserRole + 1).toString(), QStringLiteral("replacement-output"));
	QCOMPARE(labelText(page, "ackAheadEventsTotal"), QStringLiteral("0"));
	// anchor заменяет исполнение Session при том же долговечном ID, отзывая даже прежний точный выбор.
	auto anchor = currentAnchor();
	anchor.cursor = QStringLiteral("replacement-anchor");
	anchor.sessions[0].observation->instanceId = QStringLiteral("replacement-session");
	anchor.sessions[0].outputs[0].instanceId = QStringLiteral("replacement-output");
	QVERIFY(store.installAnchor(anchor));
	QVERIFY(selector->currentData().toString().isEmpty());
	QTRY_COMPARE_WITH_TIMEOUT(labelText(page, "ackAheadEventsTotal"), QStringLiteral("Observation.NoData"), 1000);
	selector->setCurrentIndex(selector->findData(QStringLiteral("output")));
	// metadata переводит выбор на другой долговечный ID Session, даже пока прежнее дерево остаётся в RAM.
	auto metadata = store.dockServer();
	metadata.session->id = QStringLiteral("next-session");
	store.setDockServer(metadata);
	QTRY_COMPARE_WITH_TIMEOUT(selector->count(), 1, 1000);
	QCOMPARE(labelText(page, "ackAheadEventsTotal"), QStringLiteral("Observation.NoData"));
}

void DiagnosticsPageTest::contextChangesClearSelection_data()
{
	QTest::addColumn<QString>("coordinate");
	QTest::newRow("server") << QStringLiteral("server");
	QTest::newRow("profile") << QStringLiteral("profile");
	QTest::newRow("stream") << QStringLiteral("stream");
}

void DiagnosticsPageTest::contextChangesClearSelection()
{
	// coordinate выбирает изменяемую часть устойчивого контекста.
	QFETCH(QString, coordinate);
	// store и page содержат прежнее дерево, которое не удаляется одним изменением выбора.
	StateStore store;
	selectContext(store);
	QVERIFY(store.installAnchor(currentAnchor()));
	DiagnosticsPage page(store, translatedKey);
	// selector адресует Output исходного контекста.
	auto *selector = page.findChild<QComboBox *>(QStringLiteral("diagnosticsOutputSelector"));
	QVERIFY(selector);
	selector->setCurrentIndex(selector->findData(QStringLiteral("output")));
	if (coordinate == QStringLiteral("server"))
		store.setConnection(QStringLiteral("http://other.invalid:8080"), ConnectionStatus::Connected);
	else
		store.setSelection(coordinate == QStringLiteral("stream") ? QStringLiteral("other-stream")
									  : QStringLiteral("stream"),
				   true, {}, {},
				   coordinate == QStringLiteral("profile") ? QStringLiteral("other-profile")
									   : QStringLiteral("obs-profile"));
	QTRY_COMPARE_WITH_TIMEOUT(selector->count(), 1, 1000);
	QVERIFY(selector->currentData().toString().isEmpty());
	QCOMPARE(labelText(page, "ackAheadEventsTotal"), QStringLiteral("Observation.NoData"));
	QVERIFY(!visibleText(page).contains(QStringLiteral("output-instance")));
}

void DiagnosticsPageTest::excludesForeignSessionsHistoryAndFailurePayloads()
{
	// anchor дополнен чужой Session с уникальными маркерами, чтобы выбор первого runtime не прошёл проверку.
	auto anchor = currentAnchor();
	// foreign сохраняет независимые instanceId во всём дереве и не относится к текущим REST-метаданным.
	RuntimeSession foreign;
	foreign.sessionId = QStringLiteral("foreign-session-marker");
	foreign.observation = known(observed(QStringLiteral("foreign-instance-marker"), QStringLiteral("RUNNING")));
	foreign.outputs.append(anchor.sessions[0].outputs[0]);
	foreign.outputs[0].instanceId = QStringLiteral("foreign-output-marker");
	foreign.outputs[0].outputId = QStringLiteral("foreign-output-id-marker");
	anchor.sessions.prepend(foreign);
	// failure размещает секрет в каждом недопустимом для страницы поле, а не в разрешённом имени назначения.
	Failure failure;
	failure.message = QStringLiteral("secret-failure-message-marker");
	failure.action = QStringLiteral("secret-failure-action-marker");
	failure.diagnosticBody = QByteArrayLiteral("secret-raw-payload-marker");
	anchor.sessions[1].outputs[0].failure = known(failure);
	anchor.sessions[1].ffmpeg[0].metrics->reason = QStringLiteral("secret-ffmpeg-reason-marker");
	// store и page показывают только Session из текущего DockContext.
	StateStore store;
	selectContext(store);
	QVERIFY(store.installAnchor(anchor));
	DiagnosticsPage page(store, translatedKey);
	// history содержит полученный позднее список прошлых эфиров, который не должен откатить runtime.
	HistoryState history;
	// historical задаёт узнаваемый прошлый эфир для проверки отсутствия его данных на странице.
	Session historical;
	historical.id = QStringLiteral("history-session-marker");
	history.sessions.insert(historical.id, historical);
	store.setHistory(history);
	// details содержит позднее подробное чтение, которое не должно заменять текущие наблюдения.
	DetailState details;
	// oldDetail связывает прежнее исполнение с тем же OutputId, чтобы выявить подмену по одному ID.
	OutputDetail oldDetail;
	oldDetail.id = QStringLiteral("output");
	oldDetail.runtime = known(anchor.sessions[0].outputs[0]);
	details.outputs.insert(oldDetail.id, oldDetail);
	store.setDetails(details);
	dispatchPendingUpdates();
	// selector содержит только два собственных выхода, включая одинаковые безопасные имена.
	auto *selector = page.findChild<QComboBox *>(QStringLiteral("diagnosticsOutputSelector"));
	QVERIFY(selector);
	QCOMPARE(selector->count(), 3);
	QCOMPARE(selector->findData(QStringLiteral("foreign-output-id-marker")), -1);
	selector->setCurrentIndex(selector->findData(QStringLiteral("output")));
	QCOMPARE(labelText(page, "ackAheadEventsTotal"), QStringLiteral("17"));
	// text охватывает также подсказки, чтобы случайный перенос причины в них обнаруживался.
	const auto text = visibleText(page);
	QVERIFY(!text.contains(QStringLiteral("foreign-")));
	QVERIFY(!text.contains(QStringLiteral("history-session-marker")));
	QVERIFY(!text.contains(QStringLiteral("secret-")));
}

void DiagnosticsPageTest::separatesInputAndGroupMeasurements()
{
	// store и page содержат нулевые накопления входа и три разные величины групповых направлений.
	StateStore store;
	selectContext(store);
	QVERIFY(store.installAnchor(currentAnchor()));
	DiagnosticsPage page(store, translatedKey);
	// input и groups содержат разные источники, которые нельзя складывать или освежать вместе.
	auto *input = page.findChild<QTreeWidget *>(QStringLiteral("diagnosticsInput"));
	auto *groups = page.findChild<QTreeWidget *>(QStringLiteral("diagnosticsGroups"));
	QVERIFY(row(input, "Diagnostics.VideoFrames"));
	QCOMPARE(row(input, "Diagnostics.VideoFrames")->text(1), QStringLiteral("0"));
	QVERIFY(row(input, "Diagnostics.FPS"));
	QCOMPARE(row(input, "Diagnostics.FPS")->text(1), QStringLiteral("Observation.NoData"));
	QVERIFY(row(input, "Diagnostics.FPS")->text(2).contains(QStringLiteral("Dock.Measuring")));
	QVERIFY(childRow(row(groups, "Diagnostics.LiveInput"), "Diagnostics.VideoFrames"));
	QVERIFY(childRow(row(groups, "Diagnostics.FallbackInput"), "Diagnostics.VideoFrames"));
	QVERIFY(childRow(row(groups, "Diagnostics.GroupOutput"), "Diagnostics.VideoFrames"));
	QCOMPARE(childRow(row(groups, "Diagnostics.LiveInput"), "Diagnostics.VideoFrames")->text(1),
		 QStringLiteral("101"));
	QCOMPARE(childRow(row(groups, "Diagnostics.FallbackInput"), "Diagnostics.VideoFrames")->text(1),
		 QStringLiteral("202"));
	QCOMPARE(childRow(row(groups, "Diagnostics.GroupOutput"), "Diagnostics.VideoFrames")->text(1),
		 QStringLiteral("303"));
	store.markRuntimeStale();
	// update подтверждает лишь Session state: connected и FPS остаются устаревшими независимо.
	RuntimeEvent update;
	update.kind = QStringLiteral("state");
	update.owner = QStringLiteral("session");
	update.sessionId = QStringLiteral("session");
	update.cursor = QStringLiteral("session-state-fresh");
	update.observation = observed(QStringLiteral("session-instance"), QStringLiteral("RUNNING"));
	update.observation.revision = 11;
	QVERIFY(store.applyEvent(update));
	QTRY_COMPARE_WITH_TIMEOUT(row(input, "Diagnostics.InputConnected")->text(2),
				  QStringLiteral("Diagnostics.Stale"), 1000);
	QVERIFY(row(input, "Diagnostics.State"));
	QCOMPARE(row(input, "Diagnostics.State")->text(2), QStringLiteral("Diagnostics.Confirmed"));
	QVERIFY(row(input, "Diagnostics.InputConnected"));
	QCOMPARE(row(input, "Diagnostics.InputConnected")->text(2), QStringLiteral("Diagnostics.Stale"));
	QVERIFY(row(input, "Diagnostics.FPS")->text(2).contains(QStringLiteral("Diagnostics.Stale")));
}

void DiagnosticsPageTest::preservesSignedFfmpegTimeAndOptionalProgress_data()
{
	QTest::addColumn<qint64>("microseconds");
	QTest::addColumn<QString>("expected");
	QTest::newRow("negative-above-double-precision")
		<< qint64{-9007199254740993LL} << QStringLiteral("-9007199254740993");
	QTest::newRow("minimum-signed-time")
		<< std::numeric_limits<qint64>::min() << QStringLiteral("-9223372036854775808");
	QTest::newRow("maximum-signed-time")
		<< std::numeric_limits<qint64>::max() << QStringLiteral("9223372036854775807");
}

void DiagnosticsPageTest::preservesSignedFfmpegTimeAndOptionalProgress()
{
	// microseconds задаёт точное исходное I64, expected — независимое десятичное представление микросекунд.
	QFETCH(qint64, microseconds);
	QFETCH(QString, expected);
	// anchor задаёт частичный progress без fps/frame с исходным знаковым временем.
	auto anchor = currentAnchor();
	anchor.sessions[0].ffmpeg[0].metrics->progress->outTimeUs = known(microseconds);
	anchor.sessions[0].ffmpeg[0].metrics->progress->fps.state = FieldState::Null;
	anchor.sessions[0].ffmpeg[0].metrics->progress->speed = known(1.25);
	// store и page показывают исходные микросекунды без преобразования через double или миллисекунды.
	StateStore store;
	selectContext(store);
	QVERIFY(store.installAnchor(anchor));
	DiagnosticsPage page(store, translatedKey);
	// tree содержит ровно одну попытку, поэтому каждый ключ progress однозначен.
	auto *tree = page.findChild<QTreeWidget *>(QStringLiteral("diagnosticsFFmpeg"));
	QVERIFY(row(tree, "Diagnostics.OutTimeUs"));
	QCOMPARE(row(tree, "Diagnostics.OutTimeUs")->text(1), expected);
	QVERIFY(row(tree, "Diagnostics.FPS"));
	QCOMPARE(row(tree, "Diagnostics.FPS")->text(1), QStringLiteral("Observation.NoData"));
	QVERIFY(row(tree, "Diagnostics.VideoFrames"));
	QCOMPARE(row(tree, "Diagnostics.VideoFrames")->text(1), QStringLiteral("Observation.NoData"));
	QVERIFY(row(tree, "Diagnostics.Speed"));
	QCOMPARE(row(tree, "Diagnostics.Speed")->text(1), QStringLiteral("1.25"));
	anchor.cursor = QStringLiteral("nullable-progress-time");
	anchor.sessions[0].ffmpeg[0].metrics->progress->outTimeUs.state = FieldState::Null;
	QVERIFY(store.installAnchor(anchor));
	QTRY_COMPARE_WITH_TIMEOUT(row(tree, "Diagnostics.OutTimeUs")->text(1), QStringLiteral("Observation.NoData"),
				  1000);
	anchor.cursor = QStringLiteral("no-progress");
	anchor.sessions[0].ffmpeg[0].metrics->progress.state = FieldState::Null;
	QVERIFY(store.installAnchor(anchor));
	QTRY_VERIFY_WITH_TIMEOUT(row(tree, "Diagnostics.Progress"), 1000);
	QCOMPARE(row(tree, "Diagnostics.Progress")->text(1), QStringLiteral("Observation.NoData"));
	QVERIFY(!row(tree, "Diagnostics.OutTimeUs"));
}

void DiagnosticsPageTest::keepsGpuFieldsIndependentAcrossReconnect()
{
	// anchor получает серверное устройство с независимыми свежими, старыми, будущими и неверными временами.
	auto anchor = currentAnchor();
	// gpu задаёт большой порог в одну минуту, чтобы граничные результаты не зависели от скорости CI.
	GPU gpu;
	gpu.status = QStringLiteral("ok");
	gpu.staleAfterNs = 60000000000ULL;
	// now и old сохраняют серверные временные строки; lastSuccessAt не заменяет sampledAt.
	const auto now = QDateTime::currentDateTimeUtc();
	const auto old = now.addSecs(-600).toString(Qt::ISODateWithMs);
	// device содержит нулевое реальное измерение и недоступный encoder без выдуманного нуля.
	GPUDevice device;
	device.uuid = QStringLiteral("server-gpu-uuid");
	device.utilization.status = QStringLiteral("ok");
	device.utilization.value = known(quint64{0});
	device.utilization.sampledAt = known(now.toString(Qt::ISODateWithMs));
	device.encoderUtilization.status = QStringLiteral("unsupported");
	device.encoderUtilization.value.state = FieldState::Null;
	device.memoryUsedBytes.status = QStringLiteral("ok");
	device.memoryUsedBytes.value = known(std::numeric_limits<quint64>::max());
	device.memoryUsedBytes.sampledAt = known(old);
	device.memoryUsedBytes.lastSuccessAt = known(old);
	device.decoderUtilization.status = QStringLiteral("ok");
	device.decoderUtilization.value = known(quint64{8});
	device.decoderUtilization.sampledAt = known(now.addSecs(600).toString(Qt::ISODateWithMs));
	device.memoryUtilization.status = QStringLiteral("ok");
	device.memoryUtilization.value = known(quint64{12});
	device.memoryUtilization.sampledAt = known(QStringLiteral("invalid-server-time"));
	gpu.devices.append(device);
	anchor.gpu = known(gpu);
	// store и page используют общий StateStore, но не считают reconnect обновлением GPU.
	StateStore store;
	selectContext(store);
	QVERIFY(store.installAnchor(anchor));
	DiagnosticsPage page(store, translatedKey);
	// tree содержит отдельные статусы и служебные метки каждого поля.
	auto *tree = page.findChild<QTreeWidget *>(QStringLiteral("diagnosticsGPU"));
	QVERIFY(row(tree, "Diagnostics.Utilization"));
	QCOMPARE(row(tree, "Diagnostics.Utilization")->text(1), QStringLiteral("0"));
	QVERIFY(!row(tree, "Diagnostics.Utilization")->text(2).contains(QStringLiteral("Diagnostics.Stale")));
	QVERIFY(row(tree, "Diagnostics.EncoderUtilization"));
	QCOMPARE(row(tree, "Diagnostics.EncoderUtilization")->text(1), QStringLiteral("Observation.NoData"));
	QVERIFY(row(tree, "Diagnostics.EncoderUtilization")
			->text(2)
			.contains(QStringLiteral("Diagnostics.Unsupported")));
	QVERIFY(row(tree, "Diagnostics.MemoryUsed"));
	QCOMPARE(row(tree, "Diagnostics.MemoryUsed")->text(1), QStringLiteral("18446744073709551615"));
	QVERIFY(row(tree, "Diagnostics.MemoryUsed")->text(2).contains(QStringLiteral("Diagnostics.Stale")));
	QVERIFY(childRow(row(tree, "Diagnostics.MemoryUsed"), "Diagnostics.LastSuccess"));
	QCOMPARE(childRow(row(tree, "Diagnostics.MemoryUsed"), "Diagnostics.LastSuccess")->text(1), old);
	QVERIFY(row(tree, "Diagnostics.DecoderUtilization"));
	QVERIFY(row(tree, "Diagnostics.DecoderUtilization")
			->text(2)
			.contains(QStringLiteral("Diagnostics.TimeUnconfirmed")));
	QVERIFY(row(tree, "Diagnostics.MemoryUtilization"));
	QVERIFY(row(tree, "Diagnostics.MemoryUtilization")
			->text(2)
			.contains(QStringLiteral("Diagnostics.TimeUnconfirmed")));
	store.markRuntimeStale();
	store.setConnection(store.serverOrigin(), ConnectionStatus::Reconnecting);
	store.setConnection(store.serverOrigin(), ConnectionStatus::Connected);
	// update поступает от другого владельца и не освежает даже ещё не просроченное измерение utilization.
	auto update = outputEvent(QStringLiteral("state"), 11);
	QVERIFY(store.applyEvent(update));
	QTRY_VERIFY_WITH_TIMEOUT(
		row(tree, "Diagnostics.Utilization")->text(2).contains(QStringLiteral("Diagnostics.Stale")), 1000);
	QVERIFY(row(tree, "Diagnostics.MemoryUsed")->text(2).contains(QStringLiteral("Diagnostics.Stale")));
	QCOMPARE(childRow(row(tree, "Diagnostics.MemoryUsed"), "Diagnostics.SampledAt")->text(1), old);
	// gpuUpdate подтверждает только собственное GPU-наблюдение; старое поле памяти остаётся просроченным по TTL.
	RuntimeEvent gpuUpdate;
	gpuUpdate.kind = QStringLiteral("gpu");
	gpuUpdate.owner = QStringLiteral("system");
	gpuUpdate.cursor = QStringLiteral("fresh-gpu-event");
	gpuUpdate.observation.instanceId = QStringLiteral("gpu-instance");
	gpuUpdate.observation.revision = 1;
	gpuUpdate.gpu = known(gpu);
	QVERIFY(store.applyEvent(gpuUpdate));
	QTRY_VERIFY_WITH_TIMEOUT(
		!row(tree, "Diagnostics.Utilization")->text(2).contains(QStringLiteral("Diagnostics.Stale")), 1000);
	QVERIFY(row(tree, "Diagnostics.MemoryUsed")->text(2).contains(QStringLiteral("Diagnostics.Stale")));
	store.markRuntimeStale();
	gpuUpdate.cursor = QStringLiteral("duplicate-gpu-revision");
	QVERIFY(store.applyEvent(gpuUpdate));
	QTRY_VERIFY_WITH_TIMEOUT(
		row(tree, "Diagnostics.Utilization")->text(2).contains(QStringLiteral("Diagnostics.Stale")), 1000);
	gpuUpdate.cursor = QStringLiteral("newer-gpu-revision");
	gpuUpdate.observation.revision = 2;
	QVERIFY(store.applyEvent(gpuUpdate));
	QTRY_VERIFY_WITH_TIMEOUT(
		!row(tree, "Diagnostics.Utilization")->text(2).contains(QStringLiteral("Diagnostics.Stale")), 1000);
	// removed должен удалить точное собственное GPU-наблюдение, не оставляя чисел прежнего устройства.
	RuntimeEvent removed;
	removed.kind = QStringLiteral("removed");
	removed.owner = QStringLiteral("system");
	removed.cursor = QStringLiteral("removed-current-gpu");
	removed.observation.instanceId = gpuUpdate.observation.instanceId;
	QVERIFY(store.applyEvent(removed));
	QTRY_VERIFY_WITH_TIMEOUT(row(tree, "Diagnostics.GPU"), 1000);
	QCOMPARE(row(tree, "Diagnostics.GPU")->text(1), QStringLiteral("Observation.NoData"));
	QVERIFY(!visibleText(page).contains(device.uuid));
}

// Проверяет границу ожидающего снимка через публичные значения и статусы GPU/ACK на странице.
void DiagnosticsPageTest::waitsForSubscriptionBeforeConfirmingGpuAndClearsRuntime()
{
	// anchor содержит измеренный GPU и текущий Output, ещё не подтверждённые подпиской SSE.
	auto anchor = currentAnchor();
	// gpu задаёт независимый серверный статус и минутный срок свежести, исключающий гонку с TTL.
	GPU gpu;
	gpu.status = QStringLiteral("ok");
	gpu.staleAfterNs = 60000000000ULL;
	// device содержит узнаваемое серверное устройство с достоверным ненулевым измерением.
	GPUDevice device;
	device.uuid = QStringLiteral("pending-server-gpu");
	device.utilization.status = QStringLiteral("ok");
	device.utilization.value = known(quint64{42});
	device.utilization.sampledAt = known(QDateTime::currentDateTimeUtc().toString(Qt::ISODateWithMs));
	gpu.devices.append(device);
	anchor.gpu = known(gpu);
	// store хранит ожидающий снимок, page наблюдает его через тот же публичный интерфейс.
	StateStore store;
	selectContext(store);
	store.setConnection(store.serverOrigin(), ConnectionStatus::Reconnecting);
	QVERIFY(store.installAnchor(anchor, true));
	DiagnosticsPage page(store, translatedKey);
	// tree адресует независимые GPU-поля, selector явно выбирает конкретный Output для проверки ACK.
	auto *tree = page.findChild<QTreeWidget *>(QStringLiteral("diagnosticsGPU"));
	auto *selector = page.findChild<QComboBox *>(QStringLiteral("diagnosticsOutputSelector"));
	QVERIFY(selector);
	selector->setCurrentIndex(selector->findData(QStringLiteral("output")));
	QVERIFY(row(tree, "Diagnostics.Utilization"));
	QCOMPARE(row(tree, "Diagnostics.Utilization")->text(1), QStringLiteral("42"));
	QVERIFY(row(tree, "Diagnostics.Utilization")->text(2).startsWith(QStringLiteral("Observation.NoData")));
	QCOMPARE(labelText(page, "diagnosticsAckAvailability"), QStringLiteral("Observation.NoData"));
	store.markRuntimeStale();
	dispatchPendingUpdates();
	QVERIFY(row(tree, "Diagnostics.Utilization")->text(2).startsWith(QStringLiteral("Observation.NoData")));
	QCOMPARE(labelText(page, "diagnosticsAckAvailability"), QStringLiteral("Observation.NoData"));
	store.setConnection(store.serverOrigin(), ConnectionStatus::Connected);
	QTRY_VERIFY_WITH_TIMEOUT(
		row(tree, "Diagnostics.Utilization")->text(2).startsWith(QStringLiteral("Diagnostics.Confirmed")),
		1000);
	QVERIFY(!row(tree, "Diagnostics.Utilization")->text(2).contains(QStringLiteral("Diagnostics.Stale")));
	QCOMPARE(row(tree, "Diagnostics.Utilization")->text(1), QStringLiteral("42"));
	QCOMPARE(labelText(page, "diagnosticsAckAvailability"), QStringLiteral("Diagnostics.Confirmed"));
	QCOMPARE(labelText(page, "ackAheadEventsTotal"), QStringLiteral("17"));
	store.clearRuntime();
	QTRY_VERIFY_WITH_TIMEOUT(row(tree, "Diagnostics.GPU"), 1000);
	QCOMPARE(row(tree, "Diagnostics.GPU")->text(1), QStringLiteral("Observation.NoData"));
	QVERIFY(!row(tree, "Diagnostics.Utilization"));
	QVERIFY(!visibleText(page).contains(device.uuid));
	QCOMPARE(selector->count(), 1);
	QVERIFY(selector->currentData().toString().isEmpty());
	QCOMPARE(labelText(page, "ackAheadEventsTotal"), QStringLiteral("Observation.NoData"));
	QCOMPARE(labelText(page, "diagnosticsAckAvailability"), QStringLiteral("Observation.NoData"));
}

void DiagnosticsPageTest::agesGpuWithoutEventsAndReleasesItsTimer()
{
	// anchor содержит свежую GPU-выборку с порогом 1 с: она должна устареть без нового события.
	auto anchor = currentAnchor();
	// gpu и device задают одну реально измеренную величину со своим sampledAt.
	GPU gpu;
	GPUDevice device;
	gpu.status = QStringLiteral("ok");
	gpu.staleAfterNs = 1000000000ULL;
	device.uuid = QStringLiteral("timer-server-gpu");
	device.utilization.status = QStringLiteral("ok");
	device.utilization.value = known(quint64{42});
	device.utilization.sampledAt = known(QDateTime::currentDateTimeUtc().toString(Qt::ISODateWithMs));
	gpu.devices.append(device);
	anchor.gpu = known(gpu);
	// store переживает удаление страницы, чтобы поздний сигнал проверял отсоединение получателя.
	StateStore store;
	selectContext(store);
	QVERIFY(store.installAnchor(anchor));
	// page освобождает виджеты и при провале проверки; timerGuard наблюдает удаление её таймера.
	auto page = std::make_unique<DiagnosticsPage>(store, translatedKey);
	page->show();
	// refreshedAnchor устанавливает sampledAt уже после создания виджетов, исключая влияние времени конструктора.
	auto refreshedAnchor = anchor;
	refreshedAnchor.cursor = QStringLiteral("fresh-gpu-before-age-wait");
	refreshedAnchor.gpu->devices[0].utilization.sampledAt =
		known(QDateTime::currentDateTimeUtc().toString(Qt::ISODateWithMs));
	QVERIFY(store.installAnchor(refreshedAnchor));
	dispatchPendingUpdates();
	// tree содержит временную величину; её статус проверяется условным ограниченным ожиданием.
	auto *tree = page->findChild<QTreeWidget *>(QStringLiteral("diagnosticsGPU"));
	QVERIFY(row(tree, "Diagnostics.Utilization"));
	QVERIFY(!row(tree, "Diagnostics.Utilization")->text(2).contains(QStringLiteral("Diagnostics.Stale")));
	QTRY_VERIFY_WITH_TIMEOUT(
		row(tree, "Diagnostics.Utilization")->text(2).contains(QStringLiteral("Diagnostics.Stale")), 5000);
	QCOMPARE(row(tree, "Diagnostics.Utilization")->text(1), QStringLiteral("42"));
	// timers содержит только таймеры страницы; timerGuard наблюдает первый из них без владения.
	const auto timers = page->findChildren<QTimer *>();
	QVERIFY(!timers.isEmpty());
	QPointer<QTimer> timerGuard(timers.front());
	page.reset();
	QVERIFY(timerGuard.isNull());
	store.setObsState(QStringLiteral("After page destruction"), true);
}

QTEST_MAIN(DiagnosticsPageTest)
#include "diagnostics-page-test.moc"
