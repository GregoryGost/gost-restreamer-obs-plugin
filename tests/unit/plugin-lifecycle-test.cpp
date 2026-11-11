#include "fixtures/fake-frontend.hpp"
#include "fixtures/api-json.hpp"
#include "fixtures/http-server.hpp"
#include "fixtures/workspace-temp.hpp"
#include "core/plugin-lifecycle.hpp"
#include "core/connection-controller.hpp"
#include "core/settings-store.hpp"
#include "core/state-store.hpp"
#include "ui/management-window.hpp"
#include "ui/restream-dock.hpp"
#include "api/api-client.hpp"
#include "api/event-client.hpp"
#include "obs/obs-stats-collector.hpp"

#include <QAbstractItemView>
#include <QApplication>
#include <QCheckBox>
#include <QComboBox>
#include <QCoreApplication>
#include <QEvent>
#include <QElapsedTimer>
#include <QLineEdit>
#include <QLabel>
#include <QListView>
#include <QListWidget>
#include <QMap>
#include <QPalette>
#include <QPushButton>
#include <QScopeGuard>
#include <QSettings>
#include <QSignalSpy>
#include <QScrollBar>
#include <QSpinBox>
#include <QStackedWidget>
#include <QTabWidget>
#include <QTest>
#include <QToolButton>
#include <QTimer>

#include <utility>
#include <thread>

namespace {

gost::TextLookup localeLookup(const QString &locale = QStringLiteral("ru-RU"))
{
	QSettings settings(QStringLiteral(GOST_TEST_LOCALE_DIR) + QLatin1Char('/') + locale + QStringLiteral(".ini"),
			   QSettings::IniFormat);
	QMap<QString, QString> strings;
	for (const auto &key : settings.allKeys())
		strings.insert(key, settings.value(key).toString());
	return [strings = std::move(strings)](const char *key) {
		const auto found = strings.constFind(QString::fromUtf8(key));
		if (found == strings.cend() || found->isEmpty()) {
			QTest::qFail("Отсутствует используемый перевод интерфейса.", __FILE__, __LINE__);
			return QString::fromUtf8(key);
		}
		return *found;
	};
}

QList<gost::ManagementWindow *> managementWindows()
{
	QList<gost::ManagementWindow *> result;
	for (auto *widget : QApplication::allWidgets()) {
		if (auto *window = dynamic_cast<gost::ManagementWindow *>(widget))
			result.append(window);
	}
	return result;
}

void dispatchQueuedEvents()
{
	QCoreApplication::sendPostedEvents(nullptr, QEvent::MetaCall);
	QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
}

} // namespace

class PluginLifecycleTest final : public QObject {
	Q_OBJECT

private slots:
	void stateStartsUnobserved();
	void stateChangesAreObservable();
	void registersBeforeFrontendReady();
	void observesCurrentProfileAndStreaming_data();
	void observesCurrentProfileAndStreaming();
	void obsStatisticsUseMonotonicIntervalsAndIndependentFramePairs();
	void obsStatisticsLoseTheRateAtOutputReplacementAndTransitions();
	void obsStatisticsStopBeforeExitReturns();
	void bothViewsFollowObservations_data();
	void bothViewsFollowObservations();
	void statusAppearanceFollowsStateAndPalette();
	void managementWindowIsLazyAndReused_data();
	void managementWindowIsLazyAndReused();
	void dockStaysCompact();
	void dockResetChangesOnlyDisplayedAccumulators();
	void dockShowsNativeCounterLimitsIndependently_data();
	void dockShowsNativeCounterLimitsIndependently();
	void dockKeepsLastValuesOnlyInTheSameContext_data();
	void dockKeepsLastValuesOnlyInTheSameContext();
	void managementNavigationHasLocalizedPagesAndIcons_data();
	void managementNavigationHasLocalizedPagesAndIcons();
	void failedDockRegistrationReleasesResources();
	void registrationExceptionReleasesAcquiredResources_data();
	void registrationExceptionReleasesAcquiredResources();
	void callbackExceptionDoesNotEscape();
	void exitCleansSynchronouslyAndLeavesInertShell();
	void queuedOpenCannotResurrectRuntime();
	void queuedFrontendEventIsDiscardedOnExit();
	void hostDestructionDoesNotDoubleDelete();
	void localesAreComplete();
	void hiddenManagementKeepsOneSubscriptionAndMetadataVersion();
	void largeStreamListAndRuntimeTrafficAllowGuiProgress();
	void runtimeUpdatePreservesOpenStreamPopup();
	void savedStreamIsUnconfirmedUntilListIsComplete();
	void partialStreamListWarnsAndAllowsSelection_data();
	void partialStreamListWarnsAndAllowsSelection();
};

void PluginLifecycleTest::stateStartsUnobserved()
{
	gost::StateStore state;
	QVERIFY(!state.profileName().has_value());
	QVERIFY(!state.streamingActive().has_value());
	QCOMPARE(state.thread(), qApp->thread());
}

void PluginLifecycleTest::stateChangesAreObservable()
{
	gost::StateStore state;
	QSignalSpy changes(&state, &gost::StateStore::obsStateChanged);
	QVERIFY(changes.isValid());
	state.setObsState(QStringLiteral("Профиль A"), true);
	QCOMPARE(changes.count(), 1);
	QVERIFY(state.profileName().has_value());
	QVERIFY(state.streamingActive().has_value());
	QCOMPARE(state.profileName().value(), QStringLiteral("Профиль A"));
	QVERIFY(state.streamingActive().value());
	state.setObsState(QStringLiteral("Профиль B"), false);
	QCOMPARE(changes.count(), 2);
	QCOMPARE(state.profileName().value(), QStringLiteral("Профиль B"));
	QVERIFY(!state.streamingActive().value());
}

void PluginLifecycleTest::registersBeforeFrontendReady()
{
	FakeFrontend frontend;
	gost::PluginLifecycle lifecycle(frontend, localeLookup());
	QVERIFY(lifecycle.load());
	QCOMPARE(frontend.dockAdds, 1);
	QCOMPARE(frontend.callbackAdds, 1);
	QCOMPARE(frontend.dockId, QStringLiteral("gost-restreamer-obs-plugin"));
	QCOMPARE(frontend.dockTitle, QStringLiteral("GOST Restreamer"));
	QVERIFY(frontend.activeCallback);
	QVERIFY(frontend.activeCookie);
	QVERIFY(frontend.content);
	QVERIFY(frontend.dock);
	QVERIFY(!frontend.dock->isVisible());
	QCOMPARE(frontend.profileReads, 0);
	QCOMPARE(frontend.streamingReads, 0);
	QVERIFY(lifecycle.stateStore());
	QVERIFY(!lifecycle.stateStore()->profileName().has_value());
	QVERIFY(!lifecycle.stateStore()->streamingActive().has_value());
	QVERIFY(lifecycle.load());
	QCOMPARE(frontend.dockAdds, 1);
	QCOMPARE(frontend.callbackAdds, 1);
	frontend.emitEvent(gost::FrontendEvent::ProfileChanged);
	frontend.emitEvent(gost::FrontendEvent::StreamingStarted);
	QCOMPARE(frontend.profileReads, 0);
	QCOMPARE(frontend.streamingReads, 0);
	QVERIFY(!lifecycle.stateStore()->streamingActive().has_value());
}

void PluginLifecycleTest::observesCurrentProfileAndStreaming_data()
{
	QTest::addColumn<bool>("initialStreaming");
	QTest::newRow("already-streaming") << true;
	QTest::newRow("idle") << false;
}

void PluginLifecycleTest::observesCurrentProfileAndStreaming()
{
	QFETCH(bool, initialStreaming);
	FakeFrontend frontend;
	frontend.streaming = initialStreaming;
	gost::PluginLifecycle lifecycle(frontend, localeLookup());
	QVERIFY(lifecycle.load());
	QSignalSpy changes(lifecycle.stateStore(), &gost::StateStore::obsStateChanged);
	QVERIFY(changes.isValid());
	frontend.emitEvent(gost::FrontendEvent::FinishedLoading);
	QVERIFY(lifecycle.stateStore()->profileName().has_value());
	QVERIFY(lifecycle.stateStore()->streamingActive().has_value());
	QCOMPARE(lifecycle.stateStore()->profileName().value(), frontend.profile);
	QCOMPARE(lifecycle.stateStore()->streamingActive().value(), initialStreaming);
	QCOMPARE(changes.count(), 1);
	frontend.profile = QStringLiteral("Другой профиль");
	frontend.emitEvent(gost::FrontendEvent::ProfileChanged);
	QCOMPARE(lifecycle.stateStore()->profileName().value(), QStringLiteral("Другой профиль"));
	frontend.streaming = !initialStreaming;
	frontend.emitEvent(gost::FrontendEvent::StreamingStarted);
	QCOMPARE(lifecycle.stateStore()->streamingActive().value(), !initialStreaming);
	frontend.streaming = initialStreaming;
	frontend.emitEvent(gost::FrontendEvent::StreamingStopped);
	QCOMPARE(lifecycle.stateStore()->streamingActive().value(), initialStreaming);
}

