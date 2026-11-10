#include "core/state-store.hpp"
#include "api/json-decoder.hpp"
#include "fixtures/api-json.hpp"

#include <QSignalSpy>
#include <QTest>

using namespace gost;
using namespace gost::api;

namespace {
template<class T> JsonField<T> known(T value)
{
	return {FieldState::Value, std::move(value)};
}
Observation observed(const QString &id, quint64 revision, const QString &state)
{
	return {id, revision, known(QStringLiteral("2026-11-10T01:02:03.123456789Z")), state, {FieldState::Null, {}}};
}
Anchor anchor()
{
	RuntimeSession session;
	session.sessionId = QStringLiteral("session");
	session.observation = known(observed(QStringLiteral("epoch:1"), 10, QStringLiteral("READY")));
	RuntimeGroup group;
	static_cast<Observation &>(group) = observed(QStringLiteral("epoch:2"), 50, QStringLiteral("READY"));
	group.groupId = QStringLiteral("group");
	group.profileId = QStringLiteral("profile");
	group.source = QStringLiteral("LIVE");
	group.sourceRevision = 3;
	group.metrics = known(GroupMetrics{});
	session.groups.append(group);
	RuntimeOutput output;
	static_cast<Observation &>(output) = observed(QStringLiteral("epoch:3"), 100, QStringLiteral("PUBLISHING"));
	output.outputId = QStringLiteral("output");
	OutputMetrics metrics;
	metrics.media.videoFrames = 500;
	output.metrics = known(metrics);
	session.outputs.append(output);
	RuntimeFFmpeg ffmpeg;
	static_cast<Observation &>(ffmpeg) = observed(QStringLiteral("epoch:4"), 80, QStringLiteral("READY"));
	ffmpeg.groupInstanceId = group.instanceId;
	ffmpeg.role = QStringLiteral("live");
	ffmpeg.metrics = known(FFmpegMetrics{});
	session.ffmpeg.append(ffmpeg);
	return {QStringLiteral("opaque-anchor"), {session}, {FieldState::Null, {}}};
}
RuntimeEvent runtimeEvent(QString owner, QString kind, QString instance, quint64 revision)
{
	RuntimeEvent result;
	result.cursor = QStringLiteral("opaque/") + owner + QLatin1Char('/') + kind + QLatin1Char('/') + instance +
			QLatin1Char('/') + QString::number(revision);
	result.kind = std::move(kind);
	result.owner = std::move(owner);
	result.sessionId = QStringLiteral("session");
	result.observation = observed(instance, revision, QString{});
	if (result.owner == QStringLiteral("output"))
		result.outputId = QStringLiteral("output");
	if (result.owner == QStringLiteral("group") || result.owner == QStringLiteral("ffmpeg")) {
		result.groupId = QStringLiteral("group");
		result.profileId = QStringLiteral("profile");
	}
	if (result.owner == QStringLiteral("ffmpeg")) {
		result.groupInstanceId = QStringLiteral("epoch:2");
		result.role = QStringLiteral("live");
	}
	return result;
}
} // namespace

class StateStoreTest final : public QObject {
	Q_OBJECT
private slots:
	void anchorDoesNotSeedMetricsRevision();
	void stateAndMetricsHaveIndependentWatermarks();
	void firstObservationCreatesOnlyKnownLane();
	void inputLifecycleZeroClearsPriorMetrics();
	void attemptClearsObservationAndMetricsRegardlessOfRevision();
	void removalMatchesIdentityAndRemovesDescendants();
	void newExecutionClearsPreviousLanes();
	void incompatibleHierarchyDoesNotAdvanceCursor();
	void historyAndRestDetailsCannotRollbackAuthority();
	void unavailableSelectionAndStalenessPreserveKnowledge();
	void concurrentGroupAndFfmpegInstancesRemainIndependent_data();
	void concurrentGroupAndFfmpegInstancesRemainIndependent();
	void systemRemovalClearsOnlyMatchingGpuObservation();
};

