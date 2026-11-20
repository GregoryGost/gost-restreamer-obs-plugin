#include "fixtures/fallback-rig.hpp"
#include "ui/fallback-editor.hpp"
#include "ui/fallback-policy-editor.hpp"
#include "ui/management-window.hpp"
#include "ui/profile-editor.hpp"
#include <QApplication>
#include <QComboBox>
#include <QFile>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QProgressBar>
#include <QPushButton>
#include <QSettings>
#include <QSignalSpy>
#include <QStackedWidget>
#include <QTableWidget>
#include <QTest>
#include <QToolButton>

using namespace gost;
using namespace gost::api;

// Проверяет настоящие Widgets через публичное состояние, локальные переводы и изолированный HTTP.
// Offscreen подтверждает модель/сигналы; нативные DPI, тема OBS и загрузка DLL проверяются отдельно.
class FallbackUiTest final : public QObject {
	Q_OBJECT
private slots:
	// Задаёт обе поставляемые локали страниц подготовки.
	void preparationPageHasNoRuntimeControls_data();
	// Все разделы единого окна содержат только настройку/подготовку и сохраняют явные доступные подписи.
	void preparationPageHasNoRuntimeControls();
	// Задаёт обе локали отображения независимых стадий upload/подготовки/current.
	void stagesAndDigitalSilenceRemainDistinct_data();
	//100% передачи не становится готовностью; current/pending/accepted и секретная причина не смешиваются.
	void stagesAndDigitalSilenceRemainDistinct();
	// Невидимая копия policy не перенаправляет общий контроллер и рабочий выбор Dock.
	void onlyActivePolicyEditorControlsTarget();
	// Обновление сразу отзывает допуск подготовки, пока адресный GET не подтвердит свежую карточку.
	void refreshRevokesPreparationUntilAssetReply();
	// Закрытие и переходы сохраняют RAM-файл/ввод и не прерывают принятую подготовку.
	void navigationAndClosePreserveWorkflow();
	// Задаёт локали полного структурного просмотра рецепта опубликованного варианта.
	void variantRecipeCanBeExpandedWithoutEditing_data();
	// Раскрытие доступно при запрете редактирования, значения audio/video/encoding не теряются при обновлении.
	void variantRecipeCanBeExpandedWithoutEditing();
};

void FallbackUiTest::preparationPageHasNoRuntimeControls_data()
{
	QTest::addColumn<QString>("locale");
	QTest::newRow("ru") << QStringLiteral("ru-RU");
	QTest::newRow("en") << QStringLiteral("en-US");
}