void PluginLifecycleTest::obsStatisticsUseMonotonicIntervalsAndIndependentFramePairs()
{
	using namespace std::chrono_literals;
	gost::OBSStatsCollector collector;
	const gost::OBSStatsCollector::Clock::time_point start{};
	gost::ObsStatisticsSample sample{.videoAvailable = true,
					 .videoIdentity = 1,
					 .outputIdentity = 2,
					 .fps = 59.5,
					 .renderTimeNs = 240000,
					 .sentBytes = 700,
					 .render = gost::FrameCounters{9, 300},
					 .encode = gost::FrameCounters{3, 120},
					 .network = gost::FrameCounters{1, 25}};
	auto observed = collector.sample(sample, true, start);
	QCOMPARE(observed.fps, std::optional{59.5});
	QCOMPARE(observed.renderTimeNs, std::optional<quint64>{240000});
	QVERIFY(!observed.bitrate);
	QVERIFY(observed.bitrateWarming);

	sample.sentBytes = 1700;
	observed = collector.sample(sample, true, start + 250ms);
	QCOMPARE(observed.bitrate, std::optional{32000.0});
	QVERIFY(observed.render && observed.encode && observed.network);
	QCOMPARE(observed.render->total, quint64{300});
	QCOMPARE(observed.render->skipped, quint64{9});
	QCOMPARE(observed.encode->total, quint64{120});
	QCOMPARE(observed.encode->skipped, quint64{3});
	QCOMPARE(observed.network->total, quint64{25});
	QCOMPARE(observed.network->skipped, quint64{1});

	observed = collector.sample(sample, true, start + 250ms);
	QVERIFY(!observed.bitrate);
	observed = collector.sample(sample, true, start + 750ms);
	QCOMPARE(observed.bitrate, std::optional{0.0});
}

void PluginLifecycleTest::obsStatisticsLoseTheRateAtOutputReplacementAndTransitions()
{
	using namespace std::chrono_literals;
	using gost::FrontendEvent;
	using gost::ObsStreamingPhase;
	gost::OBSStatsCollector collector;
	const gost::OBSStatsCollector::Clock::time_point start{};
	gost::ObsStatisticsSample sample{.videoAvailable = true,
					 .videoIdentity = 1,
					 .outputIdentity = 2,
					 .fps = 60,
					 .sentBytes = 1000,
					 .network = gost::FrameCounters{0, 30}};
	auto observed = collector.sample(sample, false, start, FrontendEvent::StreamingStarting);
	QCOMPARE(observed.phase, ObsStreamingPhase::Starting);
	QVERIFY(!observed.sentBytes && !observed.network && !observed.bitrate);
	observed = collector.sample(sample, true, start + 100ms);
	QCOMPARE(observed.phase, ObsStreamingPhase::Streaming);
	QVERIFY(observed.bitrateWarming);
	const auto firstOutput = observed.outputEpoch;
	const auto video = observed.videoEpoch;

	sample.sentBytes = 1500;
	sample.outputIdentity = 3;
	observed = collector.sample(sample, true, start + 200ms);
	QVERIFY(observed.outputEpoch != firstOutput);
	QCOMPARE(observed.videoEpoch, video);
	QVERIFY(!observed.bitrate && observed.bitrateWarming);
	const auto replacement = observed.outputEpoch;

	sample.sentBytes = 100;
	observed = collector.sample(sample, true, start + 300ms);
	QVERIFY(observed.outputEpoch != replacement);
	QVERIFY(!observed.bitrate && observed.bitrateWarming);
	observed = collector.sample(sample, true, start + 400ms, FrontendEvent::StreamingStopping);
	QCOMPARE(observed.phase, ObsStreamingPhase::Stopping);
	observed = collector.sample(sample, false, start + 500ms);
	QCOMPARE(observed.phase, ObsStreamingPhase::Stopped);
	QVERIFY(!observed.sentBytes && !observed.network && !observed.bitrate);
	QCOMPARE(observed.fps, std::optional{60.0});

	sample.outputIdentity.reset();
	observed = collector.sample(sample, true, start + 600ms, FrontendEvent::StreamingStarted);
	QCOMPARE(observed.phase, ObsStreamingPhase::Streaming);
	QVERIFY(!observed.sentBytes && !observed.network && !observed.bitrate);
}

void PluginLifecycleTest::obsStatisticsStopBeforeExitReturns()
{
	FakeFrontend frontend;
	frontend.statistics.videoAvailable = true;
	frontend.statistics.fps = 48.0;
	gost::PluginLifecycle lifecycle(frontend, localeLookup());
	QVERIFY(lifecycle.load());
	QCOMPARE(frontend.statisticsReads, 0);
	frontend.emitEvent(gost::FrontendEvent::FinishedLoading);
	QTRY_COMPARE_WITH_TIMEOUT(lifecycle.stateStore()->obsStatistics().fps, std::optional{48.0}, 1500);
	QVERIFY(frontend.statisticsReads > 0);
	QCOMPARE(frontend.statisticsReleases, 0);
	const int reads = frontend.statisticsReads;

	frontend.emitEvent(gost::FrontendEvent::Exit);
	QCOMPARE(frontend.statisticsReleases, 1);
	QVERIFY(!lifecycle.stateStore());
	frontend.lastCallback(gost::FrontendEvent::StreamingStarted, frontend.lastCookie);
	lifecycle.shutdown();
	dispatchQueuedEvents();
	QCOMPARE(frontend.statisticsReads, reads);
	QCOMPARE(frontend.statisticsReleases, 1);
	QCOMPARE(frontend.callsAfterExit, 0);
}

void PluginLifecycleTest::managementWindowIsLazyAndReused_data()
{
	QTest::addColumn<QString>("locale");
	QTest::newRow("russian") << QStringLiteral("ru-RU");
	QTest::newRow("english") << QStringLiteral("en-US");
}

void PluginLifecycleTest::bothViewsFollowObservations_data()
{
	QTest::addColumn<QString>("locale");
	QTest::newRow("russian") << QStringLiteral("ru-RU");
	QTest::newRow("english") << QStringLiteral("en-US");
}

void PluginLifecycleTest::bothViewsFollowObservations()
{
	QFETCH(QString, locale);
	const auto text = localeLookup(locale);
	FakeFrontend frontend;
	gost::PluginLifecycle lifecycle(frontend, text);
	QVERIFY(lifecycle.load());
	lifecycle.openManagementWindow();
	QCOMPARE(managementWindows().size(), 1);
	auto *window = managementWindows().front();
	auto *dockProfile = frontend.content->findChild<QLabel *>(QStringLiteral("obsProfile"));
	auto *windowProfile = window->findChild<QLabel *>(QStringLiteral("obsProfile"));
	auto *dockStreaming = frontend.content->findChild<QLabel *>(QStringLiteral("obsStreaming"));
	auto *windowStreaming = window->findChild<QLabel *>(QStringLiteral("obsStreaming"));
	auto *server = frontend.content->findChild<QLabel *>(QStringLiteral("serverObservation"));
	auto *stream = frontend.content->findChild<QLabel *>(QStringLiteral("streamObservation"));
	QVERIFY(dockProfile && windowProfile && dockStreaming && windowStreaming && server && stream);
	for (auto *label : {dockProfile, windowProfile, dockStreaming, windowStreaming, server, stream})
		QCOMPARE(label->text(), text("Observation.NoData"));
	frontend.profile = QStringLiteral("<b>Профиль & OBS</b>");
	frontend.streaming = true;
	frontend.emitEvent(gost::FrontendEvent::FinishedLoading);
	for (auto *label : {dockProfile, windowProfile}) {
		QCOMPARE(label->text(), frontend.profile);
		QCOMPARE(label->textFormat(), Qt::PlainText);
	}
	QCOMPARE(dockStreaming->text(), text("Dock.ObsStreaming"));
	QCOMPARE(windowStreaming->text(), text("OBS.StreamingActive"));
	window->close();
	frontend.profile = QStringLiteral("Переименованный профиль");
	frontend.streaming = false;
	frontend.emitEvent(gost::FrontendEvent::ProfileChanged);
	QCOMPARE(dockProfile->text(), frontend.profile);
	QCOMPARE(windowProfile->text(), frontend.profile);
	QCOMPARE(dockStreaming->text(), text("Dock.ObsStopped"));
	QCOMPARE(windowStreaming->text(), text("OBS.StreamingInactive"));
	QCOMPARE(server->text(), text("Observation.NoData"));
	QCOMPARE(stream->text(), text("Observation.NoData"));
}

