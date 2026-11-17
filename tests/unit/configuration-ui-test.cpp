#include "api/json-decoder.hpp"
#include "api/json-encoder.hpp"
#include "fixtures/api-json.hpp"
#include "fixtures/configuration-rig.hpp"
#include "core/plugin-lifecycle.hpp"
#include "ui/configuration-editor.hpp"
#include "ui/management-window.hpp"
#include "ui/profile-editor.hpp"
#include "ui/secret-field.hpp"
#include "ui/operation-error-dialog.hpp"
#include <QApplication>
#include <QCheckBox>
#include <QClipboard>
#include <QComboBox>
#include <QFile>
#include <QGroupBox>
#include <QHelpEvent>
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
	// data сохраняет тип технического значения: ProfileText использует точные UTF-8 байты,
	// остальные конечные поля — QString. Подпись пункта в сравнении не участвует.
	const QVariant data = field->currentData().metaType() == QMetaType::fromType<QByteArray>()
				      ? QVariant(value.toUtf8())
				      : QVariant(value);
	// index адресует точное типизированное значение и не допускает выбора отключённого исходного пункта.
	const int index = field->findData(data);
	if (index < 0 || !(field->model()->flags(field->model()->index(index, 0)) & Qt::ItemIsEnabled))
		return false;
	field->setCurrentIndex(index);
	return QMetaObject::invokeMethod(field, "activated", Q_ARG(int, index));
}
// Накладывает patch на независимый документ base; вложенные объекты дополняются без изменения остальных полей.
QJsonObject mergeProfileFields(QJsonObject base, const QJsonObject &patch)
{
	// field указывает на одно явно заданное поле сценария и его ожидаемое значение.
	for (auto field = patch.begin(); field != patch.end(); ++field) {
		if (field.value().isObject())
			base.insert(field.key(),
				    mergeProfileFields(base.value(field.key()).toObject(), field.value().toObject()));
		else
			base.insert(field.key(), field.value());
	}
	return base;
}

// Возвращает полный независимый HEVC/AAC-рецепт с явными изменениями patch в JSON.
// Значения сценариев следуют закреплённым таблицам API для кодеков, качества, HDR и аудиопривязок.
QJsonObject expandedProfile(const QByteArray &patch)
{
	// base содержит все обязательные поля; новые кодировщики не наследуют блоки NVENC/SVT или custom.
	const auto base = fixture::object(R"({"mode":"transcode","live_audio_mode":"copy",
"format":{"video":{"codec":"hevc","width":1920,"height":1080,
"frame_rate":{"numerator":60,"denominator":1},"codec_profile":"main","level":"auto","tier":"",
"pixel_format":"yuv420p","bit_depth":8,
"color":{"range":"limited","primaries":"bt709","transfer":"bt709","matrix":"bt709"}},
"audio":{"codec":"aac","codec_profile":"LC","sample_rate_hz":48000,"channels":2,"channel_layout":"stereo"}},
"video_encoding":{"target_bitrate_bps":6000000,"max_bitrate_bps":6000000,"buffer_bits":12000000,
"gop_frames":120,"max_b_frames":0,"closed_gop":true},"audio_encoding":{"bitrate_bps":128000},
"encoding":{"video_encoder":"libx265","audio_encoder":"aac","rate_control":"cbr",
"rescale_output":false,"video_custom":"","audio_custom":""}})");
	return mergeProfileFields(base, fixture::object(patch));
}
} // namespace