void FallbackUiTest::preparationPageHasNoRuntimeControls()
{
	// locale выбирает реальные файлы; test замыкает все контроллеры на loopback.
	QFETCH(QString, locale);
	FallbackRig test;
	QVERIFY(test.initialize());
	// translations/text читают только поставляемый INI и не используют пользовательские настройки.
	QSettings translations(QStringLiteral(GOST_TEST_LOCALE_DIR) + QLatin1Char('/') + locale +
				       QStringLiteral(".ini"),
			       QSettings::IniFormat);
	const auto text = [&translations](const char *key) {
		return translations.value(QString::fromUtf8(key), QString::fromUtf8(key)).toString();
	};
	// window имеет все прежние страницы и новый редактор в том же экземпляре.
	ManagementWindow window(test.rig.state, text, nullptr, &test.rig.connection, &test.rig.operations,
				&test.rig.configuration, &test.fallback);
	// pages/navigation наблюдают реальное переключение; editor адресует страницу Fallback.
	auto *pages = window.findChild<QStackedWidget *>(QStringLiteral("managementPages"));
	auto *navigation = window.findChild<QListWidget *>(QStringLiteral("managementNavigation"));
	auto *editor = window.findChild<FallbackEditor *>();
	QVERIFY(pages && navigation && editor);
	window.show();
	QTRY_VERIFY(test.rig.state.configurationEditor().draft.has_value() &&
		    !test.rig.state.configurationEditor().loading);
	// row проходит все существующие страницы, включая настройки и подготовку.
	for (int row = 0; row < navigation->count(); ++row) {
		navigation->setCurrentRow(row);
		QTRY_COMPARE(pages->currentIndex(), row);
		QTRY_VERIFY(!test.rig.state.configurationEditor().loading);
		QVERIFY(!window.findChild<QPushButton *>(QStringLiteral("fallbackStartButton")));
		QVERIFY(!window.findChild<QPushButton *>(QStringLiteral("outputStartButton")));
		QVERIFY(!window.findChild<QPushButton *>(QStringLiteral("outputStopButton")));
		QVERIFY(!window.findChild<QPushButton *>(QStringLiteral("outputRestartButton")));
		QVERIFY(!window.findChild<QPushButton *>(QStringLiteral("outputDetachButton")));
	}
	navigation->setCurrentRow(4);
	QTRY_VERIFY(editor->isVisible());
	// name/path/kind проверяют доступные подписи конечного ввода, а не только наличие QWidget.
	auto *name = editor->findChild<QLineEdit *>(QStringLiteral("fallbackCreateName"));
	auto *path = editor->findChild<QLineEdit *>(QStringLiteral("fallbackFilePath"));
	auto *kind = editor->findChild<QComboBox *>(QStringLiteral("fallbackFileKind"));
	QVERIFY(name && path && kind);
	QCOMPARE(name->accessibleName(), text("Fallback.NewName"));
	QCOMPARE(path->accessibleName(), text("Fallback.SourceFile"));
	QVERIFY(path->isReadOnly());
	QCOMPARE(kind->count(), 2);
	QCOMPARE(kind->itemData(0).toString(), QStringLiteral("image"));
	QCOMPARE(kind->itemData(1).toString(), QStringLiteral("video"));
	// key задаёт материализованную подпись новой функциональности в обеих локалях.
	for (const auto *key : {"Fallback.PreparationOrder", "Fallback.Silence", "Fallback.GeneratorWarning",
				"Fallback.PreparationRestriction", "Fallback.Upload", "Fallback.Rebuild"}) {
		QVERIFY2(translations.contains(QString::fromUtf8(key)), key);
		QVERIFY(text(key) != QString::fromUtf8(key));
	}
	QVERIFY(test.rig.mutations().isEmpty());
	QCOMPARE(test.rig.frontend.stopRequests, 0);
}

void FallbackUiTest::stagesAndDigitalSilenceRemainDistinct_data()
{
	QTest::addColumn<QString>("locale");
	QTest::newRow("ru") << QStringLiteral("ru-RU");
	QTest::newRow("en") << QStringLiteral("en-US");
}