void PluginLifecycleTest::statusAppearanceFollowsStateAndPalette()
{
	const auto savedPalette = QApplication::palette();
	const auto restorePalette = qScopeGuard([&] { QApplication::setPalette(savedPalette); });
	QPalette palette = savedPalette;
	palette.setColor(QPalette::Window, QColor(QStringLiteral("#1c1f26")));
	palette.setColor(QPalette::WindowText, QColor(QStringLiteral("#ecedf2")));
	QApplication::setPalette(palette);

	gost::StateStore state;
	const auto text = localeLookup();
	gost::RestreamDock dock(state, text);
	gost::ManagementWindow window(state, text);
	auto *server = dock.findChild<QLabel *>(QStringLiteral("serverObservation"));
	auto *connection = window.findChild<QLabel *>(QStringLiteral("connectionStatus"));
	auto *dockStreaming = dock.findChild<QLabel *>(QStringLiteral("obsStreaming"));
	auto *windowStreaming = window.findChild<QLabel *>(QStringLiteral("obsStreaming"));
	QVERIFY(server && connection && dockStreaming && windowStreaming);

	for (auto *label : {server, connection, dockStreaming, windowStreaming}) {
		label->ensurePolished();
		QVERIFY(label->font().bold());
		QCOMPARE(label->palette().color(QPalette::WindowText), palette.color(QPalette::WindowText));
	}

	const QString origin = QStringLiteral("http://127.0.0.1:8080");
	using gost::ConnectionStatus;
	for (const auto status : {ConnectionStatus::Connecting, ConnectionStatus::Reconnecting}) {
		state.setConnection(origin, status);
		for (auto *label : {server, connection})
			QCOMPARE(label->palette().color(QPalette::WindowText), QColor(QStringLiteral("#ffd078")));
	}
	state.setConnection(origin, ConnectionStatus::Error);
	for (auto *label : {server, connection})
		QCOMPARE(label->palette().color(QPalette::WindowText), QColor(QStringLiteral("#ff8992")));

	state.setConnection(origin, ConnectionStatus::Connected);
	state.setObsState(QStringLiteral("Profile"), true);
	for (auto *label : {server, connection, dockStreaming, windowStreaming})
		QCOMPARE(label->palette().color(QPalette::WindowText), QColor(QStringLiteral("#6bdf91")));

	palette.setColor(QPalette::Window, QColor(QStringLiteral("#f6f7f8")));
	palette.setColor(QPalette::WindowText, QColor(QStringLiteral("#202226")));
	QApplication::setPalette(palette);
	for (auto *label : {server, connection, dockStreaming, windowStreaming})
		QCOMPARE(label->palette().color(QPalette::WindowText), QColor(QStringLiteral("#155d32")));

	state.setConnection(origin, ConnectionStatus::Disconnected);
	state.setObsState(QStringLiteral("Profile"), false);
	for (auto *label : {server, connection, dockStreaming, windowStreaming}) {
		QVERIFY(label->font().bold());
		QCOMPARE(label->palette().color(QPalette::WindowText), palette.color(QPalette::WindowText));
	}
}

void PluginLifecycleTest::managementWindowIsLazyAndReused()
{
	QFETCH(QString, locale);
	FakeFrontend frontend;
	gost::PluginLifecycle lifecycle(frontend, localeLookup(locale));
	QVERIFY(lifecycle.load());
	QVERIFY(managementWindows().isEmpty());
	QPointer<gost::StateStore> state = lifecycle.stateStore();
	QPointer<QWidget> dock = frontend.content;
	frontend.emitEvent(gost::FrontendEvent::FinishedLoading);
	auto *gear = dock->findChild<QToolButton *>();
	QVERIFY(gear);
	QTest::mouseClick(gear, Qt::LeftButton);
	QCOMPARE(managementWindows().size(), 1);
	QPointer<gost::ManagementWindow> window = managementWindows().front();
	QVERIFY(window->isWindow());
	QVERIFY(window->isVisible());
	QVERIFY(!window->testAttribute(Qt::WA_DeleteOnClose));
	QVERIFY(!window->testAttribute(Qt::WA_QuitOnClose));
	QTest::mouseClick(gear, Qt::LeftButton);
	QCOMPARE(managementWindows().size(), 1);
	QCOMPARE(managementWindows().front(), window.data());
	window->showMinimized();
	QTRY_VERIFY_WITH_TIMEOUT(window->isMinimized(), 1000);
	QTest::mouseClick(gear, Qt::LeftButton);
	QTRY_VERIFY_WITH_TIMEOUT(!window->isMinimized(), 1000);
	QVERIFY(window->isVisible());
	QCOMPARE(managementWindows().size(), 1);
	QCOMPARE(managementWindows().front(), window.data());
	window->close();
	dispatchQueuedEvents();
	QVERIFY(window);
	QVERIFY(!window->isVisible());
	QVERIFY(state);
	QVERIFY(dock);
	QCOMPARE(lifecycle.stateStore(), state.data());
	QCOMPARE(state->profileName().value(), frontend.profile);
	frontend.profile = QStringLiteral("Профиль после закрытия");
	frontend.emitEvent(gost::FrontendEvent::ProfileChanged);
	QCOMPARE(state->profileName().value(), frontend.profile);
	QTest::mouseClick(gear, Qt::LeftButton);
	QCOMPARE(managementWindows().size(), 1);
	QCOMPARE(managementWindows().front(), window.data());
	QVERIFY(window->isVisible());
	QCOMPARE(frontend.dockAdds, 1);
	QCOMPARE(frontend.callbackAdds, 1);
}

void PluginLifecycleTest::dockStaysCompact()
{
	FakeFrontend frontend;
	gost::PluginLifecycle lifecycle(frontend, localeLookup());
	QVERIFY(lifecycle.load());
	const auto *dock = frontend.content.data();
	QCOMPARE(dock->findChildren<QToolButton *>().size(), 1);
	QVERIFY(dock->findChildren<QTabWidget *>().isEmpty());
	QVERIFY(dock->findChildren<QLineEdit *>().isEmpty());
	QVERIFY(dock->findChildren<QComboBox *>().isEmpty());
	QVERIFY(dock->findChildren<QListWidget *>().isEmpty());
	QVERIFY(dock->findChildren<QStackedWidget *>().isEmpty());
}

void PluginLifecycleTest::dockResetChangesOnlyDisplayedAccumulators()
{
	gost::StateStore state;
	const auto text = localeLookup(QStringLiteral("en-US"));
	gost::RestreamDock dock(state, text);
	auto *reset = dock.findChild<QPushButton *>(QStringLiteral("statsResetButton"));
	const auto *statistics = dock.findChild<QLabel *>(QStringLiteral("obsStatistics"));
	QVERIFY(reset && statistics);
	QVERIFY(!reset->isEnabled());
	QVERIFY(statistics->text().contains(QStringLiteral("No data")));

	gost::ObsStatistics sample{.phase = gost::ObsStreamingPhase::Streaming,
				   .videoEpoch = 1,
				   .outputEpoch = 1,
				   .fps = 58.75,
				   .bitrate = 6370000,
				   .renderTimeNs = 340000,
				   .sentBytes = 5000000,
				   .render = gost::FrameCounters{12, 150},
				   .encode = gost::FrameCounters{3, 40},
				   .network = gost::FrameCounters{1, 25},
				   .observed = true};
	state.setObsStatistics(sample);
	QTRY_VERIFY_WITH_TIMEOUT(reset->isEnabled(), 1000);
	const auto before = statistics->text();
	const QStringList rates{QStringLiteral("58.75"), QStringLiteral("6.37 Mbps"), QStringLiteral("0.34 ms")};
	for (const auto &rate : rates)
		QVERIFY(before.contains(rate));
	reset->click();
	QVERIFY(statistics->text() != before);
	for (const auto &rate : rates)
		QVERIFY(statistics->text().contains(rate));
	QVERIFY(state.obsStatistics() == sample);

	sample.render = gost::FrameCounters{14, 160};
	sample.encode = gost::FrameCounters{4, 60};
	sample.network = gost::FrameCounters{1, 29};
	state.setObsStatistics(sample);
	QTRY_VERIFY_WITH_TIMEOUT(statistics->text().contains(QStringLiteral("(20.0%)")), 1000);
	QVERIFY(statistics->text().contains(QStringLiteral("(5.0%)")));
	QVERIFY(statistics->text().contains(QStringLiteral("(0.0%)")));
}

void PluginLifecycleTest::dockShowsNativeCounterLimitsIndependently_data()
{
	QTest::addColumn<int>("limitedCounter");
	QTest::newRow("render") << 0;
	QTest::newRow("encode") << 1;
	QTest::newRow("network") << 2;
}

