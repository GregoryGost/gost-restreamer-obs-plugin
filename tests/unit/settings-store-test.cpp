#include "core/settings-store.hpp"
#include "fixtures/workspace-temp.hpp"

#include <QFile>
#include <QJsonArray>
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

// Все файлы создаются через WorkspaceTemp внутри проекта. Тесты читают фактический JSON после
// уничтожения/повторного создания SettingsStore: состояние RAM не выдаётся за успешную персистентность.
class SettingsStoreTest final : public QObject {
	Q_OBJECT
private slots:
	// Задаёт допустимые HTTP(S) origin и ожидаемые канонические значения.
	void canonicalOrigins_data();
	// Проверяет нормализацию схемы, хоста, корневого пути и стандартного порта.
	void canonicalOrigins();
	// Задаёт недопустимые адреса для проверки строгой границы origin.
	void invalidOrigins_data();
	// Проверяет отказ принимать адрес вне поддержанного контракта origin.
	void invalidOrigins();
	// Проверяет запись предпочтений и отдельных привязок без серверных секретов и runtime.
	void persistsOnlyPreferencesAndPairBindings();
	// Задаёт конечный, нулевой и неограниченный бюджеты повторов SSE.
	void migratesRetryPreferenceWithExistingBinding_data();
	// Проверяет сохранение старой привязки при записи нового бюджета повторов.
	void migratesRetryPreferenceWithExistingBinding();
	// Проверяет сохранение UUID и всех привязок при штатном переименовании и перезапуске.
	void normalRenamePreservesEveryServerAcrossRestart();
	// Задаёт формы одного каталога при переименовании и последующем запуске OBS.
	void equivalentRenameLocatorsPreserveEveryServerAcrossRestart_data();
	// Проверяет сохранение UUID и привязок всех серверов при смене записи пути после rename.
	void equivalentRenameLocatorsPreserveEveryServerAcrossRestart();
	// Задаёт сохранённые старым форматом пути и эквивалентные снимки OBS.
	void legacyEquivalentLocatorsPreserveIdentity_data();
	// Проверяет чтение старого пути без миграции и восстановление его UUID по другой записи.
	void legacyEquivalentLocatorsPreserveIdentity();
	// Задаёт изменения формы пути внутри явного переключения профиля.
	void formatOnlyLocatorChangePreservesIdentity_data();
	// Проверяет, что ProfileChanging не создаёт новую идентичность для того же каталога.
	void formatOnlyLocatorChangePreservesIdentity();
	// Задаёт разные каталоги, которые нельзя объединять по сходству строк.
	void distinctLocatorsHaveIndependentIdentity_data();
	// Проверяет отдельные UUID и Stream у разных каталогов при явном переключении.
	void distinctLocatorsHaveIndependentIdentity();
	// Задаёт наблюдения при нескольких старых UUID с эквивалентными путями.
	void ambiguousEquivalentLocatorsPreserveRegistry_data();
	// Проверяет отказ угадывать UUID без потери реестра, привязок или исходного файла.
	void ambiguousEquivalentLocatorsPreserveRegistry();
	// Проверяет независимые UUID переключаемых профилей и созданной копии.
	void switchesAndDuplicatesHaveIndependentIdentity();
	// Проверяет, что повторно созданный каталог не наследует UUID удалённого профиля.
	void removedLocatorDoesNotIdentifyRecreatedProfile();
	// Проверяет отсутствие переноса привязки при недостоверном снимке профиля.
	void uncertainTransitionDoesNotMergeBindings();
	// Задаёт повреждённые и неподдержанные документы настроек.
	void corruptAndUnknownSchemaAreNeverOverwritten_data();
	// Проверяет запрет перезаписи неподдержанного входного документа.
	void corruptAndUnknownSchemaAreNeverOverwritten();
	// Проверяет сохранение рабочей RAM и прежнего файла при отказе атомарной записи.
	void writeFailureKeepsRamAndOriginalFile();
	// Проверяет явный отказ записи при отсутствии разрешённого пути настроек.
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
	// Один UUID имеет разные Stream на двух origin; DNS-имя и IP намеренно не объединяются.
	// Содержимое файла отдельно проверяет границу хранения: секреты/runtime/baseline туда не входят.
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

void SettingsStoreTest::migratesRetryPreferenceWithExistingBinding_data()
{
	QTest::addColumn<int>("retries");
	QTest::newRow("disabled") << 0;
	QTest::newRow("finite") << 7;
	QTest::newRow("unlimited") << -1;
}

void SettingsStoreTest::migratesRetryPreferenceWithExistingBinding()
{
	// Сначала читается точный старый документ без переписывания. Только изменение предпочтения
	// переводит файл в текущий формат v3; повторный load сохраняет новый лимит и прежнюю привязку.
	// retries задаёт явное новое число повторов из таблицы сценариев.
	QFETCH(int, retries);
	// temp содержит все файлы этого сценария в workspace; file адресует единственный JSON настроек.
	WorkspaceTemp temp(QStringLiteral("retry-preference"));
	QVERIFY(temp.isValid());
	const auto file = temp.filePath(QStringLiteral("settings.json"));
	// legacy воспроизводит документ v1 с существующими UUID и привязкой, которые нельзя потерять при миграции.
	const QByteArray legacy = R"({"schema_version":1,"server_origin":"http://server","auto_connect":false,
"reconnect":false,"profiles":[{"uuid":"644ee3d5-bc59-4345-8a9e-3b3fc5de1f59","name":"A","locator":"profiles/a"}],
"bindings":[{"origin":"http://server","profile_uuid":"644ee3d5-bc59-4345-8a9e-3b3fc5de1f59","stream_id":"saved"}]})";
	// origin и uuid задают точную прежнюю пару сервера и профиля для последующей проверки привязки.
	const auto origin = QStringLiteral("http://server");
	const auto uuid = QStringLiteral("644ee3d5-bc59-4345-8a9e-3b3fc5de1f59");
	QVERIFY(write(file, legacy));
	{
		// settings загружает старый файл и сохраняет его лишь после явного изменения числа повторов.
		SettingsStore settings(file);
		QVERIFY(settings.load());
		QCOMPARE(settings.sseRejectionRetries(), 3);
		QCOMPARE(settings.selectedStream(origin, uuid), QStringLiteral("saved"));
		QCOMPARE(contents(file), legacy);
		settings.setSseRejectionRetries(retries);
		QVERIFY(!settings.unsaved());
	}
	// document проверяет фактический формат на диске независимо от RAM первого экземпляра.
	const auto document = QJsonDocument::fromJson(contents(file)).object();
	QCOMPARE(document.value(QStringLiteral("schema_version")).toInt(), 3);
	QVERIFY(document.value(QStringLiteral("sse_rejection_retries")).isDouble());
	QCOMPARE(document.value(QStringLiteral("sse_rejection_retries")).toInt(-2), retries);
	// restored подтверждает чтение актуального документа новым экземпляром хранилища.
	SettingsStore restored(file);
	QVERIFY(restored.load());
	QCOMPARE(restored.sseRejectionRetries(), retries);
	QCOMPARE(restored.serverOrigin(), origin);
	QVERIFY(!restored.autoConnect());
	QVERIFY(!restored.reconnect());
	QCOMPARE(restored.selectedStream(origin, uuid), QStringLiteral("saved"));
}