void FallbackUiTest::stagesAndDigitalSilenceRemainDistinct()
{
	// locale/translations/text обеспечивают реальные подписи для трёх независимых стадий.
	QFETCH(QString, locale);
	QSettings translations(QStringLiteral(GOST_TEST_LOCALE_DIR) + QLatin1Char('/') + locale +
				       QStringLiteral(".ini"),
			       QSettings::IniFormat);
	const auto text = [&translations](const char *key) {
		return translations.value(QString::fromUtf8(key), QString::fromUtf8(key)).toString();
	};
	// test предоставляет контроллер пассивной проекции без настоящего OBS.
	FallbackRig test;
	QVERIFY(test.initialize());
	QTRY_VERIFY(test.rig.state.fallback().assetsStatus.complete);
	// editor пассивен: проекция StateStore не должна обращаться к сети или менять политику.
	FallbackEditor editor(test.rig.state, test.fallback, text);
	// state содержит полностью переданные байты при ещё не принятой записи.
	auto state = test.rig.state.fallback();
	state.bytesSent = 12345;
	state.totalBytes = 12345;
	state.outcome = FallbackWriteOutcome::Pending;
	state.current = {QStringLiteral("asset"),
			 QStringLiteral("published"),
			 std::nullopt,
			 {},
			 {RuntimeAvailability::Fresh, true, {}}};
	state.pending = {QStringLiteral("asset"), QStringLiteral("pending"), std::nullopt, {}, {}};
	state.accepted = {QStringLiteral("asset"), QStringLiteral("owned"), std::nullopt, {}, {}};
	state.acceptedAssetId = QStringLiteral("asset");
	state.acceptedGenerationId = QStringLiteral("owned");
	// current/pending/accepted — независимые DTO, позволяющие обнаружить подмену подписи.
	const auto current = decode<Generation>(
		fixture::bytes(fallbackFixture::generation(QStringLiteral("published"), QStringLiteral("ready"))));
	const auto pending = decode<Generation>(
		fixture::bytes(fallbackFixture::generation(QStringLiteral("pending"), QStringLiteral("preparing"))));
	const auto accepted = decode<Generation>(
		fixture::bytes(fallbackFixture::generation(QStringLiteral("owned"), QStringLiteral("queued"))));
	QVERIFY(current && pending && accepted);
	state.current.generation = *current.value;
	state.pending.generation = *pending.value;
	state.accepted.generation = *accepted.value;
	test.rig.state.setFallback(state);
	editor.show();
	// progress/result описывают только передачу, currentLabel/pendingLabel/acceptedLabel адресуют поколения.
	auto *progress = editor.findChild<QProgressBar *>(QStringLiteral("fallbackUploadProgress"));
	auto *result = editor.findChild<QLabel *>(QStringLiteral("fallbackResult"));
	auto *currentLabel = editor.findChild<QLabel *>(QStringLiteral("fallbackCurrent"));
	auto *pendingLabel = editor.findChild<QLabel *>(QStringLiteral("fallbackPending"));
	auto *acceptedLabel = editor.findChild<QLabel *>(QStringLiteral("fallbackAccepted"));
	QVERIFY(progress && result && currentLabel && pendingLabel && acceptedLabel);
	QCOMPARE(progress->value(), progress->maximum());
	QVERIFY(result->text().contains(text("Fallback.Sending")));
	QVERIFY(!result->text().contains(text("Fallback.Prepared")));
	QVERIFY(currentLabel->text().contains(QStringLiteral("published")));
	QVERIFY(!currentLabel->text().contains(QStringLiteral("pending")));
	QVERIFY(pendingLabel->text().contains(QStringLiteral("pending")));
	QVERIFY(acceptedLabel->text().contains(QStringLiteral("owned")));
	QCOMPARE(currentLabel->textFormat(), Qt::PlainText);
	QCOMPARE(pendingLabel->textFormat(), Qt::PlainText);
	QCOMPARE(acceptedLabel->textFormat(), Qt::PlainText);
	// silenceShown подтверждает видимую памятку цифровой тишины; label перебирает реальные подписи страницы.
	bool silenceShown = false;
	for (const auto *label : editor.findChildren<QLabel *>())
		if (label->text().contains(text("Fallback.Silence")))
			silenceShown = true;
	QVERIFY(silenceShown);
	state.accepted.generation->status = QStringLiteral("error");
	state.accepted.generation->failure = {FieldState::Value,
					      {QStringLiteral("failed"),
					       QStringLiteral("<b>server-private-reason</b>"),
					       QStringLiteral("server-private-action"),
					       {},
					       {},
					       {}}};
	test.rig.state.setFallback(state);
	// label подтверждает отсутствие повторной server reason/action в странице вместо общего popup.
	for (const auto *label : editor.findChildren<QLabel *>()) {
		QVERIFY(!label->text().contains(QStringLiteral("server-private-reason")));
		QVERIFY(!label->text().contains(QStringLiteral("server-private-action")));
	}
	QVERIFY(test.rig.mutations().isEmpty());
}