void StateStoreTest::anchorDoesNotSeedMetricsRevision()
{
	StateStore state;
	QSignalSpy changed(&state, &StateStore::runtimeStateChanged);
	QVERIFY(changed.isValid());
	QVERIFY(state.installAnchor(anchor()));
	auto metrics = runtimeEvent(QStringLiteral("output"), QStringLiteral("metrics"), QStringLiteral("epoch:3"), 1);
	OutputMetrics value;
	value.media.videoFrames = 777;
	metrics.output = known(value);
	QVERIFY(state.applyEvent(metrics));
	const auto output = state.runtime().sessions.value(QStringLiteral("session")).outputs.front();
	QCOMPARE(output.state, QStringLiteral("PUBLISHING"));
	QCOMPARE(output.revision, quint64(100));
	QCOMPARE(output.metrics.value.media.videoFrames, quint64(777));
	QCOMPARE(state.runtime().cursor, metrics.cursor);
	QCOMPARE(state.runtime().availability, RuntimeAvailability::Fresh);
	QVERIFY(changed.count() >= 2);
}

void StateStoreTest::stateAndMetricsHaveIndependentWatermarks()
{
	StateStore state;
	QVERIFY(state.installAnchor(anchor()));
	auto metrics =
		runtimeEvent(QStringLiteral("output"), QStringLiteral("metrics"), QStringLiteral("epoch:3"), 300);
	OutputMetrics value;
	value.media.videoFrames = 900;
	metrics.output = known(value);
	QVERIFY(state.applyEvent(metrics));
	auto update = runtimeEvent(QStringLiteral("output"), QStringLiteral("state"), QStringLiteral("epoch:3"), 101);
	update.observation.state = QStringLiteral("RETRYING");
	QVERIFY(state.applyEvent(update));
	const auto current = state.runtime().sessions.value(QStringLiteral("session")).outputs.front();
	QCOMPARE(current.state, QStringLiteral("RETRYING"));
	QCOMPARE(current.metrics.value.media.videoFrames, quint64(900));
	metrics.observation.revision = 299;
	metrics.output.value.media.videoFrames = 1;
	metrics.cursor = QStringLiteral("opaque-stale-metric");
	QVERIFY(state.applyEvent(metrics));
	QCOMPARE(
		state.runtime().sessions.value(QStringLiteral("session")).outputs.front().metrics.value.media.videoFrames,
		quint64(900));
	QCOMPARE(state.runtime().cursor, metrics.cursor);
}

void StateStoreTest::firstObservationCreatesOnlyKnownLane()
{
	StateStore state;
	Anchor empty{QStringLiteral("opaque-start"), {}, {FieldState::Null, {}}};
	QVERIFY(state.installAnchor(empty));
	auto metrics = runtimeEvent(QStringLiteral("output"), QStringLiteral("metrics"), QStringLiteral("epoch:3"), 1);
	OutputMetrics value;
	value.media.videoFrames = 17;
	metrics.output = known(value);
	QVERIFY(state.applyEvent(metrics));
	const auto session = state.runtime().sessions.value(QStringLiteral("session"));
	QVERIFY(!session.observation.hasValue());
	QCOMPARE(session.outputs.size(), 1);
	QCOMPARE(session.outputs.front().metrics.value.media.videoFrames, quint64(17));
	QVERIFY(session.outputs.front().state.isEmpty() || session.outputs.front().state == QStringLiteral("UNKNOWN"));
}

void StateStoreTest::inputLifecycleZeroClearsPriorMetrics()
{
	StateStore state;
	QVERIFY(state.installAnchor(anchor()));
	auto metrics =
		runtimeEvent(QStringLiteral("session"), QStringLiteral("metrics"), QStringLiteral("epoch:1"), 40);
	Input input;
	input.connected = true;
	InputMetrics value;
	value.media.videoFrames = 100;
	input.metrics = known(value);
	metrics.input = known(input);
	QVERIFY(state.applyEvent(metrics));
	auto lifecycle = runtimeEvent(QStringLiteral("session"), QStringLiteral("input"), QStringLiteral("epoch:1"), 0);
	Input disconnected;
	disconnected.metrics.state = FieldState::Null;
	lifecycle.input = known(disconnected);
	QVERIFY(state.applyEvent(lifecycle));
	const auto cleared = state.runtime().sessions.value(QStringLiteral("session")).input;
	QVERIFY(cleared.hasValue());
	QVERIFY(!cleared.value.connected);
	QVERIFY(!cleared.value.metrics.hasValue());
	metrics.observation.revision = 1;
	metrics.cursor = QStringLiteral("opaque-input-after-reconnect");
	metrics.input.value.metrics.value.media.videoFrames = 2;
	QVERIFY(state.applyEvent(metrics));
	QCOMPARE(state.runtime().sessions.value(QStringLiteral("session")).input.value.metrics.value.media.videoFrames,
		 quint64(2));
}

