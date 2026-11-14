#include "api/json-decoder.hpp"
#include "api/json-encoder.hpp"
#include "fixtures/api-json.hpp"
#include "fixtures/configuration-rig.hpp"
#include "core/plugin-lifecycle.hpp"
#include "ui/configuration-editor.hpp"
#include "ui/management-window.hpp"
#include "ui/profile-editor.hpp"
#include "ui/secret-field.hpp"
#include <QApplication>
#include <QCheckBox>
#include <QClipboard>
#include <QComboBox>
#include <QFile>
#include <QGroupBox>
#include <QLineEdit>
#include <QLabel>
#include <QListWidget>
#include <QMessageBox>
#include <QPushButton>
#include <QPointer>
#include <QScopeGuard>
#include <QSignalSpy>
#include <QSettings>
#include <QStackedWidget>
#include <QTest>
#include <QToolButton>
#include <QTimer>
#include <limits>

using namespace gost;
using namespace gost::api;

namespace {
// Возвращает ключи подписей, позволяя проверять поведение формы без подмены значений API переводом.
QString translatedKey(const char *key)
{
	return QString::fromUtf8(key);
}
// Передаёт виджету field явный пользовательский выбор value и сигнал activated для RAM-формы.
bool choose(QComboBox *field, const QString &value)
{
	if (!field)
		return false;
	// index адресует точное контрактное значение, а не локализованный отображаемый текст.
	const int index = field->findData(value);
	if (index < 0)
		return false;
	field->setCurrentIndex(index);
	return QMetaObject::invokeMethod(field, "activated", Q_ARG(int, index));
}
} // namespace

// Проверяет структурные Widgets через публичный ввод и результаты, без нативной OBS-сессии.
class ConfigurationUiTest final : public QObject {
	Q_OBJECT
private slots:
	// Маска не меняет точную строку Copy, включая userinfo/path/query, и не отмечает draft изменённым.
	void maskedCopyUsesReceivedValueWithoutRevealingOrEditing();
	// Missing и пустой допустимый ключ показываются по-разному и не подменяются фиктивным значением.
	void unavailableAndEmptyKeysDisableCopy();
	// Редактирование одного поля сохраняет точные пробелы и допускает намеренно пустой ключ.
	void secretReplacementHasExplicitMissingAndEmptyIntent();
	// Новое чтение сущности и hideSecret возвращают маску, сохраняя несохранённую замену.
	void receivedEntityChangeAndPageRemaskHideBothRepresentations();
	// Задаёт четыре общих редактора и пустую либо изменённую форму создания.
	void existingRecordCanBeSelectedAfterCreate_data();
	// После «Создать» повторный щелчок по прежней записи снова показывает её параметры без записи на сервер.
	void existingRecordCanBeSelectedAfterCreate();
	// Задаёт legacy/null/missing и полный v2 с двоичным ProfileText.
	void structuredProfilePreservesUntouchedFields_data();
	// Отражение и чтение формы не теряют FPS, байты, LIVE AAC и присутствие encoding.
	void structuredProfilePreservesUntouchedFields();
	// Изменение одного поля не пересобирает незатронутый двоичный текст или весь рецепт.
	void changingOneProfileFieldPreservesBytesAndLegacyAudio();
	// Пользователь не может удалить сохранённый encoding v2 или выбрать transcode AAC для video copy.
	void incompatibleAudioAndRemovingV2AreRejected();
	// Смена video mode старого профиля требует явного решения об обработке LIVE AAC.
	void changingLegacyVideoModeRequiresExplicitAudioDecision();
	// Пустой новый профиль не сохраняется с вымышленными форматами или площадочными presets.
	void newProfileRequiresExplicitMediaFormat();
	// Дробный размер и переполнение битрейта вызывают ошибку вместо округления.
	void invalidStructuredInputIsNotSilentlyNormalized();
	// Все три битрейта переводятся в кбит/с без потери последних битов исходного uint64.
	void bitrateUnitsPreservePrecisionAndConvertExplicitInput();
	// Типовые FPS превращаются в точные контрактные дроби, нестандартная дробь сохраняется.
	void frameRateChoicesKeepExactRatios();
	// Списки сохраняют неизвестные и пустые значения, а сворачивание не теряет введённый текст.
	void advancedChoicesAndVisibilityKeepUnsavedValues();
	// Пресеты сравниваются с полными профилями владельца, независимо от кода редактора.
	void presetsMatchVerifiedCompleteProfiles();
	// Задаёт русский и английский языки с короткими стандартными ответами.
	void confirmationButtonsFollowLocaleAndKeepSafeDefault_data();
	// Настоящий диалог сохраняет стандартную идентичность ответов и выбор «Нет» по умолчанию.
	void confirmationButtonsFollowLocaleAndKeepSafeDefault();
	// Переход между отдельными страницами сохраняет имя группы и ошибочный сырой ввод профиля.
	void separatePagesKeepDirtyFormsAcrossNavigation();
	// Единственные поля адреса/ключа отправляют точный новый ключ и явную очистку без флажка замены.
	void destinationSecretsEditAndClearThroughSingleFields();
	// Очистка обязательного адреса показывает ошибку рядом с полем и не отправляет изменение на сервер.
	void clearingDestinationEndpointShowsLocalFieldError();
	// Поток показывает только свои назначения, без сводки групп и зависимости от их загрузки.
	void streamSelectionUsesOwnDestinationNamesWithoutGroupSummary();
	// Задаёт обе локализации для проверки отсутствия управления эфиром в редакторах.
	void configurationPagesExcludeRuntimeControls_data();
	// Потоки и выходы содержат только редактирование; команды текущего эфира остаются в Dock.
	void configurationPagesExcludeRuntimeControls();
	// Переход из Dock открывает назначение по ID, паркует прежний ввод и отвергает устаревшую цель.
	void dockDetailsOpenSavedDestinationAndPreserveDraft();
	// Сервис выбирается закрытым списком, неизвестное значение сохраняется до явного выбора.
	void platformChoicesPreserveUnknownService();
	// Карточка отправляет только изменённое имя, сохраняя точные секреты и рабочий Stream.
	void destinationCardSavesNameWithoutReplacingSecrets();
	// Селектор отношений использует точный ID, фильтрует чужие группы и сохраняет неподтверждённую ссылку.
	void relationChoicesKeepExactIdentifiersAndUnknownReferences();
	// Отказ подтверждения dirty-навигации сохраняет ввод; скрытие страницы маскирует секреты.
	void dirtyNavigationRequiresDecisionAndPageRemasksSecrets();
	// Задаёт оба пользовательских режима штатного журнала.
	void clientLogsExcludeServerSecretsInBothModes_data();
	// Успехи и отказы CRUD проходят через sink Runtime без payload, ключей, endpoint и сырых серверных текстов.
	void clientLogsExcludeServerSecretsInBothModes();
	// Задаёт открытие существующей карточки и создание новой после явного отказа от stale draft.
	void explicitDiscardAfterProfileChangeAllowsCurrentContext_data();
	// Подтверждённый отказ от старого ввода не отправляет его и не блокирует текущий UUID.
	void explicitDiscardAfterProfileChangeAllowsCurrentContext();
	// Чистый Unknown DELETE нельзя обойти кнопкой создания; адресная сверка 404 разрешает новое намерение.
	void unknownDeleteRequiresReconciliationBeforeReplacingEditor();
	// EXIT внутри реального подтверждения синхронно удаляет Runtime и окно без поздних вызовов Frontend.
	void reopeningManagementAfterProfileChangeSurvivesExitInsideConfirmation();
	// Задаёт HTTP503, отсутствие карточки404 и timeout первого адресного чтения.
	void failedFirstDetailReadKeepsVisibleErrorAndReload_data();
	// Неуспешное первое чтение оставляет доступное сообщение и явный Reload без создания фиктивного draft.
	void failedFirstDetailReadKeepsVisibleErrorAndReload();
	// Успешное удаление после сохранения обновляет inline-результат даже при отсутствии draft.
	void deletingSavedGroupShowsDeletedInsteadOfPreviousSavedMessage();
};

// Использует независимые серверные документы для проверки общего механизма выбора карточек.
void ConfigurationUiTest::existingRecordCanBeSelectedAfterCreate_data()
{
	QTest::addColumn<int>("kindValue");
	QTest::addColumn<QString>("id");
	QTest::addColumn<QString>("expectedName");
	QTest::addColumn<bool>("dirtyNewDraft");
	// kind перебирает редакторы с общими списком записей и кнопкой создания.
	for (const auto kind : {ConfigurationKind::Stream, ConfigurationKind::Destination, ConfigurationKind::Group,
				ConfigurationKind::Profile}) {
		// document задаёт независимые ID и имя записи, к которой пользователь возвращается из создания.
		QJsonObject document;
		switch (kind) {
		case ConfigurationKind::Stream:
			document = fixture::stream();
			break;
		case ConfigurationKind::Destination:
			document = fixture::destination();
			break;
		case ConfigurationKind::Group:
			document = fixture::group();
			break;
		case ConfigurationKind::Profile:
			document = fixture::profile();
			break;
		}
		QTest::newRow(qPrintable(document.value(QStringLiteral("id")).toString() + QStringLiteral("-empty")))
			<< static_cast<int>(kind) << document.value(QStringLiteral("id")).toString()
			<< document.value(QStringLiteral("name")).toString() << false;
		QTest::newRow(qPrintable(document.value(QStringLiteral("id")).toString() + QStringLiteral("-dirty")))
			<< static_cast<int>(kind) << document.value(QStringLiteral("id")).toString()
			<< document.value(QStringLiteral("name")).toString() << true;
	}
}

