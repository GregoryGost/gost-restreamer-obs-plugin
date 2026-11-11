#include "core/stats-baseline.hpp"

#include <QtTest/QtTest>

#include <limits>

namespace {

using gost::StatsBaseline;
using Counters = StatsBaseline::Counters;
using Period = StatsBaseline::Period;

const StatsBaseline::Identity firstRun{QStringLiteral("output-a"), 1};

} // namespace

// Проверки чистой арифметики без цикла событий Qt, OBS и сети. Числа выбраны так, чтобы полный счётчик,
// дельта после reset и дельта после разрыва давали разные проценты. Каждый сценарий создаёт новый baseline.
class StatsBaselineTest final : public QObject {
	Q_OBJECT
private slots:
	void usesTheSamePeriodForBothCounters();
	void staleReadingsCannotReplaceOrResetTheBaseline();
	void regressionStartsANewPeriod();
	void changesOfInstanceAndEpochStartNewRuns();
	void saturationDoesNotBecomeAZeroIncrement();
	void missingDenominatorDoesNotBecomeZeroPercent();
};

void StatsBaselineTest::usesTheSamePeriodForBothCounters()
{
	StatsBaseline baseline;
	baseline.observe(firstRun, Counters{8, 100}, true);
	QVERIFY(baseline.canReset());
	QCOMPARE(baseline.reading().percent, std::optional{8.0});
	baseline.reset();
	auto reading = baseline.reading();
	QVERIFY(reading.counters);
	QCOMPARE(reading.counters->count, quint64{0});
	QCOMPARE(reading.counters->total, std::optional<quint64>{0});
	QVERIFY(!reading.percent);
	QCOMPARE(reading.period, Period::SinceReset);

	baseline.observe(firstRun, Counters{10, 110}, true);
	// После сброса прирост равен 2/10, поэтому ожидается 20%, а не отношение полных накоплений 10/110.
	reading = baseline.reading();
	QVERIFY(reading.counters);
	QCOMPARE(reading.counters->count, quint64{2});
	QCOMPARE(reading.counters->total, std::optional<quint64>{10});
	QCOMPARE(reading.percent, std::optional{20.0});

	baseline.reset();
	baseline.observe(firstRun, Counters{11, 130}, true);
	reading = baseline.reading();
	QVERIFY(reading.counters);
	QCOMPARE(reading.counters->count, quint64{1});
	QCOMPARE(reading.counters->total, std::optional<quint64>{20});
	QCOMPARE(reading.percent, std::optional{5.0});
}

void StatsBaselineTest::staleReadingsCannotReplaceOrResetTheBaseline()
{
	// Большие устаревшие числа намеренно отличаются от последнего свежего чтения. Они не должны ни
	// попасть на экран, ни стать скрытой точкой следующего reset; восстановление продолжает прежний период.
	StatsBaseline baseline;
	baseline.observe(firstRun, Counters{4, 40}, true);
	baseline.reset();
	baseline.observe(firstRun, Counters{6, 50}, true);
	baseline.observe(firstRun, Counters{700, 900}, false);
	QVERIFY(!baseline.canReset());
	baseline.reset();
	auto reading = baseline.reading();
	QVERIFY(reading.counters);
	QCOMPARE(reading.counters->count, quint64{2});
	QCOMPARE(reading.counters->total, std::optional<quint64>{10});

	baseline.observe(firstRun, Counters{8, 60}, true);
	reading = baseline.reading();
	QVERIFY(reading.counters);
	QCOMPARE(reading.counters->count, quint64{4});
	QCOMPARE(reading.counters->total, std::optional<quint64>{20});
	QCOMPARE(reading.period, Period::SinceReset);
}

void StatsBaselineTest::regressionStartsANewPeriod()
{
	// Сначала уменьшается лишь total, затем лишь count. Любой регресс разрывает сразу всю пару,
	// иначе unsigned-вычитание либо смешанные периоды дали бы правдоподобный неверный процент.
	StatsBaseline baseline;
	baseline.observe(firstRun, Counters{10, 100}, true);
	baseline.reset();
	baseline.observe(firstRun, Counters{15, 150}, true);

	baseline.observe(firstRun, Counters{16, 120}, true);
	auto reading = baseline.reading();
	QVERIFY(reading.counters);
	QCOMPARE(reading.counters->count, quint64{0});
	QCOMPARE(reading.counters->total, std::optional<quint64>{0});
	QVERIFY(!reading.percent);
	QCOMPARE(reading.period, Period::SinceNewSample);

	baseline.observe(firstRun, Counters{17, 124}, true);
	QCOMPARE(baseline.reading().percent, std::optional{25.0});
	baseline.observe(firstRun, Counters{15, 130}, true);
	reading = baseline.reading();
	QVERIFY(reading.counters);
	QCOMPARE(reading.counters->count, quint64{0});
	QCOMPARE(reading.counters->total, std::optional<quint64>{0});
}