void StateStoreTest::attemptClearsObservationAndMetricsRegardlessOfRevision()
{
	StateStore state;
	QVERIFY(state.installAnchor(anchor()));
	auto attempt = runtimeEvent(QStringLiteral("ffmpeg"), QStringLiteral("attempt"), QStringLiteral("epoch:4"), 80);
	attempt.attemptActive = false;
	QVERIFY(state.applyEvent(attempt));
	const auto reset = state.runtime().sessions.value(QStringLiteral("session")).ffmpeg.front();
	QVERIFY(!reset.metrics.hasValue());
	QVERIFY(reset.state.isEmpty() || reset.state == QStringLiteral("UNKNOWN"));
	auto metrics = runtimeEvent(QStringLiteral("ffmpeg"), QStringLiteral("metrics"), QStringLiteral("epoch:4"), 1);
	FFmpegMetrics value;
	value.progressLosses = 7;
	metrics.ffmpeg = known(value);
	QVERIFY(state.applyEvent(metrics));
	QCOMPARE(state.runtime().sessions.value(QStringLiteral("session")).ffmpeg.front().metrics.value.progressLosses,
		 quint64(7));
}

void StateStoreTest::removalMatchesIdentityAndRemovesDescendants()
{
	StateStore state;
	QVERIFY(state.installAnchor(anchor()));
	auto wrong = runtimeEvent(QStringLiteral("output"), QStringLiteral("removed"), QStringLiteral("epoch:999"), 0);
	QVERIFY(state.applyEvent(wrong));
	QCOMPARE(state.runtime().sessions.value(QStringLiteral("session")).outputs.size(), 1);
	auto group = runtimeEvent(QStringLiteral("group"), QStringLiteral("removed"), QStringLiteral("epoch:2"), 0);
	QVERIFY(state.applyEvent(group));
	const auto remaining = state.runtime().sessions.value(QStringLiteral("session"));
	QVERIFY(remaining.groups.isEmpty());
	QVERIFY(remaining.ffmpeg.isEmpty());
	QCOMPARE(remaining.outputs.size(), 1);
	auto session = runtimeEvent(QStringLiteral("session"), QStringLiteral("removed"), QStringLiteral("epoch:1"), 0);
	QVERIFY(state.applyEvent(session));
	QVERIFY(state.runtime().sessions.isEmpty());
}

void StateStoreTest::newExecutionClearsPreviousLanes()
{
	StateStore state;
	QVERIFY(state.installAnchor(anchor()));
	auto replacement =
		runtimeEvent(QStringLiteral("output"), QStringLiteral("state"), QStringLiteral("epoch:30"), 1);
	replacement.observation.state = QStringLiteral("CONNECTING");
	QVERIFY(state.applyEvent(replacement));
	const auto current = state.runtime().sessions.value(QStringLiteral("session")).outputs.front();
	QCOMPARE(current.instanceId, QStringLiteral("epoch:30"));
	QCOMPARE(current.state, QStringLiteral("CONNECTING"));
	QVERIFY(!current.metrics.hasValue());
	QVERIFY(state.applyEvent(
		runtimeEvent(QStringLiteral("output"), QStringLiteral("removed"), QStringLiteral("epoch:3"), 0)));
	QCOMPARE(state.runtime().sessions.value(QStringLiteral("session")).outputs.front().instanceId,
		 QStringLiteral("epoch:30"));
}

void StateStoreTest::incompatibleHierarchyDoesNotAdvanceCursor()
{
	StateStore state;
	QVERIFY(state.installAnchor(anchor()));
	auto invalid = runtimeEvent(QStringLiteral("ffmpeg"), QStringLiteral("state"), QStringLiteral("epoch:4"), 81);
	invalid.groupInstanceId = QStringLiteral("missing-parent");
	invalid.observation.state = QStringLiteral("FAILED");
	QVERIFY(!state.applyEvent(invalid));
	QCOMPARE(state.runtime().cursor, QStringLiteral("opaque-anchor"));
	QCOMPARE(state.runtime().sessions.value(QStringLiteral("session")).ffmpeg.front().state,
		 QStringLiteral("READY"));
}