// Повторяет пользовательский путь через настоящий QListWidget и контроллер, не вызывая open вместо щелчка.
void ConfigurationUiTest::existingRecordCanBeSelectedAfterCreate()
{
	// kindValue выбирает редактор; id и expectedName — точную запись и её серверное имя.
	// dirtyNewDraft добавляет несохранённое имя новой записи перед возвращением к существующей.
	QFETCH(int, kindValue);
	QFETCH(QString, id);
	QFETCH(QString, expectedName);
	QFETCH(bool, dirtyNewDraft);
	// kind задаёт типизированный вид страницы; rig изолирует все файлы и HTTP внутри теста.
	const auto kind = static_cast<ConfigurationKind>(kindValue);
	ConfigurationRig rig;
	QVERIFY(rig.initialize());
	// page показывает настоящий список; list адресует строки, name — видимое имя текущей карточки.
	ConfigurationEditor page(rig.state, rig.configuration, translatedKey, kind);
	auto *list = page.findChild<QListWidget *>(QStringLiteral("configurationList"));
	auto *name = page.findChild<QLineEdit *>(QStringLiteral("configurationName"));
	// create находит пользовательскую кнопку по независимому ключу подписи, без прямого вызова контроллера.
	QPushButton *create = nullptr;
	// button перебирает только действия этой страницы и исключает вложенные кнопки других полей.
	for (auto *button : page.findChildren<QPushButton *>())
		if (button->text() == translatedKey("Editor.Create"))
			create = button;
	QVERIFY(list && name && create);
	page.resize(900, 600);
	page.show();
	rig.configuration.refresh(kind);
	QTRY_COMPARE_WITH_TIMEOUT(list->count(), 1, 1000);
	QTest::mouseClick(list->viewport(), Qt::LeftButton, Qt::NoModifier,
			  list->visualItemRect(list->item(0)).center());
	QTRY_VERIFY_WITH_TIMEOUT(
		rig.state.configurationEditor().base.has_value() && rig.state.configurationEditor().id == id, 1000);
	QCOMPARE(list->currentRow(), 0);
	QCOMPARE(name->text(), expectedName);
	create->click();
	QVERIFY(rig.state.configurationEditor().creating);
	QVERIFY(rig.state.configurationEditor().id.isEmpty());
	QCOMPARE(name->text(), QString{});
	QCOMPARE(list->currentRow(), -1);
	QVERIFY(list->selectedItems().isEmpty());
	if (dirtyNewDraft)
		QTest::keyClicks(name, "Unsaved new record");
	QTest::mouseClick(list->viewport(), Qt::LeftButton, Qt::NoModifier,
			  list->visualItemRect(list->item(0)).center());
	QTRY_VERIFY_WITH_TIMEOUT(!rig.state.configurationEditor().creating &&
					 rig.state.configurationEditor().base.has_value() &&
					 rig.state.configurationEditor().id == id,
				 1000);
	QCOMPARE(list->currentRow(), 0);
	QVERIFY(name->isVisible() && name->isEnabled());
	QCOMPARE(name->text(), expectedName);
	QCOMPARE(rig.state.selectedStreamId(), QStringLiteral("stream"));
	QVERIFY(rig.mutations().isEmpty());
}

void ConfigurationUiTest::maskedCopyUsesReceivedValueWithoutRevealingOrEditing()
{
	// original/restore сохраняют clipboard этого изолированного offscreen-процесса после проверки.
	const auto original = QApplication::clipboard()->text();
	const auto restore = qScopeGuard([&] { QApplication::clipboard()->setText(original); });
	// field получает точный endpoint; view/eye/copy — публичные элементы чтения, spy отслеживает draft.
	SecretField field(translatedKey);
	field.setReceived({FieldState::Value, fixture::endpoint()});
	auto *view = field.findChild<QLineEdit *>(QStringLiteral("secretReceived"));
	auto *eye = field.findChild<QToolButton *>(QStringLiteral("secretEye"));
	auto *copy = field.findChild<QToolButton *>(QStringLiteral("secretCopy"));
	QSignalSpy spy(&field, &SecretField::changed);
	QVERIFY(view && eye && copy);
	QVERIFY(spy.isValid());
	QCOMPARE(view->echoMode(), QLineEdit::Password);
	copy->click();
	QCOMPARE(QApplication::clipboard()->text(), fixture::endpoint());
	QCOMPARE(view->echoMode(), QLineEdit::Password);
	eye->click();
	QCOMPARE(view->text(), fixture::endpoint());
	QCOMPARE(spy.size(), 0);
	QVERIFY(!field.replacement());
}
void ConfigurationUiTest::unavailableAndEmptyKeysDisableCopy()
{
	// field/view/copy сравнивают две доступные пользователю причины отсутствия операции Copy.
	SecretField field(translatedKey);
	auto *view = field.findChild<QLineEdit *>(QStringLiteral("secretReceived"));
	auto *copy = field.findChild<QToolButton *>(QStringLiteral("secretCopy"));
	QVERIFY(view && copy);
	field.setReceived({FieldState::Missing, QStringLiteral("stale-secret")}, true);
	QVERIFY(view->text().isEmpty());
	QCOMPARE(view->placeholderText(), QStringLiteral("Secret.Unavailable"));
	QVERIFY(!copy->isEnabled());
	field.setReceived({FieldState::Value, QString{}}, true);
	QVERIFY(view->text().isEmpty());
	QCOMPARE(view->placeholderText(), QStringLiteral("Secret.Unset"));
	QVERIFY(!copy->isEnabled());
	QVERIFY(field.received().hasValue());
	QVERIFY(field.received()->isEmpty());
}
void ConfigurationUiTest::secretReplacementHasExplicitMissingAndEmptyIntent()
{
	// field хранит серверную базу, input принимает точное пользовательское намерение без второго поля.
	SecretField field(translatedKey);
	field.setEditable(true);
	field.setReceived({FieldState::Value, QStringLiteral("server-key")}, true);
	auto *input = field.findChild<QLineEdit *>(QStringLiteral("secretReceived"));
	QVERIFY(input);
	QCOMPARE(field.findChildren<QLineEdit *>().size(), 1);
	QVERIFY(!input->isReadOnly());
	QVERIFY(!field.replacement());
	input->selectAll();
	QTest::keyClicks(input, "  new-key/+%2F  ");
	QVERIFY(field.replacement().has_value());
	QCOMPARE(*field.replacement(), QStringLiteral("  new-key/+%2F  "));
	QCOMPARE(*field.received(), QStringLiteral("server-key"));
	input->selectAll();
	QTest::keyClick(input, Qt::Key_Backspace);
	QVERIFY(field.replacement().has_value());
	QVERIFY(field.replacement()->isEmpty());
	field.clearReplacement();
	QVERIFY(!field.replacement());
	QCOMPARE(input->text(), QStringLiteral("server-key"));
}
void ConfigurationUiTest::receivedEntityChangeAndPageRemaskHideBothRepresentations()
{
	// field/view/eye показывают несохранённый ввод в единственном маскируемом поле.
	SecretField field(translatedKey);
	field.setEditable(true);
	field.setReceived({FieldState::Value, QStringLiteral("first")});
	field.setReplacement(QStringLiteral("draft"));
	auto *view = field.findChild<QLineEdit *>(QStringLiteral("secretReceived"));
	auto *eye = field.findChild<QToolButton *>(QStringLiteral("secretEye"));
	QVERIFY(view && eye);
	eye->click();
	QCOMPARE(view->text(), QStringLiteral("draft"));
	QCOMPARE(view->echoMode(), QLineEdit::Normal);
	field.hideSecret();
	QCOMPARE(view->echoMode(), QLineEdit::Password);
	QCOMPARE(*field.replacement(), QStringLiteral("draft"));
	eye->click();
	field.setReceived({FieldState::Value, QStringLiteral("second")});
	QCOMPARE(view->echoMode(), QLineEdit::Password);
}
void ConfigurationUiTest::structuredProfilePreservesUntouchedFields_data()
{
	QTest::addColumn<QJsonObject>("document");
	// document последовательно задаёт независимые формы присутствия encoding и LIVE AAC.
	auto document = fixture::groupProfile();
	QTest::newRow("legacy-null") << document;
	document.remove(QStringLiteral("encoding"));
	QTest::newRow("legacy-missing") << document;
	document.insert(QStringLiteral("encoding"), fixture::encoding());
	document.insert(QStringLiteral("live_audio_mode"), QStringLiteral("copy"));
	QTest::newRow("explicit-v2") << document;
	// format/video добавляют непредставимый текст, который нельзя восстанавливать из QString.
	auto format = document.value(QStringLiteral("format")).toObject();
	auto video = format.value(QStringLiteral("video")).toObject();
	video.insert(QStringLiteral("level"), fixture::object(R"({"encoding":"base64","value":"/wBh"})"));
	format.insert(QStringLiteral("video"), video);
	document.insert(QStringLiteral("format"), format);
	QTest::newRow("binary-text") << document;
}
void ConfigurationUiTest::structuredProfilePreservesUntouchedFields()
{
	// document содержит полное независимое представление профиля выбранной строки сценария.
	QFETCH(QJsonObject, document);
	// decoded/editor/restored отражают обычное открытие и чтение полной формы без пользовательских правок.
	const auto decoded = decode<GroupProfile>(fixture::bytes(document));
	QVERIFY(decoded);
	ProfileEditor editor(translatedKey);
	editor.setProfile(*decoded.value);
	GroupProfile restored;
	QString error;
	QSignalSpy spy(&editor, &ProfileEditor::changed);
	QVERIFY(spy.isValid());
	QVERIFY2(editor.readProfile(restored, error), qPrintable(error));
	// encoded сравнивается с независимым исходным JSON целиком, а не с результатом самого редактора.
	const auto encoded = encodeProfile(restored);
	QVERIFY(encoded);
	QCOMPARE(*encoded.value, document);
	QCOMPARE(spy.size(), 0);
}
void ConfigurationUiTest::changingOneProfileFieldPreservesBytesAndLegacyAudio()
{
	// decoded/profile/editor задают старый профиль с двоичным уровнем и отсутствующим LIVE AAC.
	const auto decoded = decode<GroupProfile>(fixture::bytes(fixture::groupProfile()));
	QVERIFY(decoded);
	auto profile = *decoded.value;
	profile.format.video.level.bytes = QByteArray::fromHex("ff0061");
	ProfileEditor editor(translatedKey);
	editor.setProfile(profile);
	// bitrate — единственный пользовательски изменённый ввод, остальные поля должны остаться точными.
	auto *bitrate = editor.findChild<QLineEdit *>(QStringLiteral("audio.bitrate"));
	QVERIFY(bitrate);
	bitrate->selectAll();
	QTest::keyClicks(bitrate, "128");
	// restored/error принимают полный результат и возможную ошибку структурного ввода.
	GroupProfile restored;
	QString error;
	QVERIFY2(editor.readProfile(restored, error), qPrintable(error));
	QCOMPARE(restored.audioEncoding.bitrateBps, quint64(128000));
	QCOMPARE(restored.format.video.level.bytes, QByteArray::fromHex("ff0061"));
	QVERIFY(restored.liveAudioMode.isMissing());
	QVERIFY(restored.encoding.isNull());
	QCOMPARE(restored.format.video.frameRate.numerator, quint32(60000));
	QCOMPARE(restored.format.video.frameRate.denominator, quint32(1001));
}
void ConfigurationUiTest::incompatibleAudioAndRemovingV2AreRejected()
{
	// decoded/profile/editor задают допустимый явный рецепт v2 и LIVE transcode при video transcode.
	const auto decoded = decode<GroupProfile>(fixture::bytes(fixture::groupProfile()));
	QVERIFY(decoded);
	auto profile = *decoded.value;
	profile.encoding = {FieldState::Value, *decode<Encoding>(fixture::bytes(fixture::encoding())).value};
	profile.liveAudioMode = {FieldState::Value, QStringLiteral("transcode")};
	ProfileEditor editor(translatedKey);
	editor.setProfile(profile);
	// recipe позволяет пользователю запросить legacy, но уже сохранённый v2 нельзя молча удалить.
	auto *recipe = editor.findChild<QComboBox *>(QStringLiteral("encodingPresence"));
	QVERIFY(recipe);
	recipe->setCurrentIndex(1);
	QVERIFY(QMetaObject::invokeMethod(recipe, "activated", Q_ARG(int, 1)));
	// restored/error фиксируют отказ удаления и затем независимый отказ несовместимого режима AAC.
	GroupProfile restored;
	QString error;
	QVERIFY(!editor.readProfile(restored, error));
	QVERIFY(!error.isEmpty());
	editor.setProfile(profile);
	QVERIFY(choose(editor.findChild<QComboBox *>(QStringLiteral("mode")), QStringLiteral("copy")));
	QVERIFY(!editor.readProfile(restored, error));
	QVERIFY(!error.isEmpty());
}
void ConfigurationUiTest::changingLegacyVideoModeRequiresExplicitAudioDecision()
{
	// decoded/editor задают старый transcode с наследуемой обработкой AAC.
	const auto decoded = decode<GroupProfile>(fixture::bytes(fixture::groupProfile()));
	QVERIFY(decoded);
	ProfileEditor editor(translatedKey);
	editor.setProfile(*decoded.value);
	QVERIFY(choose(editor.findChild<QComboBox *>(QStringLiteral("mode")), QStringLiteral("copy")));
	// restored/error не должны получить молчаливое переключение наследуемого transcode AAC в copy.
	GroupProfile restored;
	QString error;
	QVERIFY(!editor.readProfile(restored, error));
	QVERIFY(!error.isEmpty());
	QVERIFY(choose(editor.findChild<QComboBox *>(QStringLiteral("live_audio_mode")), QStringLiteral("copy")));
	QVERIFY2(editor.readProfile(restored, error), qPrintable(error));
	QCOMPARE(*restored.liveAudioMode, QStringLiteral("copy"));
}
void ConfigurationUiTest::newProfileRequiresExplicitMediaFormat()
{
	// profile содержит только разрешённый default LIVE AAC copy, без вымышленных размеров/частот/битрейтов.
	GroupProfile profile;
	profile.mode = QStringLiteral("copy");
	profile.liveAudioMode = {FieldState::Value, QStringLiteral("copy")};
	// editor/restored/error проверяют обязательный новый формат при создании.
	ProfileEditor editor(translatedKey);
	editor.setProfile(profile, true);
	GroupProfile restored;
	QString error;
	QVERIFY(!editor.readProfile(restored, error));
	QVERIFY(!error.isEmpty());
}
// Некорректный размер и слишком большой кбит/с не превращаются в округлённый либо переполненный DTO.
void ConfigurationUiTest::invalidStructuredInputIsNotSilentlyNormalized()
{
	// decoded/editor задают исходный допустимый профиль до независимых ошибочных вводов.
	const auto decoded = decode<GroupProfile>(fixture::bytes(fixture::groupProfile()));
	QVERIFY(decoded);
	ProfileEditor editor(translatedKey);
	editor.setProfile(*decoded.value);
	// resolution принимает единый пользовательский размер; restored/error фиксируют отказ чтения формы.
	auto *resolution = editor.findChild<QComboBox *>(QStringLiteral("video.resolution"));
	GroupProfile restored;
	QString error;
	QVERIFY(resolution && resolution->isEditable());
	resolution->lineEdit()->selectAll();
	QTest::keyClicks(resolution->lineEdit(), "1920.5x1080");
	QVERIFY(!editor.readProfile(restored, error));
	QVERIFY(!error.isEmpty());
	editor.setProfile(*decoded.value);
	// bitrate превышает uint64 после перевода кбит/с в бит/с ровно на один бит.
	auto *bitrate = editor.findChild<QLineEdit *>(QStringLiteral("video.target"));
	QVERIFY(bitrate);
	bitrate->selectAll();
	QTest::keyClicks(bitrate, "18446744073709551.616");
	QVERIFY(!editor.readProfile(restored, error));
	QVERIFY(!error.isEmpty());
}

