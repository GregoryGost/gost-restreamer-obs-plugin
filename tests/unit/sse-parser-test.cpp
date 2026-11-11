#include "api/sse-parser.hpp"

#include <QTest>

using namespace gost::api;

class SSEParserTest final : public QObject {
	Q_OBJECT
private slots:
	void fragmentedUnicodeAndLineEndings_data();
	void fragmentedUnicodeAndLineEndings();
	void commentsAndUnknownFieldsDoNotCreateEvents();
	void unfinishedFrameIsNotPublished_data();
	void unfinishedFrameIsNotPublished();
	void invalidUtf8IsRejected_data();
	void invalidUtf8IsRejected();
	void frameAndQueueLimitsAreBounded();
	void drainingQueueReleasesBudget();
	void resetDiscardsPartialAndFailedInput();
};

void SSEParserTest::fragmentedUnicodeAndLineEndings_data()
{
	QTest::addColumn<QByteArray>("ending");
	QTest::newRow("lf") << QByteArray("\n");
	QTest::newRow("crlf") << QByteArray("\r\n");
	QTest::newRow("cr") << QByteArray("\r");
}

void SSEParserTest::fragmentedUnicodeAndLineEndings()
{
	QFETCH(QByteArray, ending);
	const auto expected = QStringLiteral("{\"name\":\"Поток 😀\",\n\"value\":1}").toUtf8();
	const auto wire = QByteArray::fromHex("efbbbf") + "event: metrics" + ending + "id: opaque/%2B=" + ending +
			  "data: " + QStringLiteral("{\"name\":\"Поток 😀\",").toUtf8() + ending +
			  "data: \"value\":1}" + ending + ending;
	for (qsizetype cut = 0; cut <= wire.size(); ++cut) {
		SSEParser parser;
		QVERIFY(parser.feed(wire.first(cut)));
		QVERIFY(parser.feed(wire.sliced(cut)));
		QVERIFY(parser.hasFrame());
		const auto frame = parser.takeFrame();
		QCOMPARE(frame.event, QStringLiteral("metrics"));
		QCOMPARE(frame.id, QStringLiteral("opaque/%2B="));
		QCOMPARE(frame.data, expected);
		QVERIFY(!parser.hasFrame());
		QVERIFY(!parser.hasError());
	}
	SSEParser bytewise;
	for (const auto byte : wire)
		QVERIFY(bytewise.feed(QByteArray(1, byte)));
	QVERIFY(bytewise.hasFrame());
	QCOMPARE(bytewise.takeFrame().data, expected);
}

void SSEParserTest::commentsAndUnknownFieldsDoNotCreateEvents()
{
	SSEParser parser;
	QVERIFY(parser.feed(": heartbeat\n\nretry: 1000\nunknown: ignored\n\n"));
	QVERIFY(!parser.hasFrame());
	QVERIFY(parser.feed("event: state\nid: cursor\ndata:\ndata:  leading space\n\n"));
	QVERIFY(parser.hasFrame());
	const auto frame = parser.takeFrame();
	QCOMPARE(frame.data, QByteArray("\n leading space"));
	QCOMPARE(frame.id, QStringLiteral("cursor"));
}

void SSEParserTest::unfinishedFrameIsNotPublished_data()
{
	QTest::addColumn<QByteArray>("unfinishedData");
	QTest::newRow("complete-code-point") << QByteArray("{\"state\":\"RUNNING\"}");
	QTest::newRow("partial-code-point") << QByteArray::fromHex("e2");
}

void SSEParserTest::unfinishedFrameIsNotPublished()
{
	QFETCH(QByteArray, unfinishedData);
	SSEParser parser;
	QVERIFY(parser.feed("event: state\nid: unfinished\ndata: " + unfinishedData));
	QVERIFY(!parser.hasFrame());
	QVERIFY(parser.finish());
	QVERIFY(!parser.hasError());
	QVERIFY(!parser.hasFrame());
}

void SSEParserTest::invalidUtf8IsRejected_data()
{
	QTest::addColumn<QByteArray>("invalid");
	QTest::newRow("overlong") << QByteArray::fromHex("c0af");
	QTest::newRow("surrogate") << QByteArray::fromHex("eda080");
	QTest::newRow("out-of-range") << QByteArray::fromHex("f4908080");
	QTest::newRow("stray-continuation") << QByteArray::fromHex("80");
	QTest::newRow("truncated") << QByteArray::fromHex("e282");
}

void SSEParserTest::invalidUtf8IsRejected()
{
	QFETCH(QByteArray, invalid);
	SSEParser parser;
	const auto wire = "data: " + invalid + "\n\n";
	parser.feed(wire);
	QVERIFY(parser.hasError());
	QCOMPARE(parser.error().kind, ErrorKind::Decode);
	QVERIFY(!parser.hasFrame());
}

void SSEParserTest::frameAndQueueLimitsAreBounded()
{
	Limits limits;
	limits.frameBytes = 64;
	SSEParser oversized(limits);
	QVERIFY(!oversized.feed("data: " + QByteArray(128, 'a')));
	QVERIFY(oversized.hasError());
	QCOMPARE(oversized.error().kind, ErrorKind::Limit);
	limits.frameBytes = 1024;
	limits.pendingEvents = 2;
	SSEParser queued(limits);
	QVERIFY(!queued.feed("data: a\n\ndata: b\n\ndata: c\n\n"));
	QVERIFY(queued.hasError());
	QCOMPARE(queued.error().kind, ErrorKind::Limit);
	limits.pendingEvents = 100;
	limits.pendingBytes = 32;
	SSEParser bytes(limits);
	QVERIFY(!bytes.feed("data: " + QByteArray(40, 'b') + "\n\n"));
	QCOMPARE(bytes.error().kind, ErrorKind::Limit);
}

void SSEParserTest::drainingQueueReleasesBudget()
{
	Limits limits;
	limits.pendingEvents = 1;
	SSEParser parser(limits);
	for (int index = 0; index < 50; ++index) {
		QVERIFY(parser.feed("id: " + QByteArray::number(index) + "\ndata: x\n\n"));
		QCOMPARE(parser.pendingFrames(), 1);
		QCOMPARE(parser.takeFrame().id, QString::number(index));
		QCOMPARE(parser.pendingFrames(), 0);
		QCOMPARE(parser.pendingBytes(), qsizetype(0));
	}
}

void SSEParserTest::resetDiscardsPartialAndFailedInput()
{
	SSEParser parser;
	QVERIFY(parser.feed("data: old"));
	parser.reset();
	QVERIFY(parser.feed("data: fresh\n\n"));
	QCOMPARE(parser.takeFrame().data, QByteArray("fresh"));
	parser.feed("data: " + QByteArray::fromHex("ff") + "\n\n");
	QVERIFY(parser.hasError());
	parser.reset();
	QVERIFY(!parser.hasError());
	QVERIFY(parser.feed("data: recovered\n\n"));
	QCOMPARE(parser.takeFrame().data, QByteArray("recovered"));
}

QTEST_GUILESS_MAIN(SSEParserTest)
#include "sse-parser-test.moc"