void FallbackUiTest::onlyActivePolicyEditorControlsTarget()
{
	// test содержит рабочий Stream; first/second — две реальные копии политики разных страниц.
	FallbackRig test;
	QVERIFY(test.initialize());
	// text возвращает ключи без перевода, чтобы адресация виджетов не зависела от подписи.
	const auto text = [](const char *key) {
		return QString::fromUtf8(key);
	};
	FallbackPolicyEditor first(test.rig.state, test.fallback, text);
	FallbackPolicyEditor second(test.rig.state, test.fallback, text);
	first.setStreamId(QStringLiteral("stream"));
	first.setActive(true);
	QTRY_COMPARE(test.rig.state.fallback().policy.streamId, QStringLiteral("stream"));
	second.setStreamId(QStringLiteral("other-stream"));
	QCoreApplication::processEvents();
	QCOMPARE(test.rig.state.fallback().policy.streamId, QStringLiteral("stream"));
	QCOMPARE(test.rig.state.selectedStreamId(), QStringLiteral("stream"));
	first.setActive(false);
	second.setActive(true);
	QTRY_COMPARE(test.rig.state.fallback().policy.streamId, QStringLiteral("other-stream"));
	QTRY_VERIFY(test.rig.state.fallback().policy.base.has_value());
	// changed является несвязанным обновлением общего состояния, которое видят обе копии.
	auto changed = test.rig.state.fallback();
	changed.draftName = QStringLiteral("Другой ввод");
	test.rig.state.setFallback(changed);
	QCoreApplication::processEvents();
	QCOMPARE(test.rig.state.fallback().policy.streamId, QStringLiteral("other-stream"));
	QCOMPARE(test.rig.state.selectedStreamId(), QStringLiteral("stream"));
	QVERIFY(test.rig.mutations().isEmpty());
}

// Проверяет согласованность публичной свежести, допуска контроллера и кнопки до ответа Asset и после него.
void FallbackUiTest::refreshRevokesPreparationUntilAssetReply()
{
	// held сохраняет точный запрос без ответа и живёт дольше перехватчика внутри test.
	std::optional<HttpServer::Request> held;
	// test соединяет настоящий контроллер и StateStore с собственным петлевым сервером.
	FallbackRig test;
	QVERIFY(test.initialize());
	QTRY_VERIFY_WITH_TIMEOUT(test.rig.state.fallback().assetsStatus.complete && !test.rig.state.fallback().loading,
				 1500);
	QVERIFY(test.fallback.selectAsset(QStringLiteral("asset")));
	QTRY_VERIFY_WITH_TIMEOUT(test.fallback.canPrepare() && !test.rig.state.fallback().loading &&
					 test.rig.state.fallback().current.variantsStatus.complete,
				 1500);
	QVERIFY(test.rig.state.fallback().assetFresh);
	QVERIFY(test.rig.state.fallback().policy.streamId.isEmpty());
	// text возвращает ключ key без перевода: тест адресует кнопку по objectName, а не по её подписи.
	const auto text = [](const char *key) {
		return QString::fromUtf8(key);
	};
	// editor остаётся пассивным и не открывает policy target, чья публикация могла бы скрыть пропуск уведомления.
	FallbackEditor editor(test.rig.state, test.fallback, text);
	// rebuild показывает реальный доступ к подготовке, установленный последним уведомлением StateStore.
	auto *rebuild = editor.findChild<QPushButton *>(QStringLiteral("fallbackRebuild"));
	QVERIFY(rebuild);
	QVERIFY(rebuild->isEnabled());
	// Перехватчик удерживает только следующий точный GET Asset; request содержит пришедший HTTP-запрос.
	test.intercept = [&held](const HttpServer::Request &request) {
		if (!held && request.method == "GET" && request.target == "/api/v1/fallback-assets/asset") {
			held = request;
			return true;
		}
		return false;
	};
	test.fallback.refresh();
	// Здесь нет обработки событий: ни ответ GET, ни другая публикация не успеют исправить устаревшую кнопку.
	QVERIFY(!test.rig.state.fallback().assetFresh);
	QVERIFY(!test.fallback.canPrepare());
	QVERIFY(!rebuild->isEnabled());
	QVERIFY(!test.fallback.rebuild());
	QTRY_VERIFY_WITH_TIMEOUT(held.has_value(), 1500);
	QVERIFY(held->socket);
	QVERIFY(!test.rig.state.fallback().assetFresh);
	QVERIFY(!rebuild->isEnabled());
	QVERIFY(test.rig.mutations().isEmpty());
	// Новая серверная revision отличает завершившийся GET от случайного возврата прежнего допуска.
	test.asset.insert(QStringLiteral("revision"), 2);
	HttpServer::reply(*held, fixture::bytes(test.asset));
	QTRY_VERIFY_WITH_TIMEOUT(
		test.rig.state.fallback().assetFresh && test.fallback.canPrepare() && rebuild->isEnabled(), 1500);
	QVERIFY(test.rig.state.fallback().asset.has_value());
	QCOMPARE(test.rig.state.fallback().asset->revision, quint64(2));
	QVERIFY(test.rig.state.fallback().policy.streamId.isEmpty());
	QVERIFY(test.rig.mutations().isEmpty());
	QCOMPARE(test.rig.frontend.stopRequests, 0);
}