// Проверяет десятичные единицы без double, включая максимум uint64 и некратный 1000 остаток.
void ConfigurationUiTest::bitrateUnitsPreservePrecisionAndConvertExplicitInput()
{
	// decoded/profile задают три независимых больших значения в бит/с.
	const auto decoded = decode<GroupProfile>(fixture::bytes(fixture::groupProfile()));
	QVERIFY(decoded);
	auto profile = *decoded.value;
	profile.videoEncoding.targetBitrateBps = 6000123;
	profile.videoEncoding.maxBitrateBps = std::numeric_limits<quint64>::max();
	profile.audioEncoding.bitrateBps = 160001;
	// editor/restored/error отражают профиль и возвращают изменённый DTO.
	ProfileEditor editor(translatedKey);
	editor.setProfile(profile);
	GroupProfile restored;
	QString error;
	QVERIFY2(editor.readProfile(restored, error), qPrintable(error));
	QCOMPARE(restored.videoEncoding.targetBitrateBps, quint64(6000123));
	QCOMPARE(restored.videoEncoding.maxBitrateBps, std::numeric_limits<quint64>::max());
	QCOMPARE(restored.audioEncoding.bitrateBps, quint64(160001));
	// target/maximum/audio показывают точные десятичные кбит/с и принимают обычный ввод OBS.
	auto *target = editor.findChild<QLineEdit *>(QStringLiteral("video.target"));
	auto *maximum = editor.findChild<QLineEdit *>(QStringLiteral("video.maximum"));
	auto *audio = editor.findChild<QLineEdit *>(QStringLiteral("audio.bitrate"));
	QVERIFY(target && maximum && audio);
	QCOMPARE(QString(target->text()).replace(QLatin1Char(','), QLatin1Char('.')), QStringLiteral("6000.123"));
	QCOMPARE(QString(maximum->text()).replace(QLatin1Char(','), QLatin1Char('.')),
		 QStringLiteral("18446744073709551.615"));
	QCOMPARE(QString(audio->text()).replace(QLatin1Char(','), QLatin1Char('.')), QStringLiteral("160.001"));
	maximum->selectAll();
	QTest::keyClicks(maximum, "18446744073709551.615");
	target->selectAll();
	QTest::keyClicks(target, "6000");
	audio->selectAll();
	QTest::keyClicks(audio, "160");
	QVERIFY2(editor.readProfile(restored, error), qPrintable(error));
	QCOMPARE(restored.videoEncoding.targetBitrateBps, quint64(6000000));
	QCOMPARE(restored.audioEncoding.bitrateBps, quint64(160000));
	QCOMPARE(restored.videoEncoding.maxBitrateBps, std::numeric_limits<quint64>::max());
}

// Проверяет сохранение нестандартной дроби и точное преобразование трёх типовых частот.
void ConfigurationUiTest::frameRateChoicesKeepExactRatios()
{
	// decoded/profile содержат нестандартную исходную частоту, которая не обязана быть в базовом списке.
	const auto decoded = decode<GroupProfile>(fixture::bytes(fixture::groupProfile()));
	QVERIFY(decoded);
	auto profile = *decoded.value;
	profile.format.video.frameRate = {12345, 678};
	// editor/fps дают единый закрытый выбор; restored/error принимают полный результат.
	ProfileEditor editor(translatedKey);
	editor.setProfile(profile);
	auto *fps = editor.findChild<QComboBox *>(QStringLiteral("video.fps"));
	GroupProfile restored;
	QString error;
	QVERIFY(fps && !fps->isEditable());
	QVERIFY2(editor.readProfile(restored, error), qPrintable(error));
	QCOMPARE(restored.format.video.frameRate.numerator, quint32(12345));
	QCOMPARE(restored.format.video.frameRate.denominator, quint32(678));
	QVERIFY(choose(fps, QStringLiteral("60000/1001")));
	QVERIFY2(editor.readProfile(restored, error), qPrintable(error));
	QCOMPARE(restored.format.video.frameRate.numerator, quint32(60000));
	QCOMPARE(restored.format.video.frameRate.denominator, quint32(1001));
	QVERIFY(choose(fps, QStringLiteral("30000/1001")));
	QVERIFY2(editor.readProfile(restored, error), qPrintable(error));
	QCOMPARE(restored.format.video.frameRate.numerator, quint32(30000));
	QCOMPARE(restored.format.video.frameRate.denominator, quint32(1001));
	QVERIFY(choose(fps, QStringLiteral("60/1")));
	QVERIFY2(editor.readProfile(restored, error), qPrintable(error));
	QCOMPARE(restored.format.video.frameRate.numerator, quint32(60));
	QCOMPARE(restored.format.video.frameRate.denominator, quint32(1));
}

// Скрытие расширенного раздела и смена кодека сохраняют неизвестные байты и недопустимый сырой ввод.
void ConfigurationUiTest::advancedChoicesAndVisibilityKeepUnsavedValues()
{
	// decoded/profile моделируют старый профиль с неизвестным именем кодек-профиля и пустым tier.
	const auto decoded = decode<GroupProfile>(fixture::bytes(fixture::groupProfile()));
	QVERIFY(decoded);
	auto profile = *decoded.value;
	profile.format.video.codecProfile.bytes = QByteArray("unlisted-profile");
	profile.format.video.tier.bytes.clear();
	// editor/advanced/content адресуют общую форму и раскрываемый раздел.
	ProfileEditor editor(translatedKey);
	editor.setProfile(profile);
	auto *advanced = editor.findChild<QToolButton *>(QStringLiteral("profileAdvanced"));
	auto *content = editor.findChild<QWidget *>(QStringLiteral("profileAdvancedContent"));
	QVERIFY(advanced && content);
	QVERIFY(!advanced->isChecked());
	QVERIFY(content->isHidden());
	advanced->click();
	// codecProfile/tier остаются закрытыми списками с точными исходными байтами.
	auto *codecProfile = editor.findChild<QComboBox *>(QStringLiteral("video.codec_profile"));
	auto *tier = editor.findChild<QComboBox *>(QStringLiteral("video.tier"));
	QVERIFY(codecProfile && tier);
	QVERIFY(!codecProfile->isEditable() && !tier->isEditable());
	QCOMPARE(codecProfile->currentData().toByteArray(), QByteArray("unlisted-profile"));
	QVERIFY(tier->currentData().toByteArray().isEmpty());
	// gop сохраняет ошибочный промежуточный ввод, который нельзя терять при сворачивании.
	auto *gop = editor.findChild<QLineEdit *>(QStringLiteral("video.gop"));
	QVERIFY(gop);
	gop->selectAll();
	QTest::keyClicks(gop, "12x");
	advanced->click();
	advanced->click();
	QCOMPARE(gop->text(), QStringLiteral("12x"));
	QVERIFY(choose(editor.findChild<QComboBox *>(QStringLiteral("video.codec")), QStringLiteral("av1")));
	QVERIFY(choose(editor.findChild<QComboBox *>(QStringLiteral("video.codec")), QStringLiteral("h264")));
	QCOMPARE(gop->text(), QStringLiteral("12x"));
	QCOMPARE(codecProfile->currentData().toByteArray(), QByteArray("unlisted-profile"));
}