void PluginLifecycleTest::dockShowsNativeCounterLimitsIndependently()
{
	QFETCH(int, limitedCounter);
	gost::StateStore state;
	const auto text = localeLookup(QStringLiteral("en-US"));
	gost::RestreamDock dock(state, text);
	const auto *statistics = dock.findChild<QLabel *>(QStringLiteral("obsStatistics"));
	QVERIFY(statistics);
	const gost::ObsStatisticsSample sample{.videoAvailable = true,
					       .videoIdentity = 1,
					       .outputIdentity = 2,
					       .fps = 48.25,
					       .render = gost::FrameCounters{5, 100},
					       .encode = gost::FrameCounters{6, 100},
					       .network = gost::FrameCounters{7, 100},
					       .renderLimited = limitedCounter == 0,
					       .encodeLimited = limitedCounter == 1,
					       .networkLimited = limitedCounter == 2};
	gost::OBSStatsCollector collector;
	const auto observed = collector.sample(sample, true, {});
	QCOMPARE(observed.renderLimited, sample.renderLimited);
	QCOMPARE(observed.encodeLimited, sample.encodeLimited);
	QCOMPARE(observed.networkLimited, sample.networkLimited);
	state.setObsStatistics(observed);
	QTRY_VERIFY_WITH_TIMEOUT(statistics->text().contains(QStringLiteral("48.25")), 1000);
	const auto lines = statistics->text().split(QLatin1Char('\n'));
	QStringList counterValues;
	for (int i = 0; i < 3; ++i) {
		counterValues.append(i == limitedCounter ? text("Dock.ChangeUnavailable")
							 : text("Dock.PairPeriod")
								   .arg(QString::number(i + 5), QStringLiteral("100"),
									QStringLiteral("%1.0%").arg(i + 5),
									text("Dock.PeriodRun")));
	}
	const auto counterLines =
		text("Dock.LocalCounters")
			.arg(counterValues.at(0), counterValues.at(1), counterValues.at(2), text("Observation.NoData"))
			.split(QLatin1Char('\n'));
	for (const auto &line : counterLines)
		QCOMPARE(lines.count(line), 1);
}

void PluginLifecycleTest::dockKeepsLastValuesOnlyInTheSameContext_data()
{
	QTest::addColumn<QString>("locale");
	QTest::addColumn<QString>("currentRate");
	QTest::addColumn<QString>("lastRate");
	QTest::addColumn<QString>("resumedRate");
	QTest::addColumn<QString>("replacementRate");
	QTest::addColumn<QString>("currentSession");
	QTest::addColumn<QString>("noCurrentData");
	QTest::addColumn<QString>("lastValues");
	QTest::newRow("russian")
		<< QStringLiteral("ru-RU") << QStringLiteral("8,37") << QStringLiteral("9,37")
		<< QStringLiteral("10,37") << QStringLiteral("12,37") << QStringLiteral("Рестрим запущен")
		<< QStringLiteral("Данные отсутствуют") << QStringLiteral("Последние полученные значения");
	QTest::newRow("english")
		<< QStringLiteral("en-US") << QStringLiteral("8.37") << QStringLiteral("9.37")
		<< QStringLiteral("10.37") << QStringLiteral("12.37") << QStringLiteral("Restream running")
		<< QStringLiteral("No current data") << QStringLiteral("Last received values");
}

void PluginLifecycleTest::dockKeepsLastValuesOnlyInTheSameContext()
{
	QFETCH(QString, locale);
	QFETCH(QString, currentRate);
	QFETCH(QString, lastRate);
	QFETCH(QString, resumedRate);
	QFETCH(QString, replacementRate);
	QFETCH(QString, currentSession);
	QFETCH(QString, noCurrentData);
	QFETCH(QString, lastValues);
	using namespace gost;
	using namespace gost::api;
	StateStore state;
	const auto origin = QStringLiteral("http://127.0.0.1:8080");
	const auto streamId = QStringLiteral("stream");
	const auto sessionId = QStringLiteral("session");
	const auto profileId = QStringLiteral("profile-a");
	Stream stream;
	stream.id = streamId;
	stream.name = QStringLiteral("Selected");
	state.setStreams({stream}, true);
	state.setConnection(origin, ConnectionStatus::Connected);
	state.setSelection(streamId, true, {}, {}, profileId);
	state.setObsState(QStringLiteral("OBS profile"), false);
	const auto text = localeLookup(locale);
	RestreamDock dock(state, text);
	auto *outputs = dock.findChild<QListView *>(QStringLiteral("outputList"));
	auto *reset = dock.findChild<QPushButton *>(QStringLiteral("statsResetButton"));
	const auto *server = dock.findChild<QLabel *>(QStringLiteral("serverObservation"));
	const auto *sessionLabel = dock.findChild<QLabel *>(QStringLiteral("sessionObservation"));
	const auto *ingest = dock.findChild<QLabel *>(QStringLiteral("ingestStatistics"));
	const auto *notice = dock.findChild<QLabel *>(QStringLiteral("serverDataNotice"));
	const auto *localStatistics = dock.findChild<QLabel *>(QStringLiteral("obsStatistics"));
	QVERIFY(outputs && reset && server && sessionLabel && ingest && notice && localStatistics);

	RuntimeSession session;
	session.sessionId = sessionId;
	session.observation = {FieldState::Value, Observation{.instanceId = QStringLiteral("epoch:1"),
							      .state = QStringLiteral("RUNNING")}};
	Input input;
	input.connected = true;
	input.metrics.state = FieldState::Value;
	input.metrics.value.sourceEpoch = 1;
	auto &inputBitrate = input.metrics.value.media.rates.payloadBitrate;
	inputBitrate.status = QStringLiteral("available");
	inputBitrate.value = {FieldState::Value, 8370000.0};
	session.input = {FieldState::Value, input};
	DockServerState metadata;
	metadata.sessionStatus = {RuntimeAvailability::Fresh, true, {}};
	metadata.session = DockSession{sessionId, QStringLiteral("RUNNING"), false};
	metadata.outputsStatus = {RuntimeAvailability::Fresh, true, {}};
	for (int i = 0; i < 20; ++i) {
		RuntimeOutput output;
		output.instanceId = QStringLiteral("epoch:%1").arg(i + 2);
		output.outputId = QStringLiteral("out-%1").arg(i);
		output.state = QStringLiteral("PUBLISHING");
		if (i == 0) {
			output.metrics.state = FieldState::Value;
			output.metrics.value.counters.insert(QStringLiteral("reconnects"), 7);
			output.metrics.value.queueDrops.insert(QStringLiteral("video"), 11);
			output.metrics.value.deliveryDrops.insert(QStringLiteral("video"), 13);
		}
		metadata.outputs.insert(output.outputId, {QStringLiteral("Channel %1").arg(i), true, true});
		session.outputs.append(output);
	}
	QVERIFY(state.installAnchor({QStringLiteral("anchor"), {session}, {FieldState::Null, {}}}));
	state.setDockServer(metadata);
	dock.resize(360, 560);
	dock.show();
	QTRY_VERIFY_WITH_TIMEOUT(ingest->text().contains(currentRate), 1000);
	QVERIFY(sessionLabel->text().contains(currentSession));
	QVERIFY(!notice->text().contains(lastValues));
	QCOMPARE(outputs->model()->rowCount(), 20);
	const auto firstRow = outputs->model()->index(0, 0);
	const auto rowBeforeReset = firstRow.data().toString();
	QVERIFY(rowBeforeReset.contains(QStringLiteral("Channel 0")));
	QVERIFY(rowBeforeReset.contains(QStringLiteral("11")));
	QVERIFY(rowBeforeReset.contains(QStringLiteral("13")));
	QVERIFY(reset->isEnabled());
	reset->click();
	const auto rowAfterReset = firstRow.data().toString();
	QVERIFY(rowAfterReset.contains(text("Dock.PeriodReset")));
	QVERIFY(!rowAfterReset.contains(QStringLiteral("11")));
	QVERIFY(!rowAfterReset.contains(QStringLiteral("13")));
	const auto raw = state.runtime().sessions.value(sessionId).outputs.front().metrics.value;
	QCOMPARE(raw.counters.value(QStringLiteral("reconnects")), quint64{7});
	QCOMPARE(raw.queueDrops.value(QStringLiteral("video")), quint64{11});
	QCOMPARE(raw.deliveryDrops.value(QStringLiteral("video")), quint64{13});
	QTRY_VERIFY_WITH_TIMEOUT(outputs->verticalScrollBar()->maximum() > 0, 1000);
	QVERIFY(dock.width() >= 300 && dock.width() <= 400);
	outputs->scrollToBottom();
	QTRY_VERIFY_WITH_TIMEOUT(outputs->verticalScrollBar()->value() > 0, 1000);
	QVERIFY(!server->visibleRegion().isEmpty());
	QVERIFY(!sessionLabel->visibleRegion().isEmpty());

	RuntimeEvent metric;
	metric.cursor = QStringLiteral("latest-before-disconnect");
	metric.kind = QStringLiteral("metrics");
	metric.owner = QStringLiteral("session");
	metric.sessionId = sessionId;
	metric.observation.instanceId = session.observation->instanceId;
	metric.observation.revision = 1;
	inputBitrate.value = {FieldState::Value, 9370000.0};
	metric.input = {FieldState::Value, input};
	QVERIFY(state.applyEvent(metric));
	state.markRuntimeStale();
	state.setConnection(origin, ConnectionStatus::Reconnecting);
	QTRY_VERIFY_WITH_TIMEOUT(notice->text().contains(noCurrentData), 1000);
	QVERIFY(notice->text().contains(lastValues));
	const auto lastBitrate = text("Dock.LastValue").arg(text("Dock.Megabits").arg(lastRate));
	QVERIFY(ingest->text().contains(lastBitrate));
	QVERIFY(!ingest->text().contains(currentRate));
	state.setConnection(origin, ConnectionStatus::Connected);
	QTRY_VERIFY_WITH_TIMEOUT(notice->text().contains(lastValues), 1000);
	QVERIFY(ingest->text().contains(lastBitrate));

	metric.cursor = QStringLiteral("exact-metric-after-reconnect");
	metric.observation.revision = 2;
	inputBitrate.value = {FieldState::Value, 10370000.0};
	metric.input = {FieldState::Value, input};
	QVERIFY(state.applyEvent(metric));
	QCOMPARE(state.dockServer().sessionStatus.availability, RuntimeAvailability::Stale);
	QTRY_VERIFY_WITH_TIMEOUT(ingest->text().contains(resumedRate), 1000);
	const auto resumedBitrate = text("Dock.Megabits").arg(resumedRate);
	const auto lastResumedBitrate = text("Dock.LastValue").arg(resumedBitrate);
	QVERIFY(!ingest->text().contains(lastResumedBitrate));
	QVERIFY(!sessionLabel->text().contains(currentSession));

	state.markRuntimeStale();
	state.setConnection(origin, ConnectionStatus::Reconnecting);
	QTRY_VERIFY_WITH_TIMEOUT(ingest->text().contains(lastResumedBitrate), 1000);
	session.observation->instanceId = QStringLiteral("replacement:1");
	session.outputs.front().instanceId = QStringLiteral("replacement:2");
	inputBitrate.value = {FieldState::Value, 12370000.0};
	session.input = {FieldState::Value, input};
	QVERIFY(state.installAnchor({QStringLiteral("replacement-anchor"), {session}, {FieldState::Null, {}}}, true));
	state.setObsStatistics({.fps = 43.0, .observed = true});
	QTRY_VERIFY_WITH_TIMEOUT(localStatistics->text().contains(QStringLiteral("43")), 1000);
	QVERIFY(ingest->text().contains(lastResumedBitrate));
	QVERIFY(!ingest->text().contains(replacementRate));
	state.setConnection(origin, ConnectionStatus::Connected);
	QTRY_VERIFY_WITH_TIMEOUT(ingest->text().contains(replacementRate), 1000);
	QVERIFY(!ingest->text().contains(resumedRate));

	state.setSelection(streamId, true, {}, {}, QStringLiteral("profile-b"));
	QTRY_VERIFY_WITH_TIMEOUT(!ingest->text().contains(replacementRate), 1000);
	QVERIFY(!notice->text().contains(lastValues));
	QVERIFY(!sessionLabel->text().contains(currentSession));
}