void FallbackUiTest::navigationAndClosePreserveWorkflow()
{
	// test держит собственное new в queued до закрытия окна.
	FallbackRig test;
	test.generations.insert(QStringLiteral("new"),
				fallbackFixture::generation(QStringLiteral("new"), QStringLiteral("queued")));
	QVERIFY(test.initialize());
	// text возвращает ключи без перевода, чтобы адресация виджетов не зависела от подписи.
	const auto text = [](const char *key) {
		return QString::fromUtf8(key);
	};
	// window скрывает/открывает тот же экземпляр без переноса workflow в UI.
	ManagementWindow window(test.rig.state, text, nullptr, &test.rig.connection, &test.rig.operations,
				&test.rig.configuration, &test.fallback);
	// navigation/editor/name/path адресуют реальные элементы одной страницы.
	auto *navigation = window.findChild<QListWidget *>(QStringLiteral("managementNavigation"));
	auto *editor = window.findChild<FallbackEditor *>();
	QVERIFY(navigation && editor);
	window.show();
	QTRY_VERIFY(test.rig.state.configurationEditor().draft.has_value() &&
		    !test.rig.state.configurationEditor().loading);
	navigation->setCurrentRow(4);
	QTRY_VERIFY(editor->isVisible());
	QVERIFY(test.fallback.selectAsset(QStringLiteral("asset")));
	QTRY_VERIFY(test.rig.state.fallback().assetFresh);
	QVERIFY(test.fallback.setDraft(QStringLiteral("Новый ввод"),
				       test.rig.temp.filePath(QStringLiteral("chosen.png")), QStringLiteral("image")));
	QVERIFY(test.fallback.rebuild());
	QTRY_COMPARE(test.rig.state.fallback().outcome, FallbackWriteOutcome::Accepted);
	QVERIFY(window.close());
	QVERIFY(!window.isVisible());
	test.generations.insert(QStringLiteral("new"),
				fallbackFixture::generation(QStringLiteral("new"), QStringLiteral("ready")));
	QTRY_COMPARE(test.rig.state.fallback().observation, FallbackObservation::Ready);
	window.show();
	navigation->setCurrentRow(5);
	navigation->setCurrentRow(4);
	QTRY_VERIFY(editor->isVisible());
	auto *name = editor->findChild<QLineEdit *>(QStringLiteral("fallbackCreateName"));
	auto *path = editor->findChild<QLineEdit *>(QStringLiteral("fallbackFilePath"));
	QVERIFY(name && path);
	QCOMPARE(name->text(), QStringLiteral("Новый ввод"));
	QCOMPARE(path->text(), test.rig.temp.filePath(QStringLiteral("chosen.png")));
	QCOMPARE(test.rig.state.fallback().acceptedGenerationId, QStringLiteral("new"));
	QCOMPARE(test.rig.mutations().size(), 1);
	QCOMPARE(test.rig.frontend.stopRequests, 0);
}

void FallbackUiTest::variantRecipeCanBeExpandedWithoutEditing_data()
{
	QTest::addColumn<QString>("locale");
	QTest::newRow("ru") << QStringLiteral("ru-RU");
	QTest::newRow("en") << QStringLiteral("en-US");
}