// Сравнивает полные серверные документы, включая расширенные параметры, с независимой фикстурой владельца.
void ConfigurationUiTest::presetsMatchVerifiedCompleteProfiles()
{
	// source читает зафиксированный полный список; items содержит все три подтверждённых профиля.
	QFile source(QStringLiteral(GOST_TEST_FIXTURE_DIR) + QStringLiteral("/backend/profile-presets.json"));
	QVERIFY(source.open(QIODevice::ReadOnly));
	const auto items = QJsonDocument::fromJson(source.readAll()).object().value(QStringLiteral("items")).toArray();
	QCOMPARE(items.size(), 3);
	// item содержит независимое имя и полный профиль, без предположений о недостающих расширенных значениях.
	for (const auto &item : items) {
		// expected задаёт полный документ владельца; presetId связывает его с публичным выбором.
		const auto expected = item.toObject();
		const auto presetId = expected.value(QStringLiteral("name")).toString() == QStringLiteral("AV1 1440p60")
					      ? QStringLiteral("av1-1440p60")
				      : expected.value(QStringLiteral("name")).toString() ==
						      QStringLiteral("H.264 1440p60")
					      ? QStringLiteral("h264-1440p60")
					      : QStringLiteral("h264-1080p60");
		// editor/preset/restored/error проверяют создание полного профиля без раскрытия расширенных параметров.
		ProfileEditor editor(translatedKey);
		editor.setProfile(GroupProfile{}, true);
		auto *preset = editor.findChild<QComboBox *>(QStringLiteral("profilePreset"));
		GroupProfile restored;
		QString error;
		QVERIFY(choose(preset, presetId));
		QVERIFY2(editor.readProfile(restored, error), qPrintable(error));
		// encoded сравнивается целиком, поэтому неверный скрытый параметр также провалит проверку.
		const auto encoded = encodeProfile(restored);
		QVERIFY(encoded);
		QCOMPARE(*encoded.value, expected.value(QStringLiteral("profile")).toObject());
	}
}

// Задаёт реальные языки ресурсов OBS и независимые ожидаемые подписи ответов.
void ConfigurationUiTest::confirmationButtonsFollowLocaleAndKeepSafeDefault_data()
{
	QTest::addColumn<QString>("locale");
	QTest::addColumn<QString>("yes");
	QTest::addColumn<QString>("no");
	QTest::newRow("russian") << QStringLiteral("ru-RU") << QStringLiteral("Да") << QStringLiteral("Нет");
	QTest::newRow("english") << QStringLiteral("en-US") << QStringLiteral("Yes") << QStringLiteral("No");
}

// Проверяет локализацию настоящего подтверждения отказа от черновика с исходным безопасным default.
void ConfigurationUiTest::confirmationButtonsFollowLocaleAndKeepSafeDefault()
{
	// locale адресует файл локализации, yes/no содержат ожидаемые пользовательские ответы.
	QFETCH(QString, locale);
	QFETCH(QString, yes);
	QFETCH(QString, no);
	// translations остаётся живым до удаления page; переводчик читает только поставляемую локализацию.
	QSettings translations(QStringLiteral(GOST_TEST_LOCALE_DIR) + QLatin1Char('/') + locale +
				       QStringLiteral(".ini"),
			       QSettings::IniFormat);
	// rig/page предоставляют реальную dirty-форму и диалог, без копирования механизма локализации.
	ConfigurationRig rig;
	QVERIFY(rig.initialize());
	ConfigurationEditor page(
		rig.state, rig.configuration,
		[&translations](const char *key) {
			return translations.value(QString::fromUtf8(key), QString::fromUtf8(key)).toString();
		},
		ConfigurationKind::Group);
	QVERIFY(rig.configuration.open(ConfigurationKind::Group, QStringLiteral("group")));
	QTRY_VERIFY_WITH_TIMEOUT(rig.state.configurationEditor().base.has_value(), 1000);
	// name создаёт пользовательское изменение, answered подтверждает фактическое открытие QMessageBox.
	auto *name = page.findChild<QLineEdit *>(QStringLiteral("configurationName"));
	QVERIFY(name);
	name->selectAll();
	QTest::keyClicks(name, "Unsaved group");
	bool answered = false;
	// actualYes/actualNo сохраняют наблюдаемые подписи; defaultIsNo фиксирует безопасный выбор до закрытия.
	QString actualYes;
	QString actualNo;
	bool defaultIsNo = false;
	// Callback снимает данные настоящего диалога и закрывает его до проверок, чтобы отказ не оставил exec открытым.
	QTimer::singleShot(0, &page, [&] {
		// dialog принадлежит только проверяемой странице; кнопки проверяются по стандартной идентичности.
		if (auto *dialog = page.findChild<QMessageBox *>()) {
			answered = true;
			actualYes = dialog->button(QMessageBox::Yes)->text();
			actualNo = dialog->button(QMessageBox::No)->text();
			defaultIsNo = dialog->defaultButton() == dialog->button(QMessageBox::No);
			dialog->done(QMessageBox::No);
		}
	});
	QVERIFY(!page.canLeave());
	QVERIFY(answered);
	QCOMPARE(actualYes, yes);
	QCOMPARE(actualNo, no);
	QVERIFY(defaultIsNo);
	QCOMPARE(name->text(), QStringLiteral("Unsaved group"));
	QVERIFY(rig.mutations().isEmpty());
}

// Проверяет, что две страницы сохраняют собственный ввод, включая ещё невалидные строки профиля.
void ConfigurationUiTest::separatePagesKeepDirtyFormsAcrossNavigation()
{
	// rig/window используют единое общее состояние и только петлевой сервер.
	ConfigurationRig rig;
	QVERIFY(rig.initialize());
	ManagementWindow window(rig.state, translatedKey, nullptr, &rig.connection, &rig.operations,
				&rig.configuration);
	// navigation/stack переключают самостоятельные разделы 2 и 3 без внутренних вкладок.
	auto *navigation = window.findChild<QListWidget *>();
	auto *stack = window.findChild<QStackedWidget *>();
	QVERIFY(navigation && stack);
	navigation->setCurrentRow(2);
	QVERIFY(rig.configuration.open(ConfigurationKind::Group, QStringLiteral("group"), true));
	QTRY_VERIFY_WITH_TIMEOUT(rig.state.configurationEditor().base.has_value(), 1000);
	// groupName содержит несохранённое имя группы, которое не должно очищаться при выборе профилей.
	auto *groupName = stack->widget(2)->findChild<QLineEdit *>(QStringLiteral("configurationName"));
	QVERIFY(groupName);
	groupName->selectAll();
	QTest::keyClicks(groupName, "Retained group");
	navigation->setCurrentRow(3);
	QCOMPARE(stack->currentIndex(), 3);
	QVERIFY(rig.configuration.open(ConfigurationKind::Profile, QStringLiteral("profile"), true));
	QTRY_VERIFY_WITH_TIMEOUT(rig.state.configurationEditor().base.has_value(), 1000);
	// profile/gop адресуют сырую форму: ошибочный ввод нельзя восстановить только из типизированного draft.
	auto *profile = stack->widget(3)->findChild<ProfileEditor *>();
	QVERIFY(profile);
	auto *gop = profile->findChild<QLineEdit *>(QStringLiteral("video.gop"));
	QVERIFY(gop);
	gop->selectAll();
	QTest::keyClicks(gop, "12x");
	navigation->setCurrentRow(2);
	QCOMPARE(stack->currentIndex(), 2);
	QCOMPARE(groupName->text(), QStringLiteral("Retained group"));
	QCOMPARE(std::get<Group>(*rig.state.configurationEditor().draft).name, QStringLiteral("Retained group"));
	navigation->setCurrentRow(3);
	QCOMPARE(stack->currentIndex(), 3);
	QCOMPARE(gop->text(), QStringLiteral("12x"));
	QVERIFY(rig.mutations().isEmpty());
}

// Обычное редактирование точного ключа и его очистка создают только соответствующий PATCH.
void ConfigurationUiTest::destinationSecretsEditAndClearThroughSingleFields()
{
	// rig/page открывают существующее назначение с серверным адресом и ключом.
	ConfigurationRig rig;
	QVERIFY(rig.initialize());
	ConfigurationEditor page(rig.state, rig.configuration, translatedKey, ConfigurationKind::Destination);
	QVERIFY(rig.configuration.open(ConfigurationKind::Destination, QStringLiteral("destination")));
	QTRY_VERIFY_WITH_TIMEOUT(rig.state.configurationEditor().base.has_value(), 1000);
	// keyField находится по значению серверного ключа, без зависимости от порядка полей карточки.
	SecretField *keyField = nullptr;
	// secret перебирает два публичных поля адреса/ключа и выбирает полученный ключ.
	for (auto *secret : page.findChildren<SecretField *>())
		if (secret->received().hasValue() && *secret->received() == QStringLiteral("key/%2F+="))
			keyField = secret;
	QVERIFY(keyField);
	// input/save представляют единственное редактируемое поле ключа и общую кнопку сохранения.
	auto *input = keyField->findChild<QLineEdit *>(QStringLiteral("secretReceived"));
	auto *save = page.findChild<QPushButton *>(QStringLiteral("configurationSave"));
	QVERIFY(input && save);
	QCOMPARE(keyField->findChildren<QLineEdit *>().size(), 1);
	input->selectAll();
	QTest::keyClicks(input, "  exact/+%2F  ");
	save->click();
	QTRY_COMPARE_WITH_TIMEOUT(rig.state.configurationEditor().outcome, ConfigurationOutcome::Saved, 1000);
	QCOMPARE(rig.mutations().size(), 1);
	QCOMPARE(QJsonDocument::fromJson(rig.mutations().first().body).object(),
		 (QJsonObject{{QStringLiteral("stream_key"), QStringLiteral("  exact/+%2F  ")},
			      {QStringLiteral("expected_revision"), QStringLiteral("1")}}));
	input->selectAll();
	QTest::keyClick(input, Qt::Key_Backspace);
	save->click();
	QTRY_COMPARE_WITH_TIMEOUT(rig.mutations().size(), 2, 1000);
	QTRY_VERIFY_WITH_TIMEOUT(!rig.state.configurationEditor().writing, 1000);
	QCOMPARE(QJsonDocument::fromJson(rig.mutations().last().body).object(),
		 (QJsonObject{{QStringLiteral("stream_key"), QString{}},
			      {QStringLiteral("expected_revision"), QStringLiteral("2")}}));
	QCOMPARE(rig.destination.value(QStringLiteral("endpoint")).toString(), fixture::endpoint());
}