// Проверяет структурные Widgets через публичный ввод и результаты, без нативной OBS-сессии.
class ConfigurationUiTest final : public QObject {
	Q_OBJECT
private slots:
	// Задаёт полные допустимые профили новых кодеков, HDR, качества и аудиопривязок.
	void expandedProfilesKeepFiniteIdentityAndCompleteRecipe_data();
	// Загрузка и явный выбор тех же пунктов сохраняют технические значения и весь исходящий профиль.
	void expandedProfilesKeepFiniteIdentityAndCompleteRecipe();
	// Смена аудиокодека и HDR-формата сохраняет несовместимый ввод до явного исправления.
	void expandedDependenciesRequireExplicitRepair();
	// Нулевой QP и дробный CRF имеют раздельное присутствие, а отключённый ввод остаётся в RAM.
	void qualityModeChangesKeepOnlyApplicableValue();
	// Конечные флаги остаются ComboBox, а отключённый точный CRF сохраняет промежуточную RAM строку.
	void finiteSelectorsAndQualityDraftStayExact();
	// Правка одной custom пары сохраняет канонический порядок всех незатронутых опций.
	void editingCustomPairKeepsCanonicalOrder();
	// Неизвестные custom пары требуют явного удаления, а исходный документ остаётся в RAM.
	void unsupportedCustomCanBeExplicitlyRepaired();
	// Один popup/log соответствует одной операции; поздний контекст и повтор не порождают уведомления.
	void operationFailurePopupIsSinglePlainTextAndContextBound();
	// Открывает ещё не исполнявшееся назначение в прежнем окне с парковкой черновиков и маскированием.
	void destinationDetailsWithoutSessionPreserveDraftAndMasks();
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
	// Задаёт полные versionless профили с явными audio mode.
	void structuredProfilePreservesUntouchedFields_data();
	// Отражение и чтение формы не теряют FPS, байты, LIVE AAC и присутствие encoding.
	void structuredProfilePreservesUntouchedFields();
	// Правка одного поля сохраняет остальные точные настройки полного рецепта.
	void changingOneProfileFieldPreservesExplicitRecipe();
	// Полный encoding обязателен, несовместимые video/audio сочетания не отправляются.
	void fullEncodingAndAudioDependenciesAreEnforced();
	// Неизвестный codec_profile требует явного исправления, исходный документ сохраняется.
	void unknownProfileValueRequiresExplicitRepair();
	// Пустой новый профиль не сохраняется с вымышленными форматами или площадочными presets.
	void newProfileRequiresExplicitMediaFormat();
	// Дробный размер и переполнение битрейта вызывают ошибку вместо округления.
	void invalidStructuredInputIsNotSilentlyNormalized();
	// Все три битрейта переводятся в кбит/с без потери последних битов исходного uint64.
	void bitrateUnitsPreservePrecisionAndConvertExplicitInput();
	// Задаёт обе локализации подсказки с основным и максимальным битрейтами.
	void bitrateHelpTracksDraftWithoutChangingIt_data();
	// Подсказка обновляется после ввода, пресета и восстановления; мышь и клавиатура не меняют профиль.
	void bitrateHelpTracksDraftWithoutChangingIt();
	// Задаёт обе локализации причины локального CBR-отказа.
	void profileValidationShowsReasonOnceAndTurnsGreenAfterCorrection_data();
	// Отказ формы и сериализатора красный, причина понятна, явное исправление позволяет подтверждённое сохранение.
	void profileValidationShowsReasonOnceAndTurnsGreenAfterCorrection();
	// Серверный путь и HTML-причина передаются уведомлению, без повторения в форме и доверия к messageKey.
	void nestedServerValidationIsPlainTextAndKeepsReasonOnce();
	// Серверная причина не дублируется в форме карточки, включая полученные в ней секретные значения.
	void serverValidationReasonIsAbsentFromEditor();
	// Задаёт подтверждённый отказ, конфликт и неопределённый исход отправленного изменения.
	void mutationFailuresStayRedWithoutClaimingUnknownWasRejected_data();
	// Красный цвет не превращает потерянный ответ в подтверждённый отказ и не повторяет изменение.
	void mutationFailuresStayRedWithoutClaimingUnknownWasRejected();
	// Очистка endpoint после потерянного PATCH сохраняет неизвестный исход и не повторяет запись.
	void unknownDestinationValidationKeepsUnknownOutcome();
	// Частичный успех выбора назначений не выдаётся зелёным общим результатом.
	void partialDestinationSaveKeepsErrorTone();
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
	void clientLogsIncludeOnlyReceivedFailureDiagnostics_data();
	// Журнал содержит полученную диагностику отказа, но не запросы и успешную конфигурацию.
	void clientLogsIncludeOnlyReceivedFailureDiagnostics();
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
	// Подтверждённое удаление снимает старую локальную ошибку только после начала записи.
	// Выбирает локальную числовую ошибку профиля либо inline-ошибку адреса назначения.
	void deletingRecordReplacesStaleValidationWhilePending_data();
	// Проверяет отменённое и принятое удаление после локального отказа сохранения.
	void deletingRecordReplacesStaleValidationWhilePending();
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
// Полные ответы содержат явный audio mode; каждый вариант сохраняется без скрытой подстановки.
void ConfigurationUiTest::structuredProfilePreservesUntouchedFields_data()
{
	QTest::addColumn<QJsonObject>("document");
	// document задаёт полный рецепт; audio mode изменяется независимо от прочих параметров.
	auto document = fixture::groupProfile();
	QTest::newRow("explicit-copy") << document;
	document.insert(QStringLiteral("live_audio_mode"), QStringLiteral("transcode"));
	QTest::newRow("explicit-transcode") << document;
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
	// error принимает объяснение отказа чтения, spy выявляет непрошенные изменения при заполнении формы.
	QString error;
	QSignalSpy spy(&editor, &ProfileEditor::changed);
	QVERIFY(spy.isValid());
	QVERIFY2(editor.readProfile(restored, error), qPrintable(error));
	// encoded сравнивается с независимым исходным JSON целиком, а не с результатом самого редактора.
	const auto encoded = fixture::qtResult(encodeProfile(restored));
	QVERIFY(encoded);
	QCOMPARE(*encoded.value, document);
	QCOMPARE(spy.size(), 0);
}
// Правка битрейта звука сохраняет прочие поля полного явного рецепта.
void ConfigurationUiTest::changingOneProfileFieldPreservesExplicitRecipe()
{
	// decoded задаёт независимый исходный рецепт; editor показывает его пользователю.
	const auto decoded = decode<GroupProfile>(fixture::bytes(fixture::groupProfile()));
	QVERIFY(decoded);
	ProfileEditor editor(translatedKey);
	editor.setProfile(*decoded.value);
	// bitrate адресует единственный изменяемый ввод; restored/error принимают итог формы.
	auto *bitrate = editor.findChild<QLineEdit *>(QStringLiteral("audio.bitrate"));
	GroupProfile restored;
	QString error;
	QVERIFY(bitrate);
	bitrate->selectAll();
	QTest::keyClicks(bitrate, "128");
	QVERIFY2(editor.readProfile(restored, error), qPrintable(error));
	QCOMPARE(restored.audioEncoding.bitrateBps, quint64(128000));
	QCOMPARE(restored.format.video.level.bytes, QByteArray("5.1"));
	QCOMPARE(restored.liveAudioMode.value, QStringLiteral("copy"));
	QCOMPARE(restored.encoding->videoEncoder, QStringLiteral("h264_nvenc"));
	QCOMPARE(restored.format.video.frameRate.numerator, quint32(60000));
	QCOMPARE(restored.format.video.frameRate.denominator, quint32(1001));
}

// Полный редактор не предлагает удаление encoding, а несовместимая смена режима не становится допустимым запросом.
void ConfigurationUiTest::fullEncodingAndAudioDependenciesAreEnforced()
{
	// decoded/profile задают явный LIVE transcode; editor содержит только конечные элементы выбора.
	const auto decoded = decode<GroupProfile>(fixture::bytes(fixture::groupProfile()));
	QVERIFY(decoded);
	auto profile = *decoded.value;
	profile.liveAudioMode = {FieldState::Value, QStringLiteral("transcode")};
	ProfileEditor editor(translatedKey);
	editor.setProfile(profile);
	QVERIFY(!editor.findChild<QComboBox *>(QStringLiteral("encodingPresence")));
	// restored/error фиксируют недопустимый промежуточный рецепт после смены video mode.
	GroupProfile restored;
	QString error;
	QVERIFY(choose(editor.findChild<QComboBox *>(QStringLiteral("mode")), QStringLiteral("copy")));
	QVERIFY(!editor.readProfile(restored, error));
	QVERIFY(!error.isEmpty());
}

// Неизвестное исходное значение остаётся видимым, недоступным для выбора и не проходит полную замену.
void ConfigurationUiTest::unknownProfileValueRequiresExplicitRepair()
{
	// decoded/profile содержат неизвестный codec_profile; originalDocument не должен теряться в RAM.
	const auto decoded = decode<GroupProfile>(fixture::bytes(fixture::groupProfile()));
	QVERIFY(decoded);
	auto profile = *decoded.value;
	profile.format.video.codecProfile.bytes = "unknown-profile";
	// editor/selector представляют неизвестное значение отдельным отключённым пунктом.
	ProfileEditor editor(translatedKey);
	editor.setProfile(profile);
	auto *selector = editor.findChild<QComboBox *>(QStringLiteral("video.codec_profile"));
	QVERIFY(selector);
	QVERIFY(!selector->isEditable());
	QCOMPARE(selector->currentData().toByteArray(), QByteArray("unknown-profile"));
	QVERIFY(!(selector->model()->flags(selector->model()->index(selector->currentIndex(), 0)) & Qt::ItemIsEnabled));
	// restored/error требуют явного исправления на разрешённое high.
	GroupProfile restored;
	QString error;
	QVERIFY(!editor.readProfile(restored, error));
	QVERIFY(choose(selector, QStringLiteral("high")));
	QVERIFY2(editor.readProfile(restored, error), qPrintable(error));
	QCOMPARE(restored.originalDocument, profile.originalDocument);
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
	QVERIFY(!editor.readProfile(restored, error));
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
	QTest::keyClicks(maximum, "8000.001");
	target->selectAll();
	QTest::keyClicks(target, "6000");
	audio->selectAll();
	QTest::keyClicks(audio, "160");
	QVERIFY2(editor.readProfile(restored, error), qPrintable(error));
	QCOMPARE(restored.videoEncoding.targetBitrateBps, quint64(6000000));
	QCOMPARE(restored.audioEncoding.bitrateBps, quint64(160000));
	QCOMPARE(restored.videoEncoding.maxBitrateBps, quint64(8000001));
}

// Проверяет две поставляемые локализации без фиксации их конкретной пунктуации.
void ConfigurationUiTest::bitrateHelpTracksDraftWithoutChangingIt_data()
{
	QTest::addColumn<QString>("locale");
	QTest::newRow("russian") << QStringLiteral("ru-RU");
	QTest::newRow("english") << QStringLiteral("en-US");
}

// Читает подсказку при скрытом максимуме, затем проверяет пользовательский ввод, пресет и восстановленный черновик.
void ConfigurationUiTest::bitrateHelpTracksDraftWithoutChangingIt()
{
	// locale выбирает настоящие ресурсы; translations живёт дольше использующего его переводчика формы.
	QFETCH(QString, locale);
	QSettings translations(QStringLiteral(GOST_TEST_LOCALE_DIR) + QLatin1Char('/') + locale +
				       QStringLiteral(".ini"),
			       QSettings::IniFormat);
	// decoded/profile задают независимые точные значения в бит/с, которые UI показывает в кбит/с.
	const auto decoded = decode<GroupProfile>(fixture::bytes(fixture::groupProfile()));
	QVERIFY(decoded);
	auto profile = *decoded.value;
	profile.videoEncoding.targetBitrateBps = 10000123;
	profile.videoEncoding.maxBitrateBps = 12000456;
	// editor получает реальные переводы; key адресует одну подпись в выбранном файле локализации.
	ProfileEditor editor([&translations](const char *key) {
		return translations.value(QString::fromUtf8(key), QString::fromUtf8(key)).toString();
	});
	editor.setProfile(profile);
	editor.show();
	// help/target/maximum/advanced адресуют подсказку, пару значений и свёрнутый по умолчанию раздел.
	auto *help = editor.findChild<QToolButton *>(QStringLiteral("videoBitrateHelp"));
	auto *target = editor.findChild<QLineEdit *>(QStringLiteral("video.target"));
	auto *maximum = editor.findChild<QLineEdit *>(QStringLiteral("video.maximum"));
	auto *advanced = editor.findChild<QToolButton *>(QStringLiteral("profileAdvanced"));
	QVERIFY(help && target && maximum && advanced);
	QVERIFY(!help->icon().isNull());
	QVERIFY(help->focusPolicy() & Qt::TabFocus);
	QVERIFY(!help->accessibleName().isEmpty());
	QVERIFY(!help->accessibleName().startsWith(QStringLiteral("Profile.")));
	QVERIFY(!advanced->isChecked());
	QVERIFY(!maximum->isVisible());
	QVERIFY(help->isVisible());
	QVERIFY(help->toolTip().contains(QStringLiteral("10000.123")) ||
		help->toolTip().contains(QStringLiteral("10000,123")));
	QVERIFY(help->toolTip().contains(QStringLiteral("12000.456")) ||
		help->toolTip().contains(QStringLiteral("12000,456")));
	QVERIFY(help->accessibleDescription().contains(target->text()));
	QVERIFY(help->accessibleDescription().contains(maximum->text()));
	QVERIFY(!help->toolTip().contains(QStringLiteral("Profile.BitrateHelp")));
	// beforeTarget/beforeMaximum и changes фиксируют неизменность значений и отсутствие сигнала редактирования.
	const auto beforeTarget = target->text();
	const auto beforeMaximum = maximum->text();
	QSignalSpy changes(&editor, &ProfileEditor::changed);
	QVERIFY(changes.isValid());
	// hover доставляет обычный запрос tooltip непосредственно собственной кнопке offscreen-формы.
	QHelpEvent hover(QEvent::ToolTip, help->rect().center(), help->mapToGlobal(help->rect().center()));
	QApplication::sendEvent(help, &hover);
	QTest::mouseClick(help, Qt::LeftButton);
	help->setFocus();
	QTest::keyClick(help, Qt::Key_Space);
	QCOMPARE(changes.size(), 0);
	QCOMPARE(target->text(), beforeTarget);
	QCOMPARE(maximum->text(), beforeMaximum);
	target->selectAll();
	QTest::keyClicks(target, "9000");
	QVERIFY(help->toolTip().contains(QStringLiteral("9000")));
	QVERIFY(!help->toolTip().contains(QStringLiteral("10000")));
	QVERIFY(help->toolTip().contains(QStringLiteral("12000")));
	QCOMPARE(maximum->text(), beforeMaximum);
	advanced->click();
	maximum->selectAll();
	QTest::keyClicks(maximum, "13000");
	advanced->click();
	QVERIFY(!maximum->isVisible());
	QVERIFY(help->toolTip().contains(QStringLiteral("13000")));
	// retained содержит ввод до пресета; preset заполняет всю форму известным профилем владельца.
	const auto retained = editor.captureDraft();
	auto *preset = editor.findChild<QComboBox *>(QStringLiteral("profilePreset"));
	QVERIFY(choose(preset, QStringLiteral("h264-1080p60")));
	QVERIFY(help->toolTip().contains(QStringLiteral("6000")));
	QVERIFY(!help->toolTip().contains(QStringLiteral("9000")));
	changes.clear();
	editor.restoreDraft(retained);
	QCOMPARE(changes.size(), 0);
	QCOMPARE(target->text(), QStringLiteral("9000"));
	QVERIFY(help->toolTip().contains(QStringLiteral("9000")));
	QVERIFY(help->toolTip().contains(QStringLiteral("13000")));
	// restored и error проверяют реальные значения DTO после всех чисто справочных действий.
	GroupProfile restored;
	QString error;
	QVERIFY2(editor.readProfile(restored, error), qPrintable(error));
	QCOMPARE(restored.videoEncoding.targetBitrateBps, quint64(9000000));
	QCOMPARE(restored.videoEncoding.maxBitrateBps, quint64(13000000));
	editor.setProfile(profile);
	QVERIFY(help->toolTip().contains(QStringLiteral("10000")));
	QVERIFY(help->toolTip().contains(QStringLiteral("12000")));
}

// Проверяет обе локализации одной причины и общего результата сохранения.
void ConfigurationUiTest::profileValidationShowsReasonOnceAndTurnsGreenAfterCorrection_data()
{
	QTest::addColumn<QString>("locale");
	QTest::newRow("russian") << QStringLiteral("ru-RU");
	QTest::newRow("english") << QStringLiteral("en-US");
}

// Ошибочный текст и CBR-расхождение не отправляют HTTP, не меняют максимум и не теряют пользовательский ввод.
void ConfigurationUiTest::profileValidationShowsReasonOnceAndTurnsGreenAfterCorrection()
{
	// locale/translations выбирают настоящие подписи без подмены пользовательского текста ключами API.
	QFETCH(QString, locale);
	QSettings translations(QStringLiteral(GOST_TEST_LOCALE_DIR) + QLatin1Char('/') + locale +
				       QStringLiteral(".ini"),
			       QSettings::IniFormat);
	// rig/model/encoding/video задают корректный CBR-рецепт сервера с обоими битрейтами 12 Мбит/с.
	ConfigurationRig rig;
	QVERIFY(rig.initialize());
	auto model = fixture::groupProfile();
	auto encoding = fixture::encoding();
	encoding.insert(QStringLiteral("rate_control"), QStringLiteral("cbr"));
	model.insert(QStringLiteral("encoding"), encoding);
	model.insert(QStringLiteral("live_audio_mode"), QStringLiteral("copy"));
	auto video = model.value(QStringLiteral("video_encoding")).toObject();
	video.insert(QStringLiteral("target_bitrate_bps"), QJsonValue(qint64(12000000)));
	video.insert(QStringLiteral("max_bitrate_bps"), QJsonValue(qint64(12000000)));
	model.insert(QStringLiteral("video_encoding"), video);
	rig.profile.insert(QStringLiteral("profile"), model);
	// page использует реальный редактор; key адресует локализованную подпись или причину.
	ConfigurationEditor page(
		rig.state, rig.configuration,
		[&translations](const char *key) {
			return translations.value(QString::fromUtf8(key), QString::fromUtf8(key)).toString();
		},
		ConfigurationKind::Profile);
	page.show();
	QVERIFY(rig.configuration.open(ConfigurationKind::Profile, QStringLiteral("profile")));
	QTRY_VERIFY_WITH_TIMEOUT(rig.state.configurationEditor().base.has_value(), 1000);
	// target/maximum/save/message адресуют пользовательский ввод, скрытый максимум, действие и inline-результат.
	auto *target = page.findChild<QLineEdit *>(QStringLiteral("video.target"));
	auto *maximum = page.findChild<QLineEdit *>(QStringLiteral("video.maximum"));
	auto *save = page.findChild<QPushButton *>(QStringLiteral("configurationSave"));
	auto *message = page.findChild<QLabel *>(QStringLiteral("configurationMessage"));
	QVERIFY(target && maximum && save && message);
	QVERIFY(!maximum->isVisible());
	// initialOutcome остаётся прежним: отключённая кнопка не передаёт локальную ошибку контроллеру записи.
	const auto initialOutcome = rig.state.configurationEditor().outcome;
	target->selectAll();
	QTest::keyClicks(target, "12x");
	QVERIFY(!save->isEnabled());
	save->click();
	QCOMPARE(message->property("statusTone").toString(), QStringLiteral("error"));
	QCOMPARE(message->textFormat(), Qt::PlainText);
	QCOMPARE(target->text(), QStringLiteral("12x"));
	QVERIFY(rig.mutations().isEmpty());
	target->selectAll();
	QTest::keyClicks(target, "10000");
	QVERIFY(!save->isEnabled());
	save->click();
	QCOMPARE(rig.state.configurationEditor().outcome, initialOutcome);
	QCOMPARE(message->property("statusTone").toString(), QStringLiteral("error"));
	// reason/notSaved/label — независимые публичные ресурсы точной причины, общего отказа и понятного поля.
	const auto reason = translations.value(QStringLiteral("Validation.CbrBitrateMismatch")).toString();
	const auto notSaved = translations.value(QStringLiteral("Configuration.NotSaved")).toString();
	const auto label = translations.value(QStringLiteral("Profile.MaxBitrate")).toString();
	QVERIFY(!reason.isEmpty() && !notSaved.isEmpty() && !label.isEmpty());
	QCOMPARE(message->text().count(reason), 1);
	QCOMPARE(message->text().count(notSaved), 1);
	QVERIFY(message->text().contains(label));
	QVERIFY(!message->text().contains(QStringLiteral("profile.video_encoding")));
	QCOMPARE(target->text(), QStringLiteral("10000"));
	QCOMPARE(maximum->text(), QStringLiteral("12000"));
	QCOMPARE(std::get<Profile>(*rig.state.configurationEditor().draft).profile.videoEncoding.targetBitrateBps,
		 quint64(10000000));
	QCOMPARE(std::get<Profile>(*rig.state.configurationEditor().draft).profile.videoEncoding.maxBitrateBps,
		 quint64(12000000));
	QVERIFY(rig.mutations().isEmpty());
	target->selectAll();
	QTest::keyClicks(target, "12000");
	QVERIFY(save->isEnabled());
	QVERIFY(rig.state.configurationEditor().fieldIssues.isEmpty());
	QVERIFY(!message->text().contains(reason));
	save->click();
	QTRY_COMPARE_WITH_TIMEOUT(rig.state.configurationEditor().outcome, ConfigurationOutcome::Saved, 1000);
	QCOMPARE(message->property("statusTone").toString(), QStringLiteral("active"));
	QVERIFY(message->text().contains(translations.value(QStringLiteral("Configuration.Saved")).toString()));
	QVERIFY(!message->text().contains(notSaved));
	QVERIFY(!message->text().contains(reason));
	QCOMPARE(rig.mutations().size(), 1);
}

// Серверный отказ передаётся уведомлению операции, а форма не повторяет его причину и вложенный путь.
void ConfigurationUiTest::nestedServerValidationIsPlainTextAndKeepsReasonOnce()
{
	// translations предоставляет настоящие подписи формы, чтобы отсутствие причины не зависело от заглушки.
	QSettings translations(QStringLiteral(GOST_TEST_LOCALE_DIR) + QStringLiteral("/en-US.ini"),
			       QSettings::IniFormat);
	// rig/page открывают обычный серверный профиль; key адресует поставляемую английскую подпись.
	ConfigurationRig rig;
	QVERIFY(rig.initialize());
	// failures получает отдельный серверный отказ; repaint формы его не заменяет.
	QSignalSpy failures(&rig.configuration, &ConfigurationController::operationFailed);
	QVERIFY(failures.isValid());
	ConfigurationEditor page(
		rig.state, rig.configuration,
		[&translations](const char *key) {
			return translations.value(QString::fromUtf8(key), QString::fromUtf8(key)).toString();
		},
		ConfigurationKind::Profile);
	QVERIFY(rig.configuration.open(ConfigurationKind::Profile, QStringLiteral("profile")));
	QTRY_VERIFY_WITH_TIMEOUT(rig.state.configurationEditor().base.has_value(), 1000);
	// Перехватчик request возвращает структурированную ошибку известного вложенного поля с недоверенным ключом.
	rig.intercept = [](const HttpServer::Request &request) {
		if (request.method != "PATCH")
			return false;
		HttpServer::reply(
			request,
			R"({"status":422,"title":"Validation","errors":[{"location":"body.profile.video_encoding.max_bitrate_bps","message":"<b>Maximum rejected</b>","value":"private-field-value","messageKey":"Configuration.Saved"}]})",
			422, "application/problem+json");
		return true;
	};
	// target/save/message адресуют настоящий пользовательский ввод и результат карточки.
	auto *target = page.findChild<QLineEdit *>(QStringLiteral("video.target"));
	auto *save = page.findChild<QPushButton *>(QStringLiteral("configurationSave"));
	auto *message = page.findChild<QLabel *>(QStringLiteral("configurationMessage"));
	QVERIFY(target && save && message);
	target->selectAll();
	QTest::keyClicks(target, "7000");
	save->click();
	QTRY_COMPARE_WITH_TIMEOUT(rig.state.configurationEditor().outcome, ConfigurationOutcome::Rejected, 1000);
	QCOMPARE(rig.mutations().size(), 1);
	QVERIFY(rig.state.configurationEditor().fieldIssues.isEmpty());
	QCOMPARE(failures.size(), 1);
	// notification сохраняет PlainText причину и точный серверный путь для общего popup.
	const auto notification = qvariant_cast<OperationFailure>(failures.at(0).at(0));
	QVERIFY(notification.reason.contains(QStringLiteral("<b>Maximum rejected</b>")));
	QVERIFY(notification.reason.contains(QStringLiteral("body.profile.video_encoding.max_bitrate_bps")));
	QCOMPARE(message->textFormat(), Qt::PlainText);
	QCOMPARE(message->property("statusTone").toString(), QStringLiteral("error"));
	QVERIFY(!message->text().contains(QStringLiteral("Maximum rejected")));
	QVERIFY(!message->text().contains(QStringLiteral("<b>Maximum rejected</b>")));
	QVERIFY(!message->text().contains(QStringLiteral("profile.video_encoding")));
	QVERIFY(!message->text().contains(QStringLiteral("private-field-value")));
	QVERIFY(!message->text().contains(QStringLiteral("Configuration.Saved")));
	QCOMPARE(target->text(), QStringLiteral("7000"));
	target->selectAll();
	QTest::keyClicks(target, "6000");
	QVERIFY(rig.state.configurationEditor().fieldIssues.isEmpty());
	QVERIFY(!message->text().contains(QStringLiteral("Maximum rejected")));
}

// Не повторяет серверную причину отказа в открытой карточке назначения; она принадлежит общему уведомлению.
void ConfigurationUiTest::serverValidationReasonIsAbsentFromEditor()
{
	// translations предоставляет настоящие подписи формы с неизменными форматами подстановки.
	QSettings translations(QStringLiteral(GOST_TEST_LOCALE_DIR) + QStringLiteral("/en-US.ini"),
			       QSettings::IniFormat);
	// rig/page содержат синтетические endpoint и stream_key, которые сервер включает в причину отказа.
	ConfigurationRig rig;
	QVERIFY(rig.initialize());
	// Переводчик key возвращает поставляемую подпись, сохраняя подстановки редактора.
	ConfigurationEditor page(
		rig.state, rig.configuration,
		[&translations](const char *key) {
			return translations.value(QString::fromUtf8(key), QString::fromUtf8(key)).toString();
		},
		ConfigurationKind::Destination);
	QVERIFY(rig.configuration.open(ConfigurationKind::Destination, QStringLiteral("destination")));
	QTRY_VERIFY_WITH_TIMEOUT(rig.state.configurationEditor().base.has_value(), 1000);
	// Перехватчик request вставляет секреты в структурированную причину, а не только в отдельное value.
	rig.intercept = [&rig](const HttpServer::Request &request) {
		if (request.method != "PATCH")
			return false;
		// issue/problem моделируют обычную причину с чувствительными значениями из текущей серверной базы.
		const QJsonObject issue{{QStringLiteral("location"), QStringLiteral("body.name")},
					{QStringLiteral("message"),
					 QStringLiteral("Name rejected for ") + fixture::endpoint() + QLatin1Char(' ') +
						 rig.destination.value(QStringLiteral("stream_key")).toString()},
					{QStringLiteral("value"), QStringLiteral("private-field-value")}};
		const QJsonObject problem{{QStringLiteral("status"), 422},
					  {QStringLiteral("errors"), QJsonArray{issue}}};
		HttpServer::reply(request, fixture::bytes(problem), 422, "application/problem+json");
		return true;
	};
	// name/save/message адресуют безопасное поле изменения и единственный inline-результат.
	auto *name = page.findChild<QLineEdit *>(QStringLiteral("configurationName"));
	auto *save = page.findChild<QPushButton *>(QStringLiteral("configurationSave"));
	auto *message = page.findChild<QLabel *>(QStringLiteral("configurationMessage"));
	QVERIFY(name && save && message);
	name->selectAll();
	QTest::keyClicks(name, "Rejected destination name");
	save->click();
	QTRY_COMPARE_WITH_TIMEOUT(rig.state.configurationEditor().outcome, ConfigurationOutcome::Rejected, 1000);
	QVERIFY(!message->text().contains(QStringLiteral("Name rejected for")));
	QVERIFY(!message->text().contains(fixture::endpoint()));
	QVERIFY(!message->text().contains(rig.destination.value(QStringLiteral("stream_key")).toString()));
	QVERIFY(!message->text().contains(QStringLiteral("private-field-value")));
	QCOMPARE(message->textFormat(), Qt::PlainText);
	QCOMPARE(message->property("statusTone").toString(), QStringLiteral("error"));
}

// Потеря ответа, серверная валидация и конфликт имеют разные результаты при общем красном оформлении.
void ConfigurationUiTest::mutationFailuresStayRedWithoutClaimingUnknownWasRejected_data()
{
	QTest::addColumn<int>("status");
	QTest::addColumn<int>("outcomeValue");
	QTest::newRow("lost-response") << 0 << static_cast<int>(ConfigurationOutcome::Unknown);
	QTest::newRow("validation-rejected") << 422 << static_cast<int>(ConfigurationOutcome::Rejected);
	QTest::newRow("revision-conflict") << 409 << static_cast<int>(ConfigurationOutcome::Conflict);
}

// Отправляет одно намерение и проверяет безопасную классификацию без автоматической повторной записи.
void ConfigurationUiTest::mutationFailuresStayRedWithoutClaimingUnknownWasRejected()
{
	// status выбирает ответ сервера; outcomeValue задаёт независимо ожидаемую классификацию результата.
	QFETCH(int, status);
	QFETCH(int, outcomeValue);
	// rig/page открывают редактируемую группу до управляемого отказа PATCH.
	ConfigurationRig rig;
	QVERIFY(rig.initialize());
	ConfigurationEditor page(rig.state, rig.configuration, translatedKey, ConfigurationKind::Group);
	QVERIFY(rig.configuration.open(ConfigurationKind::Group, QStringLiteral("group")));
	QTRY_VERIFY_WITH_TIMEOUT(rig.state.configurationEditor().base.has_value(), 1000);
	// Перехватчик request обрывает ответ либо возвращает отказ; при 409 обновляет независимую серверную ревизию.
	rig.intercept = [&rig, status](const HttpServer::Request &request) {
		if (request.method != "PATCH")
			return false;
		if (status == 0) {
			if (request.socket)
				request.socket->abort();
		} else {
			if (status == 409)
				rig.group.insert(QStringLiteral("revision"), QJsonValue(qint64(2)));
			HttpServer::reply(request,
					  status == 409 ? R"({"status":409,"code":"revision_conflict"})" : "{}", status,
					  "application/problem+json");
		}
		return true;
	};
	// name/save/message адресуют обычное имя группы, действие сохранения и видимый результат.
	auto *name = page.findChild<QLineEdit *>(QStringLiteral("configurationName"));
	auto *save = page.findChild<QPushButton *>(QStringLiteral("configurationSave"));
	auto *message = page.findChild<QLabel *>(QStringLiteral("configurationMessage"));
	QVERIFY(name && save && message);
	name->selectAll();
	QTest::keyClicks(name, "Retained group name");
	save->click();
	QTRY_COMPARE_WITH_TIMEOUT(rig.state.configurationEditor().outcome,
				  static_cast<ConfigurationOutcome>(outcomeValue), 1000);
	QCOMPARE(message->property("statusTone").toString(), QStringLiteral("error"));
	QCOMPARE(rig.mutations().size(), 1);
	QCOMPARE(name->text(), QStringLiteral("Retained group name"));
	QVERIFY(!message->text().contains(QStringLiteral("Configuration.Saved")));
	if (status == 0) {
		QVERIFY(message->text().contains(QStringLiteral("Configuration.Unknown")));
		QVERIFY(!message->text().contains(QStringLiteral("Configuration.Rejected")));
		QVERIFY(rig.state.configurationEditor().reconciliationRequired);
		QVERIFY(!save->isEnabled());
	}
}

// Потерянный PATCH остаётся неизвестным, даже если позднее пользователь очищает обязательный адрес.
void ConfigurationUiTest::unknownDestinationValidationKeepsUnknownOutcome()
{
	// rig/page открывают существующее назначение через изолированный HTTP-сервер.
	ConfigurationRig rig;
	QVERIFY(rig.initialize());
	ConfigurationEditor page(rig.state, rig.configuration, translatedKey, ConfigurationKind::Destination);
	page.show();
	QVERIFY(rig.configuration.open(ConfigurationKind::Destination, QStringLiteral("destination")));
	QTRY_VERIFY_WITH_TIMEOUT(rig.state.configurationEditor().base.has_value(), 1000);
	// name/save/message адресуют изменяемое поле и общий результат потерянного сохранения.
	auto *name = page.findChild<QLineEdit *>(QStringLiteral("configurationName"));
	auto *save = page.findChild<QPushButton *>(QStringLiteral("configurationSave"));
	auto *message = page.findChild<QLabel *>(QStringLiteral("configurationMessage"));
	QVERIFY(name && save && message);
	// endpointField выбирается по полученному адресу, не по порядку полей формы.
	SecretField *endpointField = nullptr;
	// secret перебирает поля адреса и ключа, оставляя только полученный endpoint.
	for (auto *secret : page.findChildren<SecretField *>())
		if (secret->received().hasValue() && *secret->received() == fixture::endpoint())
			endpointField = secret;
	QVERIFY(endpointField);
	// input позволяет проверить пустой адрес, сохраняя обычный пользовательский путь ввода.
	auto *input = endpointField->findChild<QLineEdit *>(QStringLiteral("secretReceived"));
	QVERIFY(input);
	// Перехватчик request обрывает единственный PATCH, не отправляя его повторно.
	rig.intercept = [](const HttpServer::Request &request) {
		if (request.method != "PATCH")
			return false;
		if (request.socket)
			request.socket->abort();
		return true;
	};
	name->selectAll();
	QTest::keyClicks(name, "Retained destination name");
	save->click();
	QTRY_COMPARE_WITH_TIMEOUT(rig.state.configurationEditor().outcome, ConfigurationOutcome::Unknown, 1000);
	QVERIFY(rig.state.configurationEditor().reconciliationRequired);
	QCOMPARE(rig.mutations().size(), 1);
	input->selectAll();
	QTest::keyClick(input, Qt::Key_Backspace);
	QVERIFY(input->text().isEmpty());
	// error остаётся непосредственно у поля адреса, пока общий итог хранит неизвестность PATCH.
	auto *error = page.findChild<QLabel *>(QStringLiteral("configurationEndpointError"));
	QVERIFY(error);
	QVERIFY(error->isVisible());
	QCOMPARE(error->text(), QStringLiteral("Editor.EndpointRequired"));
	QVERIFY(message->text().contains(QStringLiteral("Configuration.Unknown")));
	QVERIFY(!message->text().contains(QStringLiteral("Configuration.NotSaved")));
	QCOMPARE(message->property("statusTone").toString(), QStringLiteral("error"));
	QCOMPARE(rig.mutations().size(), 1);
}

// Общий результат остаётся ошибкой, когда один из двух независимых PATCH назначений отклонён.
void ConfigurationUiTest::partialDestinationSaveKeepsErrorTone()
{
	// rig/config/documents задают два собственных включённых назначения и полный доступный список.
	ConfigurationRig rig;
	QVERIFY(rig.initialize());
	auto config = rig.state.configuration();
	QMap<QString, QJsonObject> documents;
	// id адресует одно назначение; document/decoded — независимые серверная и клиентская копии.
	for (const auto &id : {QStringLiteral("a"), QStringLiteral("b")}) {
		auto document = fixture::destination();
		document.insert(QStringLiteral("id"), id);
		document.insert(QStringLiteral("name"), QStringLiteral("Destination ") + id);
		const auto decoded = decode<Destination>(fixture::bytes(document));
		QVERIFY(decoded);
		documents.insert(id, document);
		config.destinations.insert(id, *decoded.value);
	}
	config.destinationsStatus = {RuntimeAvailability::Fresh, true, {}};
	rig.state.setConfiguration(config);
	// Перехватчик request обслуживает список и карточки; b отклоняется, a получает подтверждённую новую ревизию.
	rig.intercept = [&documents](const HttpServer::Request &request) {
		if (request.method == "GET" && (request.target.startsWith("/api/v1/destinations?") ||
						request.target.startsWith("/api/v1/streams/stream/destinations"))) {
			HttpServer::reply(request, fixture::page({documents.value(QStringLiteral("a")),
								  documents.value(QStringLiteral("b"))}));
			return true;
		}
		// id извлекает непрозрачную цель только для простых синтетических адресов a/b этой фикстуры.
		const auto id =
			QString::fromUtf8(request.target).section(QLatin1Char('/'), -1).section(QLatin1Char('?'), 0, 0);
		if (!documents.contains(id))
			return false;
		if (request.method == "PATCH" && id == QStringLiteral("b")) {
			HttpServer::reply(request, "{}", 422, "application/problem+json");
			return true;
		}
		if (request.method == "PATCH") {
			documents[id].insert(QStringLiteral("enabled"), false);
			documents[id].insert(QStringLiteral("revision"), QJsonValue(qint64(2)));
		}
		HttpServer::reply(request, fixture::bytes(documents.value(id)));
		return true;
	};
	// page/message отражают публичный итог того же контроллера, который выполняет реальные петлевые HTTP-запросы.
	ConfigurationEditor page(rig.state, rig.configuration, translatedKey, ConfigurationKind::Stream);
	auto *message = page.findChild<QLabel *>(QStringLiteral("configurationMessage"));
	QVERIFY(message);
	QVERIFY(rig.configuration.open(ConfigurationKind::Stream, QStringLiteral("stream")));
	QTRY_VERIFY_WITH_TIMEOUT(rig.state.configurationEditor().base.has_value(), 1000);
	QVERIFY(rig.configuration.setDestinationEnabled(QStringLiteral("a"), false));
	QVERIFY(rig.configuration.setDestinationEnabled(QStringLiteral("b"), false));
	QVERIFY(rig.configuration.save());
	QTRY_VERIFY_WITH_TIMEOUT(!rig.state.configurationEditor().writing, 1000);
	QCOMPARE(rig.state.configurationEditor().destinationOutcomes.value(QStringLiteral("a")),
		 ConfigurationOutcome::Saved);
	QCOMPARE(rig.state.configurationEditor().destinationOutcomes.value(QStringLiteral("b")),
		 ConfigurationOutcome::Rejected);
	QVERIFY(message->text().contains(QStringLiteral("Configuration.DestinationsPartial")));
	QCOMPARE(message->property("statusTone").toString(), QStringLiteral("error"));
	QCOMPARE(rig.mutations().size(), 2);
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

// Скрытие расширенного раздела и смена кодека сохраняют неизвестное значение и незавершённый ввод.
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
	QVERIFY(choose(editor.findChild<QComboBox *>(QStringLiteral("video.codec")), QStringLiteral("hevc")));
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
		const auto encoded = fixture::qtResult(encodeProfile(restored));
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
			      {QStringLiteral("expected_revision"), QJsonValue(qint64(1))}}));
	input->selectAll();
	QTest::keyClick(input, Qt::Key_Backspace);
	save->click();
	QTRY_COMPARE_WITH_TIMEOUT(rig.mutations().size(), 2, 1000);
	QTRY_VERIFY_WITH_TIMEOUT(!rig.state.configurationEditor().writing, 1000);
	QCOMPARE(QJsonDocument::fromJson(rig.mutations().last().body).object(),
		 (QJsonObject{{QStringLiteral("stream_key"), QString{}},
			      {QStringLiteral("expected_revision"), QJsonValue(qint64(2))}}));
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
		 QJsonObject({{QStringLiteral("expected_revision"), QJsonValue(qint64(1))},
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

void ConfigurationUiTest::clientLogsIncludeOnlyReceivedFailureDiagnostics_data()
{
	QTest::addColumn<bool>("detailed");
	QTest::newRow("normal") << false;
	QTest::newRow("detailed") << true;
}
void ConfigurationUiTest::clientLogsIncludeOnlyReceivedFailureDiagnostics()
{
	// detailed выбирает обычный либо подробный режим: оба сохраняют полученную диагностику отказа.
	QFETCH(bool, detailed);
	// temp/file изолируют preferences настоящего Runtime, logs принимает только его итоговые записи.
	WorkspaceTemp temp(QStringLiteral("safe-client-log"));
	QVERIFY(temp.isValid());
	const auto file = temp.filePath(QStringLiteral("settings.json"));
	QStringList logs;
	{
		// preferences задаёт режим журнала; совместимый auto_connect не управляет текущим runtime.
		// Сеть не запускается до отсутствующего в этом сценарии события FinishedLoading.
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
	// secret перечисляет значения запроса и успешного DTO: они не входят в полученную диагностику ошибки.
	for (const auto &secret :
	     {QStringLiteral("private-user-payload"), fixture::endpoint(), QStringLiteral("key/%2F+=")})
		QVERIFY2(!combined.contains(secret), "Журнал не должен выгружать запрос или успешную конфигурацию.");
	QVERIFY(combined.contains(QStringLiteral("server-raw-secret")));
	QVERIFY(combined.contains(QStringLiteral("synthetic-private-payload")));
	QVERIFY(combined.contains(QStringLiteral("clipboard-secret")));
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
// Первое неуспешное чтение остаётся красным и доступным пользователю до явного повторного GET.
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
	// Перехватчик request отклоняет только первое адресное чтение; последующее обслуживает обычная фикстура.
	rig.intercept = [&](const HttpServer::Request &request) {
		if (request.method != "GET" || request.target != "/api/v1/destinations/destination")
			return false;
		++detailReads;
		if (detailReads != 1)
			return false;
		if (status)
			HttpServer::reply(request,
					  status == 409 ? R"({"status":409,"code":"revision_conflict"})" : "{}", status,
					  "application/problem+json");
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
	QCOMPARE(message->property("statusTone").toString(), QStringLiteral("error"));
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

// Подтверждённые сохранение и удаление показываются зелёным, а удаление заменяет предыдущий текст Saved.
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
	QCOMPARE(message->property("statusTone").toString(), QStringLiteral("active"));
	// deleted отражает серверное удаление: последующий список не возвращает уже удалённую Group.
	bool deleted = false;
	// Перехватчик request подтверждает DELETE и исключает удалённую запись из последующего серверного списка.
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
	QCOMPARE(message->property("statusTone").toString(), QStringLiteral("active"));
	QVERIFY(!message->text().contains(QStringLiteral("Configuration.Saved")));
	QCOMPARE(rig.state.selectedStreamId(), QStringLiteral("stream"));
	QCOMPARE(rig.mutations().size(), 2);
}

// Задаёт два вида локальной валидации, которые не должны стать результатом принятого DELETE.
void ConfigurationUiTest::deletingRecordReplacesStaleValidationWhilePending_data()
{
	QTest::addColumn<bool>("endpointCase");
	QTest::newRow("profile-number") << false;
	QTest::newRow("destination-endpoint") << true;
}

// Подтверждённый DELETE снимает прежнюю локальную ошибку при ожидании ответа и показывает зелёный результат.
void ConfigurationUiTest::deletingRecordReplacesStaleValidationWhilePending()
{
	// endpointCase выбирает inline-проверку адреса вместо числового ввода профиля.
	QFETCH(bool, endpointCase);
	// kind/id адресуют запись; collectionPath определяет только последующее чтение её коллекции.
	const auto kind = endpointCase ? ConfigurationKind::Destination : ConfigurationKind::Profile;
	const auto id = endpointCase ? QStringLiteral("destination") : QStringLiteral("profile");
	const auto collectionPath = endpointCase ? QByteArrayLiteral("/api/v1/destinations")
						 : QByteArrayLiteral("/api/v1/transcode-profiles");
	// rig/page открывают запись через изолированный HTTP-сервер.
	ConfigurationRig rig;
	QVERIFY(rig.initialize());
	rig.configApi.setHttpTimeoutMs(2000);
	ConfigurationEditor page(rig.state, rig.configuration, translatedKey, kind);
	page.show();
	QVERIFY(rig.configuration.open(kind, id));
	QTRY_VERIFY_WITH_TIMEOUT(rig.state.configurationEditor().base.has_value(), 1000);
	// target адресует исходный ввод; для назначения поле выбирается по полученному endpoint.
	QLineEdit *target = page.findChild<QLineEdit *>(QStringLiteral("video.target"));
	if (endpointCase) {
		// secret перебирает маскированные строки и отличает endpoint от ключа трансляции.
		for (auto *secret : page.findChildren<SecretField *>())
			if (secret->received().hasValue() && *secret->received() == fixture::endpoint())
				target = secret->findChild<QLineEdit *>(QStringLiteral("secretReceived"));
	}
	// save/remove/message адресуют действия и общую сводку карточки.
	auto *save = page.findChild<QPushButton *>(QStringLiteral("configurationSave"));
	auto *message = page.findChild<QLabel *>(QStringLiteral("configurationMessage"));
	QPushButton *remove = nullptr;
	// button перебирает действия страницы и выбирает удаление по локализованной подписи.
	for (auto *button : page.findChildren<QPushButton *>())
		if (button->text() == QStringLiteral("Editor.Delete"))
			remove = button;
	QVERIFY(target && save && remove && message);
	target->selectAll();
	if (endpointCase)
		QTest::keyClick(target, Qt::Key_Backspace);
	else
		QTest::keyClicks(target, "12x");
	save->click();
	QVERIFY(message->text().contains(QStringLiteral("Configuration.NotSaved")));
	QCOMPARE(message->property("statusTone").toString(), QStringLiteral("error"));
	// deleteRequest удерживает только живой сокет DELETE до явного ответа теста; deleted управляет списком после успеха.
	HttpServer::Request deleteRequest;
	bool deleted = false;
	// Перехватчик сохраняет DELETE без ответа, а после подтверждения отдаёт пустой список текущего вида.
	rig.intercept = [&deleteRequest, &deleted, collectionPath](const HttpServer::Request &request) {
		if (request.method == "DELETE") {
			deleteRequest = request;
			return true;
		}
		if (deleted && request.method == "GET" && request.target.startsWith(collectionPath)) {
			HttpServer::reply(request, fixture::page({}));
			return true;
		}
		return false;
	};
	// cancelled фиксирует отказ от первой попытки удаления до отправки HTTP.
	bool cancelled = false;
	// Таймер выбирает No, чтобы проверить, что непринятое действие не очищает локальную ошибку.
	QTimer::singleShot(0, &page, [&page, &cancelled] {
		// dialog — открытое подтверждение удаления этой страницы.
		if (auto *dialog = page.findChild<QMessageBox *>()) {
			cancelled = true;
			dialog->done(QMessageBox::No);
		}
	});
	remove->click();
	QVERIFY(cancelled);
	QVERIFY(!rig.state.configurationEditor().writing);
	// Повторная отрисовка после отмены проверяет, что причина сохранилась в локальном состоянии формы.
	rig.state.setConfigurationEditor(rig.state.configurationEditor());
	QVERIFY(message->text().contains(QStringLiteral("Configuration.NotSaved")));
	QCOMPARE(message->property("statusTone").toString(), QStringLiteral("error"));
	QVERIFY(rig.mutations().isEmpty());
	// confirmed фиксирует ответ только на диалог этой страницы.
	bool confirmed = false;
	// Таймер выбирает Yes в настоящем модальном подтверждении, после чего UI ждёт ответ HTTP.
	QTimer::singleShot(0, &page, [&page, &confirmed] {
		// dialog — открытое подтверждение удаления, если обработчик успел создать его.
		if (auto *dialog = page.findChild<QMessageBox *>()) {
			confirmed = true;
			dialog->done(QMessageBox::Yes);
		}
	});
	remove->click();
	QVERIFY(confirmed);
	QTRY_VERIFY_WITH_TIMEOUT(rig.state.configurationEditor().writing && deleteRequest.socket, 1000);
	QCOMPARE(target->text(), endpointCase ? QString{} : QStringLiteral("12x"));
	QVERIFY(!message->text().contains(QStringLiteral("Configuration.NotSaved")));
	QCOMPARE(message->property("statusTone").toString(), QStringLiteral("pending"));
	if (endpointCase) {
		// inlineError больше не описывает принятое удаление, хотя пустой ввод остаётся в поле.
		auto *inlineError = page.findChild<QLabel *>(QStringLiteral("configurationEndpointError"));
		QVERIFY(inlineError);
		QVERIFY(inlineError->isHidden());
		QCOMPARE(inlineError->property("statusTone").toString(), QStringLiteral("neutral"));
	}
	QVERIFY(deleteRequest.socket);
	deleted = true;
	HttpServer::reply(deleteRequest, {}, 204, {});
	QTRY_COMPARE_WITH_TIMEOUT(rig.state.configurationEditor().outcome, ConfigurationOutcome::Deleted, 1000);
	QCOMPARE(message->text(), QStringLiteral("Configuration.Deleted"));
	QCOMPARE(message->property("statusTone").toString(), QStringLiteral("active"));
	QVERIFY(!message->text().contains(QStringLiteral("Configuration.NotSaved")));
	QCOMPARE(rig.mutations().size(), 1);
}

// Нормализованный custom проверяется полным валидатором после правки первой из двух пар.
void ConfigurationUiTest::editingCustomPairKeepsCanonicalOrder()
{
	// decoded/profile задают независимо допустимые параметры NVENC с двумя упорядоченными опциями.
	const auto decoded = decode<GroupProfile>(fixture::bytes(fixture::groupProfile()));
	QVERIFY(decoded);
	auto profile = *decoded.value;
	profile.encoding->videoCustom = QStringLiteral("nonref_p=0 strict_gop=1");
	// editor отображает полный рецепт до единственной пользовательской правки custom.
	ProfileEditor editor(translatedKey);
	editor.setProfile(profile);
	// nonref адресует единственную пользовательскую правку; restored/error принимают полный результат.
	auto *nonref = editor.findChild<QComboBox *>(QStringLiteral("custom.nonref_p"));
	GroupProfile restored;
	QString error;
	QVERIFY(choose(nonref, QStringLiteral("1")));
	QVERIFY2(editor.readProfile(restored, error), qPrintable(error));
	QCOMPARE(restored.encoding->videoCustom, QStringLiteral("nonref_p=1 strict_gop=1"));
	QVERIFY(encodeProfile(restored));
}

// Просмотр неизвестного source не позволяет отправить его как допустимую полную замену.
void ConfigurationUiTest::unsupportedCustomCanBeExplicitlyRepaired()
{
	// document содержит новую неизвестную пару, original сохраняет её исходный lossless документ.
	auto document = fixture::groupProfile();
	// encoding получает неизвестную пару без изменения остальных исходных параметров.
	auto encoding = document.value(QStringLiteral("encoding")).toObject();
	encoding.insert(QStringLiteral("video_custom"), QStringLiteral("nonref_p=1 unsupported=1"));
	document.insert(QStringLiteral("encoding"), encoding);
	const auto original = decode<GroupProfile>(fixture::bytes(document));
	QVERIFY(original);
	// editor/source показывают неизвестную строку отключённым пунктом без свободного CLI.
	ProfileEditor editor(translatedKey);
	editor.setProfile(*original.value);
	auto *source = editor.findChild<QComboBox *>(QStringLiteral("unsupportedCustom.video"));
	QVERIFY(source && !source->isEditable());
	QCOMPARE(source->currentData().toString(), QStringLiteral("nonref_p=1 unsupported=1"));
	QVERIFY(!(source->model()->flags(source->model()->index(source->currentIndex(), 0)) & Qt::ItemIsEnabled));
	// restored/error требуют решения пользователя; удаление лишнего сохраняет известную пару.
	GroupProfile restored;
	QString error;
	QVERIFY(!editor.readProfile(restored, error));
	QVERIFY(choose(source, QString{}));
	QVERIFY2(editor.readProfile(restored, error), qPrintable(error));
	QCOMPARE(restored.encoding->videoCustom, QStringLiteral("nonref_p=1"));
	QCOMPARE(restored.originalDocument, original.value->originalDocument);
	QVERIFY(original.value->originalDocument.toObject()
			.value(QStringLiteral("encoding"))
			.toObject()
			.value(QStringLiteral("video_custom"))
			.toString()
			.contains(QStringLiteral("unsupported=1")));
}

// Настоящая CRUD ошибка проходит через Runtime в один временный диалог и одну запись штатного журнала.
void ConfigurationUiTest::operationFailurePopupIsSinglePlainTextAndContextBound()
{
	// temp/file изолируют настройки; logs хранит только сообщения переданного Runtime sink.
	WorkspaceTemp temp(QStringLiteral("operation-notification"));
	QVERIFY(temp.isValid());
	const auto file = temp.filePath(QStringLiteral("settings.json"));
	QStringList logs;
	{
		// preferences сохраняет совместимый флаг; сеть здесь не запускается без FinishedLoading.
		SettingsStore preferences(file);
		preferences.setAutoConnect(false);
		QVERIFY(preferences.save());
	}
	// server возвращает настоящий Problem с буквальными HTML тегами и синтетическим маркером диагностики.
	HttpServer server;
	QVERIFY(server.listen());
	server.handler = [](const HttpServer::Request &request) {
		if (request.method == "PATCH")
			HttpServer::reply(
				request,
				R"({"status":422,"code":"invalid_profile","detail":"<b>rejected</b>","action":"<i>repair</i>","diagnostic":"synthetic-secret-marker"})",
				422, "application/problem+json");
		else
			HttpServer::reply(request, fixture::bytes(fixture::destination()));
	};
	// frontend/lifecycle создают настоящий presenter с родителем главного OBS окна фикстуры.
	// Переданный обработчик журнала добавляет подготовленное message в logs без обращения к OBS.
	FakeFrontend frontend;
	PluginLifecycle lifecycle(frontend, translatedKey,
				  {QStringLiteral("test"), file, [&](LogSeverity, const QString &message) {
					   logs.append(message);
				   }});
	QVERIFY(lifecycle.load());
	// state/controller дают публичную точку явной операции; failures независимо наблюдает событие владельца.
	auto *state = lifecycle.stateStore();
	auto *controller = lifecycle.configurationController();
	QVERIFY(state && controller);
	state->setConnection(server.origin().toString(), ConnectionStatus::Connected);
	state->setSelection({}, true, {}, {}, QStringLiteral("profile-uuid"));
	QSignalSpy failures(controller, &ConfigurationController::operationFailed);
	QVERIFY(failures.isValid());
	QVERIFY(controller->open(ConfigurationKind::Destination, QStringLiteral("destination")));
	QTRY_VERIFY_WITH_TIMEOUT(state->configurationEditor().base.has_value(), 1000);
	QVERIFY(frontend.host->findChildren<OperationErrorDialog *>().isEmpty());
	// draft задаёт первое явное изменение; подтверждённый отказ не сохраняет diagnosticBody в StateStore.
	auto draft = std::get<Destination>(*state->configurationEditor().draft);
	draft.name = QStringLiteral("first mutation");
	QVERIFY(controller->setDraft(draft, {QStringLiteral("name")}));
	QVERIFY(controller->save());
	QTRY_COMPARE_WITH_TIMEOUT(failures.size(), 1, 1000);
	// dialog/reason/action адресуют единственное окно и два обычных текстовых блока.
	QPointer<OperationErrorDialog> dialog = frontend.host->findChild<OperationErrorDialog *>();
	QVERIFY(dialog);
	auto *reason = dialog->findChild<QLabel *>(QStringLiteral("operationErrorReason"));
	auto *action = dialog->findChild<QLabel *>(QStringLiteral("operationErrorAction"));
	QVERIFY(reason && action);
	QCOMPARE(reason->textFormat(), Qt::PlainText);
	QCOMPARE(action->textFormat(), Qt::PlainText);
	QCOMPARE(reason->text(), QStringLiteral("<b>rejected</b>"));
	QCOMPARE(action->text(), QStringLiteral("<i>repair</i>"));
	QCOMPARE(frontend.host->findChildren<OperationErrorDialog *>().size(), 1);
	// notification содержит уже проверенный event; countLogs различает отдельные записи, не число вхождений в body.
	auto notification = qvariant_cast<OperationFailure>(failures.at(0).at(0));
	const auto countLogs = [&logs] {
		return logs.filter(QStringLiteral("Отказ операции:")).size();
	};
	QCOMPARE(countLogs(), 1);
	QVERIFY(logs.join(QLatin1Char('\n')).contains(QStringLiteral("synthetic-secret-marker")));
	QVERIFY(notification.diagnosticBody.contains("synthetic-secret-marker"));
	QVERIFY(state->configurationEditor().messageKey != QStringLiteral("synthetic-secret-marker"));
	controller->operationFailed(notification);
	QCOMPARE(countLogs(), 1);
	QCOMPARE(frontend.host->findChildren<OperationErrorDialog *>().size(), 1);
	// late моделирует чужой context на границе presenter: даже новый ID не разрешает старому профилю окно/log.
	auto late = notification;
	late.generation += 1;
	late.context.profileUuid = QStringLiteral("foreign-profile");
	controller->operationFailed(late);
	QCOMPARE(countLogs(), 1);
	// Второе реальное намерение обновляет прежний popup, а не создаёт второе окно или историю.
	draft.name = QStringLiteral("second mutation");
	QVERIFY(controller->setDraft(draft, {QStringLiteral("name")}));
	QVERIFY(controller->save());
	QTRY_COMPARE_WITH_TIMEOUT(countLogs(), 2, 1000);
	QCOMPARE(frontend.host->findChild<OperationErrorDialog *>(), dialog.data());
	QCOMPARE(frontend.host->findChildren<OperationErrorDialog *>().size(), 1);
	lifecycle.shutdown();
	QVERIFY(dialog.isNull());
	QCOMPARE(frontend.callsAfterExit, 0);
}

// UI использует конечные технические значения, а quality диапазон остаётся точным текстовым вводом.
void ConfigurationUiTest::finiteSelectorsAndQualityDraftStayExact()
{
	// decoded/profile задают полный HEVC CRF рецепт с точными сотыми.
	const auto decoded = decode<GroupProfile>(fixture::bytes(fixture::groupProfile()));
	QVERIFY(decoded);
	auto profile = *decoded.value;
	profile.format.video.codec = QStringLiteral("hevc");
	profile.format.video.codecProfile.bytes = "main";
	profile.format.video.level.bytes = "auto";
	profile.videoEncoding = {0, 0, 0, 120, 0, true};
	profile.encoding->videoEncoder = QStringLiteral("libx265");
	profile.encoding->videoCustom.clear();
	profile.encoding->rateControl = QStringLiteral("crf");
	profile.encoding->nvenc.state = FieldState::Missing;
	profile.encoding->crfHundredths = {FieldState::Value, 2225};
	// editor/crf/control адресуют настоящий ввод и динамическую применимость качества.
	ProfileEditor editor(translatedKey);
	editor.setProfile(profile);
	auto *crf = editor.findChild<QLineEdit *>(QStringLiteral("encoding.crf"));
	auto *control = editor.findChild<QComboBox *>(QStringLiteral("encoding.rate_control"));
	QVERIFY(crf && control && !control->isEditable());
	QCOMPARE(QString(crf->text()).replace(QLatin1Char(','), QLatin1Char('.')), QStringLiteral("22.25"));
	// name перечисляет конечные boolean поля; selector не допускает произвольную строку вместо bool.
	for (const auto &name : {QStringLiteral("video.closed_gop"), QStringLiteral("encoding.rescale"),
				 QStringLiteral("nvenc.spatial"), QStringLiteral("nvenc.temporal")}) {
		// selector адресует текущий конечный флаг, не допускающий произвольного строкового ввода.
		auto *selector = editor.findChild<QComboBox *>(name);
		QVERIFY(selector && !selector->isEditable());
	}
	crf->selectAll();
	QTest::keyClicks(crf, "22.");
	QVERIFY(choose(control, QStringLiteral("vbr")));
	QVERIFY(!crf->isEnabled());
	QCOMPARE(crf->text(), QStringLiteral("22."));
	// draft сохраняет выключенную незавершённую строку при скрытии и восстановлении формы.
	const auto draft = editor.captureDraft();
	editor.hide();
	editor.setProfile(profile);
	editor.restoreDraft(draft);
	QCOMPARE(crf->text(), QStringLiteral("22."));
	QVERIFY(choose(control, QStringLiteral("crf")));
	QVERIFY(crf->isEnabled());
	crf->selectAll();
	QTest::keyClicks(crf, "22.25");
	// restored/error проверяют точное число после явного завершения ввода.
	GroupProfile restored;
	QString error;
	QVERIFY2(editor.readProfile(restored, error), qPrintable(error));
	QCOMPARE(restored.encoding->crfHundredths.value, 2225);
}

// Проверяет по одному полному рецепту на существенную GUI-границу, не дублируя чистую матрицу валидатора.
void ConfigurationUiTest::expandedProfilesKeepFiniteIdentityAndCompleteRecipe_data()
{
	QTest::addColumn<QJsonObject>("document");
	// add создаёт отдельную строку name из независимого JSON patch; значения не вычисляются редактором.
	const auto add = [](const char *name, const QByteArray &patch) {
		QTest::newRow(name) << expandedProfile(patch);
	};
	add("hevc-cbr-aac-5.1", R"({"format":{"audio":{"channels":6,"channel_layout":"5.1"}},
"audio_encoding":{"bitrate_bps":384000}})");
	add("hevc-cqp-zero-opus", R"({"format":{"audio":{"codec":"opus","codec_profile":"celt"}},
"video_encoding":{"target_bitrate_bps":0,"max_bitrate_bps":0,"buffer_bits":0},
"encoding":{"audio_encoder":"libopus","rate_control":"cqp","qp":0}})");
	add("hevc-pq-crf-flac", R"({"live_audio_mode":"transcode",
"format":{"video":{"codec_profile":"main10","pixel_format":"yuv420p10le","bit_depth":10,
"color":{"primaries":"bt2020","transfer":"smpte2084","matrix":"bt2020nc"}},
"audio":{"codec":"flac","codec_profile":"s16"}},"audio_encoding":{"bitrate_bps":0},
"video_encoding":{"target_bitrate_bps":0,"max_bitrate_bps":0,"buffer_bits":0},
"encoding":{"audio_encoder":"flac","rate_control":"crf","crf":23.51}})");
	add("vp9-hlg-crf-zero-mp3", R"({"mode":"copy",
"format":{"video":{"codec":"vp9","codec_profile":"profile2","pixel_format":"yuv420p10le","bit_depth":10,
"color":{"primaries":"bt2020","transfer":"arib-std-b67","matrix":"bt2020nc"}},
"audio":{"codec":"mp3","codec_profile":"mpeg1-layer3"}},
"video_encoding":{"target_bitrate_bps":0,"max_bitrate_bps":0,"buffer_bits":0},
"encoding":{"video_encoder":"libvpx-vp9","audio_encoder":"libmp3lame","rate_control":"crf","crf":0}})");
	add("vp9-vbr-ac3", R"({"format":{"video":{"codec":"vp9","codec_profile":"profile0"},
"audio":{"codec":"ac3","codec_profile":"ac3"}},"audio_encoding":{"bitrate_bps":192000},
"video_encoding":{"max_bitrate_bps":0,"buffer_bits":0},
"encoding":{"video_encoder":"libvpx-vp9","audio_encoder":"ac3","rate_control":"vbr"}})");
	add("hevc-nvenc-vbr", R"({"video_encoding":{"max_bitrate_bps":8000000},
"encoding":{"video_encoder":"hevc_nvenc","rate_control":"vbr"}})");
	add("x264-cqp-zero", R"({"format":{"video":{"codec":"h264","codec_profile":"high444","pixel_format":"yuv444p"}},
"video_encoding":{"target_bitrate_bps":0,"max_bitrate_bps":0,"buffer_bits":0},
"encoding":{"video_encoder":"libx264","rate_control":"cqp","qp":0}})");
	add("x264-crf-zero", R"({"format":{"video":{"codec":"h264","codec_profile":"high444","pixel_format":"yuv444p"}},
"video_encoding":{"target_bitrate_bps":0,"max_bitrate_bps":0,"buffer_bits":0},
"encoding":{"video_encoder":"libx264","rate_control":"crf","crf":0}})");
	add("aom-crf-professional", R"({"mode":"copy",
"format":{"video":{"codec":"av1","codec_profile":"professional","pixel_format":"yuv422p10le","bit_depth":10}},
"video_encoding":{"target_bitrate_bps":0,"max_bitrate_bps":0,"buffer_bits":0},
"encoding":{"video_encoder":"libaom-av1","rate_control":"crf","crf":35}})");
	add("svt-cqp", R"({"mode":"copy","format":{"video":{"codec":"av1"}},
"video_encoding":{"target_bitrate_bps":0,"max_bitrate_bps":0,"buffer_bits":0},
"encoding":{"video_encoder":"libsvtav1","rate_control":"cqp","qp":35,
"svt_av1":{"preset":8,"tuning":"psnr","aq_mode":0,"lookahead_frames":-1}}})");
	add("aac-2.1", R"({"format":{"audio":{"channels":3,"channel_layout":"2.1"}}})");
	add("aac-3.0", R"({"format":{"audio":{"channels":3,"channel_layout":"3.0"}}})");
}

