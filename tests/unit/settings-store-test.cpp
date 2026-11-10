#include "core/settings-store.hpp"
#include "fixtures/workspace-temp.hpp"

#include <QFile>
#include <QJsonDocument>
#include <QJsonObject>
#include <QTest>

using namespace gost;

namespace {
ProfileDescriptor profile(QString name, QString path, QStringList inventory)
{
	return {std::move(name), std::move(path), std::move(inventory), true};
}

QByteArray contents(const QString &path)
{
	QFile file(path);
	return file.open(QIODevice::ReadOnly) ? file.readAll() : QByteArray{};
}

bool write(const QString &path, const QByteArray &bytes)
{
	QFile file(path);
	return file.open(QIODevice::WriteOnly | QIODevice::Truncate) && file.write(bytes) == bytes.size();
}
} // namespace

class SettingsStoreTest final : public QObject {
	Q_OBJECT
private slots:
	void canonicalOrigins_data();
	void canonicalOrigins();
	void invalidOrigins_data();
	void invalidOrigins();
	void persistsOnlyPreferencesAndPairBindings();
	void normalRenamePreservesEveryServerAcrossRestart();
	void switchesAndDuplicatesHaveIndependentIdentity();
	void removedLocatorDoesNotIdentifyRecreatedProfile();
	void uncertainTransitionDoesNotMergeBindings();
	void corruptAndUnknownSchemaAreNeverOverwritten_data();
	void corruptAndUnknownSchemaAreNeverOverwritten();
	void writeFailureKeepsRamAndOriginalFile();
	void emptyPathHasNoImplicitFallback();
};

void SettingsStoreTest::canonicalOrigins_data()
{
	QTest::addColumn<QString>("input");
	QTest::addColumn<QString>("expected");
	QTest::newRow("http") << QStringLiteral("HTTP://Example.COM:80/") << QStringLiteral("http://example.com");
	QTest::newRow("https") << QStringLiteral("https://Example.COM:443") << QStringLiteral("https://example.com");
	QTest::newRow("port") << QStringLiteral("http://127.0.0.1:8080/") << QStringLiteral("http://127.0.0.1:8080");
	QTest::newRow("ipv6") << QStringLiteral("http://[::1]:8080/") << QStringLiteral("http://[::1]:8080");
}

void SettingsStoreTest::canonicalOrigins()
{
	QFETCH(QString, input);
	QFETCH(QString, expected);
	const auto result = SettingsStore::canonicalOrigin(input);
	QVERIFY(result);
	QCOMPARE(*result, expected);
}

void SettingsStoreTest::invalidOrigins_data()
{
	QTest::addColumn<QString>("input");
	for (const auto &value : {"", "localhost:8080", "ftp://localhost", "http://", "http://user:pass@host",
				  "http://host/path", "http://host/?q=1", "http://host/#fragment", "http://host:70000"})
		QTest::newRow(value) << QString::fromLatin1(value);
}

void SettingsStoreTest::invalidOrigins()
{
	QFETCH(QString, input);
	QVERIFY(!SettingsStore::canonicalOrigin(input));
}

void SettingsStoreTest::persistsOnlyPreferencesAndPairBindings()
{
	WorkspaceTemp temp(QStringLiteral("settings"));
	QVERIFY(temp.isValid());
	const auto file = temp.filePath(QStringLiteral("settings.json"));
	QString firstUuid;
	{
		SettingsStore settings(file);
		QVERIFY(settings.load());
		const auto first = settings.observeProfile(FrontendEvent::FinishedLoading,
							   profile(QStringLiteral("A"), QStringLiteral("profiles/a"),
								   {QStringLiteral("A")}));
		QVERIFY(first.resolved);
		firstUuid = first.uuid;
		QVERIFY(!firstUuid.isEmpty());
		QVERIFY(settings.setServerOrigin(QStringLiteral("http://Example.COM:80/")));
		settings.setAutoConnect(false);
		settings.setReconnect(false);
		QVERIFY(settings.setSelectedStream(QStringLiteral("http://example.com"), first.uuid,
						   QStringLiteral("stream-a")));
		QVERIFY(settings.setSelectedStream(QStringLiteral("http://127.0.0.1"), first.uuid,
						   QStringLiteral("stream-b")));
		QVERIFY(settings.save());
		QVERIFY(!settings.unsaved());
	}
	SettingsStore restored(file);
	QVERIFY(restored.load());
	QCOMPARE(restored.serverOrigin(), QStringLiteral("http://example.com"));
	QVERIFY(!restored.autoConnect());
	QVERIFY(!restored.reconnect());
	QCOMPARE(restored.selectedStream(QStringLiteral("http://example.com"), firstUuid), QStringLiteral("stream-a"));
	QCOMPARE(restored.selectedStream(QStringLiteral("http://127.0.0.1"), firstUuid), QStringLiteral("stream-b"));
	QVERIFY(restored.selectedStream(QStringLiteral("http://localhost"), firstUuid).isEmpty());
	const auto document = contents(file);
	QVERIFY(QJsonDocument::fromJson(document).isObject());
	for (const auto *forbidden : {"publication_key", "stream_key", "endpoint", "cursor", "baseline", "metrics"})
		QVERIFY2(!document.contains(QByteArray("\"") + forbidden + "\""), forbidden);
}