// Пустой обязательный адрес отвергается локально; введённая пустота остаётся доступной для исправления.
void ConfigurationUiTest::clearingDestinationEndpointShowsLocalFieldError()
{
	// rig/page открывают адрес назначения без настоящего backend или изменения текущего эфира.
	ConfigurationRig rig;
	QVERIFY(rig.initialize());
	ConfigurationEditor page(rig.state, rig.configuration, translatedKey, ConfigurationKind::Destination);
	page.show();
	QVERIFY(rig.configuration.open(ConfigurationKind::Destination, QStringLiteral("destination")));
	QTRY_VERIFY_WITH_TIMEOUT(rig.state.configurationEditor().base.has_value(), 1000);
	// endpointField выбирается по полученному адресу, без зависимости от порядка секретных полей.
	SecretField *endpointField = nullptr;
	// secret проходит поля ключа и адреса этой карточки.
	for (auto *secret : page.findChildren<SecretField *>())
		if (secret->received().hasValue() && *secret->received() == fixture::endpoint())
			endpointField = secret;
	QVERIFY(endpointField);
	// input/save адресуют обычное редактирование и единственную кнопку сохранения карточки.
	auto *input = endpointField->findChild<QLineEdit *>(QStringLiteral("secretReceived"));
	auto *save = page.findChild<QPushButton *>(QStringLiteral("configurationSave"));
	QVERIFY(input && save);
	input->selectAll();
	QTest::keyClick(input, Qt::Key_Backspace);
	save->click();
	// error принадлежит строке обязательного адреса и объясняет отказ до передачи HTTP.
	auto *error = page.findChild<QLabel *>(QStringLiteral("configurationEndpointError"));
	QVERIFY(error);
	QVERIFY(error->isVisible());
	QCOMPARE(error->text(), QStringLiteral("Editor.EndpointRequired"));
	QVERIFY(input->text().isEmpty());
	QVERIFY(rig.mutations().isEmpty());
	QCOMPARE(rig.destination.value(QStringLiteral("endpoint")).toString(), fixture::endpoint());
}

// Проверяет выбор собственных назначений без сводки групп и зависимости от загрузки групп.
void ConfigurationUiTest::streamSelectionUsesOwnDestinationNamesWithoutGroupSummary()
{
	// rig/page предоставляют Stream и настоящее управление RAM-намерением выбора.
	ConfigurationRig rig;
	QVERIFY(rig.initialize());
	// translations предоставляет реальные пользовательские подписи карточки потока.
	QSettings translations(QStringLiteral(GOST_TEST_LOCALE_DIR) + QStringLiteral("/ru-RU.ini"),
			       QSettings::IniFormat);
	// Переводчик читает поставляемые подписи; key сохраняет точное имя запрошенного ресурса.
	ConfigurationEditor page(
		rig.state, rig.configuration,
		[&translations](const char *key) {
			return translations.value(QString::fromUtf8(key), QString::fromUtf8(key)).toString();
		},
		ConfigurationKind::Stream);
	QVERIFY(rig.configuration.open(ConfigurationKind::Stream, QStringLiteral("stream")));
	QTRY_VERIFY_WITH_TIMEOUT(rig.state.configurationEditor().base.has_value(), 1000);
	// config/own/second/foreign содержат два своих назначения и одно чужое с таким же именем.
	auto config = rig.state.configuration();
	auto own = *decode<Destination>(fixture::bytes(fixture::destination())).value;
	auto second = own;
	auto foreign = own;
	own.name = QStringLiteral("YouTube main");
	second.id = QStringLiteral("second-destination-id");
	second.name = QStringLiteral("Twitch extra");
	second.enabled = false;
	foreign.id = QStringLiteral("foreign-destination-id");
	foreign.streamId = QStringLiteral("other-stream");
	// group связывает оба своих назначения с понятным именем одной группы кодирования.
	auto group = *decode<Group>(fixture::bytes(fixture::group())).value;
	group.id = QStringLiteral("opaque-encoding-id");
	group.name = QStringLiteral("HD encoding group");
	own.groupId = group.id;
	second.groupId = group.id;
	config.destinations = {{own.id, own}, {second.id, second}, {foreign.id, foreign}};
	config.groups = {{group.id, group}};
	config.destinationsStatus = {RuntimeAvailability::Fresh, true, {}};
	config.groupsStatus = {RuntimeAvailability::Stale, false, {}};
	rig.state.setConfiguration(config);
	// choices содержит участие назначений; status показывает только реальную неполноту их списка.
	auto *choices = page.findChild<QListWidget *>(QStringLiteral("streamDestinations"));
	auto *status = page.findChild<QLabel *>(QStringLiteral("streamDestinationsStatus"));
	QVERIFY(choices && status);
	QVERIFY(!page.findChild<QLabel *>(QStringLiteral("streamSelectedGroups")));
	QVERIFY(status->isHidden());
	QCOMPARE(choices->count(), 2);
	// secondItem сохраняет адрес нужного элемента по ID, не по совпадающему имени.
	QListWidgetItem *secondItem = nullptr;
	// index/item последовательно проверяют каждую видимую строку и её точную связь.
	for (int index = 0; index < choices->count(); ++index) {
		auto *item = choices->item(index);
		QVERIFY(item->data(Qt::UserRole).toString() != foreign.id);
		QVERIFY(!item->text().contains(item->data(Qt::UserRole).toString()));
		if (item->data(Qt::UserRole).toString() == second.id)
			secondItem = item;
	}
	QVERIFY(secondItem);
	QCOMPARE(secondItem->checkState(), Qt::Unchecked);
	// label проверяет, что справочное название группы не попало в другое текстовое поле карточки.
	for (const auto *label : page.findChildren<QLabel *>())
		QVERIFY(!label->text().contains(group.name));
	secondItem->setCheckState(Qt::Checked);
	QCOMPARE(rig.state.configurationEditor().destinationChanges.value(second.id), true);
	QVERIFY(rig.mutations().isEmpty());
}

// Проверяет отсутствие отложенных действий заглушки при обеих поставляемых локализациях.
void ConfigurationUiTest::configurationPagesExcludeRuntimeControls_data()
{
	QTest::addColumn<QString>("locale");
	QTest::newRow("ru") << QStringLiteral("ru-RU");
	QTest::newRow("en") << QStringLiteral("en-US");
}

// Страницы конфигурации не дублируют управление эфиром даже при готовом заключении о заглушке.
void ConfigurationUiTest::configurationPagesExcludeRuntimeControls()
{
	// locale выбирает поставляемые подписи; rig изолирует контроллеры и HTTP внутри workspace.
	QFETCH(QString, locale);
	ConfigurationRig rig;
	QVERIFY(rig.initialize());
	// translations и text предоставляют реальные подписи без пользовательских настроек.
	QSettings translations(QStringLiteral(GOST_TEST_LOCALE_DIR) + QLatin1Char('/') + locale +
				       QStringLiteral(".ini"),
			       QSettings::IniFormat);
	const auto text = [&translations](const char *key) {
		return translations.value(QString::fromUtf8(key), QString::fromUtf8(key)).toString();
	};
	// window содержит настоящие редакторы; pages и navigation переключают разделы единого окна.
	ManagementWindow window(rig.state, text, nullptr, &rig.connection, &rig.operations, &rig.configuration);
	auto *pages = window.findChild<QStackedWidget *>(QStringLiteral("managementPages"));
	auto *navigation = window.findChild<QListWidget *>(QStringLiteral("managementNavigation"));
	QVERIFY(pages && navigation);
	window.show();
	QTRY_VERIFY_WITH_TIMEOUT(
		rig.state.configurationEditor().draft.has_value() && !rig.state.configurationEditor().loading, 1000);
	QVERIFY(pages->widget(0)->findChild<ConfigurationEditor *>());
	navigation->setCurrentRow(1);
	QTRY_COMPARE_WITH_TIMEOUT(pages->currentIndex(), 1, 1000);
	QVERIFY(pages->widget(1)->findChild<ConfigurationEditor *>());
	QVERIFY(!window.findChild<QGroupBox *>(QStringLiteral("outputFallbackControls")));
	QVERIFY(!window.findChild<QPushButton *>(QStringLiteral("fallbackCheckButton")));
	QVERIFY(!window.findChild<QPushButton *>(QStringLiteral("fallbackStartButton")));
	QVERIFY(!window.findChild<QLabel *>(QStringLiteral("fallbackPrecheckStatus")));
	QVERIFY(!window.findChild<QLabel *>(QStringLiteral("fallbackDestinations")));
	QVERIFY(!window.findChild<QPushButton *>(QStringLiteral("outputDetachButton")));
	QVERIFY(!window.findChild<QPushButton *>(QStringLiteral("outputStartButton")));
	QVERIFY(!window.findChild<QPushButton *>(QStringLiteral("outputStopButton")));
	QVERIFY(!window.findChild<QPushButton *>(QStringLiteral("outputRestartButton")));
	QVERIFY(!window.findChild<QComboBox *>(QStringLiteral("currentOutputs")));
	QVERIFY(!window.findChild<QLabel *>(QStringLiteral("selectedOutputStatus")));
	QVERIFY(!window.findChild<QLabel *>(QStringLiteral("outputOperationStatus")));
	// button проверяет отсутствие отдельного прикрепления и в дочерних карточках.
	for (const auto *button : window.findChildren<QPushButton *>())
		QVERIFY(button->text() != text("Operation.AddOutput"));
	// operations моделирует уже полученное заключение без команды запуска и реального эфира.
	auto operations = rig.state.operations();
	operations.fallback.context = rig.state.dockContext();
	operations.fallback.readiness = FallbackReadiness::Ready;
	operations.fallback.destinations = {{QStringLiteral("destination"), QStringLiteral("YouTube")}};
	rig.state.setOperations(operations);
	QVERIFY(!window.findChild<QGroupBox *>(QStringLiteral("outputFallbackControls")));
	QVERIFY(!window.findChild<QPushButton *>(QStringLiteral("fallbackStartButton")));
	QVERIFY(rig.mutations().isEmpty());
}

