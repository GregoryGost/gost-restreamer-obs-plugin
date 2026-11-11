#include "api/api-client.hpp"
#include "core/settings-store.hpp"

#include <QCoreApplication>
#include <QTest>

using namespace gost::api;

// Ограниченная внешняя проверка трёх GET: live, ready и capabilities через продуктовый клиент.
// Адрес задаётся только GOST_TEST_BACKEND_URL; отсутствие адреса даёт код пропуска CTest 77.
// Успех не подтверждает сборку backend по commit, RTMP/FFmpeg, SSE или готовность реального эфира.
class BackendReadOnlyTest final : public QObject {
	Q_OBJECT
private slots:
	void initTestCase();
	void health_data();
	void health();
	void capabilities();

private:
	QUrl origin_;
};

void BackendReadOnlyTest::initTestCase()
{
	const auto origin = gost::SettingsStore::canonicalOrigin(QString::fromUtf8(qgetenv("GOST_TEST_BACKEND_URL")));
	QVERIFY2(origin, "Некорректный адрес тестового сервера.");
	origin_ = QUrl(*origin);
}

void BackendReadOnlyTest::health_data()
{
	QTest::addColumn<bool>("ready");
	QTest::addColumn<QString>("status");
	QTest::newRow("live") << false << QStringLiteral("live");
	QTest::newRow("ready") << true << QStringLiteral("ready");
}

void BackendReadOnlyTest::health()
{
	QFETCH(bool, ready);
	QFETCH(QString, status);
	Limits limits;
	limits.httpDeadlineMs = 3000;
	limits.httpIdleMs = 3000;
	limits.restBodyBytes = 256 * 1024;
	APIClient api(limits);
	const RequestContext expected{1, 1};
	api.configure(origin_, expected);
	std::optional<Result<Health>> result;
	RequestContext received;
	auto callback = [&](auto context, auto value) {
		received = context;
		result = std::move(value);
	};
	if (ready)
		api.getReady(this, callback);
	else
		api.getLive(this, callback);
	QTRY_VERIFY_WITH_TIMEOUT(result.has_value(), 3500);
	QVERIFY2(bool(*result), "Продуктовый клиент не получил корректный ответ health.");
	QVERIFY(received == expected);
	QCOMPARE(result->value->status, status);
	QVERIFY(result->value->reason.isEmpty());
}

void BackendReadOnlyTest::capabilities()
{
	// Проверяется доступность формы наблюдения, а не обязательная готовность каждого инструмента:
	// unverified/недоступная проверка допустимы, если сервер явно сообщает их состояние.
	Limits limits;
	limits.httpDeadlineMs = 3000;
	limits.httpIdleMs = 3000;
	limits.restBodyBytes = 256 * 1024;
	APIClient api(limits);
	api.configure(origin_, {1, 1});
	std::optional<Result<Capabilities>> result;
	api.getCapabilities(this, [&](auto, auto value) { result = std::move(value); });
	QTRY_VERIFY_WITH_TIMEOUT(result.has_value(), 3500);
	QVERIFY2(bool(*result), "Продуктовый клиент не получил корректные возможности backend.");
	for (const auto *capability :
	     {&result->value->ffmpeg, &result->value->ffprobe, &result->value->helper, &result->value->nvidia}) {
		QVERIFY(!capability->status.isEmpty());
		QVERIFY(capability->checkedAt.hasValue() || capability->checkedAt.isNull());
	}
}

int main(int argc, char **argv)
{
	QCoreApplication application(argc, argv);
	// Код 77 согласован с CTest: отсутствие явно заданного сервера означает пропуск внешней проверки.
	if (qEnvironmentVariableIsEmpty("GOST_TEST_BACKEND_URL")) {
		qInfo("Проверка backend пропущена: GOST_TEST_BACKEND_URL не задан.");
		return 77;
	}
	BackendReadOnlyTest test;
	return QTest::qExec(&test, argc, argv);
}

#include "backend-readonly-test.moc"