// Каждый выбранный пункт доступен как точное itemData; повторный явный выбор также проходит проекцию формы.
void ConfigurationUiTest::expandedProfilesKeepFiniteIdentityAndCompleteRecipe()
{
	// document задаёт независимый полный ожидаемый результат текущего сценария.
	QFETCH(QJsonObject, document);
	// decoded проверяет обычную границу чтения DTO, editor содержит реальные Widgets и их привязки.
	const auto decoded = decode<GroupProfile>(fixture::bytes(document));
	QVERIFY(decoded);
	ProfileEditor editor(translatedKey);
	// changes отличает отражение загруженного рецепта от явного пользовательского выбора.
	QSignalSpy changes(&editor, &ProfileEditor::changed);
	QVERIFY(changes.isValid());
	editor.setProfile(*decoded.value);
	QCOMPARE(changes.size(), 0);
	// restored/error проверяют чтение сразу после загрузки; loaded сравнивает весь рецепт до activated.
	GroupProfile restored;
	QString error;
	QVERIFY2(editor.readProfile(restored, error), qPrintable(error));
	const auto loaded = fixture::qtResult(encodeProfile(restored));
	QVERIFY(loaded);
	QCOMPARE(*loaded.value, document);
	// video, audio, color и encoding берут ожидаемые технические значения из независимого документа.
	const auto video =
		document.value(QStringLiteral("format")).toObject().value(QStringLiteral("video")).toObject();
	const auto audio =
		document.value(QStringLiteral("format")).toObject().value(QStringLiteral("audio")).toObject();
	const auto color = video.value(QStringLiteral("color")).toObject();
	const auto encoding = document.value(QStringLiteral("encoding")).toObject();
	// expected связывает публичный формат с конкретными конечными контролами, включая тип QByteArray для ProfileText.
	const QVariantMap expected{
		{QStringLiteral("mode"), document.value(QStringLiteral("mode")).toString()},
		{QStringLiteral("live_audio_mode"), document.value(QStringLiteral("live_audio_mode")).toString()},
		{QStringLiteral("video.codec"), video.value(QStringLiteral("codec")).toString()},
		{QStringLiteral("video.codec_profile"),
		 video.value(QStringLiteral("codec_profile")).toString().toUtf8()},
		{QStringLiteral("video.level"), video.value(QStringLiteral("level")).toString().toUtf8()},
		{QStringLiteral("video.tier"), video.value(QStringLiteral("tier")).toString().toUtf8()},
		{QStringLiteral("video.pixel_format"), video.value(QStringLiteral("pixel_format")).toString().toUtf8()},
		{QStringLiteral("video.bit_depth"), QString::number(video.value(QStringLiteral("bit_depth")).toInt())},
		{QStringLiteral("video.color.range"), color.value(QStringLiteral("range")).toString().toUtf8()},
		{QStringLiteral("video.color.primaries"), color.value(QStringLiteral("primaries")).toString().toUtf8()},
		{QStringLiteral("video.color.transfer"), color.value(QStringLiteral("transfer")).toString().toUtf8()},
		{QStringLiteral("video.color.matrix"), color.value(QStringLiteral("matrix")).toString().toUtf8()},
		{QStringLiteral("audio.codec"), audio.value(QStringLiteral("codec")).toString()},
		{QStringLiteral("audio.codec_profile"),
		 audio.value(QStringLiteral("codec_profile")).toString().toUtf8()},
		{QStringLiteral("audio.sample_rate"),
		 QString::number(audio.value(QStringLiteral("sample_rate_hz")).toInt())},
		{QStringLiteral("audio.channels"), QString::number(audio.value(QStringLiteral("channels")).toInt())},
		{QStringLiteral("audio.channel_layout"),
		 audio.value(QStringLiteral("channel_layout")).toString().toUtf8()},
		{QStringLiteral("encoding.video_encoder"), encoding.value(QStringLiteral("video_encoder")).toString()},
		{QStringLiteral("encoding.audio_encoder"), encoding.value(QStringLiteral("audio_encoder")).toString()},
		{QStringLiteral("encoding.rate_control"), encoding.value(QStringLiteral("rate_control")).toString()}};
	// field указывает на ожидаемый технический выбор; selector принадлежит только текущему редактору.
	for (auto field = expected.begin(); field != expected.end(); ++field) {
		auto *selector = editor.findChild<QComboBox *>(field.key());
		QVERIFY2(selector, qPrintable(field.key()));
		QVERIFY(!selector->isEditable());
		QVERIFY(selector->isEnabled());
		QCOMPARE(selector->currentData().metaType(), field.value().metaType());
		QCOMPARE(selector->currentData(), field.value());
		QVERIFY(selector->model()->flags(selector->model()->index(selector->currentIndex(), 0)) &
			Qt::ItemIsEnabled);
		QVERIFY(choose(selector, field.value().toString()));
	}
	// qp/crf проверяют применимость диапазонов независимо от общего каталога реализации.
	auto *qp = editor.findChild<QLineEdit *>(QStringLiteral("encoding.qp"));
	auto *crf = editor.findChild<QLineEdit *>(QStringLiteral("encoding.crf"));
	QVERIFY(qp && crf);
	QCOMPARE(qp->isEnabled(), encoding.contains(QStringLiteral("qp")));
	QCOMPARE(crf->isEnabled(), encoding.contains(QStringLiteral("crf")));
	// encoded выявляет потерю любого поля или утечку неприменимого блока после явного выбора.
	QVERIFY2(editor.readProfile(restored, error), qPrintable(error));
	const auto encoded = fixture::qtResult(encodeProfile(restored));
	QVERIFY(encoded);
	QCOMPARE(*encoded.value, document);
}