// Переходит от несохранённой группы к назначению из Dock без команд текущему эфиру.
void ConfigurationUiTest::dockDetailsOpenSavedDestinationAndPreserveDraft()
{
	// rig предоставляет рабочий контекст и серверные карточки только внутри теста.
	ConfigurationRig rig;
	QVERIFY(rig.initialize());
	QVERIFY(rig.configuration.open(ConfigurationKind::Group, QStringLiteral("group")));
	QTRY_VERIFY_WITH_TIMEOUT(rig.state.configurationEditor().base.has_value(), 1000);
	// draft хранит прежний ввод группы, который переход к выходу не должен терять.
	auto draft = std::get<Group>(*rig.state.configurationEditor().draft);
	draft.name = QStringLiteral("Unsaved group name");
	QVERIFY(rig.configuration.setDraft(draft, {QStringLiteral("name")}));
	// metadata связывает точный Output текущего эфира с сохранённым назначением.
	DockServerState metadata;
	metadata.session = DockSession{QStringLiteral("session"), QStringLiteral("RUNNING"), false};
	metadata.sessionStatus = {RuntimeAvailability::Fresh, true, {}};
	metadata.outputsStatus = {RuntimeAvailability::Fresh, true, {}};
	metadata.outputs.insert(QStringLiteral("output"),
				{QStringLiteral("Output snapshot"), true, true, QStringLiteral("destination")});
	rig.state.setDockServer(metadata);
	// window начинает с открытой группы; pages и navigation отражают переход между существующими редакторами.
	ManagementWindow window(rig.state, translatedKey, nullptr, &rig.connection, &rig.operations,
				&rig.configuration);
	auto *pages = window.findChild<QStackedWidget *>(QStringLiteral("managementPages"));
	auto *navigation = window.findChild<QListWidget *>(QStringLiteral("managementNavigation"));
	QVERIFY(pages && navigation);
	window.show();
	QTRY_VERIFY_WITH_TIMEOUT(!rig.state.configurationEditor().loading, 1000);
	QCOMPARE(pages->currentIndex(), 2);
	window.selectOutput(rig.state.dockContext(), QStringLiteral("session"), QStringLiteral("output"));
	QTRY_VERIFY_WITH_TIMEOUT(!rig.state.configurationEditor().loading &&
					 rig.state.configurationEditor().id == QStringLiteral("destination"),
				 1000);
	QCOMPARE(pages->currentIndex(), 1);
	// destinationEditor и name подтверждают видимую карточку, а не только значение в контроллере.
	auto *destinationEditor = pages->widget(1)->findChild<ConfigurationEditor *>();
	QVERIFY(destinationEditor);
	auto *name = destinationEditor->findChild<QLineEdit *>(QStringLiteral("configurationName"));
	QVERIFY(name && name->isVisible());
	QCOMPARE(destinationEditor->selectedDestinationId(), QStringLiteral("destination"));
	QCOMPARE(name->text(), rig.destination.value(QStringLiteral("name")).toString());
	navigation->setCurrentRow(2);
	QTRY_COMPARE_WITH_TIMEOUT(rig.state.configurationEditor().kind, ConfigurationKind::Group, 1000);
	QCOMPARE(std::get<Group>(*rig.state.configurationEditor().draft).name, draft.name);
	window.selectOutput(rig.state.dockContext(), QStringLiteral("previous-session"), QStringLiteral("output"));
	QCOMPARE(pages->currentIndex(), 2);
	QCOMPARE(std::get<Group>(*rig.state.configurationEditor().draft).name, draft.name);
	// staleContext моделирует ссылку Dock, полученную до смены рабочего потока.
	auto staleContext = rig.state.dockContext();
	staleContext.streamId = QStringLiteral("other-stream");
	window.selectOutput(staleContext, QStringLiteral("session"), QStringLiteral("output"));
	QCOMPARE(pages->currentIndex(), 2);
	QCOMPARE(rig.state.dockServer().session->id, QStringLiteral("session"));
	QVERIFY(rig.state.dockServer().outputs.value(QStringLiteral("output")).attached);
	QVERIFY(rig.mutations().isEmpty());
}

// Проверяет сохранение произвольной существующей площадки и подтверждённое соответствие имён API.
void ConfigurationUiTest::platformChoicesPreserveUnknownService()
{
	// rig/page содержат старое назначение сервиса, отсутствующего в трёх предлагаемых вариантах.
	ConfigurationRig rig;
	QVERIFY(rig.initialize());
	rig.destination.insert(QStringLiteral("platform"), QStringLiteral("legacy-service"));
	ConfigurationEditor page(rig.state, rig.configuration, translatedKey, ConfigurationKind::Destination);
	QVERIFY(rig.configuration.open(ConfigurationKind::Destination, QStringLiteral("destination")));
	QTRY_VERIFY_WITH_TIMEOUT(rig.state.configurationEditor().base.has_value(), 1000);
	// platform сохраняет исходный технический токен до явного выбора пользователя.
	auto *platform = page.findChild<QComboBox *>(QStringLiteral("configurationPlatform"));
	QVERIFY(platform && !platform->isEditable());
	QCOMPARE(platform->currentData().toString(), QStringLiteral("legacy-service"));
	QCOMPARE(std::get<Destination>(*rig.state.configurationEditor().draft).platform,
		 QStringLiteral("legacy-service"));
	QVERIFY(rig.state.configurationEditor().dirtyFields.isEmpty());
	QVERIFY(platform->findData(QStringLiteral("youtube")) >= 0);
	QVERIFY(platform->findData(QStringLiteral("twitch")) >= 0);
	QVERIFY(platform->findData(QStringLiteral("vk")) >= 0);
	QVERIFY(choose(platform, QStringLiteral("vk")));
	QCOMPARE(std::get<Destination>(*rig.state.configurationEditor().draft).platform, QStringLiteral("vk"));
	QVERIFY(rig.mutations().isEmpty());
}

void ConfigurationUiTest::destinationCardSavesNameWithoutReplacingSecrets()
{
	// rig/page предоставляют настоящую карточку Destination поверх независимого configuration API.
	ConfigurationRig rig;
	QVERIFY(rig.initialize());
	// metadata моделирует работающий эфир: сохранение карточки не должно посылать ему start/stop/add-output.
	DockServerState metadata;
	metadata.session = DockSession{QStringLiteral("session"), QStringLiteral("RUNNING"), false};
	metadata.sessionStatus = {RuntimeAvailability::Fresh, true, {}};
	metadata.outputsStatus = {RuntimeAvailability::Fresh, true, {}};
	metadata.outputs.insert(QStringLiteral("output"),
				{QStringLiteral("Running output"), true, true, QStringLiteral("destination")});
	rig.state.setDockServer(metadata);
	ConfigurationEditor page(rig.state, rig.configuration, translatedKey, ConfigurationKind::Destination);
	page.activate();
	QVERIFY(rig.configuration.open(ConfigurationKind::Destination, QStringLiteral("destination")));
	QTRY_VERIFY_WITH_TIMEOUT(rig.state.configurationEditor().base.has_value(), 1000);
	// name/save моделируют пользовательский ввод и явную отправку формы.
	auto *name = page.findChild<QLineEdit *>(QStringLiteral("configurationName"));
	auto *save = page.findChild<QPushButton *>(QStringLiteral("configurationSave"));
	QVERIFY(name && save);
	name->selectAll();
	QTest::keyClicks(name, "Renamed destination");
	QVERIFY(save->isEnabled());
	save->click();
	QTRY_COMPARE_WITH_TIMEOUT(rig.state.configurationEditor().outcome, ConfigurationOutcome::Saved, 1000);
	QCOMPARE(rig.mutations().size(), 1);
	QCOMPARE(QJsonDocument::fromJson(rig.mutations().first().body).object(),
		 QJsonObject({{QStringLiteral("expected_revision"), QStringLiteral("1")},
			      {QStringLiteral("name"), QStringLiteral("Renamed destination")}}));
	QCOMPARE(rig.state.selectedStreamId(), QStringLiteral("stream"));
	QCOMPARE(page.selectedDestinationId(), QStringLiteral("destination"));
}
void ConfigurationUiTest::relationChoicesKeepExactIdentifiersAndUnknownReferences()
{
	// rig/config дают две одноимённые группы разных потоков и ссылку на пока неизвестную группу.
	ConfigurationRig rig;
	QVERIFY(rig.initialize());
	rig.destination.insert(QStringLiteral("group_id"), QStringLiteral("unknown/group"));
	ConfigurationEditor page(rig.state, rig.configuration, translatedKey, ConfigurationKind::Destination);
	QVERIFY(rig.configuration.open(ConfigurationKind::Destination, QStringLiteral("destination")));
	QTRY_VERIFY_WITH_TIMEOUT(rig.state.configurationEditor().base.has_value(), 1000);
	// config/own/foreign намеренно имеют одинаковые имена, чтобы выбор по подписи оказался ошибкой.
	auto config = rig.state.configuration();
	auto own = *decode<Group>(fixture::bytes(fixture::group())).value;
	auto foreign = own;
	own.id = QStringLiteral("exact/group");
	foreign.id = QStringLiteral("foreign/group");
	foreign.streamId = QStringLiteral("foreign-stream");
	config.groups = {{own.id, own}, {foreign.id, foreign}};
	config.groupsStatus = {RuntimeAvailability::Fresh, false, {}};
	rig.state.setConfiguration(config);
	// groups отражает точный ID неподтверждённой связи и исключает группу другого Stream.
	auto *groups = page.findChild<QComboBox *>(QStringLiteral("configurationGroup"));
	QVERIFY(groups);
	QCOMPARE(groups->currentData().toString(), QStringLiteral("unknown/group"));
	QVERIFY(groups->findData(own.id) >= 0);
	QCOMPARE(groups->findData(foreign.id), -1);
	QVERIFY(choose(groups, own.id));
	QCOMPARE(std::get<Destination>(*rig.state.configurationEditor().draft).groupId, own.id);
	QCOMPARE(rig.state.selectedStreamId(), QStringLiteral("stream"));
}
void ConfigurationUiTest::dirtyNavigationRequiresDecisionAndPageRemasksSecrets()
{
	// rig/page открывают Stream с постоянным серверным ключом и несохранённым именем.
	ConfigurationRig rig;
	QVERIFY(rig.initialize());
	ConfigurationEditor page(rig.state, rig.configuration, translatedKey, ConfigurationKind::Stream);
	QVERIFY(rig.configuration.open(ConfigurationKind::Stream, QStringLiteral("stream")));
	QTRY_VERIFY_WITH_TIMEOUT(rig.state.configurationEditor().base.has_value(), 1000);
	// name/eye/view — обычный ввод и независимые действия карточки ключа.
	auto *name = page.findChild<QLineEdit *>(QStringLiteral("configurationName"));
	auto *eye = page.findChild<QToolButton *>(QStringLiteral("secretEye"));
	auto *view = page.findChild<QLineEdit *>(QStringLiteral("secretReceived"));
	QVERIFY(name && eye && view);
	eye->click();
	QCOMPARE(view->text(), rig.stream.value(QStringLiteral("publication_key")).toString());
	name->selectAll();
	QTest::keyClicks(name, "Unsaved name");
	// Одноразовый callback отвечает No только модальному подтверждению этой страницы.
	QTimer::singleShot(0, &page, [&page] {
		// dialog адресует только подтверждение ухода с этой dirty страницы.
		if (auto *dialog = page.findChild<QMessageBox *>())
			dialog->done(QMessageBox::No);
	});
	QVERIFY(!page.canLeave());
	QCOMPARE(std::get<Stream>(*rig.state.configurationEditor().draft).name, QStringLiteral("Unsaved name"));
	page.hideSecrets();
	QCOMPARE(view->echoMode(), QLineEdit::Password);
	QCOMPARE(name->text(), QStringLiteral("Unsaved name"));
	QVERIFY(rig.mutations().isEmpty());
}

