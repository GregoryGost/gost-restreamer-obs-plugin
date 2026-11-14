#include "configuration-editor.hpp"
#include "profile-editor.hpp"
#include "secret-field.hpp"
#include "confirmation-dialog.hpp"
#include "core/configuration-controller.hpp"
#include "core/state-store.hpp"
#include <QCheckBox>
#include <QComboBox>
#include <QFormLayout>
#include <QGroupBox>
#include <QHeaderView>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QMessageBox>
#include <QPointer>
#include <QPushButton>
#include <QScrollArea>
#include <QSignalBlocker>
#include <QStyle>
#include <QIcon>
#include <QVariantMap>
#include <QTableWidget>
#include <QTabWidget>
#include <QUuid>
#include <QVBoxLayout>
#include <algorithm>
#include <utility>
#include <limits>
#include <map>
#include <tuple>

namespace gost {
namespace {
// Возвращает безопасное имя записи без сериализации секретных полей.
QString entityName(const ConfigurationEntity &entity)
{
	// Посетитель читает только общее поле name каждого конфигурационного DTO.
	return std::visit([](const auto &record) { return record.name; }, entity);
}
// Возвращает точную серверную ревизию записи для сравнения, без числа с плавающей точкой.
quint64 entityRevision(const ConfigurationEntity &entity)
{
	// Посетитель читает только revision, не смешивая её с ревизиями runtime.
	return std::visit([](const auto &record) { return record.revision; }, entity);
}
// Присваивает общее имя в типизированном DTO; value остаётся обычным текстом.
void setEntityName(ConfigurationEntity &entity, const QString &value)
{
	// Посетитель меняет только разрешённое общее поле имени.
	std::visit([&value](auto &record) { record.name = value; }, entity);
}
// Возвращает подпись обычного поля для сравнения; секреты и полный профиль сюда не передаются.
QString ordinaryValue(const ConfigurationEntity &entity, const QString &field)
{
	if (field == QStringLiteral("name"))
		return entityName(entity);
	// destination даёт доступ только к обычным полям назначения для сравнения.
	if (const auto *destination = std::get_if<api::Destination>(&entity)) {
		if (field == QStringLiteral("stream_id"))
			return destination->streamId;
		if (field == QStringLiteral("group_id"))
			return destination->groupId;
		if (field == QStringLiteral("platform"))
			return destination->platform;
		if (field == QStringLiteral("enabled"))
			return destination->enabled ? QStringLiteral("true") : QStringLiteral("false");
	}
	// group содержит обычные ID отношений одной версии группы.
	if (const auto *group = std::get_if<api::Group>(&entity)) {
		if (field == QStringLiteral("stream_id"))
			return group->streamId;
		if (field == QStringLiteral("profile_id"))
			return group->profileId;
	}
	return {};
}
} // namespace

/** Синхронизирует единственную RAM-форму с одной страницей. Сигналы отражения
 * не создают новых намерений, несохранённый ввод не переписывается SSE-обновлениями. */
class ConfigurationEditor::Impl {
public:
	// Создаёт дочерние элементы и подписки; общие сервисы заимствуются в GUI-потоке.
	Impl(ConfigurationEditor &owner, StateStore &state, ConfigurationController &controller, TextLookup text,
	     ConfigurationKind kind);
	// Обновляет списки и сообщения; форму заменяет только при реальной смене базы/карточки.
	void refresh();
	// Отражает полный draft и отдельные намерения замены секретов без новых сигналов ввода.
	void populate(const ConfigurationEditorState &editor);
	// Собирает типизированный draft и изменённые JSON-поля; сохраняет невалидный ввод в виджетах.
	void edited(const QString &field);
	// Проверяет обязательность адреса только для создания и явной замены Destination.endpoint.
	bool endpointRequiredButEmpty(const ConfigurationEditorState &editor) const;
	// Показывает локальную ошибку адреса рядом с полем после попытки сохранить.
	void updateEndpointRequiredError(const ConfigurationEditorState &editor);
	// Обновляет отношения по ID и видимые связи, сохраняя неизвестные ссылки отдельными пунктами.
	void relations();
	// Обновляет один селектор relation по именам/ID; неизвестный id остаётся явно неподтверждённым.
	void relation(QComboBox *combo, const QMap<QString, QString> &items, const QString &id, bool complete);
	// Показывает безопасное сравнение base/mine/server внутри страницы.
	void conflict();
	// Явно разрешает конфликт, начиная с нового серверного DTO, затем отправляет подтверждённый draft.
	void resolveConflict();
	// Передаёт save только после локальной валидации полного профиля и текущего ввода.
	void save();
	// Запрашивает подтверждение с PlainText без интерпретации серверных имён как HTML.
	bool confirm(const char *message);
	// Возвращает принадлежность карточки странице, даже до первого DTO или после удаления.
	bool ownsCard() const;
	// Требует принадлежащую странице карточку и полученный редактируемый DTO.
	bool ownsEditor() const;
	// Ключ необработанного ввода профиля включает ревизию исходной базы.
	using RawDraftKey = std::tuple<QString, QString, QString, QString, quint64, bool>;
	// Возвращает адрес необработанной формы editor, не смешивая ввод разных серверов и версий.
	RawDraftKey rawDraftKey(const ConfigurationEditorState &editor) const;
	// Необработанные поля остаются в RAM при навигации, включая невалидные числовые строки.
	std::map<RawDraftKey, QVariantMap> profileDrafts_;
	// owner_ задаёт QObject-контекст; state_ предоставляет кеш, controller_ выполняет намерения, text_ переводит подписи.
	ConfigurationEditor &owner_;
	StateStore &state_;
	ConfigurationController &controller_;
	TextLookup text_;
	// kind_ фиксирует вид страницы; selectedId_ хранит просмотр без изменения рабочего Stream.
	ConfigurationKind kind_;
	QString selectedId_;
	// loadedIdentity_ различает контекст/карточку/ревизию; updating_ подавляет вложенное отражение ввода.
	QString loadedIdentity_;
	// browsingContext_ запрещает перенос просматриваемого ID в другой сервер, OBS Profile или рабочий выбор.
	QString browsingContext_;
	// Последний просмотр каждой пары сервер/OBS Profile/рабочий Stream не переносится между контекстами.
	using BrowseKey = std::tuple<QString, QString, QString>;
	// browsingAddress_ фиксирует контекст последнего отображения, чтобы перед сменой сохранить его выбор.
	std::optional<BrowseKey> browsingAddress_;
	// contextSelections_ возвращает последнюю выбранную карточку при возврате в известный контекст.
	std::map<BrowseKey, QString> contextSelections_;
	bool updating_ = false;
	// profileDirty_ сохраняет наличие невалидного пользовательского ввода до исправления, не теряя сам ввод.
	bool profileDirty_ = false;
	// localError_ сохраняет локальную ошибку структурного ввода при обычных обновлениях SSE.
	QString localError_;
	// endpointValidationAttempted_ сохраняет видимость ошибки адреса до исправления или смены карточки.
	bool endpointValidationAttempted_ = false;
	// list_ содержит только ID и безопасные имена; form_ объединяет поля выбранной карточки.
	QListWidget *list_ = nullptr;
	QWidget *form_ = nullptr;
	// inputFields_ отключает только редактирование; результат и перечитывание доступны без DTO.
	QWidget *inputFields_ = nullptr;
	// status_ показывает полноту списка; message_ — безопасный результат/ошибки текущей карточки.
	QLabel *status_ = nullptr;
	QLabel *message_ = nullptr;
	// endpointRequiredError_ располагает ошибку обязательного адреса непосредственно под его полем.
	QLabel *endpointRequiredError_ = nullptr;
	// name_ и platform_ содержат обычный ввод; stream_/group_/profile_ выбирают точные ID отношений.
	QLineEdit *name_ = nullptr;
	QComboBox *platform_ = nullptr;
	QComboBox *stream_ = nullptr;
	QComboBox *group_ = nullptr;
	QComboBox *profile_ = nullptr;
	// destinationSelection_ выбирает участие собственных назначений Stream только в будущих запусках.
	QListWidget *destinationSelection_ = nullptr;
	// relationships_ показывает связи карточки либо неполноту списка назначений Stream.
	QLabel *relationships_ = nullptr;
	// firstSecret_ представляет publication_key/endpoint, secondSecret_ — stream_key назначения.
	SecretField *firstSecret_ = nullptr;
	SecretField *secondSecret_ = nullptr;
	// profileEditor_ редактирует полный рецепт только на странице Profile.
	ProfileEditor *profileEditor_ = nullptr;
	// create_, save_, remove_ и reload_ передают явные действия контроллеру конфигурации.
	QPushButton *create_ = nullptr;
	QPushButton *save_ = nullptr;
	QPushButton *remove_ = nullptr;
	QPushButton *reload_ = nullptr;
	// conflictBox_ и conflictLayout_ содержат локальное сравнение; resolution_ требует явного выбора полного профиля.
	QGroupBox *conflictBox_ = nullptr;
	QVBoxLayout *conflictLayout_ = nullptr;
	QComboBox *resolution_ = nullptr;
	// conflictIdentity_ предотвращает пересоздание сравнения при каждом событии телеметрии.
	QString conflictIdentity_;
};

ConfigurationEditor::Impl::Impl(ConfigurationEditor &owner, StateStore &state, ConfigurationController &controller,
				TextLookup text, ConfigurationKind kind)
	: owner_(owner),
	  state_(state),
	  controller_(controller),
	  text_(std::move(text)),
	  kind_(kind)
{
	// layout размещает список слева и прокручиваемую карточку справа; left содержит список и его действия.
	auto *layout = new QHBoxLayout(&owner_);
	auto *left = new QVBoxLayout;
	list_ = new QListWidget(&owner_);
	list_->setObjectName(QStringLiteral("configurationList"));
	list_->setAccessibleName(text_("Editor.Entities"));
	list_->setMinimumWidth(160);
	list_->setMaximumWidth(280);
	left->addWidget(list_, 1);
	status_ = new QLabel(&owner_);
	status_->setTextFormat(Qt::PlainText);
	status_->setWordWrap(true);
	left->addWidget(status_);
	create_ = new QPushButton(text_("Editor.Create"), &owner_);
	left->addWidget(create_);
	// refreshButton перечитывает список без автоматического отказа от dirty карточки.
	auto *refreshButton = new QPushButton(text_("Editor.RefreshList"), &owner_);
	left->addWidget(refreshButton);
	layout->addLayout(left);
	// scroll и cardLayout помещают длинную структурную форму в существующую страницу.
	auto *scroll = new QScrollArea(&owner_);
	form_ = new QWidget(scroll);
	auto *cardLayout = new QVBoxLayout(form_);
	// inputLayout содержит поля и отношения отдельно от сообщений и действий карточки.
	inputFields_ = new QWidget(form_);
	auto *inputLayout = new QVBoxLayout(inputFields_);
	inputLayout->setContentsMargins(0, 0, 0, 0);
	cardLayout->addWidget(inputFields_);
	scroll->setWidgetResizable(true);
	scroll->setWidget(form_);
	layout->addWidget(scroll, 1);
	// fields объединяет обычные свойства и отношения с локализованными подписями.
	auto *fields = new QFormLayout;
	fields->setRowWrapPolicy(QFormLayout::WrapLongRows);
	fields->setFieldGrowthPolicy(QFormLayout::AllNonFixedFieldsGrow);
	inputLayout->addLayout(fields);
	name_ = new QLineEdit(form_);
	name_->setMaxLength(std::numeric_limits<int>::max());
	name_->setObjectName(QStringLiteral("configurationName"));
	name_->setAccessibleName(text_("Editor.Name"));
	fields->addRow(text_("Editor.Name"), name_);
	// Обработчик имени не срабатывает при отражении setText и не включает profile в PATCH при переименовании.
	QObject::connect(name_, &QLineEdit::textEdited, &owner_, [this] { edited(QStringLiteral("name")); });
	if (kind_ == ConfigurationKind::Destination || kind_ == ConfigurationKind::Group) {
		stream_ = new QComboBox(form_);
		stream_->setObjectName(QStringLiteral("configurationStream"));
		fields->addRow(text_("Stream.Label"), stream_);
		// Пользовательский выбор сохраняет точный Stream ID; рабочая привязка OBS остаётся прежней.
		QObject::connect(stream_, &QComboBox::activated, &owner_, [this] {
			edited(QStringLiteral("stream_id"));
			relations();
		});
	}
	if (kind_ == ConfigurationKind::Destination) {
		group_ = new QComboBox(form_);
		group_->setObjectName(QStringLiteral("configurationGroup"));
		fields->addRow(text_("Editor.Group"), group_);
		// Обработчик адресует группу по itemData, а не отображаемому имени.
		QObject::connect(group_, &QComboBox::activated, &owner_,
				 [this] { edited(QStringLiteral("group_id")); });
		platform_ = new QComboBox(form_);
		platform_->addItem(QStringLiteral("YouTube"), QStringLiteral("youtube"));
		platform_->addItem(QStringLiteral("Twitch"), QStringLiteral("twitch"));
		platform_->addItem(text_("Editor.PlatformVk"), QStringLiteral("vk"));
		platform_->setObjectName(QStringLiteral("configurationPlatform"));
		fields->addRow(text_("Editor.Platform"), platform_);
		// ItemData сохраняет проверенные обозначения сервера; неизвестное значение добавляется при чтении.
		QObject::connect(platform_, &QComboBox::activated, &owner_,
				 [this] { edited(QStringLiteral("platform")); });
	}
	if (kind_ == ConfigurationKind::Group) {
		profile_ = new QComboBox(form_);
		profile_->setObjectName(QStringLiteral("configurationProfile"));
		fields->addRow(text_("Editor.Profile"), profile_);
		// Обработчик выбирает точный Profile ID для связи Group.
		QObject::connect(profile_, &QComboBox::activated, &owner_,
				 [this] { edited(QStringLiteral("profile_id")); });
	}
	if (kind_ == ConfigurationKind::Stream || kind_ == ConfigurationKind::Destination) {
		firstSecret_ = new SecretField(text_, form_);
		fields->addRow(text_(kind_ == ConfigurationKind::Stream ? "Editor.PublicationKey" : "Editor.Endpoint"),
			       firstSecret_);
		if (kind_ == ConfigurationKind::Destination) {
			endpointRequiredError_ = new QLabel(form_);
			endpointRequiredError_->setObjectName(QStringLiteral("configurationEndpointError"));
			endpointRequiredError_->setTextFormat(Qt::PlainText);
			endpointRequiredError_->setWordWrap(true);
			endpointRequiredError_->hide();
			fields->addRow(endpointRequiredError_);
		}
		// Секретное намерение хранится отдельно от серверного значения и фиксированной маски.
		QObject::connect(firstSecret_, &SecretField::changed, &owner_, [this] {
			edited(kind_ == ConfigurationKind::Stream ? QStringLiteral("publication_key")
								  : QStringLiteral("endpoint"));
		});
		if (kind_ == ConfigurationKind::Stream) {
			// generateKey выполняет генерацию лишь после явного нажатия в форме создания.
			auto *generateKey = new QPushButton(text_("Secret.Generate"), form_);
			generateKey->setObjectName(QStringLiteral("generatePublicationKey"));
			fields->addRow(generateKey);
			// Обработчик создаёт новый UUID-ключ только для ещё не созданного Stream.
			QObject::connect(generateKey, &QPushButton::clicked, &owner_, [this] {
				if (!ownsEditor() || !state_.configurationEditor().creating)
					return;
				firstSecret_->setReplacement(QUuid::createUuid().toString(QUuid::WithoutBraces));
				edited(QStringLiteral("publication_key"));
			});
		} else {
			secondSecret_ = new SecretField(text_, form_);
			fields->addRow(text_("Editor.StreamKey"), secondSecret_);
			// Обработчик позволяет явно очистить ключ пустой заменой; Missing не создаёт замены.
			QObject::connect(secondSecret_, &SecretField::changed, &owner_,
					 [this] { edited(QStringLiteral("stream_key")); });
		}
	}
	if (kind_ == ConfigurationKind::Profile) {
		profileEditor_ = new ProfileEditor(text_, form_);
		inputLayout->addWidget(profileEditor_);
		// Невалидный числовой ввод остаётся в форме, но не может быть отправлен save.
		QObject::connect(profileEditor_, &ProfileEditor::changed, &owner_,
				 [this] { edited(QStringLiteral("profile")); });
	}
	if (kind_ == ConfigurationKind::Stream) {
		// Список содержит имена и скрытые ID только собственных назначений; флаги отражаются без HTTP из виджета.
		destinationSelection_ = new QListWidget(form_);
		destinationSelection_->setObjectName(QStringLiteral("streamDestinations"));
		destinationSelection_->setAccessibleName(text_("Editor.StreamDestinations"));
		fields->addRow(text_("Editor.StreamDestinations"), destinationSelection_);
		// Callback передаёт ровно один флаг контроллеру, который проверяет полноту и принадлежность списка.
		QObject::connect(destinationSelection_, &QListWidget::itemChanged, &owner_,
				 [this](QListWidgetItem *item) {
					 if (!updating_ && ownsEditor())
						 controller_.setDestinationEnabled(item->data(Qt::UserRole).toString(),
										   item->checkState() == Qt::Checked);
				 });
	}
	relationships_ = new QLabel(form_);
	if (kind_ == ConfigurationKind::Stream)
		relationships_->setObjectName(QStringLiteral("streamDestinationsStatus"));
	relationships_->setTextFormat(Qt::PlainText);
	relationships_->setWordWrap(true);
	inputLayout->addWidget(relationships_);
	message_ = new QLabel(form_);
	message_->setTextFormat(Qt::PlainText);
	message_->setWordWrap(true);
	message_->setObjectName(QStringLiteral("configurationMessage"));
	cardLayout->addWidget(message_);
	// actions размещает явные действия карточки; они не исполняют HTTP непосредственно.
	auto *actions = new QHBoxLayout;
	save_ = new QPushButton(text_("Editor.Save"), form_);
	remove_ = new QPushButton(text_("Editor.Delete"), form_);
	reload_ = new QPushButton(text_("Editor.Reload"), form_);
	reload_->setObjectName(QStringLiteral("configurationRefresh"));
	reload_->setIcon(QIcon::fromTheme(QStringLiteral("view-refresh"),
					  owner_.style()->standardIcon(QStyle::SP_BrowserReload)));
	save_->setObjectName(QStringLiteral("configurationSave"));
	actions->addWidget(save_);
	actions->addWidget(remove_);
	actions->addWidget(reload_);
	cardLayout->addLayout(actions);
	// Явное сохранение проходит проверку полного текущего ввода.
	QObject::connect(save_, &QPushButton::clicked, &owner_, [this] { save(); });
	// Подтверждение удаления фиксирует текущую карточку; контроллер проверяет её исходную ревизию.
	QObject::connect(remove_, &QPushButton::clicked, &owner_, [this] {
		if (ownsEditor() && confirm("Editor.ConfirmDelete"))
			controller_.remove();
	});
	// Перечитывание после Unknown не является автоматическим повтором потерянного POST.
	QObject::connect(reload_, &QPushButton::clicked, &owner_, [this] {
		if (ownsCard())
			controller_.reload();
	});
	conflictBox_ = new QGroupBox(text_("Editor.Conflict"), form_);
	conflictLayout_ = new QVBoxLayout(conflictBox_);
	cardLayout->addWidget(conflictBox_);
	cardLayout->addStretch();
	// Выбор строки сохраняет прежний черновик в RAM перед сменой карточки; отказ возвращает прежний выбор.
	QObject::connect(list_, &QListWidget::currentItemChanged, &owner_, [this](QListWidgetItem *current) {
		if (updating_ || !current)
			return;
		// id фиксирует выбранную сущность до парковки прежнего черновика и открытия новой карточки.
		const auto id = current->data(Qt::UserRole).toString();
		if (id == selectedId_ && ownsEditor())
			return;
		// alive защищает обработчик, если синхронное уведомление StateStore удалило страницу.
		const QPointer<ConfigurationEditor> alive(&owner_);
		if (!owner_.canLeave(true)) {
			if (alive)
				refresh();
			return;
		}
		owner_.hideSecrets();
		// opened фиксирует результат до проверки живой страницы после синхронного сигнала StateStore.
		const bool opened = controller_.restoreDraft(kind_, id) || controller_.open(kind_, id, true);
		if (!alive)
			return;
		if (opened) {
			selectedId_ = id;
			emit owner_.selectionChanged();
		}
		if (!alive)
			return;
		refresh();
	});
	// Создание разрешает новый черновик после явного решения о прежнем вводе.
	QObject::connect(create_, &QPushButton::clicked, &owner_, [this] {
		// alive защищает обработчик и после диалога, и после публикации нового состояния контроллером.
		const QPointer<ConfigurationEditor> alive(&owner_);
		if (!owner_.canLeave())
			return;
		if (!alive)
			return;
		owner_.hideSecrets();
		// newCard адресует только прежний необработанный ввод формы создания данного контекста.
		auto newCard = state_.configurationEditor();
		newCard.id.clear();
		newCard.base.reset();
		newCard.creating = true;
		profileDrafts_.erase(rawDraftKey(newCard));
		// created фиксирует результат до обращения к членам потенциально удалённой страницы.
		const bool created = controller_.beginCreate(kind_, true);
		if (!alive)
			return;
		if (created) {
			selectedId_.clear();
			loadedIdentity_.clear();
			refresh();
			emit owner_.selectionChanged();
		}
	});
	// Обновление списка не заменяет редактируемую форму.
	QObject::connect(refreshButton, &QPushButton::clicked, &owner_, [this] { controller_.refresh(kind_); });
	// Подписки привязаны к owner_, поэтому уничтожение виджета отключает обработчики.
	QObject::connect(&state_, &StateStore::configurationEditorChanged, &owner_, [this] { refresh(); });
	QObject::connect(&state_, &StateStore::serverStateChanged, &owner_, [this] { refresh(); });
}

// Сопоставляет вид и логический контекст, не скрывая ошибку отсутствующего DTO.
bool ConfigurationEditor::Impl::ownsCard() const
{
	// editor и context сравнивают общую RAM-форму с текущим логическим выбором сервера/OBS Profile.
	const auto &editor = state_.configurationEditor();
	const auto &context = state_.dockContext();
	return editor.context.valid() && editor.kind == kind_ && editor.context.origin == state_.serverOrigin() &&
	       editor.context.profileUuid == context.profileUuid &&
	       editor.context.workingStreamId == state_.selectedStreamId();
}

// Разрешает работу с полями только после получения DTO принадлежащей странице карточки.
bool ConfigurationEditor::Impl::ownsEditor() const
{
	return ownsCard() && state_.configurationEditor().draft.has_value();
}
// Возвращает адрес необработанного профиля; revision исключает восстановление старого ввода после сохранения.
ConfigurationEditor::Impl::RawDraftKey
ConfigurationEditor::Impl::rawDraftKey(const ConfigurationEditorState &editor) const
{
	return {editor.context.origin,
		editor.context.profileUuid,
		editor.context.workingStreamId,
		editor.id,
		editor.base ? entityRevision(*editor.base) : 0,
		editor.creating};
}
// Подтверждает действие для неизменившейся карточки; вложенный цикл диалога не переносит ответ на новую цель.
bool ConfigurationEditor::Impl::confirm(const char *message)
{
	// alive отслеживает удаление страницы во вложенном цикле; before фиксирует точную цель подтверждения.
	const QPointer<ConfigurationEditor> alive(&owner_);
	const auto before = state_.configurationEditor();
	// generation различает также новые формы с теми же видом, пустым ID и набором изменённых полей.
	const auto generation = controller_.editorGeneration();
	// dialog принадлежит странице; QPointer замечает его удаление вместе с владельцем во время exec.
	QPointer<QMessageBox> dialog = new QMessageBox(QMessageBox::Question, text_("Operation.ConfirmTitle"),
						       text_(message), QMessageBox::Yes | QMessageBox::No, &owner_);
	dialog->setTextFormat(Qt::PlainText);
	dialog->setDefaultButton(QMessageBox::No);
	localizeConfirmation(*dialog, text_);
	// result фиксируется до удаления диалога; выбор не переносится на другую карточку или контекст.
	const int result = dialog->exec();
	if (dialog)
		delete dialog.data();
	if (!alive || result != QMessageBox::Yes)
		return false;
	// after содержит состояние после вложенного цикла, где могли пройти смена сервера и новые обработчики.
	const auto &after = state_.configurationEditor();
	return generation == controller_.editorGeneration() && before.context == after.context &&
	       before.kind == after.kind && before.id == after.id && before.creating == after.creating &&
	       before.dirtyFields == after.dirtyFields && !after.writing &&
	       (before.base ? entityRevision(*before.base) : 0) == (after.base ? entityRevision(*after.base) : 0) &&
	       (before.serverConflict ? entityRevision(*before.serverConflict) : 0) ==
		       (after.serverConflict ? entityRevision(*after.serverConflict) : 0);
}

void ConfigurationEditor::Impl::relation(QComboBox *combo, const QMap<QString, QString> &items, const QString &id,
					 bool complete)
{
	if (!combo)
		return;
	// wanted содержит весь ожидаемый список с точными ID; отсутствие неизвестной ссылки не превращается в пустой выбор.
	QList<QPair<QString, QString>> wanted;
	wanted.append({text_("Editor.Choose"), QString{}});
	// item содержит имя и идентичность одной полученной записи.
	for (auto item = items.cbegin(); item != items.cend(); ++item)
		wanted.append({item.value(), item.key()});
	if (!id.isEmpty() && !items.contains(id))
		wanted.append({text_(complete ? "Editor.ReferenceMissing" : "Editor.ReferenceUnknown").arg(id), id});
	// changed исключает перестройку раскрытого списка при обычной телеметрии.
	bool changed = combo->count() != wanted.size();
	// row сравнивает полный набор подписей и точных itemData.
	for (int row = 0; !changed && row < wanted.size(); ++row)
		changed = combo->itemText(row) != wanted.at(row).first ||
			  combo->itemData(row).toString() != wanted.at(row).second;
	// blocked не позволяет отражению отношения стать пользовательским изменением.
	const QSignalBlocker blocked(combo);
	if (changed) {
		combo->clear();
		// item содержит отображаемое имя и точный ID одной связи.
		for (const auto &item : wanted)
			combo->addItem(item.first, item.second);
	}
	combo->setCurrentIndex(qMax(0, combo->findData(id)));
}

void ConfigurationEditor::Impl::relations()
{
	if (!ownsEditor())
		return;
	// config содержит подтверждённые коллекции; draft читается копией до любых синхронных сигналов.
	const auto &config = state_.configuration();
	const auto draft = *state_.configurationEditor().draft;
	if (kind_ == ConfigurationKind::Stream) {
		// editor предоставляет только явные изменения выбора; blocked исключает обратные события отражения.
		const auto &editor = state_.configurationEditor();
		const QSignalBlocker blocked(destinationSelection_);
		// ids содержит только назначения открытого потока, независимо от доступности списка групп.
		QStringList ids;
		// item проходит полный известный кеш, но в список попадают только назначения открытого Stream.
		for (const auto &item : config.destinations)
			if (!editor.creating && item.streamId == editor.id)
				ids.append(item.id);
		// changed проверяет состав и подписи, чтобы телеметрия не сбрасывала фокус и прокрутку списка.
		bool changed = destinationSelection_->count() != ids.size();
		// row связывает видимый элемент с устойчивой идентичностью назначения.
		for (int row = 0; !changed && row < ids.size(); ++row)
			changed = destinationSelection_->item(row)->data(Qt::UserRole).toString() != ids.at(row) ||
				  destinationSelection_->item(row)->text() !=
					  config.destinations.value(ids.at(row)).name;
		if (changed) {
			destinationSelection_->clear();
			// id адресует собственное назначение; entry хранит его скрытый ID, не показывая техническую строку.
			for (const auto &id : ids) {
				auto *entry =
					new QListWidgetItem(config.destinations.value(id).name, destinationSelection_);
				entry->setData(Qt::UserRole, id);
			}
		}
		// row обновляет только состояние выбора и доступность одного пункта без изменения серверных данных.
		for (int row = 0; row < ids.size(); ++row) {
			// destination является подтверждённой записью; entry — принадлежащим списку элементом.
			const auto &destination = config.destinations[ids.at(row)];
			auto *entry = destinationSelection_->item(row);
			// selected учитывает несохранённое намерение раньше серверного флага.
			const bool selected = editor.destinationChanges.value(destination.id, destination.enabled);
			entry->setCheckState(selected ? Qt::Checked : Qt::Unchecked);
			entry->setFlags(
				Qt::ItemIsUserCheckable | Qt::ItemIsSelectable |
				(config.destinationsStatus.complete &&
						 config.destinationsStatus.availability == RuntimeAvailability::Fresh &&
						 !editor.loading && !editor.writing &&
						 !editor.destinationReconciliation.contains(destination.id)
					 ? Qt::ItemIsEnabled
					 : Qt::NoItemFlags));
		}
		relationships_->setText(
			config.destinationsStatus.complete ? QString{} : text_("Configuration.DestinationsIncomplete"));
		relationships_->setVisible(!config.destinationsStatus.complete);
		return;
	}
	// streams, groups и profiles сопоставляют видимые подписи точным серверным идентичностям.
	QMap<QString, QString> streams;
	QMap<QString, QString> groups;
	QMap<QString, QString> profiles;
	// item проходит кеш потоков без изменения текущей рабочей привязки.
	for (const auto &item : config.streams)
		streams[item.id] = item.name;
	// item проходит кеш профилей для выбора связи Group.
	for (const auto &item : config.profiles)
		profiles[item.id] = item.name;
	// streamId содержит принадлежность текущего draft; related перечисляет известные обратные связи.
	QString streamId;
	QStringList related;
	// destination и group извлекают принадлежность текущего типизированного draft.
	if (const auto *destination = std::get_if<api::Destination>(&draft))
		streamId = destination->streamId;
	if (const auto *group = std::get_if<api::Group>(&draft))
		streamId = group->streamId;
	// item фильтрует группы по точному выбранному Stream; чужие группы не предлагаются.
	for (const auto &item : config.groups)
		if (item.streamId == streamId)
			groups[item.id] = item.name;
	relation(stream_, streams, streamId, config.streamsStatus.complete);
	// destination и group предоставляют точные ID текущих отношений для отражения селекторов.
	if (const auto *destination = std::get_if<api::Destination>(&draft))
		relation(group_, groups, destination->groupId, config.groupsStatus.complete);
	if (const auto *group = std::get_if<api::Group>(&draft))
		relation(profile_, profiles, group->profileId, config.profilesStatus.complete);
	// Карточка группы задаёт связи селекторами; обратный список назначений в ней не отображается.
	if (kind_ == ConfigurationKind::Group) {
		relationships_->clear();
		relationships_->hide();
		return;
	}
	// item показывает группы текущего профиля; имена выводятся только обычным текстом.
	for (const auto &item : config.groups)
		if (kind_ == ConfigurationKind::Profile && item.profileId == selectedId_)
			related.append(text_("Editor.GroupRelation").arg(item.name, item.id));
	if (!config.groupsStatus.complete || !config.destinationsStatus.complete)
		related.append(text_("Editor.RelationsIncomplete"));
	relationships_->setText(related.join(QLatin1Char('\n')));
}

void ConfigurationEditor::Impl::populate(const ConfigurationEditorState &editor)
{
	if (!editor.draft)
		return;
	updating_ = true;
	profileDirty_ = false;
	endpointValidationAttempted_ = false;
	localError_.clear();
	name_->setText(entityName(*editor.draft));
	// stream содержит карточку публикации, чей серверный ключ нельзя менять после создания.
	if (const auto *stream = std::get_if<api::Stream>(&*editor.draft)) {
		firstSecret_->setReceived(editor.base ? std::get<api::Stream>(*editor.base).publicationKey
						      : api::JsonField<QString>{});
		firstSecret_->setEditable(editor.creating);
		firstSecret_->setReplacement(editor.creating && stream->publicationKey.hasValue()
						     ? std::optional<QString>(*stream->publicationKey)
						     : std::nullopt);
		// generate доступна только в карточке создания; существующий ключ серверного Stream неизменяем.
		if (auto *generate = form_->findChild<QPushButton *>(QStringLiteral("generatePublicationKey")))
			generate->setVisible(editor.creating);
	}
	// destination содержит точные секретные строки и обычные свойства назначения.
	if (const auto *destination = std::get_if<api::Destination>(&*editor.draft)) {
		if (platform_->findData(destination->platform) < 0)
			platform_->addItem(destination->platform, destination->platform);
		platform_->setCurrentIndex(platform_->findData(destination->platform));
		// base содержит только полученные серверные значения; новый ввод остаётся отдельным намерением.
		const auto base = editor.base ? std::get<api::Destination>(*editor.base) : api::Destination{};
		firstSecret_->setReceived(base.endpoint);
		secondSecret_->setReceived(base.streamKey, true);
		firstSecret_->setEditable(true);
		secondSecret_->setEditable(true);
		firstSecret_->setReplacement(
			(editor.creating || editor.dirtyFields.contains(QStringLiteral("endpoint"))) &&
					destination->endpoint.hasValue()
				? std::optional<QString>(*destination->endpoint)
				: std::nullopt);
		secondSecret_->setReplacement(
			(editor.creating || editor.dirtyFields.contains(QStringLiteral("stream_key"))) &&
					destination->streamKey.hasValue()
				? std::optional<QString>(*destination->streamKey)
				: std::nullopt);
	}
	updateEndpointRequiredError(editor);
	// profile предоставляет полный рецепт без преобразования в промежуточный JSON.
	if (const auto *profile = std::get_if<api::Profile>(&*editor.draft)) {
		profileEditor_->setProfile(profile->profile, editor.creating);
		// raw содержит точный ранее оставленный ввод, который не обязательно преобразуется в допустимый DTO.
		const auto raw = profileDrafts_.find(rawDraftKey(editor));
		if (raw != profileDrafts_.end()) {
			profileEditor_->restoreDraft(raw->second);
			profileDirty_ = editor.dirtyFields.contains(QStringLiteral("profile"));
			// checked нужен только для безопасного восстановления локального сообщения невалидного поля.
			auto checked = profile->profile;
			profileEditor_->readProfile(checked, localError_);
		}
	}
	relations();
	updating_ = false;
}

void ConfigurationEditor::Impl::refresh()
{
	if (updating_)
		return;
	updating_ = true;
	// config и editor представляют независимые серверный кеш и пользовательский draft.
	const auto &config = state_.configuration();
	const auto &editor = state_.configurationEditor();
	// contextIdentity изменяется вместе с логическим контекстом, не с обычной телеметрией.
	const auto contextIdentity = state_.serverOrigin() + QLatin1Char('\n') + state_.dockContext().profileUuid +
				     QLatin1Char('\n') + state_.selectedStreamId();
	if (browsingContext_ != contextIdentity) {
		if (profileEditor_ && !loadedIdentity_.isEmpty() && editor.kind == kind_ && editor.draft)
			profileDrafts_.insert_or_assign(rawDraftKey(editor), profileEditor_->captureDraft());
		if (browsingAddress_)
			contextSelections_.insert_or_assign(*browsingAddress_, selectedId_);
		browsingAddress_ =
			BrowseKey{state_.serverOrigin(), state_.dockContext().profileUuid, state_.selectedStreamId()};
		browsingContext_ = contextIdentity;
		// previous возвращает лишь выбор этого контекста; неизвестный адрес начинает с новой карточки.
		const auto previous = contextSelections_.find(*browsingAddress_);
		selectedId_ = previous == contextSelections_.end() ? QString{} : previous->second;
		loadedIdentity_.clear();
		localError_.clear();
		endpointValidationAttempted_ = false;
		if (endpointRequiredError_) {
			endpointRequiredError_->clear();
			endpointRequiredError_->hide();
		}
		owner_.hideSecrets();
	}
	// names и complete выбираются только по виду текущей страницы.
	QMap<QString, QString> names;
	bool complete = false;
	// item в каждой ветви — одна запись выбранной коллекции; другие коллекции не подменяют её полноту.
	switch (kind_) {
	case ConfigurationKind::Stream:
		for (const auto &item : config.streams)
			names[item.id] = item.name;
		complete = config.streamsStatus.complete;
		break;
	case ConfigurationKind::Destination:
		for (const auto &item : config.destinations)
			names[item.id] = item.name;
		complete = config.destinationsStatus.complete;
		break;
	case ConfigurationKind::Group:
		for (const auto &item : config.groups)
			names[item.id] = item.name;
		complete = config.groupsStatus.complete;
		break;
	case ConfigurationKind::Profile:
		for (const auto &item : config.profiles)
			names[item.id] = item.name;
		complete = config.profilesStatus.complete;
		break;
	}
	if (ownsCard())
		selectedId_ = editor.id;
	// listChanged сохраняет раскрытие и выбор при обновлениях несвязанных метрик.
	bool listChanged = list_->count() != names.size();
	// row и item сверяют полные видимые имена и точные ID строк.
	int row = 0;
	for (auto item = names.cbegin(); !listChanged && item != names.cend(); ++item, ++row)
		listChanged = list_->item(row)->text() != item.value() ||
			      list_->item(row)->data(Qt::UserRole).toString() != item.key();
	if (listChanged) {
		list_->clear();
		// item содержит безопасное имя и ID одной серверной записи.
		for (auto item = names.cbegin(); item != names.cend(); ++item) {
			// entry принадлежит list_ и хранит адрес сущности независимо от её имени.
			auto *entry = new QListWidgetItem(item.value(), list_);
			entry->setData(Qt::UserRole, item.key());
		}
	}
	// selectedRow восстанавливает точный ID либо снимает прежний выбор при открытии новой карточки.
	int selectedRow = -1;
	// index ищет строку текущей карточки; отсутствие совпадения не оставляет выделенной другую запись.
	for (int index = 0; index < list_->count(); ++index) {
		if (list_->item(index)->data(Qt::UserRole).toString() == selectedId_) {
			selectedRow = index;
			break;
		}
	}
	list_->setCurrentRow(selectedRow);
	status_->setText(text_(complete ? "Editor.ListComplete" : "Editor.ListIncomplete").arg(names.size()));
	create_->setEnabled(!editor.writing);
	// hasCard сохраняет сообщения без DTO; editable допускает изменение лишь полученной свободной формы.
	const bool hasCard = ownsCard();
	const bool editable = ownsEditor() && !editor.loading && !editor.writing;
	form_->setEnabled(hasCard);
	inputFields_->setVisible(ownsEditor());
	inputFields_->setEnabled(editable);
	if (ownsEditor()) {
		// identity меняется только вместе с контекстом/видом/ID/ревизией, а не при обычном SSE.
		const auto identity = editor.context.origin + QLatin1Char('\n') + editor.context.profileUuid +
				      QLatin1Char('\n') + editor.context.workingStreamId + QLatin1Char('\n') +
				      editor.id + QLatin1Char('\n') +
				      QString::number(editor.base ? entityRevision(*editor.base) : 0) +
				      (editor.creating ? QStringLiteral("new") : QString{});
		if (loadedIdentity_ != identity) {
			loadedIdentity_ = identity;
			populate(editor);
		}
		relations();
	} else {
		loadedIdentity_.clear();
		localError_.clear();
		endpointValidationAttempted_ = false;
		if (endpointRequiredError_) {
			endpointRequiredError_->clear();
			endpointRequiredError_->hide();
		}
		owner_.hideSecrets();
	}
	if (hasCard) {
		// messages содержит только безопасные локализованные ключи и привязанные к полям пояснения.
		QStringList messages;
		if (!localError_.isEmpty())
			messages.append(localError_);
		if (!editor.messageKey.isEmpty())
			messages.append(text_(editor.messageKey.toUtf8().constData()));
		if (!editor.destinationMessageKey.isEmpty())
			messages.append(text_(editor.destinationMessageKey.toUtf8().constData()));
		// outcome отображает итог отдельного назначения по безопасному имени, сохраняя различие Unknown и успеха.
		for (auto outcome = editor.destinationOutcomes.cbegin(); outcome != editor.destinationOutcomes.cend();
		     ++outcome) {
			// key сопоставляет результат известной локализованной подписи без серверной диагностики.
			const char *key = outcome.value() == ConfigurationOutcome::Saved      ? "Configuration.Saved"
					  : outcome.value() == ConfigurationOutcome::Conflict ? "Configuration.Conflict"
					  : outcome.value() == ConfigurationOutcome::Unknown  ? "Configuration.Unknown"
											     : "Configuration.Rejected";
			messages.append(
				text_("Editor.DestinationResult")
					.arg(editor.destinationNames.value(outcome.key(), text_("Dock.UnnamedOutput")),
					     text_(key)));
		}
		// issue содержит очищенное контроллером пояснение без request value и сырых тел ответа.
		for (auto issue = editor.fieldIssues.cbegin(); issue != editor.fieldIssues.cend(); ++issue)
			messages.append(
				text_("Editor.FieldIssue").arg(issue.key(), text_(issue.value().toUtf8().constData())));
		message_->setText(messages.join(QLatin1Char('\n')));
	} else {
		message_->clear();
	}
	save_->setEnabled(editable && !editor.reconciliationRequired && !editor.serverConflict &&
			  editor.destinationReconciliation.isEmpty() &&
			  (editor.creating || !editor.dirtyFields.isEmpty() || !editor.destinationChanges.isEmpty()));
	remove_->setEnabled(editable && !editor.creating && editor.base.has_value() && !editor.reconciliationRequired &&
			    !editor.serverConflict);
	reload_->setEnabled(hasCard && !editor.loading && !editor.writing &&
			    (!editor.creating || editor.reconciliationRequired));
	conflict();
	updating_ = false;
}

void ConfigurationEditor::Impl::edited(const QString &field)
{
	if (updating_ || !ownsEditor())
		return;
	// editor копируется до setDraft, чей сигнал может синхронно вызвать refresh.
	const auto editor = state_.configurationEditor();
	if (editor.writing)
		return;
	// draft и dirty описывают только текущую карточку и явные JSON-поля PATCH.
	auto draft = *editor.draft;
	auto dirty = editor.dirtyFields;
	dirty.insert(field);
	setEntityName(draft, name_->text());
	// stream допускает ввод publication_key только до создания записи.
	if (auto *stream = std::get_if<api::Stream>(&draft)) {
		if (editor.creating) {
			// replacement отделяет отсутствие ввода от точной новой строки ключа.
			const auto replacement = firstSecret_->replacement();
			stream->publicationKey.state = replacement ? api::FieldState::Value : api::FieldState::Missing;
			stream->publicationKey.value = replacement.value_or(QString{});
		}
	}
	// destination собирает обычные свойства и отдельные намерения секретных полей.
	if (auto *destination = std::get_if<api::Destination>(&draft)) {
		destination->platform = platform_->currentData().toString();
		destination->streamId = stream_->currentData().toString();
		destination->groupId = group_->currentData().toString();
		// endpoint и key содержат явные изменения; отсутствие намерения исключает поле из PATCH.
		const auto endpoint = firstSecret_->replacement();
		const auto key = secondSecret_->replacement();
		if (endpoint) {
			destination->endpoint = {api::FieldState::Value, *endpoint};
			dirty.insert(QStringLiteral("endpoint"));
		} else {
			dirty.remove(QStringLiteral("endpoint"));
			if (editor.creating)
				destination->endpoint = {};
		}
		if (key) {
			destination->streamKey = {api::FieldState::Value, *key};
			dirty.insert(QStringLiteral("stream_key"));
		} else {
			dirty.remove(QStringLiteral("stream_key"));
			if (editor.creating)
				destination->streamKey = {};
		}
	}
	// group содержит выбранные ID Stream/Profile, без поиска по отображаемому имени.
	if (auto *group = std::get_if<api::Group>(&draft)) {
		group->streamId = stream_->currentData().toString();
		group->profileId = profile_->currentData().toString();
	}
	// profile получает весь рецепт только при изменении структурной формы.
	if (auto *profile = std::get_if<api::Profile>(&draft); profile && field == QStringLiteral("profile")) {
		profileDirty_ = true;
		// error относится к локальной валидации, без сериализации профиля или серверных секретов.
		QString error;
		profileEditor_->readProfile(profile->profile, error);
		localError_ = error;
		message_->setText(localError_);
	}
	updating_ = true;
	// alive учитывает синхронное уничтожение страницы потребителем нового состояния формы.
	const QPointer<ConfigurationEditor> alive(&owner_);
	controller_.setDraft(std::move(draft), std::move(dirty));
	if (!alive)
		return;
	updating_ = false;
	// updatedEditor отражает dirty-намерение, опубликованное синхронно обработчиком формы.
	const auto updatedEditor = state_.configurationEditor();
	updateEndpointRequiredError(updatedEditor);
	// Доступность действий меняется без populate, сохраняя невалидный текст в форме.
	save_->setEnabled(!editor.reconciliationRequired && !editor.serverConflict &&
			  editor.destinationReconciliation.isEmpty());
	if (editor.serverConflict) {
		conflictIdentity_.clear();
		conflict();
	}
}

bool ConfigurationEditor::Impl::endpointRequiredButEmpty(const ConfigurationEditorState &editor) const
{
	if (!editor.draft || editor.kind != ConfigurationKind::Destination)
		return false;
	// destination задаёт точные секретные поля карточки, включая недоступный старый endpoint.
	const auto *destination = std::get_if<api::Destination>(&*editor.draft);
	if (!destination)
		return false;
	// required отличает создание или явную замену от несвязанного редактирования старой записи.
	const bool required = editor.creating || editor.dirtyFields.contains(QStringLiteral("endpoint"));
	if (!required)
		return false;
	// replacement содержит пользовательский ввод; отсутствие значения у старой карточки не создаёт намерение.
	const auto replacement = firstSecret_->replacement();
	if (replacement)
		return replacement->isEmpty();
	// received позволяет проверить существующее поле, не принимая маску/placeholder за введённый текст.
	const auto &received = firstSecret_->received();
	return !received.hasValue() || received->isEmpty() || (editor.creating && !destination->endpoint.hasValue());
}

void ConfigurationEditor::Impl::updateEndpointRequiredError(const ConfigurationEditorState &editor)
{
	if (!endpointRequiredError_)
		return;
	// visible оставляет подсказку скрытой до ошибки и снимает её сразу после исправления адреса.
	const bool visible = endpointValidationAttempted_ && endpointRequiredButEmpty(editor);
	endpointRequiredError_->setText(visible ? text_("Editor.EndpointRequired") : QString{});
	endpointRequiredError_->setVisible(visible);
}

void ConfigurationEditor::Impl::save()
{
	if (!ownsEditor())
		return;
	// editor фиксирует текущую карточку до синхронного обновления draft.
	const auto editor = state_.configurationEditor();
	if (editor.writing || editor.reconciliationRequired || editor.serverConflict)
		return;
	endpointValidationAttempted_ = true;
	if (endpointRequiredButEmpty(editor)) {
		updateEndpointRequiredError(editor);
		return;
	}
	updateEndpointRequiredError(editor);
	// destination проверяет связь выбранной группы до передачи намерения записи контроллеру.
	if (const auto *destination = std::get_if<api::Destination>(&*editor.draft);
	    destination && (editor.creating || editor.dirtyFields.contains(QStringLiteral("stream_id")) ||
			    editor.dirtyFields.contains(QStringLiteral("group_id")))) {
		// group содержит только подтверждённую серверную группу, выбранную по ID.
		const auto group = state_.configuration().groups.constFind(destination->groupId);
		if (group == state_.configuration().groups.cend() || group->streamId != destination->streamId) {
			localError_ = text_("Editor.GroupStreamMismatch");
			message_->setText(localError_);
			return;
		}
	}
	if (profileEditor_ &&
	    (editor.creating || profileDirty_ || editor.dirtyFields.contains(QStringLiteral("profile")))) {
		// draft содержит полный исходный профиль и имя; error описывает локально невалидное поле.
		auto draft = *editor.draft;
		QString error;
		if (!profileEditor_->readProfile(std::get<api::Profile>(draft).profile, error)) {
			localError_ = error;
			message_->setText(error);
			return;
		}
		// dirty отправляет profile целиком только после действительного изменения либо create.
		auto dirty = editor.dirtyFields;
		dirty.insert(QStringLiteral("profile"));
		updating_ = true;
		// alive и accepted проверяются до доступа к странице после синхронной публикации draft.
		const QPointer<ConfigurationEditor> alive(&owner_);
		const bool accepted = controller_.setDraft(std::move(draft), std::move(dirty));
		if (!alive)
			return;
		updating_ = false;
		if (!accepted)
			return;
		localError_.clear();
	}
	controller_.save();
}

void ConfigurationEditor::Impl::conflict()
{
	// editor фиксирует снимок конфликта на период построения PlainText-сравнения.
	const auto editor = state_.configurationEditor();
	// visible разрешает сравнение только собственной карточки с тремя доступными версиями.
	const bool visible = ownsEditor() && editor.base && editor.draft && editor.serverConflict;
	conflictBox_->setVisible(visible);
	if (!visible) {
		conflictIdentity_.clear();
		return;
	}
	// identity включает обе ревизии; повторный 409 строит новое сравнение.
	const auto identity = editor.id + QString::number(entityRevision(*editor.base)) + QLatin1Char('/') +
			      QString::number(entityRevision(*editor.serverConflict));
	if (identity == conflictIdentity_)
		return;
	conflictIdentity_ = identity;
	resolution_ = nullptr;
	// item удаляет только прежние элементы этого локального сравнения.
	while (auto *item = conflictLayout_->takeAt(0)) {
		delete item->widget();
		delete item;
	}
	// tabs разделяет обычные поля, секреты и полный профиль внутри одной существующей страницы.
	auto *tabs = new QTabWidget(conflictBox_);
	conflictLayout_->addWidget(tabs);
	// table хранит только обычные значения, без полного DTO и секретных полей.
	auto *table = new QTableWidget(0, 4, tabs);
	table->setHorizontalHeaderLabels(
		{text_("Editor.Field"), text_("Editor.Base"), text_("Editor.Mine"), text_("Editor.Server")});
	table->horizontalHeader()->setSectionResizeMode(QHeaderView::ResizeToContents);
	table->setEditTriggers(QAbstractItemView::NoEditTriggers);
	tabs->addTab(table, text_("Editor.Compare"));
	// field проходит только явно изменённые JSON-поля, секреты показываются отдельными маскированными виджетами.
	for (const auto &field : editor.dirtyFields) {
		if (field == QStringLiteral("profile"))
			continue;
		if (field == QStringLiteral("endpoint") || field == QStringLiteral("stream_key")) {
			// secretPage объединяет версии секретной строки; secretLayout размещает их с подписями источников.
			auto *secretPage = new QWidget(tabs);
			auto *secretLayout = new QFormLayout(secretPage);
			// source выбирает base/mine/server; каждая строка получает только одно поле, не сериализацию DTO.
			for (int source = 0; source < 3; ++source) {
				// record содержит одну версию Destination; secret показывает точное поле без нормализации адреса.
				const auto &record = std::get<api::Destination>(source == 0   ? *editor.base
										: source == 1 ? *editor.draft
											      : *editor.serverConflict);
				auto *secret = new SecretField(text_, secretPage);
				secret->setReceived(field == QStringLiteral("endpoint") ? record.endpoint
											: record.streamKey,
						    field == QStringLiteral("stream_key"));
				secretLayout->addRow(text_(source == 0   ? "Editor.Base"
							   : source == 1 ? "Editor.Mine"
									 : "Editor.Server"),
						     secret);
			}
			tabs->addTab(secretPage, text_(field == QStringLiteral("endpoint") ? "Editor.Endpoint"
											   : "Editor.StreamKey"));
			continue;
		}
		// row адресует новую строку сравнения; QTableWidgetItem всегда показывает обычный текст.
		const int row = table->rowCount();
		table->insertRow(row);
		table->setItem(row, 0, new QTableWidgetItem(field));
		table->setItem(row, 1, new QTableWidgetItem(ordinaryValue(*editor.base, field)));
		table->setItem(row, 2, new QTableWidgetItem(ordinaryValue(*editor.draft, field)));
		table->setItem(row, 3, new QTableWidgetItem(ordinaryValue(*editor.serverConflict, field)));
	}
	if (editor.dirtyFields.contains(QStringLiteral("profile"))) {
		// source создаёт прокручиваемую структурную версию профиля без необработанного JSON и FFmpeg CLI.
		for (int source = 0; source < 3; ++source) {
			// scroll содержит независимое представление base/mine/server, view отражает полный GroupProfile.
			auto *scroll = new QScrollArea(tabs);
			auto *view = new ProfileEditor(text_, scroll);
			view->setProfile(std::get<api::Profile>(source == 0   ? *editor.base
								: source == 1 ? *editor.draft
									      : *editor.serverConflict)
						 .profile);
			view->setEnabled(false);
			scroll->setWidget(view);
			scroll->setWidgetResizable(true);
			tabs->addTab(scroll, text_(source == 0   ? "Editor.Base"
						   : source == 1 ? "Editor.Mine"
								 : "Editor.Server"));
		}
		resolution_ = new QComboBox(conflictBox_);
		resolution_->addItem(text_("Editor.ChooseProfileResolution"), 0);
		resolution_->addItem(text_("Editor.UseMineProfile"), 1);
		resolution_->addItem(text_("Editor.UseServerProfile"), 2);
		conflictLayout_->addWidget(resolution_);
	}
	// accept запрашивает повторную запись на новой ревизии; обработчик clicked открывает явное разрешение конфликта.
	auto *accept = new QPushButton(text_("Editor.ConfirmConflict"), conflictBox_);
	conflictLayout_->addWidget(accept);
	QObject::connect(accept, &QPushButton::clicked, &owner_, [this] { resolveConflict(); });
}

void ConfigurationEditor::Impl::resolveConflict()
{
	// editor сохраняет точную пару конфликтующих версий до подтверждения пользователя.
	const auto editor = state_.configurationEditor();
	if (!ownsEditor() || !editor.serverConflict || !editor.draft)
		return;
	if (resolution_ && resolution_->currentData().toInt() == 0) {
		message_->setText(text_("Editor.ChooseProfileResolution"));
		return;
	}
	if (!confirm("Editor.ConfirmConflictWrite"))
		return;
	// resolved начинается с свежего сервера; dirty определяет только явно изменённые пользователем поля.
	auto resolved = *editor.serverConflict;
	auto dirty = editor.dirtyFields;
	if (dirty.contains(QStringLiteral("name")))
		setEntityName(resolved, entityName(*editor.draft));
	// destination начинается со свежей версии сервера; изменённые поля заменяются ниже по одному.
	if (auto *destination = std::get_if<api::Destination>(&resolved)) {
		// mine содержит полный пользовательский ввод, но копируются только отмеченные JSON-поля.
		const auto &mine = std::get<api::Destination>(*editor.draft);
		if (dirty.contains(QStringLiteral("stream_id")))
			destination->streamId = mine.streamId;
		if (dirty.contains(QStringLiteral("group_id")))
			destination->groupId = mine.groupId;
		if (dirty.contains(QStringLiteral("platform")))
			destination->platform = mine.platform;
		if (dirty.contains(QStringLiteral("enabled")))
			destination->enabled = mine.enabled;
		if (dirty.contains(QStringLiteral("endpoint")))
			destination->endpoint = mine.endpoint;
		if (dirty.contains(QStringLiteral("stream_key")))
			destination->streamKey = mine.streamKey;
	}
	// group сохраняет все свежие поля, которых пользователь не изменял.
	if (auto *group = std::get_if<api::Group>(&resolved)) {
		// mine содержит пользовательские ID связей; свежие неизменённые поля остаются серверными.
		const auto &mine = std::get<api::Group>(*editor.draft);
		if (dirty.contains(QStringLiteral("stream_id")))
			group->streamId = mine.streamId;
		if (dirty.contains(QStringLiteral("profile_id")))
			group->profileId = mine.profileId;
	}
	// profile заменяет полный рецепт только после явного выбора и повторной локальной валидации ввода.
	if (auto *profile = std::get_if<api::Profile>(&resolved); profile && resolution_) {
		if (resolution_->currentData().toInt() == 1) {
			// error описывает неверное поле структурной формы, не раскрывая секретных DTO.
			QString error;
			if (!profileEditor_->readProfile(profile->profile, error)) {
				localError_ = error;
				message_->setText(error);
				return;
			}
		} else
			dirty.remove(QStringLiteral("profile"));
	}
	// alive и accepted защищают продолжение после синхронного уведомления новой серверной базы.
	const QPointer<ConfigurationEditor> alive(&owner_);
	const bool accepted = controller_.confirmConflict(std::move(resolved), std::move(dirty));
	if (!alive)
		return;
	if (accepted) {
		loadedIdentity_.clear();
		refresh();
		if (!alive)
			return;
		controller_.save();
	}
}

// Создаёт представление данного вида конфигурации, заимствуя общий контроллер и StateStore.
ConfigurationEditor::ConfigurationEditor(StateStore &state, ConfigurationController &controller, TextLookup text,
					 ConfigurationKind kind, QWidget *parent)
	: QWidget(parent),
	  impl_(std::make_unique<Impl>(*this, state, controller, std::move(text), kind))
{
	impl_->refresh();
}
// Удаляет привязки страницы; общие RAM-карточки продолжает хранить контроллер.
ConfigurationEditor::~ConfigurationEditor() = default;

// Открывает последнюю карточку данной страницы из RAM либо серверного GET без потери оставленного ввода.
void ConfigurationEditor::activate()
{
	// alive замечает удаление страницы при подтверждении или синхронном уведомлении общего состояния.
	const QPointer<ConfigurationEditor> alive(this);
	hideSecrets();
	if (!impl_->ownsCard()) {
		if (!canLeave(true))
			return;
		if (impl_->controller_.restoreDraft(impl_->kind_, impl_->selectedId_)) {
			// Восстановленная карточка уже опубликована контроллером; сетевое открытие не требуется.
		} else if (!impl_->selectedId_.isEmpty())
			impl_->controller_.open(impl_->kind_, impl_->selectedId_, true);
		else
			impl_->controller_.beginCreate(impl_->kind_, true);
		if (!alive)
			return;
	}
	impl_->controller_.refresh(impl_->kind_);
	if (!alive)
		return;
	// Связи читаются как полные коллекции; фильтрация UI не удаляет записи общего кеша.
	if (impl_->kind_ != ConfigurationKind::Stream)
		impl_->controller_.refresh(ConfigurationKind::Stream);
	if (!alive)
		return;
	if (impl_->kind_ != ConfigurationKind::Group)
		impl_->controller_.refresh(ConfigurationKind::Group);
	if (!alive)
		return;
	if (impl_->kind_ != ConfigurationKind::Profile)
		impl_->controller_.refresh(ConfigurationKind::Profile);
	if (!alive)
		return;
	if (impl_->kind_ != ConfigurationKind::Destination)
		impl_->controller_.refresh(ConfigurationKind::Destination);
	if (!alive)
		return;
	impl_->refresh();
}
// Разрешает переход после RAM-парковки или явного отказа от ввода; занятая форма не переключается.
bool ConfigurationEditor::canLeave(bool preserveDraft)
{
	// editor фиксирует текущий глобальный draft: входящая страница тоже не может молча его потерять.
	const auto editor = impl_->state_.configurationEditor();
	if (editor.loading || editor.writing)
		return false;
	if (preserveDraft) {
		if (editor.reconciliationRequired && impl_->ownsCard())
			return false;
		if (impl_->profileEditor_ && impl_->ownsEditor())
			impl_->profileDrafts_.insert_or_assign(impl_->rawDraftKey(editor),
							       impl_->profileEditor_->captureDraft());
		return impl_->controller_.parkDraft();
	}
	if (editor.dirtyFields.isEmpty() && editor.destinationChanges.isEmpty())
		return true;
	// generation связывает решение пользователя с этим вводом, а не новой формой из вложенного цикла.
	const auto generation = impl_->controller_.editorGeneration();
	// alive защищает impl_ после вложенного цикла подтверждения и синхронной публикации сброса draft.
	const QPointer<ConfigurationEditor> alive(this);
	if (!impl_->confirm("Editor.DiscardDraft"))
		return false;
	if (!alive)
		return false;
	if (editor.draft) {
		// accepted подтверждает фактический сброс dirtyFields; отказ не разрешает переход.
		const bool accepted = impl_->controller_.discardDraft(generation);
		if (!alive || !accepted)
			return false;
	}
	impl_->loadedIdentity_.clear();
	impl_->profileDrafts_.erase(impl_->rawDraftKey(editor));
	return true;
}
void ConfigurationEditor::hideSecrets()
{
	if (!impl_)
		return;
	// field включает основные и конфликтные секреты; каждый возвращается к маске независимо.
	for (auto *field : findChildren<SecretField *>())
		field->hideSecret();
}
QString ConfigurationEditor::selectedDestinationId() const
{
	return impl_->kind_ == ConfigurationKind::Destination && impl_->ownsEditor() &&
			       !impl_->state_.configurationEditor().creating
		       ? impl_->state_.configurationEditor().id
		       : QString{};
}

} // namespace gost
