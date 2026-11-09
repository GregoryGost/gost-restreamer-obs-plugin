#include <QCoreApplication>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonParseError>
#include <QNetworkAccessManager>
#include <QNetworkProxy>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QSignalSpy>
#include <QTest>
#include <QTimer>
#include <QUrl>

#include <memory>

class BackendReadOnlyTest final : public QObject {
	Q_OBJECT

private slots:
	void initTestCase();
	void health_data();
	void health();
	void capabilities();

private:
	QJsonObject get(const QString &path);
	QUrl baseUrl_;
};

void BackendReadOnlyTest::initTestCase()
{
	baseUrl_ = QUrl(QString::fromUtf8(qgetenv("GOST_TEST_BACKEND_URL")), QUrl::StrictMode);
	QVERIFY2(baseUrl_.isValid() && !baseUrl_.host().isEmpty(), "Некорректный адрес тестового сервера.");
	QVERIFY2(baseUrl_.scheme() == QStringLiteral("http") || baseUrl_.scheme() == QStringLiteral("https"),
		 "Адрес тестового сервера должен использовать HTTP или HTTPS.");
	QVERIFY2(baseUrl_.userInfo().isEmpty() && !baseUrl_.hasQuery() && !baseUrl_.hasFragment(),
		 "Адрес тестового сервера не должен содержать учётные данные, query или fragment.");
	QVERIFY2(baseUrl_.path().isEmpty() || baseUrl_.path() == QStringLiteral("/"),
		 "Укажите базовый адрес тестового сервера без пути API.");
}

QJsonObject BackendReadOnlyTest::get(const QString &path)
{
	constexpr qsizetype responseLimit = 256 * 1024;
	QNetworkAccessManager manager;
	manager.setProxy(QNetworkProxy::NoProxy);
	QUrl url = baseUrl_;
	url.setPath(path);
	QNetworkRequest request(url);
	request.setAttribute(QNetworkRequest::RedirectPolicyAttribute, QNetworkRequest::ManualRedirectPolicy);
	request.setRawHeader("Accept", "application/json");
	request.setTransferTimeout(3000);
	std::unique_ptr<QNetworkReply> reply(manager.get(request));
	reply->setReadBufferSize(responseLimit + 1);
	QByteArray bytes;
	bool oversized = false;
	const auto consume = [&] {
		bytes += reply->read(responseLimit + 1 - bytes.size());
		if (bytes.size() > responseLimit) {
			oversized = true;
			reply->abort();
		}
	};
	QObject::connect(reply.get(), &QNetworkReply::readyRead, reply.get(), consume);
	QTimer deadline;
	deadline.setSingleShot(true);
	QObject::connect(&deadline, &QTimer::timeout, reply.get(), &QNetworkReply::abort);
	QSignalSpy finished(reply.get(), &QNetworkReply::finished);
	if (!finished.isValid()) {
		QTest::qFail("Не удалось наблюдать завершение HTTP-запроса.", __FILE__, __LINE__);
		return {};
	}
	deadline.start(3000);
	if (!reply->isFinished() && !finished.wait(3500)) {
		QTest::qFail("HTTP-запрос не завершился в пределах бюджета.", __FILE__, __LINE__);
		return {};
	}
	deadline.stop();
	consume();
	if (oversized || reply->error() != QNetworkReply::NoError ||
	    reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt() != 200) {
		QTest::qFail("Сервер не вернул ограниченный успешный ответ HTTP 200.", __FILE__, __LINE__);
		return {};
	}
	QJsonParseError parseError;
	const auto document = QJsonDocument::fromJson(bytes, &parseError);
	if (parseError.error != QJsonParseError::NoError || !document.isObject()) {
		QTest::qFail("Ответ сервера должен быть корректным объектом JSON.", __FILE__, __LINE__);
		return {};
	}
	return document.object();
}

void BackendReadOnlyTest::health_data()
{
	QTest::addColumn<QString>("path");
	QTest::addColumn<QString>("status");
	QTest::newRow("live") << QStringLiteral("/api/v1/health/live") << QStringLiteral("live");
	QTest::newRow("ready") << QStringLiteral("/api/v1/health/ready") << QStringLiteral("ready");
}

void BackendReadOnlyTest::health()
{
	QFETCH(QString, path);
	QFETCH(QString, status);
	const auto body = get(path);
	if (QTest::currentTestFailed())
		return;
	QCOMPARE(body.value(QStringLiteral("status")).toString(), status);
	QVERIFY(body.value(QStringLiteral("reason")).isString());
	QVERIFY(body.value(QStringLiteral("reason")).toString().isEmpty());
}

void BackendReadOnlyTest::capabilities()
{
	const auto body = get(QStringLiteral("/api/v1/system/capabilities"));
	if (QTest::currentTestFailed())
		return;
	for (const auto *name : {"ffmpeg", "ffprobe", "helper", "nvidia"}) {
		const auto recordValue = body.value(QString::fromLatin1(name));
		QVERIFY2(recordValue.isObject(), "Отсутствует объект возможностей backend.");
		const auto record = recordValue.toObject();
		QVERIFY(record.value(QStringLiteral("status")).isString());
		QVERIFY(!record.value(QStringLiteral("status")).toString().isEmpty());
		QVERIFY(record.value(QStringLiteral("reason")).isString());
		const auto checkedAt = record.value(QStringLiteral("checked_at"));
		QVERIFY(checkedAt.isString() || checkedAt.isNull());
	}
}

int main(int argc, char **argv)
{
	QCoreApplication application(argc, argv);
	if (qEnvironmentVariableIsEmpty("GOST_TEST_BACKEND_URL")) {
		qInfo("Проверка backend пропущена: GOST_TEST_BACKEND_URL не задан.");
		return 77;
	}
	BackendReadOnlyTest test;
	return QTest::qExec(&test, argc, argv);
}

#include "backend-readonly-test.moc"