void SettingsStoreTest::normalRenamePreservesEveryServerAcrossRestart()
{
	WorkspaceTemp temp(QStringLiteral("rename"));
	QVERIFY(temp.isValid());
	const auto file = temp.filePath(QStringLiteral("settings.json"));
	QString uuid;
	{
		SettingsStore settings(file);
		QVERIFY(settings.load());
		const auto initial = settings.observeProfile(
			FrontendEvent::FinishedLoading,
			profile(QStringLiteral("Old"), QStringLiteral("profiles/old"), {QStringLiteral("Old")}));
		QVERIFY(initial.resolved);
		uuid = initial.uuid;
		QVERIFY(settings.setSelectedStream(QStringLiteral("http://server-a"), uuid, QStringLiteral("a")));
		QVERIFY(settings.setSelectedStream(QStringLiteral("https://server-b"), uuid, QStringLiteral("b")));
		const auto renamed = settings.observeProfile(
			FrontendEvent::ProfileChanged,
			profile(QStringLiteral("New"), QStringLiteral("profiles/new"), {QStringLiteral("New")}));
		QVERIFY(renamed.resolved);
		QVERIFY(renamed.renamed);
		QCOMPARE(renamed.uuid, uuid);
		const auto confirmed = settings.observeProfile(
			FrontendEvent::ProfileRenamed,
			profile(QStringLiteral("New"), QStringLiteral("profiles/new"), {QStringLiteral("New")}));
		QVERIFY(confirmed.resolved);
		QCOMPARE(confirmed.uuid, uuid);
		QCOMPARE(settings.selectedStream(QStringLiteral("http://server-a"), uuid), QStringLiteral("a"));
		QCOMPARE(settings.selectedStream(QStringLiteral("https://server-b"), uuid), QStringLiteral("b"));
		QVERIFY(settings.save());
	}
	SettingsStore restored(file);
	QVERIFY(restored.load());
	const auto result = restored.observeProfile(FrontendEvent::FinishedLoading,
						    profile(QStringLiteral("New"), QStringLiteral("profiles/new"),
							    {QStringLiteral("New")}));
	QVERIFY(result.resolved);
	QCOMPARE(result.uuid, uuid);
	QCOMPARE(restored.selectedStream(QStringLiteral("http://server-a"), uuid), QStringLiteral("a"));
	QCOMPARE(restored.selectedStream(QStringLiteral("https://server-b"), uuid), QStringLiteral("b"));
}

void SettingsStoreTest::switchesAndDuplicatesHaveIndependentIdentity()
{
	WorkspaceTemp temp(QStringLiteral("profiles"));
	QVERIFY(temp.isValid());
	SettingsStore settings(temp.filePath(QStringLiteral("settings.json")));
	QVERIFY(settings.load());
	const auto a =
		profile(QStringLiteral("A"), QStringLiteral("profiles/a"), {QStringLiteral("A"), QStringLiteral("B")});
	const auto b = profile(QStringLiteral("B"), QStringLiteral("profiles/b"), a.inventory);
	const auto first = settings.observeProfile(FrontendEvent::FinishedLoading, a);
	QVERIFY(first.resolved);
	QVERIFY(settings.setSelectedStream(QStringLiteral("http://server"), first.uuid, QStringLiteral("same-id")));
	settings.observeProfile(FrontendEvent::ProfileChanging, a);
	const auto second = settings.observeProfile(FrontendEvent::ProfileChanged, b);
	QVERIFY(second.resolved);
	QVERIFY(second.uuid != first.uuid);
	QVERIFY(settings.selectedStream(QStringLiteral("http://server"), second.uuid).isEmpty());
	QVERIFY(settings.setSelectedStream(QStringLiteral("http://server"), second.uuid, QStringLiteral("other-id")));
	settings.observeProfile(FrontendEvent::ProfileChanging, b);
	const auto back = settings.observeProfile(FrontendEvent::ProfileChanged, a);
	QCOMPARE(back.uuid, first.uuid);
	QCOMPARE(settings.selectedStream(QStringLiteral("http://server"), back.uuid), QStringLiteral("same-id"));
	const auto duplicate =
		settings.observeProfile(FrontendEvent::ProfileChanged,
					profile(QStringLiteral("A copy"), QStringLiteral("profiles/copy"),
						{QStringLiteral("A"), QStringLiteral("B"), QStringLiteral("A copy")}));
	QVERIFY(duplicate.resolved);
	QVERIFY(duplicate.uuid != first.uuid);
	QVERIFY(duplicate.uuid != second.uuid);
	QVERIFY(settings.selectedStream(QStringLiteral("http://server"), duplicate.uuid).isEmpty());
}