void StateStoreTest::historyAndRestDetailsCannotRollbackAuthority()
{
	StateStore state;
	QVERIFY(state.installAnchor(anchor()));
	HistoryState history;
	history.sessions.insert(QStringLiteral("session"), *decode<Session>(fixture::bytes(fixture::session())).value);
	history.outputs.insert(QStringLiteral("output"), *decode<Output>(fixture::bytes(fixture::output())).value);
	state.setHistory(history);
	ConfigurationState config;
	Stream current;
	current.id = QStringLiteral("stream");
	current.name = QStringLiteral("Новая конфигурация");
	config.streams.insert(current.id, current);
	state.setConfiguration(config);
	DetailState details;
	OutputDetail old;
	old.runtime = known(RuntimeOutput{});
	old.runtime.value.state = QStringLiteral("STOPPED");
	details.outputs.insert(QStringLiteral("output"), old);
	state.setDetails(details);
	QCOMPARE(state.runtime().sessions.value(QStringLiteral("session")).outputs.front().state,
		 QStringLiteral("PUBLISHING"));
	QCOMPARE(state.runtime().cursor, QStringLiteral("opaque-anchor"));
	QCOMPARE(state.history().sessions.value(QStringLiteral("session")).stream.name, QStringLiteral("Поток"));
	QCOMPARE(state.history().outputs.value(QStringLiteral("output")).destination.endpoint.value,
		 fixture::endpoint());
}

void StateStoreTest::unavailableSelectionAndStalenessPreserveKnowledge()
{
	StateStore state;
	state.setObsState(QStringLiteral("OBS"), true);
	state.setSelection(QStringLiteral("saved-missing"), true);
	Stream other;
	other.id = QStringLiteral("other");
	state.setStreams({other}, true);
	QVERIFY(!state.selectionAvailable());
	QCOMPARE(state.selectedStreamId(), QStringLiteral("saved-missing"));
	QVERIFY(state.installAnchor(anchor()));
	state.markRuntimeStale();
	QCOMPARE(state.runtime().availability, RuntimeAvailability::Stale);
	QCOMPARE(state.runtime().sessions.value(QStringLiteral("session")).outputs.front().state,
		 QStringLiteral("PUBLISHING"));
	state.clearServerState();
	QCOMPARE(state.runtime().availability, RuntimeAvailability::Unknown);
	QVERIFY(state.runtime().sessions.isEmpty());
	QCOMPARE(state.profileName().value(), QStringLiteral("OBS"));
	QVERIFY(state.streamingActive().value());
}

void StateStoreTest::concurrentGroupAndFfmpegInstancesRemainIndependent_data()
{
	QTest::addColumn<QString>("role");
	QTest::newRow("named-role") << QStringLiteral("live");
	QTest::newRow("unnamed-role") << QString{};
}