void SettingsStoreTest::normalRenamePreservesEveryServerAcrossRestart()
{
	// Проверяется смена и имени, и locator без ProfileChanging, затем отдельный ProfileRenamed.
	// Сохранность одного UUID и обеих привязок проверяется также после нового экземпляра хранилища.
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
		// Воспроизводится порядок OBS: ProfileChanged с новым каталогом предшествует ProfileRenamed.
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

// Обе формы пути в каждой строке обозначают один каталог без обращения к файловой системе.
void SettingsStoreTest::equivalentRenameLocatorsPreserveEveryServerAcrossRestart_data()
{
	QTest::addColumn<QString>("renamedPath");
	QTest::addColumn<QString>("restartedPath");
	QTest::newRow("observed-native-restart")
		<< QStringLiteral("../../config/obs-studio/basic/profiles/Isolated_Renamed")
		<< QStringLiteral("../../config\\obs-studio/basic/profiles/Isolated_Renamed");
	QTest::newRow("reverse-native-separators")
		<< QStringLiteral("..\\..\\config\\obs-studio\\basic\\profiles\\Isolated_Renamed")
		<< QStringLiteral("../../config/obs-studio/basic/profiles/Isolated_Renamed");
	QTest::newRow("dot-segments-and-trailing-separator")
		<< QStringLiteral("../../config/obs-studio/basic/profiles/./Isolated_Renamed/")
		<< QStringLiteral("../../config/obs-studio/basic/profiles/unused/../Isolated_Renamed");
}

// OBS сначала сообщает новый каталог через Changed, затем подтверждает Renamed; новый процесс
// может вернуть другой вид разделителей. Выбор проверяется по восстановленному UUID, а не по старому ключу.
void SettingsStoreTest::equivalentRenameLocatorsPreserveEveryServerAcrossRestart()
{
	// renamedPath и restartedPath воспроизводят записи каталога до и после перезапуска.
	QFETCH(QString, renamedPath);
	QFETCH(QString, restartedPath);
	// temp изолирует JSON в workspace, file указывает единственный файл настроек сценария.
	WorkspaceTemp temp(QStringLiteral("rename-path-format"));
	QVERIFY(temp.isValid());
	const auto file = temp.filePath(QStringLiteral("settings.json"));
	// uuid сохраняет исходную идентичность для проверки нового экземпляра хранилища.
	QString uuid;
	{
		// settings обрабатывает события переименования и сохраняет две серверные привязки.
		SettingsStore settings(file);
		QVERIFY(settings.load());
		// initial фиксирует исходный профиль до смены имени и каталога.
		const auto initial = settings.observeProfile(
			FrontendEvent::FinishedLoading,
			profile(QStringLiteral("Isolated"),
				QStringLiteral("../../config/obs-studio/basic/profiles/Isolated"),
				{QStringLiteral("Isolated")}));
		QVERIFY(initial.resolved);
		uuid = initial.uuid;
		QVERIFY(settings.setSelectedStream(QStringLiteral("http://server-a"), uuid,
						   QStringLiteral("stream-a")));
		QVERIFY(settings.setSelectedStream(QStringLiteral("https://server-b"), uuid,
						   QStringLiteral("stream-b")));
		// renamed проверяет перенос прежнего UUID при наблюдаемом порядке событий OBS.
		const auto renamed = settings.observeProfile(FrontendEvent::ProfileChanged,
							     profile(QStringLiteral("Isolated_Renamed"), renamedPath,
								     {QStringLiteral("Isolated_Renamed")}));
		QVERIFY(renamed.resolved);
		QVERIFY(renamed.renamed);
		QCOMPARE(renamed.uuid, uuid);
		// confirmed завершает rename тем же снимком до сохранения и уничтожения settings.
		const auto confirmed = settings.observeProfile(FrontendEvent::ProfileRenamed,
							       profile(QStringLiteral("Isolated_Renamed"), renamedPath,
								       {QStringLiteral("Isolated_Renamed")}));
		QVERIFY(confirmed.resolved);
		QCOMPARE(confirmed.uuid, uuid);
		QVERIFY(settings.save());
	}
	// restored читает только диск, поэтому прежняя RAM не может скрыть потерю идентичности.
	SettingsStore restored(file);
	QVERIFY(restored.load());
	// result воспроизводит снимок каталога при новом запуске OBS.
	const auto result = restored.observeProfile(FrontendEvent::FinishedLoading,
						    profile(QStringLiteral("Isolated_Renamed"), restartedPath,
							    {QStringLiteral("Isolated_Renamed")}));
	QVERIFY(result.resolved);
	QCOMPARE(result.uuid, uuid);
	QCOMPARE(restored.profileUuid(), uuid);
	QCOMPARE(restored.profiles().size(), 1);
	QCOMPARE(restored.selectedStream(QStringLiteral("http://server-a"), result.uuid), QStringLiteral("stream-a"));
	QCOMPARE(restored.selectedStream(QStringLiteral("https://server-b"), result.uuid), QStringLiteral("stream-b"));
}

// Таблица проверяет совместимость с ненормализованными locator старых документов в обоих направлениях.
void SettingsStoreTest::legacyEquivalentLocatorsPreserveIdentity_data()
{
	QTest::addColumn<QString>("storedPath");
	QTest::addColumn<QString>("observedPath");
	QTest::newRow("legacy-backslashes") << QStringLiteral("..\\..\\config\\obs-studio\\basic\\profiles\\A")
					    << QStringLiteral("../../config/obs-studio/basic/profiles/A");
	QTest::newRow("legacy-forward-slashes") << QStringLiteral("../../config/obs-studio/basic/profiles/A")
						<< QStringLiteral("../../config\\obs-studio/basic/profiles/A");
	QTest::newRow("legacy-dot-segments")
		<< QStringLiteral("profiles/unused/.././A/") << QStringLiteral("profiles/A");
	QTest::newRow("observed-dot-segments")
		<< QStringLiteral("profiles/A") << QStringLiteral("profiles/unused/.././A/");
}

// Сам load не переписывает schema v1. После наблюдения другой формы пути прежний UUID и обе
// привязки должны остаться доступными также после сохранения и повторного чтения документа.
void SettingsStoreTest::legacyEquivalentLocatorsPreserveIdentity()
{
	// storedPath задаёт старую запись locator, observedPath — эквивалентный снимок текущего OBS.
	QFETCH(QString, storedPath);
	QFETCH(QString, observedPath);
	// temp и file размещают старый документ и последующую запись только в workspace.
	WorkspaceTemp temp(QStringLiteral("legacy-path-format"));
	QVERIFY(temp.isValid());
	const auto file = temp.filePath(QStringLiteral("settings.json"));
	// uuid заранее задан в старом JSON и не должен заменяться случайно сгенерированным значением.
	const auto uuid = QStringLiteral("644ee3d5-bc59-4345-8a9e-3b3fc5de1f59");
	// legacy содержит поддержанный документ v1 и независимые привязки двух origin к одному UUID.
	const auto legacy =
		QJsonDocument(
			QJsonObject{
				{QStringLiteral("schema_version"), 1},
				{QStringLiteral("server_origin"), QStringLiteral("http://server-a")},
				{QStringLiteral("auto_connect"), true},
				{QStringLiteral("reconnect"), true},
				{QStringLiteral("profiles"),
				 QJsonArray{QJsonObject{{QStringLiteral("uuid"), uuid},
							{QStringLiteral("name"), QStringLiteral("A")},
							{QStringLiteral("locator"), storedPath}}}},
				{QStringLiteral("bindings"),
				 QJsonArray{QJsonObject{{QStringLiteral("origin"), QStringLiteral("http://server-a")},
							{QStringLiteral("profile_uuid"), uuid},
							{QStringLiteral("stream_id"), QStringLiteral("stream-a")}},
					    QJsonObject{{QStringLiteral("origin"), QStringLiteral("https://server-b")},
							{QStringLiteral("profile_uuid"), uuid},
							{QStringLiteral("stream_id"), QStringLiteral("stream-b")}}}}})
			.toJson(QJsonDocument::Compact);
	QVERIFY(write(file, legacy));
	{
		// settings читает старую запись и разрешает её без создания дополнительного профиля.
		SettingsStore settings(file);
		QVERIFY(settings.load());
		QCOMPARE(contents(file), legacy);
		QCOMPARE(settings.profiles().value(uuid).locator, storedPath);
		// result должен вернуть UUID из файла, хотя path побайтно отличается от locator.
		const auto result =
			settings.observeProfile(FrontendEvent::FinishedLoading,
						profile(QStringLiteral("A"), observedPath, {QStringLiteral("A")}));
		QVERIFY(result.resolved);
		QCOMPARE(result.uuid, uuid);
		QCOMPARE(settings.profiles().size(), 1);
		QCOMPARE(settings.selectedStream(QStringLiteral("http://server-a"), result.uuid),
			 QStringLiteral("stream-a"));
		QCOMPARE(settings.selectedStream(QStringLiteral("https://server-b"), result.uuid),
			 QStringLiteral("stream-b"));
		QVERIFY(settings.save());
	}
	// restored проверяет совместимость уже сохранённого файла с прежней формой снимка.
	SettingsStore restored(file);
	QVERIFY(restored.load());
	// result подтверждает, что повторное чтение не размножает UUID при возврате исходной формы пути.
	const auto result = restored.observeProfile(FrontendEvent::FinishedLoading,
						    profile(QStringLiteral("A"), storedPath, {QStringLiteral("A")}));
	QVERIFY(result.resolved);
	QCOMPARE(result.uuid, uuid);
	QCOMPARE(restored.profiles().size(), 1);
	QCOMPARE(restored.selectedStream(QStringLiteral("http://server-a"), result.uuid), QStringLiteral("stream-a"));
	QCOMPARE(restored.selectedStream(QStringLiteral("https://server-b"), result.uuid), QStringLiteral("stream-b"));
}

// В каждой строке меняется только форма locator: имя и каталог профиля остаются прежними.
void SettingsStoreTest::formatOnlyLocatorChangePreservesIdentity_data()
{
	QTest::addColumn<QString>("initialPath");
	QTest::addColumn<QString>("changedPath");
	QTest::newRow("forward-to-native") << QStringLiteral("profiles/A") << QStringLiteral("profiles\\A");
	QTest::newRow("native-to-forward") << QStringLiteral("profiles\\A") << QStringLiteral("profiles/A");
	QTest::newRow("dot-segments") << QStringLiteral("profiles/A") << QStringLiteral("profiles/./unused/../A/");
}

// Явное ProfileChanging запрещает эвристику rename, но не должно мешать распознать известный каталог.
void SettingsStoreTest::formatOnlyLocatorChangePreservesIdentity()
{
	// initialPath и changedPath задают два представления каталога в одной последовательности событий.
	QFETCH(QString, initialPath);
	QFETCH(QString, changedPath);
	// temp изолирует файлы, settings хранит наблюдения и привязку единственного профиля.
	WorkspaceTemp temp(QStringLiteral("changing-path-format"));
	QVERIFY(temp.isValid());
	SettingsStore settings(temp.filePath(QStringLiteral("settings.json")));
	QVERIFY(settings.load());
	// initial содержит подтверждённую идентичность до ProfileChanging.
	const auto initial = settings.observeProfile(FrontendEvent::FinishedLoading,
						     profile(QStringLiteral("A"), initialPath, {QStringLiteral("A")}));
	QVERIFY(initial.resolved);
	QVERIFY(settings.setSelectedStream(QStringLiteral("http://server"), initial.uuid, QStringLiteral("saved")));
	settings.observeProfile(FrontendEvent::ProfileChanging,
				profile(QStringLiteral("A"), initialPath, {QStringLiteral("A")}));
	// result обязан снова подтвердить тот же UUID без признака переименования или смены идентичности.
	const auto result = settings.observeProfile(FrontendEvent::ProfileChanged,
						    profile(QStringLiteral("A"), changedPath, {QStringLiteral("A")}));
	QVERIFY(result.resolved);
	QVERIFY(!result.changed);
	QVERIFY(!result.renamed);
	QCOMPARE(result.uuid, initial.uuid);
	QCOMPARE(settings.profiles().size(), 1);
	QCOMPARE(settings.selectedStream(QStringLiteral("http://server"), result.uuid), QStringLiteral("saved"));
}

// Лексическое сравнение не подменяет известные разные пути догадкой о регистре или рабочем каталоге.
void SettingsStoreTest::distinctLocatorsHaveIndependentIdentity_data()
{
	QTest::addColumn<QString>("firstPath");
	QTest::addColumn<QString>("secondPath");
	QTest::newRow("different-directory") << QStringLiteral("profiles/A") << QStringLiteral("profiles/A-copy");
	QTest::newRow("preserve-case") << QStringLiteral("profiles/A") << QStringLiteral("profiles/a");
	QTest::newRow("relative-is-not-absolute") << QStringLiteral("profiles/A") << QStringLiteral("C:/profiles/A");
}

// При наличии обоих имён в inventory переключение должно выбрать независимый UUID без чужой привязки.
void SettingsStoreTest::distinctLocatorsHaveIndependentIdentity()
{
	// firstPath и secondPath обозначают разные каталоги, а не варианты разделителей одного пути.
	QFETCH(QString, firstPath);
	QFETCH(QString, secondPath);
	// temp и settings ограничивают сохранение двумя синтетическими профилями внутри workspace.
	WorkspaceTemp temp(QStringLiteral("distinct-profile-paths"));
	QVERIFY(temp.isValid());
	SettingsStore settings(temp.filePath(QStringLiteral("settings.json")));
	QVERIFY(settings.load());
	// first и second содержат разные имена при общем достоверном перечне профилей.
	const auto first = profile(QStringLiteral("A"), firstPath, {QStringLiteral("A"), QStringLiteral("B")});
	const auto second = profile(QStringLiteral("B"), secondPath, first.inventory);
	// initial фиксирует UUID и выбранный Stream исходного профиля.
	const auto initial = settings.observeProfile(FrontendEvent::FinishedLoading, first);
	QVERIFY(initial.resolved);
	QVERIFY(settings.setSelectedStream(QStringLiteral("http://server"), initial.uuid, QStringLiteral("first")));
	settings.observeProfile(FrontendEvent::ProfileChanging, first);
	// switched должен получить собственную идентичность без Stream исходного профиля.
	const auto switched = settings.observeProfile(FrontendEvent::ProfileChanged, second);
	QVERIFY(switched.resolved);
	QVERIFY(switched.uuid != initial.uuid);
	QVERIFY(settings.selectedStream(QStringLiteral("http://server"), switched.uuid).isEmpty());
	QVERIFY(settings.setSelectedStream(QStringLiteral("http://server"), switched.uuid, QStringLiteral("second")));
	settings.observeProfile(FrontendEvent::ProfileChanging, second);
	// returned подтверждает, что второй UUID не вытеснил первый из реестра.
	const auto returned = settings.observeProfile(FrontendEvent::ProfileChanged, first);
	QVERIFY(returned.resolved);
	QCOMPARE(returned.uuid, initial.uuid);
	QCOMPARE(settings.profiles().size(), 2);
	QCOMPARE(settings.selectedStream(QStringLiteral("http://server"), returned.uuid), QStringLiteral("first"));
	QCOMPARE(settings.selectedStream(QStringLiteral("http://server"), switched.uuid), QStringLiteral("second"));
}

// Даже точное строковое совпадение одного locator не доказывает UUID при эквивалентном втором пути.
void SettingsStoreTest::ambiguousEquivalentLocatorsPreserveRegistry_data()
{
	QTest::addColumn<QString>("observedPath");
	QTest::addColumn<bool>("knownBefore");
	QTest::newRow("exact-first-locator") << QStringLiteral("profiles/A") << false;
	QTest::newRow("exact-second-locator") << QStringLiteral("profiles\\A") << false;
	QTest::newRow("neither-exact-locator") << QStringLiteral("profiles/./A/") << false;
	QTest::newRow("after-known-profile") << QStringLiteral("profiles/A") << true;
}

// Старый формат допускает разные строки locator с одним каталогом. Отсутствие второго имени в inventory
// не разрешает выбрать первый UUID и удалить второй: неоднозначность сохраняет весь исходный документ.
void SettingsStoreTest::ambiguousEquivalentLocatorsPreserveRegistry()
{
	// observedPath выбирает совпадение с двумя старыми записями; knownBefore включает наблюдение
	// другого профиля до неоднозначного снимка, чтобы проверить сброс уже подтверждённого UUID.
	QFETCH(QString, observedPath);
	QFETCH(bool, knownBefore);
	// temp и file ограничивают неоднозначный документ workspace.
	WorkspaceTemp temp(QStringLiteral("ambiguous-profile-paths"));
	QVERIFY(temp.isValid());
	const auto file = temp.filePath(QStringLiteral("settings.json"));
	// firstUuid и secondUuid имеют независимые привязки, которые нельзя объединять по сходству путей.
	const auto firstUuid = QStringLiteral("644ee3d5-bc59-4345-8a9e-3b3fc5de1f59");
	const auto secondUuid = QStringLiteral("bfad8051-c0c4-488a-90fd-b8a688aef0ee");
	// legacy воспроизводит допустимый v1 с двумя разными строками одного каталога и разными Stream.
	const QByteArray legacy = R"({"schema_version":1,"server_origin":"http://server","auto_connect":true,
"reconnect":true,"profiles":[{"uuid":"644ee3d5-bc59-4345-8a9e-3b3fc5de1f59","name":"A","locator":"profiles/A"},
{"uuid":"bfad8051-c0c4-488a-90fd-b8a688aef0ee","name":"Old","locator":"profiles\\A"}],
"bindings":[{"origin":"http://server","profile_uuid":"644ee3d5-bc59-4345-8a9e-3b3fc5de1f59","stream_id":"first"},
{"origin":"http://server","profile_uuid":"bfad8051-c0c4-488a-90fd-b8a688aef0ee","stream_id":"second"}]})";
	QVERIFY(write(file, legacy));
	// settings принимает старый документ без автоматического исправления его неоднозначности.
	SettingsStore settings(file);
	QVERIFY(settings.load());
	QCOMPARE(contents(file), legacy);
	// knownUuid хранит идентичность другого профиля, если строка таблицы требует предшествующий выбор.
	QString knownUuid;
	if (knownBefore) {
		// known подтверждает профиль C, пока inventory достоверно содержит обе старые записи.
		const auto known = settings.observeProfile(
			FrontendEvent::FinishedLoading,
			profile(QStringLiteral("C"), QStringLiteral("profiles/C"),
				{QStringLiteral("A"), QStringLiteral("Old"), QStringLiteral("C")}));
		QVERIFY(known.resolved);
		knownUuid = known.uuid;
		QVERIFY(settings.setSelectedStream(QStringLiteral("http://server"), knownUuid,
						   QStringLiteral("known")));
		QCOMPARE(settings.profileUuid(), knownUuid);
	}
	// before содержит последний успешно сохранённый документ перед неоднозначным наблюдением.
	const auto before = contents(file);
	// result не должен выбирать ни один UUID, даже если строка одного из путей совпала точно.
	const auto result = settings.observeProfile(FrontendEvent::FinishedLoading,
						    profile(QStringLiteral("A"), observedPath, {QStringLiteral("A")}));
	QVERIFY(!result.resolved);
	QVERIFY(settings.profileUuid().isEmpty());
	QCOMPARE(settings.profiles().size(), knownBefore ? 3 : 2);
	QCOMPARE(settings.profiles().value(firstUuid).locator, QStringLiteral("profiles/A"));
	QCOMPARE(settings.profiles().value(secondUuid).locator, QStringLiteral("profiles\\A"));
	QCOMPARE(settings.selectedStream(QStringLiteral("http://server"), firstUuid), QStringLiteral("first"));
	QCOMPARE(settings.selectedStream(QStringLiteral("http://server"), secondUuid), QStringLiteral("second"));
	if (knownBefore)
		QCOMPARE(settings.selectedStream(QStringLiteral("http://server"), knownUuid), QStringLiteral("known"));
	QCOMPARE(contents(file), before);
}

