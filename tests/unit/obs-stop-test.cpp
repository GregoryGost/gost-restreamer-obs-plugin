#include "core/state-store.hpp"
#include "fixtures/fake-frontend.hpp"
#include "obs/obs-controller.hpp"

#include <QTest>
#include <QTimer>

// Проверяет подтверждение штатной остановки через реальные события Qt с управляемой границей OBS.
class ObsStopTest final : public QObject {
	Q_OBJECT
private slots:
	// Уже остановленный доступный выход завершается без вызова void stop.
	void alreadyStopped();
	// Активность и переподключение требуют отдельного события Stopped после запроса stop.
	void activeStop_data();
	// Выполняет сценарий active/reconnecting из строки данных.
	void activeStop();
	// Временные false-флаги Starting не являются остановкой.
	void startingWaitsForItsOwnStop_data();
	// Проверяет запуск до или после FinishedLoading по выбранной строке данных.
	void startingWaitsForItsOwnStop();
	// Следующий ручной запуск не преследуется остановкой предыдущего поколения.
	void newGenerationIsNotStopped();
	// Недоступное наблюдение и отказ stop дают false в ограниченный срок.
	void unavailableAndRefused_data();
	// Выполняет сценарий недоступности или неподтверждённого stop.
	void unavailableAndRefused();
	// Уничтожение получателя подавляет результат и прекращает дальнейшие обращения stop.
	void receiverDestructionCancels();
	// Shutdown подавляет callback и запрещает поздние обращения к Frontend.
	void shutdownCancels();
};

void ObsStopTest::alreadyStopped()
{
	// frontend управляет доступным остановленным выходом; state принимает независимые наблюдения.
	FakeFrontend frontend;
	gost::StateStore state;
	// obs завершается до frontend/state и выполняет callbacks в GUI-потоке.
	// obs согласует фазовые события и отдельную выборку флагов штатного выхода.
	gost::OBSController obs(frontend, state);
	obs.handleEvent(gost::FrontendEvent::FinishedLoading);
	// done отмечает доставку, confirmed содержит именно подтверждение остановки.
	bool done = false;
	bool confirmed = false;
	obs.stopStreaming(
		this,
		[&](bool result) {
			confirmed = result;
			done = true;
		},
		100);
	QTRY_VERIFY_WITH_TIMEOUT(done, 1000);
	QVERIFY(confirmed);
	QCOMPARE(frontend.stopRequests, 0);
}

void ObsStopTest::activeStop_data()
{
	QTest::addColumn<bool>("reconnecting");
	QTest::newRow("active") << false;
	QTest::newRow("reconnecting") << true;
}

void ObsStopTest::activeStop()
{
	// reconnecting выбирает переподключение вместо обычной активности штатного выхода.
	QFETCH(bool, reconnecting);
	// frontend задаёт активный или переподключающийся штатный выход; state принимает метрики.
	FakeFrontend frontend;
	gost::StateStore state;
	frontend.streaming = !reconnecting;
	frontend.reconnecting = reconnecting;
	// obs получает исходную фазу Started, отличая её от остановки с false-флагами.
	// obs согласует фазовые события и отдельную выборку флагов штатного выхода.
	gost::OBSController obs(frontend, state);
	obs.handleEvent(gost::FrontendEvent::FinishedLoading);
	obs.handleEvent(gost::FrontendEvent::StreamingStarted);
	// done отмечает доставку callback; confirmed содержит подтверждение фактической остановки.
	bool done = false;
	bool confirmed = false;
	obs.stopStreaming(
		this,
		[&](bool result) {
			confirmed = result;
			done = true;
		},
		500);
	QTRY_COMPARE_WITH_TIMEOUT(frontend.stopRequests, 1, 1000);
	QVERIFY(!done);
	frontend.streaming = false;
	frontend.reconnecting = false;
	obs.handleEvent(gost::FrontendEvent::StreamingStopped);
	QTRY_VERIFY_WITH_TIMEOUT(done, 1000);
	QVERIFY(confirmed);
	QCOMPARE(frontend.stopRequests, 1);
}

void ObsStopTest::startingWaitsForItsOwnStop_data()
{
	QTest::addColumn<bool>("early");
	QTest::newRow("ready-starting") << false;
	QTest::newRow("starting-before-ready") << true;
}

void ObsStopTest::startingWaitsForItsOwnStop()
{
	// early задаёт поступление Starting до готовности Frontend.
	QFETCH(bool, early);
	// frontend пока выдаёт false во время запуска, state не определяет успех операции.
	FakeFrontend frontend;
	gost::StateStore state;
	// obs согласует фазовые события и отдельную выборку флагов штатного выхода.
	gost::OBSController obs(frontend, state);
	if (early)
		obs.handleEvent(gost::FrontendEvent::StreamingStarting);
	QCOMPARE(frontend.streamingStateReads, 0);
	obs.handleEvent(gost::FrontendEvent::FinishedLoading);
	if (!early)
		obs.handleEvent(gost::FrontendEvent::StreamingStarting);
	// done отмечает доставку callback; confirmed содержит подтверждение фактической остановки.
	bool done = false;
	bool confirmed = false;
	obs.stopStreaming(
		this,
		[&](bool result) {
			confirmed = result;
			done = true;
		},
		500);
	QVERIFY(!done);
	QCOMPARE(frontend.stopRequests, 0);
	frontend.streaming = true;
	obs.handleEvent(gost::FrontendEvent::StreamingStarted);
	QTRY_COMPARE_WITH_TIMEOUT(frontend.stopRequests, 1, 1000);
	QVERIFY(!done);
	frontend.streaming = false;
	obs.handleEvent(gost::FrontendEvent::StreamingStopped);
	QTRY_VERIFY_WITH_TIMEOUT(done, 1000);
	QVERIFY(confirmed);
}