// Зависимые каталоги запрещают прежние значения после смены формата и не заменяют их автоматически.
void ConfigurationUiTest::expandedDependenciesRequireExplicitRepair()
{
	// source задаёт AAC 5.1 с допустимым для Opus битрейтом, чтобы отказ зависел только от аудиопривязки.
	const auto source = expandedProfile(R"({"format":{"audio":{"channels":6,"channel_layout":"5.1"}}})");
	// decoded/editor проходят обычную загрузку полного профиля, restored/error принимают результат каждой правки.
	const auto decoded = decode<GroupProfile>(fixture::bytes(source));
	QVERIFY(decoded);
	ProfileEditor editor(translatedKey);
	editor.setProfile(*decoded.value);
	GroupProfile restored;
	QString error;
	QVERIFY2(editor.readProfile(restored, error), qPrintable(error));
	QVERIFY(choose(editor.findChild<QComboBox *>(QStringLiteral("audio.codec")), QStringLiteral("opus")));
	// unsupported содержит прежние значения AAC, каждое из которых должно остаться видимым, но недоступным.
	const QMap<QString, QString> unsupported{{QStringLiteral("audio.codec_profile"), QStringLiteral("LC")},
						 {QStringLiteral("audio.channels"), QStringLiteral("6")},
						 {QStringLiteral("audio.channel_layout"), QStringLiteral("5.1")},
						 {QStringLiteral("encoding.audio_encoder"), QStringLiteral("aac")}};
	// field проходит прежние значения; selector адресует соответствующий зависимый список.
	for (auto field = unsupported.begin(); field != unsupported.end(); ++field) {
		auto *selector = editor.findChild<QComboBox *>(field.key());
		QVERIFY(selector && !selector->isEditable());
		QCOMPARE(selector->currentData().toString(), field.value());
		QVERIFY(!(selector->model()->flags(selector->model()->index(selector->currentIndex(), 0)) &
			  Qt::ItemIsEnabled));
		QVERIFY(!choose(selector, field.value()));
	}
	QVERIFY(!editor.readProfile(restored, error));
	QVERIFY(!error.isEmpty());
	QVERIFY(choose(editor.findChild<QComboBox *>(QStringLiteral("audio.codec_profile")), QStringLiteral("celt")));
	QVERIFY(choose(editor.findChild<QComboBox *>(QStringLiteral("encoding.audio_encoder")),
		       QStringLiteral("libopus")));
	QVERIFY(choose(editor.findChild<QComboBox *>(QStringLiteral("audio.channels")), QStringLiteral("2")));
	QVERIFY(choose(editor.findChild<QComboBox *>(QStringLiteral("audio.channel_layout")),
		       QStringLiteral("stereo")));
	QVERIFY2(editor.readProfile(restored, error), qPrintable(error));
	// opus ожидает только явно исправленную аудиопривязку; прочие поля полного рецепта остаются прежними.
	const auto opus = fixture::qtResult(encodeProfile(restored));
	QVERIFY(opus);
	QCOMPARE(*opus.value, mergeProfileFields(source, fixture::object(R"({"format":{"audio":{"codec":"opus",
"codec_profile":"celt","channels":2,"channel_layout":"stereo"}},"encoding":{"audio_encoder":"libopus"}})")));

	// hdr задаёт допустимый PQ-рецепт, transfer проверяет запрет сохранения PQ после явного перехода к 8 битам.
	const auto hdr = decode<GroupProfile>(fixture::bytes(expandedProfile(R"({"format":{"video":{
"codec_profile":"main10","pixel_format":"yuv420p10le","bit_depth":10,
"color":{"primaries":"bt2020","transfer":"smpte2084","matrix":"bt2020nc"}}}})")));
	QVERIFY(hdr);
	editor.setProfile(*hdr.value);
	QVERIFY2(editor.readProfile(restored, error), qPrintable(error));
	auto *transfer = editor.findChild<QComboBox *>(QStringLiteral("video.color.transfer"));
	QVERIFY(transfer);
	QVERIFY(choose(editor.findChild<QComboBox *>(QStringLiteral("video.codec_profile")), QStringLiteral("main")));
	QVERIFY(!editor.readProfile(restored, error));
	QVERIFY(choose(editor.findChild<QComboBox *>(QStringLiteral("video.pixel_format")), QStringLiteral("yuv420p")));
	QVERIFY(choose(editor.findChild<QComboBox *>(QStringLiteral("video.bit_depth")), QStringLiteral("8")));
	QCOMPARE(transfer->currentData().toByteArray(), QByteArray("smpte2084"));
	QVERIFY(!choose(transfer, QStringLiteral("smpte2084")));
	QVERIFY(!choose(transfer, QStringLiteral("arib-std-b67")));
	QVERIFY(!editor.readProfile(restored, error));
	QVERIFY(choose(transfer, QStringLiteral("bt709")));
	QVERIFY(!editor.readProfile(restored, error));
	QVERIFY(choose(editor.findChild<QComboBox *>(QStringLiteral("video.color.primaries")),
		       QStringLiteral("bt709")));
	QVERIFY(choose(editor.findChild<QComboBox *>(QStringLiteral("video.color.matrix")), QStringLiteral("bt709")));
	QVERIFY2(editor.readProfile(restored, error), qPrintable(error));
	// sdr подтверждает весь исправленный рецепт, включая сохранённые размеры, FPS, GOP и аудио.
	const auto sdr = fixture::qtResult(encodeProfile(restored));
	QVERIFY(sdr);
	QCOMPARE(*sdr.value, expandedProfile("{}"));

	// flac содержит два явных transcode-режима; live проверяет их зависимость при смене видео на copy.
	const auto flac = decode<GroupProfile>(fixture::bytes(expandedProfile(R"({"live_audio_mode":"transcode",
"format":{"audio":{"codec":"flac","codec_profile":"s16"}},"audio_encoding":{"bitrate_bps":0},
"encoding":{"audio_encoder":"flac"}})")));
	QVERIFY(flac);
	editor.setProfile(*flac.value);
	QVERIFY2(editor.readProfile(restored, error), qPrintable(error));
	auto *live = editor.findChild<QComboBox *>(QStringLiteral("live_audio_mode"));
	QVERIFY(live);
	QVERIFY(choose(editor.findChild<QComboBox *>(QStringLiteral("mode")), QStringLiteral("copy")));
	QCOMPARE(live->currentData().toString(), QStringLiteral("transcode"));
	QVERIFY(!choose(live, QStringLiteral("transcode")));
	QVERIFY(!editor.readProfile(restored, error));
	QVERIFY(choose(live, QStringLiteral("copy")));
	QVERIFY(!editor.readProfile(restored, error));
	QVERIFY(choose(editor.findChild<QComboBox *>(QStringLiteral("mode")), QStringLiteral("transcode")));
	QVERIFY(choose(live, QStringLiteral("transcode")));
	QVERIFY2(editor.readProfile(restored, error), qPrintable(error));
	QCOMPARE(restored.audioEncoding.bitrateBps, quint64(0));
}