void FallbackUiTest::variantRecipeCanBeExpandedWithoutEditing()
{
	// locale задаёт настоящий перевод; translations/text не читают пользовательскую конфигурацию Qt.
	QFETCH(QString, locale);
	QSettings translations(QStringLiteral(GOST_TEST_LOCALE_DIR) + QLatin1Char('/') + locale +
				       QStringLiteral(".ini"),
			       QSettings::IniFormat);
	const auto text = [&translations](const char *key) {
		return translations.value(QString::fromUtf8(key), QString::fromUtf8(key)).toString();
	};
	// test получает независимый серверный Variant с полным NVENC VBR/AAC-рецептом.
	FallbackRig test;
	QVERIFY(test.initialize());
	QVERIFY(test.fallback.selectAsset(QStringLiteral("asset")));
	QTRY_VERIFY(test.rig.state.fallback().current.variantsStatus.complete &&
		    !test.rig.state.fallback().current.variants.isEmpty());
	// editor показывает рецепт выбранной строки; table/profile адресуют только просмотр Variant.
	FallbackEditor editor(test.rig.state, test.fallback, text);
	editor.show();
	auto *table = editor.findChild<QTableWidget *>(QStringLiteral("fallbackVariants"));
	auto *profile = editor.findChild<ProfileEditor *>(QStringLiteral("fallbackVariantProfile"));
	QVERIFY(table && profile);
	QTRY_VERIFY(table->rowCount() > 0);
	table->setCurrentCell(0, 0);
	table->selectRow(0);
	QTRY_VERIFY(profile->isVisible());
	// advanced/content обеспечивают навигацию даже тогда, когда сами поля рецепта недоступны для изменения.
	auto *advanced = profile->findChild<QToolButton *>(QStringLiteral("profileAdvanced"));
	auto *content = profile->findChild<QWidget *>(QStringLiteral("profileAdvancedContent"));
	QVERIFY(advanced && content);
	QVERIFY(advanced->isEnabled());
	QCOMPARE(advanced->text(), text("Profile.Advanced"));
	// changes отличает навигационное раскрытие от запрещённого пользовательского изменения рецепта.
	QSignalSpy changes(profile, &ProfileEditor::changed);
	QVERIFY(changes.isValid());
	advanced->click();
	QVERIFY(advanced->isChecked());
	QVERIFY(content->isVisible());
	// expected задаёт контрактные исходные значения синтетического профиля, включая расширенные audio/video/encoding.
	const QMap<QString, QString> expected{{QStringLiteral("video.codec_profile"), QStringLiteral("high")},
					      {QStringLiteral("video.pixel_format"), QStringLiteral("yuv420p")},
					      {QStringLiteral("audio.codec"), QStringLiteral("aac")},
					      {QStringLiteral("audio.sample_rate"), QStringLiteral("48000")},
					      {QStringLiteral("encoding.video_encoder"), QStringLiteral("h264_nvenc")},
					      {QStringLiteral("encoding.audio_encoder"), QStringLiteral("aac")},
					      {QStringLiteral("encoding.rate_control"), QStringLiteral("vbr")},
					      {QStringLiteral("nvenc.lookahead"), QStringLiteral("16")}};
	// field указывает один проверяемый контрол; combo сохраняет исходное значение независимо от локализованной подписи.
	for (auto field = expected.cbegin(); field != expected.cend(); ++field) {
		auto *combo = content->findChild<QComboBox *>(field.key());
		QVERIFY2(combo, qPrintable(field.key()));
		QVERIFY(combo->isVisible());
		QCOMPARE(combo->currentData().toString(), field.value());
		QVERIFY(!combo->isEnabled());
	}
	// updated содержит несвязанное обновление прогресса; оно не должно сворачивать открытый рецепт.
	auto updated = test.rig.state.fallback();
	++updated.bytesSent;
	test.rig.state.setFallback(updated);
	QVERIFY(advanced->isChecked());
	QVERIFY(content->isVisible());
	QCOMPARE(changes.size(), 0);
	QVERIFY(test.rig.mutations().isEmpty());
	QCOMPARE(test.rig.frontend.stopRequests, 0);
}

QTEST_MAIN(FallbackUiTest)
#include "fallback-ui-test.moc"
