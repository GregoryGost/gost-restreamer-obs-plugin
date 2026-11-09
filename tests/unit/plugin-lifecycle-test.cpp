#include "fixtures/fake-frontend.hpp"
#include "core/plugin-lifecycle.hpp"
#include "core/state-store.hpp"
#include "ui/management-window.hpp"
#include "ui/restream-dock.hpp"

#include <QApplication>
#include <QComboBox>
#include <QCoreApplication>
#include <QEvent>
#include <QLineEdit>
#include <QLabel>
#include <QListWidget>
#include <QMap>
#include <QSettings>
#include <QSignalSpy>
#include <QStackedWidget>
#include <QTabWidget>
#include <QTest>
#include <QToolButton>

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
	void bothViewsFollowObservations_data();
	void bothViewsFollowObservations();
	void managementWindowIsLazyAndReused_data();
	void managementWindowIsLazyAndReused();
	void dockStaysCompact();
	void managementHasSixInternalPages_data();
	void managementHasSixInternalPages();
	void failedDockRegistrationReleasesResources();
	void registrationExceptionReleasesAcquiredResources_data();
	void registrationExceptionReleasesAcquiredResources();
	void callbackExceptionDoesNotEscape();
	void exitCleansSynchronouslyAndLeavesInertShell();
	void queuedOpenCannotResurrectRuntime();
	void queuedFrontendEventIsDiscardedOnExit();
	void hostDestructionDoesNotDoubleDelete();
	void localesAreComplete();
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
	QCOMPARE(dockStreaming->text(), text("OBS.StreamingActive"));
	QCOMPARE(windowStreaming->text(), text("OBS.StreamingActive"));
	window->close();
	frontend.profile = QStringLiteral("Переименованный профиль");
	frontend.streaming = false;
	frontend.emitEvent(gost::FrontendEvent::ProfileChanged);
	QCOMPARE(dockProfile->text(), frontend.profile);
	QCOMPARE(windowProfile->text(), frontend.profile);
	QCOMPARE(dockStreaming->text(), text("OBS.StreamingInactive"));
	QCOMPARE(windowStreaming->text(), text("OBS.StreamingInactive"));
	QCOMPARE(server->text(), text("Observation.NoData"));
	QCOMPARE(stream->text(), text("Observation.NoData"));
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

void PluginLifecycleTest::managementHasSixInternalPages_data()
{
	QTest::addColumn<QString>("locale");
	QTest::addColumn<QStringList>("pages");
	QTest::newRow("russian") << QStringLiteral("ru-RU")
				 << QStringList{QStringLiteral("Потоки"),           QStringLiteral("Выходы"),
						QStringLiteral("Группы и профили"), QStringLiteral("Заглушки"),
						QStringLiteral("Диагностика"),      QStringLiteral("Настройки")};
	QTest::newRow("english") << QStringLiteral("en-US") << QStringList{QStringLiteral("Streams"),
									   QStringLiteral("Outputs"),
									   QStringLiteral("Groups and profiles"),
									   QStringLiteral("Fallback"),
									   QStringLiteral("Diagnostics"),
									   QStringLiteral("Settings")};
}

void PluginLifecycleTest::managementHasSixInternalPages()
{
	QFETCH(QString, locale);
	QFETCH(QStringList, pages);
	gost::StateStore state;
	gost::ManagementWindow window(state, localeLookup(locale));
	auto *navigation = window.findChild<QListWidget *>();
	auto *stack = window.findChild<QStackedWidget *>();
	QVERIFY(navigation);
	QVERIFY(stack);
	QCOMPARE(navigation->count(), 6);
	QCOMPARE(stack->count(), 6);
	for (int index = 0; index < pages.size(); ++index) {
		QCOMPARE(navigation->item(index)->text(), pages.at(index));
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

QTEST_MAIN(PluginLifecycleTest)

#include "plugin-lifecycle-test.moc"