void PluginLifecycleTest::managementNavigationHasLocalizedPagesAndIcons_data()
{
	QTest::addColumn<QString>("locale");
	QTest::addColumn<QStringList>("pages");
	QTest::newRow("russian") << QStringLiteral("ru-RU")
				 << QStringList{QStringLiteral("Потоки"),           QStringLiteral("Выходы"),
						QStringLiteral("Группы и профили"), QStringLiteral("Заглушки"),
						QStringLiteral("Диагностика"),      QStringLiteral("Настройки"),
						QStringLiteral("Расширенные")};
	QTest::newRow("english") << QStringLiteral("en-US") << QStringList{QStringLiteral("Streams"),
									   QStringLiteral("Outputs"),
									   QStringLiteral("Groups and profiles"),
									   QStringLiteral("Fallback"),
									   QStringLiteral("Diagnostics"),
									   QStringLiteral("Settings"),
									   QStringLiteral("Advanced")};
}

void PluginLifecycleTest::managementNavigationHasLocalizedPagesAndIcons()
{
	QFETCH(QString, locale);
	QFETCH(QStringList, pages);
	const auto text = localeLookup(locale);
	gost::StateStore state;
	gost::ManagementWindow window(state, text);
	auto *navigation = window.findChild<QListWidget *>();
	auto *stack = window.findChild<QStackedWidget *>();
	QVERIFY(navigation);
	QVERIFY(stack);
	QCOMPARE(navigation->count(), pages.size());
	QCOMPARE(stack->count(), pages.size());
	for (int index = 0; index < pages.size(); ++index) {
		QCOMPARE(navigation->item(index)->text(), pages.at(index));
		QVERIFY(!navigation->item(index)->icon().pixmap(navigation->iconSize()).isNull());
		navigation->setCurrentRow(index);
		QCOMPARE(stack->currentIndex(), index);
		QVERIFY(stack->currentWidget());
	}
	QCOMPARE(window.findChildren<QTabWidget *>().size(), 1);
	auto *tabs = stack->widget(2)->findChild<QTabWidget *>();
	QVERIFY(tabs);
	QCOMPARE(tabs->count(), 2);
	QCOMPARE(tabs->tabText(0),
		 locale == QStringLiteral("ru-RU") ? QStringLiteral("Группы") : QStringLiteral("Groups"));
	QCOMPARE(tabs->tabText(1),
		 locale == QStringLiteral("ru-RU") ? QStringLiteral("Профили") : QStringLiteral("Profiles"));
	auto *retries = stack->currentWidget()->findChild<QSpinBox *>(QStringLiteral("sseRejectionRetries"));
	auto *message = stack->currentWidget()->findChild<QLabel *>(QStringLiteral("advancedSettingsMessage"));
	QVERIFY(retries && message);
	QVERIFY(message->isHidden());
	state.setPreferences(false, true, 3, true);
	QVERIFY(!message->isHidden());
	QCOMPARE(message->text(), text("Settings.Unsaved"));
}

void PluginLifecycleTest::failedDockRegistrationReleasesResources()
{
	FakeFrontend frontend;
	frontend.acceptDock = false;
	{
		gost::PluginLifecycle lifecycle(frontend, localeLookup());
		QVERIFY(!lifecycle.load());
		QVERIFY(!frontend.content);
		QVERIFY(!lifecycle.stateStore());
		QVERIFY(!frontend.activeCallback);
		QCOMPARE(frontend.callbackRemoves, frontend.callbackAdds);
		QCOMPARE(frontend.dockRemoves, 0);
		lifecycle.shutdown();
	}
	QCOMPARE(frontend.callbackRemoves, frontend.callbackAdds);
	QCOMPARE(frontend.invalidCallbackOperations, 0);
	QVERIFY(managementWindows().isEmpty());
}

void PluginLifecycleTest::exitCleansSynchronouslyAndLeavesInertShell()
{
	FakeFrontend frontend;
	{
		gost::PluginLifecycle lifecycle(frontend, localeLookup());
		QVERIFY(lifecycle.load());
		lifecycle.openManagementWindow();
		QCOMPARE(managementWindows().size(), 1);
		QVERIFY(frontend.lastCallback);
		QPointer<gost::StateStore> state = lifecycle.stateStore();
		QPointer<QWidget> dock = frontend.content;
		QPointer<gost::ManagementWindow> window = managementWindows().front();
		frontend.emitEvent(gost::FrontendEvent::Exit);
		QVERIFY(!state);
		QVERIFY(!dock);
		QVERIFY(!window);
		QVERIFY(!lifecycle.stateStore());
		QVERIFY(!frontend.activeCallback);
		QCOMPARE(frontend.callbackRemoves, 1);
		QCOMPARE(frontend.dockRemoves, 1);
		QCOMPARE(frontend.removedId, QStringLiteral("gost-restreamer-obs-plugin"));
		QCOMPARE(frontend.cleanupOrder, QStringList({QStringLiteral("callback"), QStringLiteral("dock")}));
		lifecycle.shutdown();
		lifecycle.openManagementWindow();
		frontend.lastCallback(gost::FrontendEvent::ProfileChanged, frontend.lastCookie);
		frontend.lastCallback(gost::FrontendEvent::Exit, frontend.lastCookie);
		dispatchQueuedEvents();
		QVERIFY(managementWindows().isEmpty());
		QCOMPARE(frontend.callbackRemoves, 1);
		QCOMPARE(frontend.dockRemoves, 1);
	}
	QCOMPARE(frontend.callsAfterExit, 0);
	QCOMPARE(frontend.invalidCallbackOperations, 0);
}

void PluginLifecycleTest::registrationExceptionReleasesAcquiredResources_data()
{
	QTest::addColumn<bool>("throwOnDock");
	QTest::newRow("dock-registration") << true;
	QTest::newRow("callback-registration") << false;
}

void PluginLifecycleTest::registrationExceptionReleasesAcquiredResources()
{
	QFETCH(bool, throwOnDock);
	FakeFrontend frontend;
	frontend.throwOnDockAdd = throwOnDock;
	frontend.throwOnCallbackAdd = !throwOnDock;
	{
		gost::PluginLifecycle lifecycle(frontend, localeLookup());
		QVERIFY(!lifecycle.load());
		QVERIFY(!lifecycle.stateStore());
		QVERIFY(!frontend.content);
		QVERIFY(!frontend.dock);
		QVERIFY(!frontend.activeCallback);
		QCOMPARE(frontend.dockRemoves, throwOnDock ? 0 : 1);
		lifecycle.shutdown();
	}
	QCOMPARE(frontend.dockRemoves, throwOnDock ? 0 : 1);
	QCOMPARE(frontend.callbackRemoves, 0);
	QVERIFY(managementWindows().isEmpty());
}