void SettingsStoreTest::removedLocatorDoesNotIdentifyRecreatedProfile()
{
	WorkspaceTemp temp(QStringLiteral("recreated"));
	QVERIFY(temp.isValid());
	SettingsStore settings(temp.filePath(QStringLiteral("settings.json")));
	QVERIFY(settings.load());
	const auto a =
		profile(QStringLiteral("A"), QStringLiteral("profiles/a"), {QStringLiteral("A"), QStringLiteral("B")});
	const auto b = profile(QStringLiteral("B"), QStringLiteral("profiles/b"), a.inventory);
	const auto old = settings.observeProfile(FrontendEvent::FinishedLoading, a);
	QVERIFY(settings.setSelectedStream(QStringLiteral("http://server"), old.uuid, QStringLiteral("old-stream")));
	settings.observeProfile(FrontendEvent::ProfileChanging, a);
	settings.observeProfile(FrontendEvent::ProfileChanged, b);
	settings.observeProfile(FrontendEvent::ProfileListChanged, profile(b.name, b.path, {QStringLiteral("B")}));
	settings.observeProfile(FrontendEvent::ProfileChanging, b);
	const auto recreated = settings.observeProfile(FrontendEvent::ProfileChanged, a);
	QVERIFY(recreated.resolved);
	QVERIFY(recreated.uuid != old.uuid);
	QVERIFY(settings.selectedStream(QStringLiteral("http://server"), recreated.uuid).isEmpty());
}

void SettingsStoreTest::uncertainTransitionDoesNotMergeBindings()
{
	WorkspaceTemp temp(QStringLiteral("uncertain"));
	QVERIFY(temp.isValid());
	SettingsStore settings(temp.filePath(QStringLiteral("settings.json")));
	QVERIFY(settings.load());
	const auto initial = settings.observeProfile(FrontendEvent::FinishedLoading,
						     profile(QStringLiteral("A"), QStringLiteral("profiles/a"),
							     {QStringLiteral("A")}));
	QVERIFY(settings.setSelectedStream(QStringLiteral("http://server"), initial.uuid, QStringLiteral("stream-a")));
	auto invalid = profile(QStringLiteral("B"), QStringLiteral("profiles/b"), {QStringLiteral("B")});
	invalid.valid = false;
	QVERIFY(!settings.observeProfile(FrontendEvent::ProfileChanged, invalid).resolved);
	QCOMPARE(settings.selectedStream(QStringLiteral("http://server"), initial.uuid), QStringLiteral("stream-a"));
}

void SettingsStoreTest::corruptAndUnknownSchemaAreNeverOverwritten_data()
{
	QTest::addColumn<QByteArray>("bytes");
	QTest::newRow("corrupt") << QByteArray("{broken");
	QTest::newRow("unknown") << QByteArray("{\"schema_version\":999,\"owner_data\":\"preserve\"}");
}

void SettingsStoreTest::corruptAndUnknownSchemaAreNeverOverwritten()
{
	QFETCH(QByteArray, bytes);
	WorkspaceTemp temp(QStringLiteral("corrupt"));
	QVERIFY(temp.isValid());
	const auto file = temp.filePath(QStringLiteral("settings.json"));
	QVERIFY(write(file, bytes));
	SettingsStore settings(file);
	QVERIFY(!settings.load());
	QVERIFY(!settings.error().isEmpty());
	settings.setAutoConnect(false);
	QVERIFY(!settings.save());
	QCOMPARE(contents(file), bytes);
}

void SettingsStoreTest::writeFailureKeepsRamAndOriginalFile()
{
	WorkspaceTemp temp(QStringLiteral("atomic"));
	QVERIFY(temp.isValid());
	const auto file = temp.filePath(QStringLiteral("settings.json"));
	SettingsStore settings(file);
	QVERIFY(settings.load());
	QVERIFY(settings.setServerOrigin(QStringLiteral("http://server-a")));
	QVERIFY(settings.save());
	const auto original = contents(file);
	QVERIFY(QFile::setPermissions(file, QFileDevice::ReadOwner));
	settings.setServerOrigin(QStringLiteral("http://server-b"));
	const bool saved = settings.save();
	const auto after = contents(file);
	QFile::setPermissions(file, QFileDevice::ReadOwner | QFileDevice::WriteOwner);
	QVERIFY(!saved);
	QCOMPARE(after, original);
	QCOMPARE(settings.serverOrigin(), QStringLiteral("http://server-b"));
	QVERIFY(settings.unsaved());
	QVERIFY(!settings.error().isEmpty());
	QVERIFY(settings.save());
	QVERIFY(!settings.unsaved());
}

void SettingsStoreTest::emptyPathHasNoImplicitFallback()
{
	SettingsStore settings(QString{});
	settings.setAutoConnect(false);
	QVERIFY(!settings.save());
	QVERIFY(settings.unsaved());
	QVERIFY(!settings.error().isEmpty());
}

QTEST_GUILESS_MAIN(SettingsStoreTest)
#include "settings-store-test.moc"
