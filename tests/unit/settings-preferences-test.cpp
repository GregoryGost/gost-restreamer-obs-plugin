#include "core/settings-store.hpp"
#include "fixtures/workspace-temp.hpp"
#include <QFile>
#include <QJsonDocument>
#include <QJsonObject>
#include <QScopeGuard>
#include <QSignalSpy>
#include <QTest>

#include <limits>

using namespace gost;

namespace {
// Возвращает точные байты файла path без обращения к настройкам пользователя.
QByteArray readSettings(const QString &path)
{
	// file читает только указанный сценарным временным каталогом документ.
	QFile file(path);
	return file.open(QIODevice::ReadOnly) ? file.readAll() : QByteArray{};
}
// Записывает bytes в отдельный файл path, проверяя полную запись синтетического документа.
bool writeSettings(const QString &path, const QByteArray &bytes)
{
	// file принадлежит одному сценарию и закрывается до создания SettingsStore.
	QFile file(path);
	return file.open(QIODevice::WriteOnly | QIODevice::Truncate) && file.write(bytes) == bytes.size();
}
} // namespace

// Проверяет пользовательские timeout/logging отдельно от существующих сценариев UUID/привязок.
class SettingsPreferencesTest final : public QObject {
	Q_OBJECT
private slots:
	// Задаёт две поддерживаемые прежние схемы с одной устойчивой привязкой.
	void legacyFormatsKeepBindingsAndUseDefaults_data();
	// Чтение старой схемы не переписывает файл и использует timeout15/обычный журнал.
	void legacyFormatsKeepBindingsAndUseDefaults();
	// Оба предела timeout и подробный журнал переживают новый экземпляр хранилища.
	void preferencesRoundTripAtBothTimeoutBoundaries();
	// Недопустимые значения не заменяют допустимое предпочтение и не переписывают файл.
	void invalidTimeoutDoesNotChangeMemoryOrDisk();
	// Повреждённый формат v3 нельзя перезаписать обычным изменением предпочтений.
	void invalidPreferencesAreNeverOverwritten_data();
	// Проверяет типы, диапазоны и отказ записи неподдерживаемого JSON.
	void invalidPreferencesAreNeverOverwritten();
	// Задаёт допустимые пределы общего сохранения SSE, REST и журнала.
	void advancedPreferencesPublishOneCompleteSnapshot_data();
	// Одна запись публикует сразу все три параметра и сохраняет прежние UUID/привязки.
	void advancedPreferencesPublishOneCompleteSnapshot();
	// Задаёт недопустимые сочетания, которые нельзя частично принять в RAM.
	void invalidAdvancedPreferencesDoNotChangeMemoryOrDisk_data();
	// Неверный параметр отклоняет весь набор без уведомления или записи.
	void invalidAdvancedPreferencesDoNotChangeMemoryOrDisk();
	// Отказ атомарной записи оставляет рабочие preferences в RAM и исходный файл на месте.
	void failedWritePreservesWorkingPreferencesAndOriginalFile();
};