void PluginLifecycleTest::callbackExceptionDoesNotEscape()
{
	FakeFrontend frontend;
	gost::PluginLifecycle lifecycle(frontend, localeLookup());
	QVERIFY(lifecycle.load());
	frontend.throwOnProfileRead = true;
	frontend.emitEvent(gost::FrontendEvent::FinishedLoading);
	QVERIFY(lifecycle.stateStore());
	QVERIFY(!lifecycle.stateStore()->profileName().has_value());
	QVERIFY(!lifecycle.stateStore()->streamingActive().has_value());
	frontend.throwOnProfileRead = false;
	frontend.emitEvent(gost::FrontendEvent::ProfileChanged);
	QVERIFY(lifecycle.stateStore()->profileName().has_value());
	QCOMPARE(lifecycle.stateStore()->profileName().value(), frontend.profile);
	frontend.emitEvent(gost::FrontendEvent::Exit);
	QVERIFY(!lifecycle.stateStore());
	QCOMPARE(frontend.callsAfterExit, 0);
}

void PluginLifecycleTest::queuedOpenCannotResurrectRuntime()
{
	FakeFrontend frontend;
	gost::PluginLifecycle lifecycle(frontend, localeLookup());
	QVERIFY(lifecycle.load());
	QVERIFY(QMetaObject::invokeMethod(
		&lifecycle, [&lifecycle] { lifecycle.openManagementWindow(); }, Qt::QueuedConnection));
	frontend.emitEvent(gost::FrontendEvent::Exit);
	dispatchQueuedEvents();
	QVERIFY(managementWindows().isEmpty());
	QCOMPARE(frontend.callsAfterExit, 0);
}

void PluginLifecycleTest::hostDestructionDoesNotDoubleDelete()
{
	FakeFrontend frontend;
	{
		gost::PluginLifecycle lifecycle(frontend, localeLookup());
		QVERIFY(lifecycle.load());
		lifecycle.openManagementWindow();
		QPointer<QWidget> dock = frontend.content;
		QPointer<gost::StateStore> state = lifecycle.stateStore();
		frontend.host.reset();
		QVERIFY(!dock);
		QVERIFY(state);
		frontend.emitEvent(gost::FrontendEvent::Exit);
		QVERIFY(!state);
		QVERIFY(managementWindows().isEmpty());
	}
	QCOMPARE(frontend.callsAfterExit, 0);
	QCOMPARE(frontend.invalidCallbackOperations, 0);
}

void PluginLifecycleTest::queuedFrontendEventIsDiscardedOnExit()
{
	FakeFrontend frontend;
	gost::PluginLifecycle lifecycle(frontend, localeLookup());
	QVERIFY(lifecycle.load());
	frontend.emitEvent(gost::FrontendEvent::FinishedLoading);
	QPointer<gost::StateStore> state = lifecycle.stateStore();
	QVERIFY(state->profileName().has_value());
	const auto observedProfile = state->profileName().value();
	const int profileReads = frontend.profileReads;
	const int streamingReads = frontend.streamingReads;
	frontend.profile = QStringLiteral("Профиль после события из рабочего потока");
	std::thread eventThread([&frontend] { frontend.emitEvent(gost::FrontendEvent::ProfileChanged); });
	eventThread.join();
	QCOMPARE(frontend.profileReads, profileReads);
	QCOMPARE(frontend.streamingReads, streamingReads);
	QCOMPARE(state->profileName().value(), observedProfile);
	frontend.emitEvent(gost::FrontendEvent::Exit);
	QVERIFY(!state);
	dispatchQueuedEvents();
	QVERIFY(!lifecycle.stateStore());
	QVERIFY(managementWindows().isEmpty());
	QCOMPARE(frontend.profileReads, profileReads);
	QCOMPARE(frontend.streamingReads, streamingReads);
	QCOMPARE(frontend.callsAfterExit, 0);
}

void PluginLifecycleTest::localesAreComplete()
{
	QSettings russian(QStringLiteral(GOST_TEST_LOCALE_DIR) + QStringLiteral("/ru-RU.ini"), QSettings::IniFormat);
	QSettings english(QStringLiteral(GOST_TEST_LOCALE_DIR) + QStringLiteral("/en-US.ini"), QSettings::IniFormat);
	QCOMPARE(russian.status(), QSettings::NoError);
	QCOMPARE(english.status(), QSettings::NoError);
	QVERIFY(!russian.allKeys().isEmpty());
	QCOMPARE(russian.allKeys(), english.allKeys());
	for (const auto &key : russian.allKeys()) {
		QVERIFY(!russian.value(key).toString().isEmpty());
		QVERIFY(!english.value(key).toString().isEmpty());
	}
}

void PluginLifecycleTest::hiddenManagementKeepsOneSubscriptionAndMetadataVersion()
{
	WorkspaceTemp temp(QStringLiteral("runtime-ui"));
	QVERIFY(temp.isValid());
	HttpServer server;
	QVERIFY(server.listen());
	QList<HttpServer::Request> subscriptions;
	server.handler = [&](const auto &request) {
		if (request.target.startsWith("/api/v1/health/ready"))
			HttpServer::reply(request, R"({"status":"ready","reason":""})");
		else if (request.target.startsWith("/api/v1/system/capabilities"))
			HttpServer::reply(request, fixture::capabilities());
		else if (request.target.startsWith("/api/v1/streams"))
			HttpServer::reply(request, fixture::page({fixture::stream()}));
		else if (request.target.startsWith("/api/v1/events/snapshot"))
			HttpServer::reply(request, R"({"cursor":"anchor","sessions":[],"gpu":null})");
		else if (request.target.startsWith("/api/v1/events?")) {
			subscriptions.append(request);
			HttpServer::stream(request, ": heartbeat\n\n");
		}
	};
	FakeFrontend frontend;
	const QString version = QStringLiteral("7.8.9-metadata");
	gost::PluginLifecycle lifecycle(frontend, localeLookup(),
					{version, temp.filePath(QStringLiteral("settings.json"))});
	QVERIFY(lifecycle.load());
	lifecycle.settingsStore()->setAutoConnect(false);
	frontend.emitEvent(gost::FrontendEvent::FinishedLoading);
	QVERIFY(lifecycle.connectionController());
	lifecycle.connectionController()->connectToServer(server.origin().toString());
	QTRY_COMPARE_WITH_TIMEOUT(subscriptions.size(), 1, 3000);
	auto *versionLabel = frontend.content->findChild<QLabel *>(QStringLiteral("pluginVersion"));
	QVERIFY(versionLabel);
	QVERIFY(versionLabel->text().contains(version));
	QCOMPARE(versionLabel->textFormat(), Qt::PlainText);
	lifecycle.openManagementWindow();
	QCOMPARE(managementWindows().size(), 1);
	QPointer<gost::ManagementWindow> window = managementWindows().front();
	QPointer<gost::StateStore> state = lifecycle.stateStore();
	QPointer<gost::ConnectionController> controller = lifecycle.connectionController();
	window->close();
	dispatchQueuedEvents();
	QVERIFY(!window->isVisible());
	QVERIFY(state && controller);
	subscriptions.front().socket->write(
		fixture::frame(fixture::event(QStringLiteral("state"), QStringLiteral("while-hidden"))));
	QTRY_COMPARE_WITH_TIMEOUT(state->runtime().cursor, QStringLiteral("while-hidden"), 2000);
	lifecycle.openManagementWindow();
	QCOMPARE(managementWindows().size(), 1);
	QCOMPARE(managementWindows().front(), window.data());
	QCOMPARE(lifecycle.connectionController(), controller.data());
	QCOMPARE(subscriptions.size(), 1);
	QVERIFY(window->isVisible());
	frontend.emitEvent(gost::FrontendEvent::Exit);
	QVERIFY(!state && !controller && !window);
	dispatchQueuedEvents();
	QCOMPARE(frontend.callsAfterExit, 0);
	for (const auto &request : server.requests)
		QCOMPARE(request.method, QByteArray("GET"));
}