void ObsStopTest::newGenerationIsNotStopped()
{
	// frontend имитирует два последовательных ручных запуска; state получает только наблюдения.
	FakeFrontend frontend;
	gost::StateStore state;
	// obs согласует фазовые события и отдельную выборку флагов штатного выхода.
	gost::OBSController obs(frontend, state);
	frontend.streaming = true;
	obs.handleEvent(gost::FrontendEvent::FinishedLoading);
	obs.handleEvent(gost::FrontendEvent::StreamingStarting);
	obs.handleEvent(gost::FrontendEvent::StreamingStarted);
	// done отмечает доставку callback; confirmed содержит подтверждение фактической остановки.
	bool done = false;
	bool confirmed = true;
	obs.stopStreaming(
		this,
		[&](bool result) {
			confirmed = result;
			done = true;
		},
		500);
	QTRY_COMPARE_WITH_TIMEOUT(frontend.stopRequests, 1, 1000);
	obs.handleEvent(gost::FrontendEvent::StreamingStarting);
	QTRY_VERIFY_WITH_TIMEOUT(done, 1000);
	QVERIFY(!confirmed);
	obs.handleEvent(gost::FrontendEvent::StreamingStarted);
	QCOMPARE(frontend.stopRequests, 1);
}

void ObsStopTest::unavailableAndRefused_data()
{
	QTest::addColumn<bool>("available");
	QTest::newRow("unavailable") << false;
	QTest::newRow("refused") << true;
}

void ObsStopTest::unavailableAndRefused()
{
	// available отличает недоступное наблюдение от доступного, но не остановившегося выхода.
	QFETCH(bool, available);
	// frontend не присылает Stopped; available отличает отказ наблюдения от продолжающегося выхода.
	FakeFrontend frontend;
	gost::StateStore state;
	frontend.streaming = true;
	frontend.streamingStateAvailable = available;
	// obs согласует фазовые события и отдельную выборку флагов штатного выхода.
	gost::OBSController obs(frontend, state);
	obs.handleEvent(gost::FrontendEvent::FinishedLoading);
	obs.handleEvent(gost::FrontendEvent::StreamingStarted);
	// done отмечает доставку callback; confirmed содержит подтверждение фактической остановки.
	bool done = false;
	bool confirmed = true;
	obs.stopStreaming(
		this,
		[&](bool result) {
			confirmed = result;
			done = true;
		},
		30);
	QTRY_VERIFY_WITH_TIMEOUT(done, 1000);
	QVERIFY(!confirmed);
	QCOMPARE(frontend.stopRequests, available ? 1 : 0);
}

void ObsStopTest::receiverDestructionCancels()
{
	// frontend остаётся в Starting без активности, поэтому отложенный stop не должен быть отправлен.
	FakeFrontend frontend;
	gost::StateStore state;
	// obs согласует фазовые события и отдельную выборку флагов штатного выхода.
	gost::OBSController obs(frontend, state);
	obs.handleEvent(gost::FrontendEvent::FinishedLoading);
	obs.handleEvent(gost::FrontendEvent::StreamingStarting);
	// receiver ограничивает callback, called обнаруживает недопустимую доставку после удаления.
	auto receiver = std::make_unique<QObject>();
	bool called = false;
	obs.stopStreaming(receiver.get(), [&](bool) { called = true; }, 30);
	receiver.reset();
	frontend.streaming = true;
	obs.handleEvent(gost::FrontendEvent::StreamingStarted);
	// barrier отмечает доставку контрольного callback после срока операции без блокировки цикла событий.
	bool barrier = false;
	QTimer::singleShot(60, this, [&] { barrier = true; });
	QTRY_VERIFY_WITH_TIMEOUT(barrier, 1000);
	QVERIFY(!called);
	QCOMPARE(frontend.stopRequests, 0);
}

void ObsStopTest::shutdownCancels()
{
	// frontend обнаруживает любые обращения после явного завершения OBS.
	FakeFrontend frontend;
	gost::StateStore state;
	// obs согласует фазовые события и отдельную выборку флагов штатного выхода.
	gost::OBSController obs(frontend, state);
	frontend.streaming = true;
	obs.handleEvent(gost::FrontendEvent::FinishedLoading);
	// called фиксирует запрещённую доставку после shutdown.
	bool called = false;
	obs.stopStreaming(this, [&](bool) { called = true; }, 30);
	obs.shutdown();
	frontend.frontendUnavailable = true;
	obs.handleEvent(gost::FrontendEvent::StreamingStopped);
	// barrier отмечает контрольную доставку после срока операции без блокировки потока.
	bool barrier = false;
	QTimer::singleShot(60, this, [&] { barrier = true; });
	QTRY_VERIFY_WITH_TIMEOUT(barrier, 1000);
	QVERIFY(!called);
	QCOMPARE(frontend.callsAfterExit, 0);
}

QTEST_MAIN(ObsStopTest)
#include "obs-stop-test.moc"