void SettingsPreferencesTest::legacyFormatsKeepBindingsAndUseDefaults_data()
{
	QTest::addColumn<int>("version");
	QTest::newRow("v1") << 1;
	QTest::newRow("v2") << 2;
}
void SettingsPreferencesTest::legacyFormatsKeepBindingsAndUseDefaults()
{
	// version выбирает поддерживаемую прежнюю схему настроек.
	QFETCH(int, version);
	// temp и path изолируют данные сценария; legacy задаёт точный старый документ.
	WorkspaceTemp temp(QStringLiteral("settings-legacy"));
	QVERIFY(temp.isValid());
	const auto path = temp.filePath(QStringLiteral("settings.json"));
	auto legacy = QByteArray(
		R"({"schema_version":1,"server_origin":"http://server","auto_connect":false,"reconnect":true,"profiles":[{"uuid":"644ee3d5-bc59-4345-8a9e-3b3fc5de1f59","name":"A","locator":"profiles/a"}],"bindings":[{"origin":"http://server","profile_uuid":"644ee3d5-bc59-4345-8a9e-3b3fc5de1f59","stream_id":"saved"}]})");
	if (version == 2) {
		legacy.replace("\"schema_version\":1", "\"schema_version\":2");
		legacy.chop(1);
		legacy += ",\"sse_rejection_retries\":7}";
	}
	QVERIFY(writeSettings(path, legacy));
	// settings читает defaults без миграции на диске; отдельная запись выполнится только по намерению.
	SettingsStore settings(path);
	QVERIFY(settings.load());
	QVERIFY(!settings.autoConnect());
	QVERIFY(settings.reconnect());
	QCOMPARE(settings.httpTimeoutSeconds(), 15);
	QVERIFY(!settings.detailedLogging());
	QCOMPARE(settings.sseRejectionRetries(), version == 1 ? 3 : 7);
	QCOMPARE(settings.selectedStream(QStringLiteral("http://server"),
					 QStringLiteral("644ee3d5-bc59-4345-8a9e-3b3fc5de1f59")),
		 QStringLiteral("saved"));
	QCOMPARE(readSettings(path), legacy);
	QVERIFY(settings.setHttpTimeoutSeconds(60));
	settings.setDetailedLogging(true);
	QVERIFY(settings.save());
	// restored доказывает миграцию вместе с UUID, прежним лимитом SSE и сохранённой привязкой.
	SettingsStore restored(path);
	QVERIFY(restored.load());
	QVERIFY(!restored.autoConnect());
	QVERIFY(restored.reconnect());
	QCOMPARE(restored.httpTimeoutSeconds(), 60);
	QVERIFY(restored.detailedLogging());
	QCOMPARE(restored.sseRejectionRetries(), version == 1 ? 3 : 7);
	QCOMPARE(restored.selectedStream(QStringLiteral("http://server"),
					 QStringLiteral("644ee3d5-bc59-4345-8a9e-3b3fc5de1f59")),
		 QStringLiteral("saved"));
	QCOMPARE(QJsonDocument::fromJson(readSettings(path)).object().value(QStringLiteral("schema_version")).toInt(),
		 3);
}
void SettingsPreferencesTest::preferencesRoundTripAtBothTimeoutBoundaries()
{
	// timeout перебирает включённые границы пользовательского диапазона 5–60 секунд.
	for (const int timeout : {5, 60}) {
		// temp/path принадлежат одной итерации, поэтому предыдущие значения не влияют на defaults.
		WorkspaceTemp temp(QStringLiteral("settings-preferences"));
		QVERIFY(temp.isValid());
		const auto path = temp.filePath(QStringLiteral("settings.json"));
		{
			// settings задаёт preferences независимо от состояния UI и backend.
			SettingsStore settings(path);
			QVERIFY(settings.load());
			QCOMPARE(settings.httpTimeoutSeconds(), 15);
			QVERIFY(!settings.detailedLogging());
			QVERIFY(settings.setHttpTimeoutSeconds(timeout));
			settings.setDetailedLogging(true);
			QVERIFY(settings.save());
		}
		// restored читает данные после уничтожения первого экземпляра, исключая проверку одной RAM.
		SettingsStore restored(path);
		QVERIFY(restored.load());
		QCOMPARE(restored.httpTimeoutSeconds(), timeout);
		QVERIFY(restored.detailedLogging());
	}
}
void SettingsPreferencesTest::invalidTimeoutDoesNotChangeMemoryOrDisk()
{
	// temp/path/settings содержат заведомо допустимое значение до негативных попыток.
	WorkspaceTemp temp(QStringLiteral("settings-range"));
	QVERIFY(temp.isValid());
	const auto path = temp.filePath(QStringLiteral("settings.json"));
	SettingsStore settings(path);
	QVERIFY(settings.setHttpTimeoutSeconds(25));
	QVERIFY(settings.save());
	// original фиксирует байты диска до любых запрещённых изменений.
	const auto original = readSettings(path);
	// timeout охватывает обе соседние границы и существенно недопустимые значения.
	for (const int timeout : {-1, 0, 4, 61, 100000}) {
		QVERIFY(!settings.setHttpTimeoutSeconds(timeout));
		QCOMPARE(settings.httpTimeoutSeconds(), 25);
		QCOMPARE(readSettings(path), original);
	}
}
void SettingsPreferencesTest::invalidPreferencesAreNeverOverwritten_data()
{
	QTest::addColumn<QByteArray>("timeout");
	QTest::addColumn<QByteArray>("detailed");
	QTest::newRow("timeout-low") << QByteArray("4") << QByteArray("\"normal\"");
	QTest::newRow("timeout-high") << QByteArray("61") << QByteArray("\"normal\"");
	QTest::newRow("timeout-fraction") << QByteArray("15.5") << QByteArray("\"normal\"");
	QTest::newRow("timeout-string") << QByteArray("\"15\"") << QByteArray("\"normal\"");
	QTest::newRow("logging-boolean") << QByteArray("15") << QByteArray("true");
	QTest::newRow("logging-unknown") << QByteArray("15") << QByteArray("\"verbose\"");
}
void SettingsPreferencesTest::invalidPreferencesAreNeverOverwritten()
{
	// timeout и detailed содержат JSON-представления проверяемого срока и log_level.
	QFETCH(QByteArray, timeout);
	QFETCH(QByteArray, detailed);
	// temp/path изолируют invalid; settings не имеет права заменить непонятный файл при изменении RAM.
	WorkspaceTemp temp(QStringLiteral("settings-invalid"));
	QVERIFY(temp.isValid());
	const auto path = temp.filePath(QStringLiteral("settings.json"));
	const auto invalid =
		QByteArray(
			"{\"schema_version\":3,\"server_origin\":\"\",\"auto_connect\":false,\"reconnect\":true,\"profiles\":[],\"bindings\":[],\"sse_rejection_retries\":3,\"http_timeout_seconds\":") +
		timeout + ",\"log_level\":" + detailed + '}';
	QVERIFY(writeSettings(path, invalid));
	SettingsStore settings(path);
	QVERIFY(!settings.load());
	settings.setDetailedLogging(true);
	settings.setHttpTimeoutSeconds(20);
	QVERIFY(!settings.save());
	QCOMPARE(readSettings(path), invalid);
}
// Проверяет обе границы REST, отсутствие повторов, безлимитный SSE и максимальный конечный лимит.
void SettingsPreferencesTest::advancedPreferencesPublishOneCompleteSnapshot_data()
{
	QTest::addColumn<int>("retries");
	QTest::addColumn<int>("seconds");
	QTest::addColumn<bool>("detailed");
	QTest::newRow("unlimited-short") << -1 << 5 << true;
	QTest::newRow("no-retries-long") << 0 << 60 << false;
	QTest::newRow("largest-finite") << std::numeric_limits<int>::max() << 25 << true;
}

// Подписчик changed должен видеть один полный набор; новый экземпляр проверяет реальную запись и привязки.
void SettingsPreferencesTest::advancedPreferencesPublishOneCompleteSnapshot()
{
	// retries, seconds и detailed задают независимо выбранное допустимое сочетание предпочтений.
	QFETCH(int, retries);
	QFETCH(int, seconds);
	QFETCH(bool, detailed);
	// temp изолирует файл сценария; path не обращается к конфигурации пользователя.
	WorkspaceTemp temp(QStringLiteral("advanced-preferences"));
	QVERIFY(temp.isValid());
	const auto path = temp.filePath(QStringLiteral("settings.json"));
	// settings создаёт реальную идентичность профиля и две независимые серверные привязки.
	SettingsStore settings(path);
	QVERIFY(settings.setServerOrigin(QStringLiteral("http://first")));
	settings.observeProfile(FrontendEvent::ProfileChanged,
				{QStringLiteral("A"), QStringLiteral("profiles/a"), {QStringLiteral("A")}, true});
	// uuid фиксирует идентичность, которая не должна меняться при сохранении предпочтений.
	const auto uuid = settings.profileUuid();
	QVERIFY(!uuid.isEmpty());
	QVERIFY(settings.setSelectedStream(QStringLiteral("http://first"), uuid, QStringLiteral("first-stream")));
	QVERIFY(settings.setSelectedStream(QStringLiteral("http://second"), uuid, QStringLiteral("second-stream")));
	// changed фиксирует число публикаций; snapshots сохраняет значения, доступные синхронному подписчику.
	QSignalSpy changed(&settings, &SettingsStore::changed);
	QVERIFY(changed.isValid());
	QList<QJsonObject> snapshots;
	// Обработчик changed читает все три рабочих поля в момент публикации, выявляя промежуточные комбинации.
	connect(&settings, &SettingsStore::changed, &settings, [&] {
		snapshots.append(QJsonObject{{QStringLiteral("retries"), settings.sseRejectionRetries()},
					     {QStringLiteral("seconds"), settings.httpTimeoutSeconds()},
					     {QStringLiteral("detailed"), settings.detailedLogging()}});
	});
	QVERIFY(settings.saveAdvancedPreferences(retries, seconds, detailed));
	QCOMPARE(changed.count(), 1);
	QCOMPARE(snapshots.size(), 1);
	QCOMPARE(snapshots.front(), (QJsonObject{{QStringLiteral("retries"), retries},
						 {QStringLiteral("seconds"), seconds},
						 {QStringLiteral("detailed"), detailed}}));
	QVERIFY(!settings.unsaved());
	// restored исключает ложный успех, при котором поменялась только память исходного экземпляра.
	SettingsStore restored(path);
	QVERIFY(restored.load());
	QCOMPARE(restored.sseRejectionRetries(), retries);
	QCOMPARE(restored.httpTimeoutSeconds(), seconds);
	QCOMPARE(restored.detailedLogging(), detailed);
	QCOMPARE(restored.serverOrigin(), QStringLiteral("http://first"));
	QVERIFY(restored.profiles().contains(uuid));
	QCOMPARE(restored.profiles().value(uuid).name, QStringLiteral("A"));
	QCOMPARE(restored.profiles().value(uuid).locator, QStringLiteral("profiles/a"));
	QCOMPARE(restored.selectedStream(QStringLiteral("http://first"), uuid), QStringLiteral("first-stream"));
	QCOMPARE(restored.selectedStream(QStringLiteral("http://second"), uuid), QStringLiteral("second-stream"));
}

// Неверное число повторов или срок REST не разрешают применить остальные допустимые поля набора.
void SettingsPreferencesTest::invalidAdvancedPreferencesDoNotChangeMemoryOrDisk_data()
{
	QTest::addColumn<int>("retries");
	QTest::addColumn<int>("seconds");
	QTest::newRow("invalid-retries") << -2 << 60;
	QTest::newRow("timeout-below-minimum") << -1 << 4;
	QTest::newRow("timeout-above-maximum") << 7 << 61;
}

// Отказ проверки набора не изменяет RAM, исходный файл и число публикаций SettingsStore.
void SettingsPreferencesTest::invalidAdvancedPreferencesDoNotChangeMemoryOrDisk()
{
	// retries и seconds содержат один недопустимый параметр вместе с другим допустимым изменением.
	QFETCH(int, retries);
	QFETCH(int, seconds);
	// temp/path/settings создают подтверждённые исходные предпочтения только внутри workspace.
	WorkspaceTemp temp(QStringLiteral("advanced-invalid"));
	QVERIFY(temp.isValid());
	const auto path = temp.filePath(QStringLiteral("settings.json"));
	SettingsStore settings(path);
	QVERIFY(settings.saveAdvancedPreferences(3, 15, false));
	// original и changed обнаруживают запись либо уведомление даже при сохранении прежних значений.
	const auto original = readSettings(path);
	QSignalSpy changed(&settings, &SettingsStore::changed);
	QVERIFY(changed.isValid());
	QVERIFY(!settings.saveAdvancedPreferences(retries, seconds, true));
	QCOMPARE(settings.sseRejectionRetries(), 3);
	QCOMPARE(settings.httpTimeoutSeconds(), 15);
	QVERIFY(!settings.detailedLogging());
	QVERIFY(!settings.unsaved());
	QCOMPARE(readSettings(path), original);
	QCOMPARE(changed.count(), 0);
}

// Отказ commit оставляет весь принятый набор в RAM; повтор без нового ввода сохраняет его после снятия запрета.
void SettingsPreferencesTest::failedWritePreservesWorkingPreferencesAndOriginalFile()
{
	// temp/path/settings создают исходный документ на диске внутри workspace.
	WorkspaceTemp temp(QStringLiteral("settings-write-failure"));
	QVERIFY(temp.isValid());
	const auto path = temp.filePath(QStringLiteral("settings.json"));
	SettingsStore settings(path);
	QVERIFY(settings.save());
	// original сохраняет проверяемую старую версию.
	const auto original = readSettings(path);
	QVERIFY(QFile::setPermissions(path, QFileDevice::ReadOwner));
	// restorePermissions возвращает возможность удалить сценарный файл даже после раннего отказа проверки.
	const auto restorePermissions =
		qScopeGuard([&] { QFile::setPermissions(path, QFileDevice::ReadOwner | QFileDevice::WriteOwner); });
	// changed проверяет одну публикацию полного набора при отказе и одну при повторном commit.
	QSignalSpy changed(&settings, &SettingsStore::changed);
	QVERIFY(changed.isValid());
	// saved и after фиксируют исход и точные байты отказавшей записи до восстановления доступа.
	const bool saved = settings.saveAdvancedPreferences(-1, 60, true);
	const auto after = readSettings(path);
	QFile::setPermissions(path, QFileDevice::ReadOwner | QFileDevice::WriteOwner);
	QVERIFY(!saved);
	QCOMPARE(after, original);
	QCOMPARE(changed.count(), 1);
	QCOMPARE(settings.sseRejectionRetries(), -1);
	QCOMPARE(settings.httpTimeoutSeconds(), 60);
	QVERIFY(settings.detailedLogging());
	QVERIFY(settings.unsaved());
	QCOMPARE(settings.errorKey(), QStringLiteral("Settings.WriteError"));
	QVERIFY(settings.saveAdvancedPreferences(-1, 60, true));
	QCOMPARE(changed.count(), 2);
	QVERIFY(!settings.unsaved());
	// restored читает все три значения после повтора, а не рабочие значения прежнего объекта.
	SettingsStore restored(path);
	QVERIFY(restored.load());
	QCOMPARE(restored.sseRejectionRetries(), -1);
	QCOMPARE(restored.httpTimeoutSeconds(), 60);
	QVERIFY(restored.detailedLogging());
}

QTEST_GUILESS_MAIN(SettingsPreferencesTest)
#include "settings-preferences-test.moc"