void SettingsStoreTest::switchesAndDuplicatesHaveIndependentIdentity()
{
	// Явное переключение с ProfileChanging и появление копии при ещё существующем оригинале нельзя
	// принять за переименование. Возврат к известному locator обязан восстановить его собственную привязку.
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
	// Реестр сначала получает достоверное исчезновение A, пока активен B. Позже то же имя/путь A
	// появляются заново: прежний UUID уже удалён и не должен вернуть старый Stream новому профилю.
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
	// Ненадёжный descriptor моделирует потерю снимка в момент перехода. UUID нового профиля
	// не определяется, а старая привязка сохраняется у прежнего UUID без переноса по похожим атрибутам.
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
	const QByteArray retryDocument = R"({"schema_version":2,"server_origin":"","auto_connect":false,
"reconnect":true,"profiles":[],"bindings":[],"sse_rejection_retries":)";
	QTest::newRow("out-of-range-retries") << retryDocument + "-2}";
	QTest::newRow("fractional-retries") << retryDocument + "1.5}";
	QTest::newRow("string-retries") << retryDocument + "\"3\"}";
}

void SettingsStoreTest::corruptAndUnknownSchemaAreNeverOverwritten()
{
	// После отказа load вызываются обычный метод изменения и явный save. Побайтовое сравнение файла
	// доказывает блокировку последующих записей, включая неизвестную схему с пользовательскими данными.
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
	// Запрет записи относится к файлу внутри проекта; проверяются сохранность файла и новые значения в памяти.
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
	// Пустой путь не должен незаметно увести тест или продукт в AppData/рабочий каталог по умолчанию.
	// Ошибка остаётся видимой, рабочее изменение RAM отмечено unsaved.
	SettingsStore settings(QString{});
	settings.setAutoConnect(false);
	QVERIFY(!settings.save());
	QVERIFY(settings.unsaved());
	QVERIFY(!settings.error().isEmpty());
}

QTEST_GUILESS_MAIN(SettingsStoreTest)
#include "settings-store-test.moc"