void ConfigurationUiTest::clientLogsExcludeServerSecretsInBothModes_data()
{
	QTest::addColumn<bool>("detailed");
	QTest::newRow("normal") << false;
	QTest::newRow("detailed") << true;
}
void ConfigurationUiTest::clientLogsExcludeServerSecretsInBothModes()
{
	// detailed выбирает normal или detailed без изменения требований к исключению секретов.
	QFETCH(bool, detailed);
	// temp/file изолируют preferences настоящего Runtime, logs принимает только его итоговые записи.
	WorkspaceTemp temp(QStringLiteral("safe-client-log"));
	QVERIFY(temp.isValid());
	const auto file = temp.filePath(QStringLiteral("settings.json"));
	QStringList logs;
	{
		// preferences отключает автоматическую сеть и задаёт проверяемый режим до загрузки Runtime.
		SettingsStore preferences(file);
		preferences.setAutoConnect(false);
		preferences.setDetailedLogging(detailed);
		QVERIFY(preferences.save());
	}
	// server обслуживает только адресные GET/PATCH, возвращая синтетические секреты в разрешённых DTO.
	HttpServer server;
	QVERIFY(server.listen());
	server.handler = [](const HttpServer::Request &request) {
		if (request.method == "PATCH") {
			HttpServer::reply(
				request,
				R"({"status":422,"title":"server-raw-secret","detail":"synthetic-private-payload","errors":[{"location":"body.name","message":"server-raw-secret","value":"clipboard-secret"}]})",
				422, "application/problem+json");
		} else if (request.target.startsWith("/api/v1/destinations/")) {
			HttpServer::reply(request, fixture::bytes(fixture::destination()));
		} else {
			HttpServer::reply(request, "{}", 404);
		}
	};
	// frontend/lifecycle создают реальный Runtime; sink захватывает подготовленные сообщения без OBS-зависимости.
	FakeFrontend frontend;
	PluginLifecycle lifecycle(frontend, translatedKey,
				  {QStringLiteral("test"), file, [&](LogSeverity, const QString &message) {
					   logs.append(message);
				   }});
	QVERIFY(lifecycle.load());
	// state/controller доступны только при живом Runtime; UUID подтверждён без пользовательского OBS.
	auto *state = lifecycle.stateStore();
	auto *controller = lifecycle.configurationController();
	QVERIFY(state && controller);
	state->setConnection(server.origin().toString(), ConnectionStatus::Connected);
	state->setSelection({}, true, {}, {}, QStringLiteral("profile-uuid"));
	QVERIFY(controller->open(ConfigurationKind::Destination, QStringLiteral("destination")));
	QTRY_VERIFY_WITH_TIMEOUT(state->configurationEditor().base.has_value(), 1000);
	// successfulLogs отделяет успешный GET: normal не пишет его технические сведения, detailed пишет.
	const auto successfulLogs = logs.join(QLatin1Char('\n'));
	QCOMPARE(successfulLogs.contains(QStringLiteral("REST GET")), detailed);
	// draft содержит новое пользовательское имя; сервер возвращает ошибку с намеренно секретными текстами.
	auto draft = std::get<Destination>(*state->configurationEditor().draft);
	draft.name = QStringLiteral("private-user-payload");
	QVERIFY(controller->setDraft(draft, {QStringLiteral("name")}));
	QVERIFY(controller->save());
	QTRY_COMPARE_WITH_TIMEOUT(state->configurationEditor().outcome, ConfigurationOutcome::Rejected, 1000);
	// combined проверяется после явного shutdown, включая сообщения teardown и конечный отказ REST.
	lifecycle.shutdown();
	const auto combined = logs.join(QLatin1Char('\n'));
	QVERIFY(!combined.isEmpty());
	QVERIFY(combined.contains(QStringLiteral("422")));
	// secret перечисляет независимые запрещённые источники: ввод, DTO и полученные ошибки.
	for (const auto &secret : {QStringLiteral("private-user-payload"), fixture::endpoint(),
				   QStringLiteral("key/%2F+="), QStringLiteral("server-raw-secret"),
				   QStringLiteral("synthetic-private-payload"), QStringLiteral("clipboard-secret")})
		QVERIFY2(!combined.contains(secret), "Секретное значение попало в штатный журнал клиента.");
}

void ConfigurationUiTest::explicitDiscardAfterProfileChangeAllowsCurrentContext_data()
{
	QTest::addColumn<bool>("creating");
	QTest::newRow("open-current-card") << false;
	QTest::newRow("create-in-current-context") << true;
}
void ConfigurationUiTest::explicitDiscardAfterProfileChangeAllowsCurrentContext()
{
	// creating выбирает новую форму либо открытие карточки после подтверждённого отказа от прежнего draft.
	QFETCH(bool, creating);
	// rig/page адресуют Stream старого UUID до явного изменения профиля.
	ConfigurationRig rig;
	QVERIFY(rig.initialize());
	ConfigurationEditor page(rig.state, rig.configuration, translatedKey, ConfigurationKind::Stream);
	QVERIFY(rig.configuration.open(ConfigurationKind::Stream, QStringLiteral("stream")));
	QTRY_VERIFY_WITH_TIMEOUT(rig.state.configurationEditor().base.has_value(), 1000);
	// name содержит несохранённый пользовательский ввод, который нельзя отправлять от нового UUID.
	auto *name = page.findChild<QLineEdit *>(QStringLiteral("configurationName"));
	QVERIFY(name);
	name->selectAll();
	QTest::keyClicks(name, "old-profile-draft");
	rig.state.setSelection(QStringLiteral("stream"), true, {}, {}, QStringLiteral("current-profile"));
	QVERIFY(!rig.configuration.save());
	// answered доказывает явное решение пользователя; callback отвечает только диалогу этой страницы.
	bool answered = false;
	QTimer::singleShot(0, &page, [&] {
		// dialog содержит подтверждение явного отказа от черновика прежнего UUID.
		if (auto *dialog = page.findChild<QMessageBox *>()) {
			answered = true;
			dialog->done(QMessageBox::Yes);
		}
	});
	QVERIFY(page.canLeave());
	QVERIFY(answered);
	if (creating) {
		QVERIFY(rig.configuration.beginCreate(ConfigurationKind::Group, true));
		QVERIFY(rig.state.configurationEditor().creating);
		QCOMPARE(rig.state.configurationEditor().kind, ConfigurationKind::Group);
	} else {
		QVERIFY(rig.configuration.open(ConfigurationKind::Group, QStringLiteral("group"), true));
		QTRY_VERIFY_WITH_TIMEOUT(rig.state.configurationEditor().base.has_value(), 1000);
		QCOMPARE(rig.state.configurationEditor().id, QStringLiteral("group"));
	}
	QCOMPARE(rig.state.configurationEditor().context.profileUuid, QStringLiteral("current-profile"));
	QVERIFY(rig.state.configurationEditor().dirtyFields.isEmpty());
	QVERIFY(rig.mutations().isEmpty());
}

void ConfigurationUiTest::unknownDeleteRequiresReconciliationBeforeReplacingEditor()
{
	// rig/page открывают чистую Group: отсутствие dirtyFields не отменяет неизвестность отправленного DELETE.
	ConfigurationRig rig;
	QVERIFY(rig.initialize());
	ConfigurationEditor page(rig.state, rig.configuration, translatedKey, ConfigurationKind::Group);
	QVERIFY(rig.configuration.open(ConfigurationKind::Group, QStringLiteral("group")));
	QTRY_VERIFY_WITH_TIMEOUT(rig.state.configurationEditor().base.has_value(), 1000);
	QVERIFY(rig.state.configurationEditor().dirtyFields.isEmpty());
	// config/other сохраняют вторую запись для проверки адресного удаления только отсутствующей Group из кеша.
	auto config = rig.state.configuration();
	auto other = config.groups.value(QStringLiteral("group"));
	other.id = QStringLiteral("other-group");
	config.groups.insert(other.id, other);
	rig.state.setConfiguration(config);
	rig.intercept = [&](const HttpServer::Request &request) {
		if (request.method == "DELETE") {
			if (request.socket)
				request.socket->abort();
			return true;
		}
		if (request.method == "GET" && request.target == "/api/v1/transcode-groups/group") {
			HttpServer::reply(request, R"({"status":404,"title":"Not found"})", 404,
					  "application/problem+json");
			return true;
		}
		return false;
	};
	QVERIFY(rig.configuration.remove());
	QTRY_COMPARE_WITH_TIMEOUT(rig.state.configurationEditor().outcome, ConfigurationOutcome::Unknown, 1000);
	QVERIFY(rig.state.configurationEditor().reconciliationRequired);
	QVERIFY(rig.state.configurationEditor().dirtyFields.isEmpty());
	QVERIFY(!rig.configuration.open(ConfigurationKind::Stream, QStringLiteral("stream"), true));
	QVERIFY(!rig.configuration.beginCreate(ConfigurationKind::Group, true));
	// create адресует обычную кнопку страницы; её явное нажатие не должно сбросить неизвестный исход.
	QPushButton *create = nullptr;
	// button перебирает действия карточки, выбирая локализованное действие создания.
	for (auto *button : page.findChildren<QPushButton *>())
		if (button->text() == QStringLiteral("Editor.Create"))
			create = button;
	QVERIFY(create);
	create->click();
	QCOMPARE(rig.state.configurationEditor().id, QStringLiteral("group"));
	QCOMPARE(rig.state.configurationEditor().outcome, ConfigurationOutcome::Unknown);
	QVERIFY(rig.state.configurationEditor().reconciliationRequired);
	QCOMPARE(rig.mutations().size(), 1);
	QVERIFY(rig.configuration.reload());
	QTRY_VERIFY_WITH_TIMEOUT(!rig.state.configurationEditor().loading &&
					 !rig.state.configurationEditor().reconciliationRequired,
				 1000);
	QCOMPARE(rig.state.configurationEditor().outcome, ConfigurationOutcome::Unknown);
	QCOMPARE(std::get<Group>(*rig.state.configurationEditor().draft).id, QStringLiteral("group"));
	QVERIFY(!rig.state.configuration().groups.contains(QStringLiteral("group")));
	QVERIFY(rig.state.configuration().groups.contains(QStringLiteral("other-group")));
	create->click();
	QVERIFY(rig.state.configurationEditor().creating);
	QVERIFY(rig.state.configurationEditor().id.isEmpty());
	QCOMPARE(rig.mutations().size(), 1);
}