void PluginLifecycleTest::largeStreamListAndRuntimeTrafficAllowGuiProgress()
{
	using namespace gost::api;
	constexpr int streamCount = 10000;
	constexpr int sessionCount = 64;
	constexpr int outputCount = 8;
	constexpr int eventCount = 256;
	constexpr int batchSize = 32;
	QVector<QByteArray> pages;
	for (int page = 0; page < 100; ++page) {
		QJsonArray items;
		for (int index = page * 100; index < (page + 1) * 100; ++index) {
			auto stream = fixture::stream(QStringLiteral("stream-%1").arg(index));
			stream.insert(QStringLiteral("name"), QStringLiteral("Поток %1").arg(index));
			items.append(stream);
		}
		pages.append(fixture::page(items, page == 99 ? QString{} : QStringLiteral("page-%1").arg(page + 1)));
	}
	Anchor anchor{QStringLiteral("load-anchor"), {}, {FieldState::Null, {}}};
	for (int index = 0; index < sessionCount; ++index) {
		RuntimeSession session;
		session.sessionId = QStringLiteral("session-%1").arg(index);
		session.observation = {
			FieldState::Value,
			{QStringLiteral("session-instance-%1").arg(index), 1, {}, QStringLiteral("RUNNING"), {}}};
		for (int group = 0; group < 4; ++group) {
			RuntimeGroup node;
			node.instanceId = QStringLiteral("group-instance-%1-%2").arg(index).arg(group);
			node.groupId = QStringLiteral("group-%1").arg(group);
			node.profileId = QStringLiteral("profile-%1").arg(group);
			node.state = QStringLiteral("READY");
			node.revision = 1;
			node.metrics = {FieldState::Value, {}};
			session.groups.append(node);
			for (const auto &role : {QStringLiteral("live"), QStringLiteral("fallback")}) {
				RuntimeFFmpeg ffmpeg;
				ffmpeg.instanceId = node.instanceId + QLatin1Char('-') + role;
				ffmpeg.groupInstanceId = node.instanceId;
				ffmpeg.role = role;
				ffmpeg.state = QStringLiteral("RUNNING");
				ffmpeg.metrics = {FieldState::Value, {}};
				session.ffmpeg.append(ffmpeg);
			}
		}
		for (int output = 0; output < outputCount; ++output) {
			RuntimeOutput node;
			node.outputId = QStringLiteral("output-%1").arg(output);
			node.instanceId = QStringLiteral("output-instance-%1-%2").arg(index).arg(output);
			node.state = QStringLiteral("PUBLISHING");
			node.revision = 1;
			node.metrics = {FieldState::Value, {}};
			session.outputs.append(node);
		}
		anchor.sessions.append(session);
	}
	gost::StateStore state;
	gost::ManagementWindow window(state, localeLookup());
	window.show();
	auto *selector = window.findChild<QComboBox *>(QStringLiteral("streamSelector"));
	auto *navigation = window.findChild<QListWidget *>(QStringLiteral("managementNavigation"));
	auto *stack = window.findChild<QStackedWidget *>(QStringLiteral("managementPages"));
	QVERIFY(selector && navigation && stack);
	HttpServer server;
	QVERIFY(server.listen());
	int pageIndex = 0;
	server.handler = [&](const auto &request) {
		if (request.target.startsWith("/api/v1/streams") && pageIndex < pages.size())
			HttpServer::reply(request, pages[pageIndex++]);
		else
			HttpServer::stream(request, ": heartbeat\n\n");
	};
	QElapsedTimer clock;
	clock.start();
	qint64 last = 0;
	qint64 maximumGap = 0;
	int uiProgress = 0;
	int progressDuringEvents = 0;
	int received = 0;
	bool guiConsistent = true;
	QTimer ticker;
	connect(&ticker, &QTimer::timeout, &window, [&] {
		const auto now = clock.nsecsElapsed();
		maximumGap = qMax(maximumGap, now - last);
		last = now;
		navigation->setCurrentRow((navigation->currentRow() + 1) % navigation->count());
		guiConsistent = guiConsistent && stack->currentIndex() == navigation->currentRow();
		++uiProgress;
		if (received > 0 && received < eventCount)
			++progressDuringEvents;
	});
	ticker.start(0);
	APIClient api;
	api.configure(server.origin(), {1, 1});
	bool loaded = false;
	bool listValid = false;
	qint64 listElapsed = 0;
	api.getAllStreams(&window, [&](auto, auto result) {
		listValid = result && result.value->complete && result.value->items.size() == streamCount;
		if (result)
			state.setStreams(std::move(result.value->items), result.value->complete);
		loaded = true;
		listElapsed = clock.elapsed();
	});
	QTRY_VERIFY_WITH_TIMEOUT(loaded, 10000);
	QVERIFY(listValid);
	QCOMPARE(selector->count(), streamCount + 1);
	state.setSelection(QStringLiteral("stream-9999"), true);
	QCOMPARE(selector->currentData().toString(), QStringLiteral("stream-9999"));
	const auto anchorStart = clock.nsecsElapsed();
	QVERIFY(state.installAnchor(anchor));
	const auto anchorElapsed = clock.nsecsElapsed() - anchorStart;
	QCOMPARE(state.runtime().sessions.size(), sessionCount);
	EventClient events;
	bool failed = false;
	bool applied = true;
	events.subscribe(
		server.origin(), anchor.cursor, {1, 1}, &window,
		[&](auto, const auto &event) {
			const bool accepted = state.applyEvent(event);
			applied = applied && accepted;
			++received;
			return accepted;
		},
		[&](auto, auto, bool) { failed = true; });
	QTRY_VERIFY_WITH_TIMEOUT(server.requests.size() == 101 || failed, 3000);
	QVERIFY(!failed);
	const auto trafficStart = clock.nsecsElapsed();
	for (int batch = 0; batch < eventCount / batchSize; ++batch) {
		QByteArray wire;
		for (int number = batch * batchSize; number < (batch + 1) * batchSize; ++number) {
			const int session = number % sessionCount;
			const int output = (number / sessionCount) % outputCount;
			auto event =
				fixture::event(QStringLiteral("metrics"), QStringLiteral("load-event-%1").arg(number));
			event.insert(QStringLiteral("owner"), QStringLiteral("output"));
			event.insert(QStringLiteral("session_id"), QStringLiteral("session-%1").arg(session));
			event.insert(QStringLiteral("output_id"), QStringLiteral("output-%1").arg(output));
			event.insert(
				QStringLiteral("observation"),
				fixture::observation(QStringLiteral("output-instance-%1-%2").arg(session).arg(output),
						     2, QString{}));
			auto metrics = fixture::outputMetrics();
			auto media = fixture::media();
			media.insert(QStringLiteral("video_frames"), QString::number(number + 1));
			metrics.insert(QStringLiteral("media"), media);
			event.insert(QStringLiteral("output"), metrics);
			wire += fixture::frame(event);
		}
		QVERIFY(server.requests.back().socket);
		server.requests.back().socket->write(wire);
		QTRY_VERIFY_WITH_TIMEOUT(received == (batch + 1) * batchSize || failed, 10000);
		QVERIFY(!failed);
	}
	const auto trafficElapsed = clock.nsecsElapsed() - trafficStart;
	maximumGap = qMax(maximumGap, clock.nsecsElapsed() - last);
	ticker.stop();
	events.shutdown();
	QVERIFY(applied && guiConsistent);
	QVERIFY(uiProgress > 0);
	QVERIFY(progressDuringEvents > 0);
	QCOMPARE(events.confirmedCursor(), QStringLiteral("load-event-255"));
	QCOMPARE(state.runtime().sessions.size(), sessionCount);
	for (int number = 0; number < eventCount; ++number) {
		const auto &session = state.runtime().sessions[QStringLiteral("session-%1").arg(number % sessionCount)];
		QCOMPARE(session.groups.size(), 4);
		QCOMPARE(session.ffmpeg.size(), 8);
		QCOMPARE(session.outputs.size(), outputCount);
		const auto &output = session.outputs[(number / sessionCount) % outputCount];
		QCOMPARE(output.state, QStringLiteral("PUBLISHING"));
		QCOMPARE(output.metrics.value.media.videoFrames, quint64(number + 1));
	}
	QCOMPARE(selector->count(), streamCount + 1);
	QCOMPARE(selector->currentData().toString(), QStringLiteral("stream-9999"));
	qInfo("Нагрузка GUI: %d Stream/100 страниц, %d Session, 256 групп, 512 Output, 512 FFmpeg; %d SSE metrics в пачках %d; список %lld мс, установка anchor %.3f мс, SSE %.3f мс, максимальный разрыв event loop %.3f мс, продвижений GUI %d (при SSE %d); Qt %s.",
	      streamCount, sessionCount, eventCount, batchSize, listElapsed, double(anchorElapsed) / 1000000.0,
	      double(trafficElapsed) / 1000000.0, double(maximumGap) / 1000000.0, uiProgress, progressDuringEvents,
	      qVersion());
}