void StateStoreTest::concurrentGroupAndFfmpegInstancesRemainIndependent()
{
	QFETCH(QString, role);
	auto initial = anchor();
	auto &session = initial.sessions.front();
	session.ffmpeg.front().role = role;
	auto group = session.groups.front();
	group.instanceId = QStringLiteral("epoch:20");
	session.groups.append(group);
	auto sibling = session.ffmpeg.front();
	sibling.instanceId = QStringLiteral("epoch:40");
	session.ffmpeg.append(sibling);
	StateStore state;
	QVERIFY(state.installAnchor(initial));
	QCOMPARE(state.runtime().sessions.value(QStringLiteral("session")).groups.size(), 2);
	QCOMPARE(state.runtime().sessions.value(QStringLiteral("session")).ffmpeg.size(), 2);
	auto update = runtimeEvent(QStringLiteral("group"), QStringLiteral("state"), QStringLiteral("epoch:20"), 51);
	update.observation.state = QStringLiteral("RETRYING");
	QVERIFY(state.applyEvent(update));
	QCOMPARE(state.runtime().sessions.value(QStringLiteral("session")).groups.size(), 2);
	auto child = runtimeEvent(QStringLiteral("ffmpeg"), QStringLiteral("state"), QStringLiteral("epoch:41"), 1);
	child.groupInstanceId = QStringLiteral("epoch:20");
	child.role = role;
	child.observation.state = QStringLiteral("STARTING");
	QVERIFY(state.applyEvent(child));
	QCOMPARE(state.runtime().sessions.value(QStringLiteral("session")).ffmpeg.size(), 3);
	auto removeSibling =
		runtimeEvent(QStringLiteral("ffmpeg"), QStringLiteral("removed"), QStringLiteral("epoch:40"), 0);
	removeSibling.role = role;
	QVERIFY(state.applyEvent(removeSibling));
	QCOMPARE(state.runtime().sessions.value(QStringLiteral("session")).ffmpeg.size(), 2);
	QVERIFY(state.applyEvent(
		runtimeEvent(QStringLiteral("group"), QStringLiteral("removed"), QStringLiteral("epoch:2"), 0)));
	const auto remaining = state.runtime().sessions.value(QStringLiteral("session"));
	QCOMPARE(remaining.groups.size(), 1);
	QCOMPARE(remaining.groups.front().instanceId, QStringLiteral("epoch:20"));
	QCOMPARE(remaining.ffmpeg.size(), 1);
	QCOMPARE(remaining.ffmpeg.front().instanceId, QStringLiteral("epoch:41"));
	QCOMPARE(remaining.ffmpeg.front().groupInstanceId, QStringLiteral("epoch:20"));
}

void StateStoreTest::systemRemovalClearsOnlyMatchingGpuObservation()
{
	StateStore state;
	auto initial = anchor();
	GPU anchored;
	anchored.status = QStringLiteral("available");
	initial.gpu = known(anchored);
	QVERIFY(state.installAnchor(initial));
	RuntimeEvent removed;
	removed.kind = QStringLiteral("removed");
	removed.owner = QStringLiteral("system");
	removed.observation.instanceId = QStringLiteral("epoch:900");
	removed.cursor = QStringLiteral("removed-after-anchor");
	QVERIFY(state.applyEvent(removed));
	QVERIFY(!state.runtime().gpu.hasValue());
	QCOMPARE(state.runtime().sessions.size(), 1);
	RuntimeEvent gpu;
	gpu.kind = QStringLiteral("gpu");
	gpu.owner = QStringLiteral("system");
	gpu.observation.instanceId = QStringLiteral("epoch:901");
	gpu.observation.revision = 5;
	gpu.cursor = QStringLiteral("gpu-a");
	GPU value;
	value.status = QStringLiteral("warming_up");
	gpu.gpu = known(value);
	QVERIFY(state.applyEvent(gpu));
	gpu.observation.instanceId = QStringLiteral("epoch:902");
	gpu.observation.revision = 1;
	gpu.cursor = QStringLiteral("gpu-b");
	gpu.gpu.value.status = QStringLiteral("available");
	QVERIFY(state.applyEvent(gpu));
	removed.cursor = QStringLiteral("removed-old-system");
	removed.observation.instanceId = QStringLiteral("epoch:901");
	QVERIFY(state.applyEvent(removed));
	QVERIFY(state.runtime().gpu.hasValue());
	QCOMPARE(state.runtime().gpu.value.status, QStringLiteral("available"));
	removed.cursor = QStringLiteral("removed-current-system");
	removed.observation.instanceId = QStringLiteral("epoch:902");
	QVERIFY(state.applyEvent(removed));
	QVERIFY(!state.runtime().gpu.hasValue());
	QCOMPARE(state.runtime().sessions.size(), 1);
	QCOMPARE(state.runtime().sessions.value(QStringLiteral("session")).outputs.front().state,
		 QStringLiteral("PUBLISHING"));
	const auto cursor = state.runtime().cursor;
	removed.kind = QStringLiteral("state");
	removed.cursor = QStringLiteral("invalid-system-state");
	QVERIFY(!state.applyEvent(removed));
	QCOMPARE(state.runtime().cursor, cursor);
}

QTEST_GUILESS_MAIN(StateStoreTest)
#include "state-store-test.moc"