void StatsBaselineTest::changesOfInstanceAndEpochStartNewRuns()
{
	// Проверяются обе координаты identity. Новое исполнение приходит без свежих счётчиков:
	// уже в этот момент старое показание должно исчезнуть, а не ждать первого нового измерения.
	StatsBaseline baseline;
	baseline.observe(firstRun, Counters{5, 60}, true);
	baseline.reset();
	const StatsBaseline::Identity secondRun{firstRun.instance, 2};
	baseline.observe(secondRun, Counters{11, 80}, true);
	auto reading = baseline.reading();
	QVERIFY(reading.counters);
	QCOMPARE(reading.counters->count, quint64{11});
	QCOMPARE(reading.counters->total, std::optional<quint64>{80});
	QCOMPARE(reading.period, Period::CurrentRun);

	baseline.reset();
	baseline.observe({QStringLiteral("output-b"), secondRun.epoch}, std::nullopt, false);
	QVERIFY(!baseline.canReset());
	QVERIFY(!baseline.reading().counters);
	baseline.observe({QStringLiteral("output-b"), secondRun.epoch}, Counters{16, 100}, true);
	reading = baseline.reading();
	QVERIFY(reading.counters);
	QCOMPARE(reading.counters->count, quint64{16});
	QCOMPARE(reading.period, Period::CurrentRun);
}

void StatsBaselineTest::saturationDoesNotBecomeAZeroIncrement()
{
	StatsBaseline baseline;
	constexpr auto maximum = std::numeric_limits<quint64>::max();
	baseline.observe(firstRun, Counters{maximum - 5, maximum - 3}, true);
	baseline.reset();
	baseline.observe(firstRun, Counters{maximum - 4, maximum - 1}, true);
	QCOMPARE(baseline.reading().percent, std::optional{50.0});

	baseline.observe(firstRun, Counters{maximum, maximum}, true, true);
	// Насыщение делает прирост неизвестным; после него первая достоверная выборка задаёт новый период.
	auto reading = baseline.reading();
	QVERIFY(reading.limited);
	QVERIFY(!reading.counters);
	QVERIFY(!reading.percent);
	QVERIFY(!baseline.canReset());

	baseline.observe(firstRun, Counters{3, 20}, true);
	reading = baseline.reading();
	QVERIFY(!reading.limited);
	QVERIFY(reading.counters);
	QCOMPARE(reading.counters->count, quint64{0});
	QCOMPARE(reading.counters->total, std::optional<quint64>{0});
	QCOMPARE(reading.period, Period::SinceNewSample);
}

void StatsBaselineTest::missingDenominatorDoesNotBecomeZeroPercent()
{
	// count без total остаётся полезен. Появление total создаёт новую границу; только следующая
	// выборка с положительным приростом знаменателя может достоверно показать нулевой процент.
	StatsBaseline baseline;
	baseline.observe(firstRun, Counters{17, std::nullopt}, true);
	QVERIFY(baseline.canReset());
	baseline.reset();
	baseline.observe(firstRun, Counters{20, std::nullopt}, true);
	auto reading = baseline.reading();
	QVERIFY(reading.counters);
	QCOMPARE(reading.counters->count, quint64{3});
	QVERIFY(!reading.counters->total);
	QVERIFY(!reading.percent);

	baseline.observe(firstRun, Counters{20, 100}, true);
	reading = baseline.reading();
	QVERIFY(reading.counters);
	QCOMPARE(reading.counters->count, quint64{0});
	QVERIFY(!reading.percent);
	baseline.observe(firstRun, Counters{20, 101}, true);
	QCOMPARE(baseline.reading().percent, std::optional{0.0});
}

QTEST_APPLESS_MAIN(StatsBaselineTest)
#include "stats-baseline-test.moc"