void PluginLifecycleTest::runtimeUpdatePreservesOpenStreamPopup()
{
	WorkspaceTemp temp(QStringLiteral("stream-popup"));
	QVERIFY(temp.isValid());
	gost::StateStore state;
	gost::SettingsStore settings(temp.filePath(QStringLiteral("settings.json")));
	gost::api::APIClient api;
	gost::api::EventClient events;
	gost::ConnectionController controller(state, settings, api, events);
	QVector<gost::api::Stream> streams;
	for (int index = 0; index < 1000; ++index) {
		gost::api::Stream stream;
		stream.id = QStringLiteral("stream-%1").arg(index);
		stream.name = QStringLiteral("Поток %1").arg(index);
		streams.append(stream);
	}
	state.setStreams(streams, true);
	state.setSelection(QStringLiteral("stream-10"), true);
	state.setConnection(QStringLiteral("http://127.0.0.1:8080"), gost::ConnectionStatus::Connected);
	QVERIFY(state.installAnchor({QStringLiteral("popup-anchor"), {}, {gost::api::FieldState::Null, {}}}));
	gost::ManagementWindow window(state, localeLookup(), nullptr, &controller);
	window.show();
	auto *navigation = window.findChild<QListWidget *>(QStringLiteral("managementNavigation"));
	auto *selector = window.findChild<QComboBox *>(QStringLiteral("streamSelector"));
	QVERIFY(navigation && selector);
	navigation->setCurrentRow(5);
	QVERIFY(selector->isEnabled());
	selector->showPopup();
	auto *view = selector->view();
	view->setCurrentIndex(view->model()->index(500, 0));
	view->scrollTo(view->currentIndex(), QAbstractItemView::PositionAtCenter);
	QTRY_VERIFY_WITH_TIMEOUT(view->isVisible(), 1000);
	const int highlighted = view->currentIndex().row();
	const int scroll = view->verticalScrollBar()->value();
	QVERIFY(scroll > 0);
	gost::api::RuntimeEvent update;
	update.cursor = QStringLiteral("popup-event");
	update.kind = QStringLiteral("state");
	update.owner = QStringLiteral("session");
	update.sessionId = QStringLiteral("session");
	update.observation.instanceId = QStringLiteral("popup-session-instance");
	update.observation.revision = 1;
	update.observation.state = QStringLiteral("RUNNING");
	QVERIFY(state.applyEvent(update));
	QVERIFY(view->isVisible());
	QCOMPARE(view->currentIndex().row(), highlighted);
	QCOMPARE(view->verticalScrollBar()->value(), scroll);
	QCOMPARE(selector->currentData().toString(), QStringLiteral("stream-10"));
	selector->hidePopup();
}

void PluginLifecycleTest::savedStreamIsUnconfirmedUntilListIsComplete()
{
	gost::StateStore state;
	const auto text = localeLookup();
	gost::ManagementWindow window(state, text);
	auto *selector = window.findChild<QComboBox *>(QStringLiteral("streamSelector"));
	QVERIFY(selector);
	state.setSelection(QStringLiteral("saved-stream"), true);
	QCOMPARE(selector->currentText(), text("Stream.Unconfirmed").arg(QStringLiteral("saved-stream")));
	state.setStreams({}, false);
	QCOMPARE(selector->currentText(), text("Stream.Unconfirmed").arg(QStringLiteral("saved-stream")));
	state.setStreams({}, true);
	QCOMPARE(selector->currentText(), text("Stream.Unavailable").arg(QStringLiteral("saved-stream")));
	gost::api::Stream stream;
	stream.id = QStringLiteral("saved-stream");
	stream.name = QStringLiteral("Подтверждённый поток");
	state.setStreams({stream}, true);
	QCOMPARE(selector->currentText(), stream.name);
}

void PluginLifecycleTest::partialStreamListWarnsAndAllowsSelection_data()
{
	QTest::addColumn<QString>("locale");
	QTest::newRow("russian") << QStringLiteral("ru-RU");
	QTest::newRow("english") << QStringLiteral("en-US");
}

void PluginLifecycleTest::partialStreamListWarnsAndAllowsSelection()
{
	QFETCH(QString, locale);
	WorkspaceTemp temp(QStringLiteral("partial-stream-list"));
	QVERIFY(temp.isValid());
	HttpServer server;
	QVERIFY(server.listen());
	const auto first = fixture::page({fixture::stream(QStringLiteral("received"))}, QStringLiteral("next-page"));
	const auto second = fixture::page({fixture::stream(QStringLiteral("saved"))});
	int readyRequests = 0;
	int pageRequests = 0;
	int snapshotRequests = 0;
	int eventRequests = 0;
	HttpServer::Request firstSubscription;
	HttpServer::Request selectedSubscription;
	server.handler = [&](const auto &request) {
		if (request.target.startsWith("/api/v1/health/ready")) {
			++readyRequests;
			HttpServer::reply(request, R"({"status":"ready","reason":""})");
		} else if (request.target.startsWith("/api/v1/system/capabilities")) {
			HttpServer::reply(request, fixture::capabilities());
		} else if (request.target.startsWith("/api/v1/streams")) {
			++pageRequests;
			HttpServer::reply(request, request.target.contains("cursor=") ? second : first);
		} else if (request.target.startsWith("/api/v1/events/snapshot")) {
			++snapshotRequests;
			HttpServer::reply(request, R"({"cursor":"anchor","sessions":[],"gpu":null})");
		} else if (request.target.startsWith("/api/v1/events?")) {
			if (++eventRequests == 1)
				firstSubscription = request;
			else
				selectedSubscription = request;
		}
	};
	const auto path = temp.filePath(QStringLiteral("settings.json"));
	gost::SettingsStore settings(path);
	settings.setAutoConnect(false);
	gost::StateStore state;
	gost::api::Limits limits;
	limits.pageWalkBytes = first.size() + second.size() - 1;
	gost::api::APIClient api(limits);
	gost::api::EventClient events;
	gost::ConnectionController controller(state, settings, api, events);
	controller.observeProfile(gost::FrontendEvent::FinishedLoading,
				  {QStringLiteral("A"), QStringLiteral("profiles/a"), {QStringLiteral("A")}, true});
	QVERIFY(state.profileResolved());
	const auto origin = server.origin().toString();
	const auto profileId = settings.profileUuid();
	QVERIFY(settings.setSelectedStream(origin, profileId, QStringLiteral("saved")));
	const auto text = localeLookup(locale);
	gost::ManagementWindow window(state, text, nullptr, &controller);
	window.show();
	auto *navigation = window.findChild<QListWidget *>(QStringLiteral("managementNavigation"));
	auto *selector = window.findChild<QComboBox *>(QStringLiteral("streamSelector"));
	auto *reconnect = window.findChild<QCheckBox *>(QStringLiteral("reconnect"));
	auto *message = window.findChild<QLabel *>(QStringLiteral("connectionMessage"));
	auto *retries = window.findChild<QSpinBox *>(QStringLiteral("sseRejectionRetries"));
	QVERIFY(navigation && selector && reconnect && message && retries);
	navigation->setCurrentRow(5);
	QVERIFY(selector->isVisible());
	QVERIFY(!retries->isVisible());
	reconnect->setChecked(false);
	navigation->setCurrentRow(6);
	QVERIFY(retries->isVisible());
	QVERIFY(!retries->isEnabled());
	navigation->setCurrentRow(5);
	reconnect->setChecked(true);
	navigation->setCurrentRow(6);
	QVERIFY(retries->isEnabled());
	QCOMPARE(retries->value(), 3);
	retries->setValue(-1);
	QCOMPARE(settings.sseRejectionRetries(), -1);
	gost::SettingsStore restored(path);
	QCOMPARE(restored.sseRejectionRetries(), -1);
	navigation->setCurrentRow(5);

	controller.connectToServer(origin);
	QTRY_VERIFY_WITH_TIMEOUT(firstSubscription.socket, 3000);
	QCOMPARE(state.connectionStatus(), gost::ConnectionStatus::Connecting);
	QCOMPARE(readyRequests, 1);
	QCOMPARE(pageRequests, 2);
	QCOMPARE(snapshotRequests, 1);
	QCOMPARE(eventRequests, 1);
	QVERIFY(!state.streamsComplete());
	QCOMPARE(state.selectedStreamId(), QStringLiteral("saved"));
	QVERIFY(!state.selectionAvailable());
	QVERIFY(restored.load());
	QCOMPARE(restored.selectedStream(origin, profileId), QStringLiteral("saved"));
	QVERIFY(message->isVisible());
	QVERIFY(message->text().contains(text("Stream.ListIncomplete")));
	QCOMPARE(selector->currentText(), text("Stream.Unconfirmed").arg(QStringLiteral("saved")));
	QVERIFY(selector->isEnabled());
	const int receivedIndex = selector->findData(QStringLiteral("received"));
	QVERIFY(receivedIndex > 0);
	selector->setCurrentIndex(receivedIndex);
	QCOMPARE(state.selectedStreamId(), QStringLiteral("received"));
	QVERIFY(state.selectionAvailable());
	QVERIFY(restored.load());
	QCOMPARE(restored.selectedStream(origin, profileId), QStringLiteral("received"));
	QTRY_VERIFY_WITH_TIMEOUT(firstSubscription.socket.isNull(), 2000);
	QTRY_VERIFY_WITH_TIMEOUT(selectedSubscription.socket, 3000);
	QCOMPARE(eventRequests, 2);
	QCOMPARE(snapshotRequests, 2);
	QCOMPARE(state.connectionStatus(), gost::ConnectionStatus::Connecting);
	HttpServer::stream(selectedSubscription);
	QTRY_COMPARE_WITH_TIMEOUT(state.connectionStatus(), gost::ConnectionStatus::Connected, 2000);
	QCOMPARE(state.selectedStreamId(), QStringLiteral("received"));
	QVERIFY(message->text().contains(text("Stream.ListIncomplete")));
	controller.shutdown();
}

QTEST_MAIN(PluginLifecycleTest)

#include "plugin-lifecycle-test.moc"