void ConfigurationUiTest::reopeningManagementAfterProfileChangeSurvivesExitInsideConfirmation()
{
	// temp/file изолируют preferences Runtime; settings отключает автоматическое подключение к внешнему серверу.
	WorkspaceTemp temp(QStringLiteral("management-reopen-teardown"));
	QVERIFY(temp.isValid());
	const auto file = temp.filePath(QStringLiteral("settings.json"));
	SettingsStore settings(file);
	settings.setAutoConnect(false);
	QVERIFY(settings.save());
	// server принимает лишь локальные чтения, начатые показом конфигурационных страниц.
	HttpServer server;
	QVERIFY(server.listen());
	server.handler = [](const HttpServer::Request &request) {
		HttpServer::reply(request, fixture::page({}));
	};
	// frontend/lifecycle создают настоящее окно и Runtime, которые EXIT обязан уничтожить синхронно.
	FakeFrontend frontend;
	PluginLifecycle lifecycle(frontend, translatedKey, {QStringLiteral("test"), file, {}});
	QVERIFY(lifecycle.load());
	// state/controller получают подтверждённый origin и UUID без обращения к пользовательскому OBS.
	auto *state = lifecycle.stateStore();
	auto *controller = lifecycle.configurationController();
	QVERIFY(state && controller);
	state->setConnection(server.origin().toString(), ConnectionStatus::Connected);
	state->setSelection({}, true, {}, {}, QStringLiteral("old-profile"));
	lifecycle.openManagementWindow();
	// window наблюдает повторно используемое окно, принадлежащее тестовому главному окну OBS.
	QPointer<ManagementWindow> window = frontend.host->findChild<ManagementWindow *>();
	QVERIFY(window);
	QVERIFY(controller->beginCreate(ConfigurationKind::Stream, true));
	// draft сначала сохраняет ввод прежнего профиля, затем получает отдельное явное изменение нынешней карточки.
	auto draft = std::get<Stream>(*state->configurationEditor().draft);
	draft.name = QStringLiteral("unsaved-old-profile");
	draft.publicationKey = {FieldState::Value, QStringLiteral("explicit-key")};
	QVERIFY(controller->setDraft(draft, {QStringLiteral("name"), QStringLiteral("publication_key")}));
	window->hide();
	state->setSelection({}, true, {}, {}, QStringLiteral("current-profile"));
	lifecycle.openManagementWindow();
	QTRY_COMPARE_WITH_TIMEOUT(state->configurationEditor().context.profileUuid, QStringLiteral("current-profile"),
				  1000);
	QVERIFY(state->configurationEditor().draft.has_value());
	QVERIFY(state->configurationEditor().dirtyFields.isEmpty());
	QVERIFY(std::get<Stream>(*state->configurationEditor().draft).name != QStringLiteral("unsaved-old-profile"));
	QVERIFY(!std::get<Stream>(*state->configurationEditor().draft).publicationKey.hasValue());
	// request исключает запись старого контекста при безопасном повторном открытии окна.
	for (const auto &request : server.requests)
		QCOMPARE(request.method, QByteArray("GET"));
	draft = std::get<Stream>(*state->configurationEditor().draft);
	draft.name = QStringLiteral("unsaved-current-profile");
	draft.publicationKey = {FieldState::Value, QStringLiteral("current-key")};
	QVERIFY(controller->setDraft(draft, {QStringLiteral("name"), QStringLiteral("publication_key")}));
	// streamPage адресует карточку потока, где явный отказ от текущего ввода открывает настоящее подтверждение.
	ConfigurationEditor *streamPage = nullptr;
	// editor перебирает страницы; только Stream предоставляет действие генерации publication_key.
	for (auto *editor : window->findChildren<ConfigurationEditor *>())
		if (editor->findChild<QPushButton *>(QStringLiteral("generatePublicationKey")))
			streamPage = editor;
	QVERIFY(streamPage);
	// exitedInsideConfirmation доказывает EXIT из настоящего exec независимо от синхронного или отложенного показа.
	bool exitedInsideConfirmation = false;
	// teardown ждёт появления диалога до ограниченного QTRY; уничтожение таймера отменяет callback при раннем отказе.
	QTimer teardown;
	teardown.setInterval(0);
	connect(&teardown, &QTimer::timeout, &teardown, [&] {
		if (window && window->findChild<QMessageBox *>()) {
			teardown.stop();
			exitedInsideConfirmation = true;
			frontend.emitEvent(FrontendEvent::Exit);
		}
	});
	teardown.start(0);
	QVERIFY(!streamPage->canLeave(false));
	QVERIFY(exitedInsideConfirmation);
	QVERIFY(window.isNull());
	QVERIFY(!lifecycle.stateStore());
	QCOMPARE(frontend.callsAfterExit, 0);
	// request перебирает все реальные запросы, исключая скрытую отправку старого пользовательского ввода.
	for (const auto &request : server.requests)
		QCOMPARE(request.method, QByteArray("GET"));
}

void ConfigurationUiTest::failedFirstDetailReadKeepsVisibleErrorAndReload_data()
{
	QTest::addColumn<int>("status");
	QTest::newRow("service-unavailable") << 503;
	QTest::newRow("not-found") << 404;
	QTest::newRow("timeout") << 0;
}
void ConfigurationUiTest::failedFirstDetailReadKeepsVisibleErrorAndReload()
{
	// status выбирает серверный отказ; ноль означает отсутствие ответа до ограниченного timeout клиента.
	QFETCH(int, status);
	// rig/page открывают адресованную карточку без прежнего DTO, которым можно было бы скрыть ошибку.
	ConfigurationRig rig;
	QVERIFY(rig.initialize());
	ConfigurationEditor page(rig.state, rig.configuration, translatedKey, ConfigurationKind::Destination);
	page.show();
	// detailReads считает только точные GET карточки, чтобы выявить скрытые повторы после первого отказа.
	int detailReads = 0;
	rig.intercept = [&](const HttpServer::Request &request) {
		if (request.method != "GET" || request.target != "/api/v1/destinations/destination")
			return false;
		++detailReads;
		if (detailReads != 1)
			return false;
		if (status)
			HttpServer::reply(request, "{}", status, "application/problem+json");
		return true;
	};
	QVERIFY(rig.configuration.open(ConfigurationKind::Destination, QStringLiteral("destination")));
	// message и reload остаются частью адресованной карточки независимо от отсутствующего editable draft.
	auto *message = page.findChild<QLabel *>(QStringLiteral("configurationMessage"));
	QPushButton *reload = nullptr;
	// button выбирает явное действие перечитывания по его локализованной подписи.
	for (auto *button : page.findChildren<QPushButton *>())
		if (button->text() == QStringLiteral("Editor.Reload"))
			reload = button;
	QVERIFY(message && reload);
	QVERIFY(!reload->icon().isNull());
	QTRY_VERIFY_WITH_TIMEOUT(!rig.state.configurationEditor().loading &&
					 rig.state.configurationEditor().messageKey ==
						 QStringLiteral("Configuration.ReadFailed"),
				 1000);
	QVERIFY(!rig.state.configurationEditor().draft.has_value());
	QCOMPARE(rig.state.configurationEditor().id, QStringLiteral("destination"));
	QCOMPARE(message->text(), QStringLiteral("Configuration.ReadFailed"));
	QVERIFY(message->isVisible());
	QVERIFY(reload->isEnabled());
	QCOMPARE(detailReads, 1);
	QVERIFY(rig.mutations().isEmpty());
	reload->click();
	QTRY_VERIFY_WITH_TIMEOUT(
		rig.state.configurationEditor().base.has_value() && !rig.state.configurationEditor().loading, 1000);
	QCOMPARE(detailReads, 2);
	QCOMPARE(std::get<Destination>(*rig.state.configurationEditor().base).id, QStringLiteral("destination"));
	QVERIFY(!message->text().contains(QStringLiteral("Configuration.ReadFailed")));
	QCOMPARE(page.findChild<QLineEdit *>(QStringLiteral("configurationName"))->text(),
		 rig.destination.value(QStringLiteral("name")).toString());
	QVERIFY(rig.mutations().isEmpty());
}

void ConfigurationUiTest::deletingSavedGroupShowsDeletedInsteadOfPreviousSavedMessage()
{
	// rig/page сначала сохраняют Group, чтобы старое сообщение Saved действительно существовало до DELETE.
	ConfigurationRig rig;
	QVERIFY(rig.initialize());
	ConfigurationEditor page(rig.state, rig.configuration, translatedKey, ConfigurationKind::Group);
	QVERIFY(rig.configuration.open(ConfigurationKind::Group, QStringLiteral("group")));
	QTRY_VERIFY_WITH_TIMEOUT(rig.state.configurationEditor().base.has_value(), 1000);
	// name/save/message адресуют пользовательский ввод, действие и inline-результат карточки.
	auto *name = page.findChild<QLineEdit *>(QStringLiteral("configurationName"));
	auto *save = page.findChild<QPushButton *>(QStringLiteral("configurationSave"));
	auto *message = page.findChild<QLabel *>(QStringLiteral("configurationMessage"));
	QVERIFY(name && save && message);
	name->selectAll();
	QTest::keyClicks(name, "Saved group");
	save->click();
	QTRY_COMPARE_WITH_TIMEOUT(rig.state.configurationEditor().outcome, ConfigurationOutcome::Saved, 1000);
	QCOMPARE(message->text(), QStringLiteral("Configuration.Saved"));
	// deleted отражает серверное удаление: последующий список не возвращает уже удалённую Group.
	bool deleted = false;
	rig.intercept = [&](const HttpServer::Request &request) {
		if (request.method == "DELETE") {
			deleted = true;
			HttpServer::reply(request, {}, 204, {});
			return true;
		}
		if (deleted && request.method == "GET" && request.target.startsWith("/api/v1/transcode-groups?")) {
			HttpServer::reply(request, fixture::page({}));
			return true;
		}
		return false;
	};
	QVERIFY(rig.configuration.remove());
	QTRY_COMPARE_WITH_TIMEOUT(rig.state.configurationEditor().outcome, ConfigurationOutcome::Deleted, 1000);
	QVERIFY(!rig.state.configurationEditor().draft.has_value());
	QCOMPARE(message->text(), QStringLiteral("Configuration.Deleted"));
	QVERIFY(!message->text().contains(QStringLiteral("Configuration.Saved")));
	QCOMPARE(rig.state.selectedStreamId(), QStringLiteral("stream"));
	QCOMPARE(rig.mutations().size(), 2);
}

QTEST_MAIN(ConfigurationUiTest)
#include "configuration-ui-test.moc"