// Смена CQP/CRF не превращает отсутствующее качество в ноль и не отправляет отключённую незавершённую строку.
void ConfigurationUiTest::qualityModeChangesKeepOnlyApplicableValue()
{
	// decoded содержит x264 High444 с явным QP0, для которого контракт допускает также CRF0.
	const auto decoded = decode<GroupProfile>(fixture::bytes(expandedProfile(R"({"format":{"video":{
"codec":"h264","codec_profile":"high444","pixel_format":"yuv444p"}},
"video_encoding":{"target_bitrate_bps":0,"max_bitrate_bps":0,"buffer_bits":0},
"encoding":{"video_encoder":"libx264","rate_control":"cqp","qp":0}})")));
	QVERIFY(decoded);
	// editor/control/qp/crf адресуют реальную форму и три поля, участвующие в смене режима качества.
	ProfileEditor editor(translatedKey);
	editor.setProfile(*decoded.value);
	auto *control = editor.findChild<QComboBox *>(QStringLiteral("encoding.rate_control"));
	auto *qp = editor.findChild<QLineEdit *>(QStringLiteral("encoding.qp"));
	auto *crf = editor.findChild<QLineEdit *>(QStringLiteral("encoding.crf"));
	QVERIFY(control && qp && crf);
	QCOMPARE(qp->text(), QStringLiteral("0"));
	QVERIFY(crf->text().isEmpty());
	// restored/error проверяют отдельное присутствие качества после пользовательского выбора и ввода.
	GroupProfile restored;
	QString error;
	QVERIFY(choose(control, QStringLiteral("crf")));
	QVERIFY(!qp->isEnabled() && crf->isEnabled());
	QVERIFY(!editor.readProfile(restored, error));
	QTest::keyClicks(crf, "23.50");
	QVERIFY2(editor.readProfile(restored, error), qPrintable(error));
	QVERIFY(restored.encoding->qp.isMissing());
	QVERIFY(restored.encoding->crfHundredths.hasValue());
	QCOMPARE(restored.encoding->crfHundredths.value, 2350);
	crf->selectAll();
	QTest::keyClicks(crf, "23.501");
	QVERIFY(!editor.readProfile(restored, error));
	QVERIFY(choose(control, QStringLiteral("cqp")));
	QVERIFY(qp->isEnabled() && !crf->isEnabled());
	QCOMPARE(qp->text(), QStringLiteral("0"));
	QCOMPARE(crf->text(), QStringLiteral("23.501"));
	QVERIFY2(editor.readProfile(restored, error), qPrintable(error));
	QVERIFY(restored.encoding->qp.hasValue());
	QCOMPARE(restored.encoding->qp.value, 0);
	QVERIFY(restored.encoding->crfHundredths.isMissing());
	// draft удерживает невалидный отключённый CRF; восстановление не должно превращать его в прежнее допустимое число.
	const auto draft = editor.captureDraft();
	editor.setProfile(*decoded.value);
	editor.restoreDraft(draft);
	QCOMPARE(crf->text(), QStringLiteral("23.501"));
	QVERIFY(choose(control, QStringLiteral("crf")));
	QVERIFY(!editor.readProfile(restored, error));
	crf->selectAll();
	QTest::keyClicks(crf, "0");
	QVERIFY2(editor.readProfile(restored, error), qPrintable(error));
	QVERIFY(restored.encoding->qp.isMissing());
	QVERIFY(restored.encoding->crfHundredths.hasValue());
	QCOMPARE(restored.encoding->crfHundredths.value, 0);
	// encoded проверяет присутствие явного нуля в JSON и отсутствие прежнего QP.
	const auto encoded = fixture::qtResult(encodeProfile(restored));
	QVERIFY(encoded);
	QVERIFY(!encoded.value->value(QStringLiteral("encoding")).toObject().contains(QStringLiteral("qp")));
	QCOMPARE(encoded.value->value(QStringLiteral("encoding")).toObject().value(QStringLiteral("crf")),
		 QJsonValue(0));
}

QTEST_MAIN(ConfigurationUiTest)
#include "configuration-ui-test.moc"

// Прямой Details работает до первого эфира, хранит чужой черновик и возвращает маску при повторном переходе.
void ConfigurationUiTest::destinationDetailsWithoutSessionPreserveDraftAndMasks()
{
	// rig содержит только настройки Stream/Destination: нет ни Session, ни attached Output, ни runtime.
	ConfigurationRig rig;
	QVERIFY(rig.initialize());
	QVERIFY(rig.configuration.open(ConfigurationKind::Group, QStringLiteral("group")));
	QTRY_VERIFY_WITH_TIMEOUT(rig.state.configurationEditor().base.has_value(), 1000);
	// groupDraft хранит несохранённое имя, которое просмотр другой карточки должен припарковать в RAM.
	auto groupDraft = std::get<Group>(*rig.state.configurationEditor().draft);
	groupDraft.name = QStringLiteral("Несохранённая группа");
	QVERIFY(rig.configuration.setDraft(groupDraft, {QStringLiteral("name")}));
	// context фиксируется до переходов; window является единственным окном управления этого сценария.
	const auto context = rig.state.dockContext();
	ManagementWindow window(rig.state, translatedKey, nullptr, &rig.connection, &rig.operations,
				&rig.configuration);
	window.show();
	// pages/navigation дают публичный результат прямой навигации, без вызова HTTP из Dock.
	auto *pages = window.findChild<QStackedWidget *>(QStringLiteral("managementPages"));
	auto *navigation = window.findChild<QListWidget *>(QStringLiteral("managementNavigation"));
	QVERIFY(pages && navigation);
	QTRY_VERIFY_WITH_TIMEOUT(!rig.state.configurationEditor().loading, 1000);
	QCOMPARE(pages->currentIndex(), 2);
	window.selectDestination(context, QStringLiteral("destination"));
	QTRY_VERIFY_WITH_TIMEOUT(!rig.state.configurationEditor().loading &&
					 rig.state.configurationEditor().id == QStringLiteral("destination"),
				 1000);
	QCOMPARE(pages->currentIndex(), 1);
	QVERIFY(rig.state.configurationEditor().draft.has_value());
	// editor отображает назначение, а secret/received/eye проверяют маску реального endpoint карточки.
	auto *editor = pages->widget(1)->findChild<ConfigurationEditor *>();
	QVERIFY(editor);
	QCOMPARE(editor->selectedDestinationId(), QStringLiteral("destination"));
	auto *secret = editor->findChild<SecretField *>();
	QVERIFY(secret);
	auto *received = secret->findChild<QLineEdit *>(QStringLiteral("secretReceived"));
	auto *eye = secret->findChild<QToolButton *>(QStringLiteral("secretEye"));
	QVERIFY(received && eye);
	QCOMPARE(received->text(), fixture::endpoint());
	QCOMPARE(received->echoMode(), QLineEdit::Password);
	eye->click();
	QCOMPARE(received->echoMode(), QLineEdit::Normal);
	// destinationDraft хранит независимый ввод показанной карточки при переходе через существующее окно.
	auto destinationDraft = std::get<Destination>(*rig.state.configurationEditor().draft);
	destinationDraft.name = QStringLiteral("Черновик площадки");
	QVERIFY(rig.configuration.setDraft(destinationDraft, {QStringLiteral("name")}));
	navigation->setCurrentRow(2);
	QTRY_COMPARE_WITH_TIMEOUT(rig.state.configurationEditor().kind, ConfigurationKind::Group, 1000);
	QCOMPARE(std::get<Group>(*rig.state.configurationEditor().draft).name, groupDraft.name);
	window.selectDestination(context, QStringLiteral("destination"));
	QTRY_COMPARE_WITH_TIMEOUT(rig.state.configurationEditor().kind, ConfigurationKind::Destination, 1000);
	QCOMPARE(std::get<Destination>(*rig.state.configurationEditor().draft).name, destinationDraft.name);
	QCOMPARE(received->echoMode(), QLineEdit::Password);
	eye->click();
	window.hide();
	window.show();
	QTRY_VERIFY_WITH_TIMEOUT(!rig.state.configurationEditor().loading, 1000);
	QCOMPARE(received->echoMode(), QLineEdit::Password);
	QCOMPARE(std::get<Destination>(*rig.state.configurationEditor().draft).name, destinationDraft.name);
	navigation->setCurrentRow(2);
	QTRY_COMPARE_WITH_TIMEOUT(rig.state.configurationEditor().kind, ConfigurationKind::Group, 1000);
	// staleContext моделирует старую ссылку Dock: он не должен менять страницу, черновик или выбранный Stream.
	auto staleContext = context;
	staleContext.streamId = QStringLiteral("other-stream");
	window.selectDestination(staleContext, QStringLiteral("destination"));
	window.selectDestination(context, QString{});
	QCOMPARE(pages->currentIndex(), 2);
	QCOMPARE(std::get<Group>(*rig.state.configurationEditor().draft).name, groupDraft.name);
	QCOMPARE(rig.state.dockContext(), context);
	QVERIFY(!rig.state.dockServer().session);
	QVERIFY(rig.state.runtime().sessions.isEmpty());
	QVERIFY(rig.mutations().isEmpty());
	QCOMPARE(rig.frontend.stopRequests, 0);
}
