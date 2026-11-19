#include "fallback-policy-editor.hpp"
#include "confirmation-dialog.hpp"
#include "core/fallback-controller.hpp"
#include "core/state-store.hpp"
#include "status-label.hpp"
#include <QComboBox>
#include <QFormLayout>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QLabel>
#include <QIcon>
#include <QStyle>
#include <QMessageBox>
#include <QPointer>
#include <QPushButton>
#include <QSignalBlocker>
#include <QVBoxLayout>
#include <utility>

namespace gost {
namespace {
// Возвращает подпись результата записи; последующее чтение не превращает Unknown в успех.
const char *writeOutcomeKey(FallbackWriteOutcome outcome)
{
	switch (outcome) {
	case FallbackWriteOutcome::Pending:
		return "Fallback.Sending";
	case FallbackWriteOutcome::Saved:
		return "Configuration.Saved";
	case FallbackWriteOutcome::Deleted:
		return "Configuration.Deleted";
	case FallbackWriteOutcome::Accepted:
		return "Fallback.Accepted";
	case FallbackWriteOutcome::Rejected:
		return "Configuration.Rejected";
	case FallbackWriteOutcome::Conflict:
		return "Configuration.Conflict";
	case FallbackWriteOutcome::Unknown:
		return "Configuration.Unknown";
	case FallbackWriteOutcome::NotSent:
		return "Fallback.NotSent";
	case FallbackWriteOutcome::None:
		return "Fallback.NoWrite";
	}
	return "Fallback.NoWrite";
}
} // namespace

/** Отражает политику явной цели; только active_ позволяет адресовать общий контроллер.
 * Переключение цели паркует ввод в контроллере, а скрытая копия не запускает чтений. */
class FallbackPolicyEditor::Impl {
public:
	// Создаёт дочернюю форму owner с заимствованными state/controller и переводами text.
	Impl(FallbackPolicyEditor &owner, StateStore &state, FallbackController &controller, TextLookup text);
	// Обновляет текст и доступность без изменения цели политики и без сетевых вызовов.
	void refresh();
	// Повторяет только адресацию явной цели активной страницы после готовности канала, без переноса между контекстами.
	void synchronizeTarget();
	// Возвращает имя точного Asset; неизвестный ID остаётся явно неподтверждённым.
	QString assetName(const QString &id) const;
	// Описывает назначение и точные интервалы ns из серверной версии policy.
	QString describe(const std::optional<api::PolicyResult> &policy) const;
	// Запрашивает разрешение CAS для неизменной цели, затем передаёт явную новую запись контроллеру.
	void resolveConflict();
	// owner_ задаёт время жизни подписок; state_ и controller_ переживают форму, text_ переводит строки.
	FallbackPolicyEditor &owner_;
	StateStore &state_;
	FallbackController &controller_;
	TextLookup text_;
	// streamId_ хранит локальную цель страницы и не меняет рабочую привязку Dock.
	QString streamId_;
	// targetContext_ фиксирует контекст явного выбора; смена сервера не переносит прежнюю цель.
	ConfigurationContext targetContext_;
	// synchronizing_ исключает повторную адресацию из синхронного сигнала публикации.
	bool synchronizing_ = false;
	// active_ разрешает адресацию контроллера только текущей странице; confirming_ исключает повторный диалог.
	bool active_ = false;
	bool confirming_ = false;
	// asset_ содержит точные ID назначений; base_ показывает подтверждённую политику обычным текстом.
	QComboBox *asset_ = nullptr;
	QLabel *base_ = nullptr;
	// result_ отделяет исход записи от наблюдения, save_/refresh_ передают явные намерения.
	StatusLabel *result_ = nullptr;
	QPushButton *save_ = nullptr;
	QPushButton *refresh_ = nullptr;
	// conflict_ содержит три версии, comparison_ их показывает, resolve_ требует явного нового подтверждения.
	QGroupBox *conflict_ = nullptr;
	QLabel *comparison_ = nullptr;
	QPushButton *resolve_ = nullptr;
};

// Строит переиспользуемую форму без чтения API и без изменения общей цели политики.
FallbackPolicyEditor::Impl::Impl(FallbackPolicyEditor &owner, StateStore &state, FallbackController &controller,
				 TextLookup text)
	: owner_(owner),
	  state_(state),
	  controller_(controller),
	  text_(std::move(text))
{
	owner_.setObjectName(QStringLiteral("fallbackPolicyEditor"));
	// layout размещает назначение, подтверждённую базу и отдельное сравнение CAS.
	auto *layout = new QVBoxLayout(&owner_);
	layout->setContentsMargins(0, 0, 0, 0);
	// fields связывает подпись выбора с доступным с клавиатуры селектором.
	auto *fields = new QFormLayout;
	fields->setRowWrapPolicy(QFormLayout::WrapLongRows);
	asset_ = new QComboBox(&owner_);
	asset_->setObjectName(QStringLiteral("fallbackPolicyAsset"));
	asset_->setAccessibleName(text_("Fallback.PolicyAsset"));
	fields->addRow(text_("Fallback.PolicyAsset"), asset_);
	layout->addLayout(fields);
	base_ = new QLabel(&owner_);
	base_->setObjectName(QStringLiteral("fallbackPolicyBase"));
	base_->setTextFormat(Qt::PlainText);
	base_->setWordWrap(true);
	layout->addWidget(base_);
	// buttons размещает только конфигурационные операции, без управления исполнением.
	auto *buttons = new QHBoxLayout;
	save_ = new QPushButton(text_("Editor.Save"), &owner_);
	save_->setObjectName(QStringLiteral("fallbackPolicySave"));
	refresh_ = new QPushButton(text_("Editor.Reload"), &owner_);
	refresh_->setObjectName(QStringLiteral("fallbackPolicyRefresh"));
	refresh_->setIcon(QIcon::fromTheme(QStringLiteral("view-refresh"),
					   owner_.style()->standardIcon(QStyle::SP_BrowserReload)));
	buttons->addWidget(save_);
	buttons->addWidget(refresh_);
	buttons->addStretch();
	layout->addLayout(buttons);
	result_ = new StatusLabel(&owner_, false);
	result_->setObjectName(QStringLiteral("fallbackPolicyResult"));
	result_->setTextFormat(Qt::PlainText);
	result_->setWordWrap(true);
	layout->addWidget(result_);
	conflict_ = new QGroupBox(text_("Editor.Conflict"), &owner_);
	conflict_->setObjectName(QStringLiteral("fallbackPolicyConflict"));
	// conflictLayout держит PlainText сравнение и кнопку повторного подтверждения.
	auto *conflictLayout = new QVBoxLayout(conflict_);
	comparison_ = new QLabel(conflict_);
	comparison_->setTextFormat(Qt::PlainText);
	comparison_->setWordWrap(true);
	conflictLayout->addWidget(comparison_);
	resolve_ = new QPushButton(text_("Editor.ConfirmConflict"), conflict_);
	resolve_->setObjectName(QStringLiteral("fallbackPolicyResolveConflict"));
	conflictLayout->addWidget(resolve_);
	layout->addWidget(conflict_);
	// Выбор пользователя меняет только RAM-назначение точного Stream активной страницы.
	QObject::connect(asset_, &QComboBox::activated, &owner_, [this] {
		if (active_ && state_.fallback().policy.streamId == streamId_)
			controller_.setPolicyAsset(asset_->currentData().toString());
	});
	// Save не выбирает новую цель и не повторяет прежнюю неизвестную запись.
	QObject::connect(save_, &QPushButton::clicked, &owner_, [this] {
		if (active_ && state_.fallback().policy.streamId == streamId_)
			controller_.savePolicy();
	});
	// Refresh читает только уже адресованную политику активной копии.
	QObject::connect(refresh_, &QPushButton::clicked, &owner_, [this] {
		if (active_ && state_.fallback().policy.streamId == streamId_)
			controller_.refreshPolicy();
	});
	// Отдельная кнопка требует нового явного разрешения CAS перед повторной записью.
	QObject::connect(resolve_, &QPushButton::clicked, &owner_, [this] { resolveConflict(); });
	// Подписки принадлежат owner_; отражение скрытой копии не меняет цель контроллера.
	QObject::connect(&state_, &StateStore::fallbackChanged, &owner_, [this] { synchronizeTarget(); });
	QObject::connect(&state_, &StateStore::serverStateChanged, &owner_, [this] { synchronizeTarget(); });
	refresh();
}

// После reconnect повторяется только выбор цели, ранее отклонённый недоступным каналом; запись не воспроизводится.
void FallbackPolicyEditor::Impl::synchronizeTarget()
{
	if (synchronizing_)
		return;
	synchronizing_ = true;
	// alive защищает продолжение после синхронного уведомления setPolicyTarget.
	const QPointer<FallbackPolicyEditor> alive(&owner_);
	if (active_ && targetContext_.valid() && targetContext_ == state_.fallback().context &&
	    state_.fallback().policy.streamId != streamId_)
		controller_.setPolicyTarget(streamId_);
	if (!alive)
		return;
	synchronizing_ = false;
	refresh();
}

// Имена служат только отображению; запись всегда использует ID из itemData.
QString FallbackPolicyEditor::Impl::assetName(const QString &id) const
{
	if (id.isEmpty())
		return text_("Fallback.Unassigned");
	// found адресует полученную запись каталога, не предполагая полноту отсутствующих записей.
	const auto found = state_.fallback().assets.constFind(id);
	return found == state_.fallback().assets.cend() ? text_("Editor.ReferenceUnknown").arg(id) : found->name;
}

// Сохраняет различие отсутствующего чтения, Missing и явного null, показывая интервалы без редактора.
QString FallbackPolicyEditor::Impl::describe(const std::optional<api::PolicyResult> &policy) const
{
	if (!policy || policy->fallback.state == api::FieldState::Missing)
		return text_("Observation.NoData");
	if (!policy->fallback.hasValue())
		return text_("Fallback.Unassigned");
	// watchdog содержит серверные целые наносекунды без преобразования через double.
	const auto &watchdog = policy->fallback->watchdog;
	return text_("Fallback.PolicyDescription")
		.arg(assetName(policy->fallback->assetId), QString::number(policy->revision),
		     QString::number(watchdog.videoTimeoutNs), QString::number(watchdog.audioTimeoutNs),
		     QString::number(watchdog.recoveryWindowNs), QString::number(watchdog.keyframeTimeoutNs));
}

// Отражает только состояние своей цели; отсутствие собственной базы не выдаётся за очищенную политику.
void FallbackPolicyEditor::Impl::refresh()
{
	// policy копируется, поскольку дальнейшие изменения виджетов могут вызвать внешние обработчики Qt.
	const auto policy = state_.fallback().policy;
	// owns требует совпадение явной цели; editable разрешает ввод свободной активной формы.
	const bool owns = !streamId_.isEmpty() && targetContext_ == state_.fallback().context &&
			  policy.streamId == streamId_;
	const bool editable = active_ && owns && !policy.loading && !state_.fallback().writing;
	// blocked запрещает отражению списка менять RAM-ввод.
	const QSignalBlocker blocked(asset_);
	// entries содержит подписи и точные ID, включая неполученную, но сохранённую связь.
	QList<QPair<QString, QString>> entries;
	entries.append({text_("Fallback.Unassigned"), QString{}});
	// asset перебирает фактически полученную часть каталога.
	for (const auto &asset : state_.fallback().assets)
		entries.append({asset.name, asset.id});
	if (owns && !policy.desiredAssetId.isEmpty() && !state_.fallback().assets.contains(policy.desiredAssetId))
		entries.append({assetName(policy.desiredAssetId), policy.desiredAssetId});
	// changed предотвращает закрытие раскрытого списка при несвязанных обновлениях состояния.
	bool changed = asset_->count() != entries.size();
	// row сверяет подпись и ID каждой строки.
	for (int row = 0; !changed && row < entries.size(); ++row)
		changed = asset_->itemText(row) != entries.at(row).first ||
			  asset_->itemData(row).toString() != entries.at(row).second;
	if (changed) {
		asset_->clear();
		// entry содержит безопасное имя и точный ID одного варианта назначения.
		for (const auto &entry : entries)
			asset_->addItem(entry.first, entry.second);
	}
	asset_->setCurrentIndex(owns ? qMax(0, asset_->findData(policy.desiredAssetId)) : 0);
	asset_->setEnabled(editable && policy.base.has_value());
	base_->setText(owns ? text_("Fallback.PolicyBase").arg(describe(policy.base)) : text_("Fallback.ChooseStream"));
	save_->setEnabled(editable && controller_.canSavePolicy() && !policy.serverConflict);
	refresh_->setEnabled(active_ && owns && !policy.loading);
	result_->setStatus(owns ? text_(writeOutcomeKey(policy.outcome)) : QString{},
			   owns && (policy.outcome == FallbackWriteOutcome::Unknown ||
				    policy.outcome == FallbackWriteOutcome::Rejected ||
				    policy.outcome == FallbackWriteOutcome::Conflict)
				   ? StatusLabel::Tone::Error
				   : StatusLabel::Tone::Neutral);
	conflict_->setVisible(owns && policy.serverConflict.has_value());
	if (owns && policy.serverConflict)
		comparison_->setText(text_("Fallback.PolicyCompare")
					     .arg(describe(policy.base), assetName(policy.desiredAssetId),
						  describe(policy.serverConflict)));
	resolve_->setEnabled(editable && controller_.canWrite() && policy.serverConflict.has_value() && !confirming_);
}

// Фиксирует контекст, цель, поколение формы и обе ревизии до вложенного event loop подтверждения.
void FallbackPolicyEditor::Impl::resolveConflict()
{
	if (confirming_ || !active_ || state_.fallback().policy.streamId != streamId_)
		return;
	// before связывает подтверждение с неизменной формой; context фиксирует сервер и профиль.
	const auto before = state_.fallback().policy;
	const auto context = state_.fallback().context;
	if (!before.base || !before.serverConflict)
		return;
	confirming_ = true;
	// alive запрещает доступ к Impl после удаления страницы во вложенном цикле.
	const QPointer<FallbackPolicyEditor> alive(&owner_);
	// dialog принадлежит странице; его удаление вместе с owner_ учитывается QPointer.
	QPointer<QMessageBox> dialog = new QMessageBox(QMessageBox::Question, text_("Operation.ConfirmTitle"),
						       text_("Editor.ConfirmConflictWrite"),
						       QMessageBox::Yes | QMessageBox::No, &owner_);
	dialog->setTextFormat(Qt::PlainText);
	dialog->setDefaultButton(QMessageBox::No);
	localizeConfirmation(*dialog, text_);
	// answer сохраняет результат до освобождения временного диалога.
	const int answer = dialog->exec();
	if (dialog)
		delete dialog.data();
	if (!alive)
		return;
	confirming_ = false;
	// after копируется после диалога: события могли сменить сервер, карточку или конфликтующую версию.
	const auto after = state_.fallback().policy;
	if (answer != QMessageBox::Yes || !active_ || state_.fallback().context != context ||
	    after.streamId != before.streamId || after.streamId != streamId_ || after.generation != before.generation ||
	    after.desiredAssetId != before.desiredAssetId || !after.base || !after.serverConflict ||
	    after.base->revision != before.base->revision ||
	    after.serverConflict->revision != before.serverConflict->revision)
		return;
	// accepted подтверждает новое разрешение CAS; прежнее намерение с исходом Unknown здесь не повторяется.
	const bool accepted = controller_.confirmPolicyConflict();
	if (!alive || !accepted || !active_ || state_.fallback().context != context ||
	    state_.fallback().policy.streamId != before.streamId ||
	    state_.fallback().policy.desiredAssetId != before.desiredAssetId)
		return;
	controller_.savePolicy();
}

// Создаёт карточку без выбора цели и сетевых побочных эффектов.
FallbackPolicyEditor::FallbackPolicyEditor(StateStore &state, FallbackController &controller, TextLookup text,
					   QWidget *parent)
	: QWidget(parent),
	  impl_(std::make_unique<Impl>(*this, state, controller, std::move(text)))
{
}
// Общие операции переживают уничтожение представления.
FallbackPolicyEditor::~FallbackPolicyEditor() = default;

// Обновляет локальный адрес; лишь активная копия имеет право переключить общую цель.
void FallbackPolicyEditor::setStreamId(QString streamId)
{
	if (impl_->streamId_ == streamId && impl_->targetContext_ == impl_->state_.fallback().context)
		return;
	impl_->streamId_ = std::move(streamId);
	impl_->targetContext_ = impl_->state_.fallback().context;
	// alive защищает продолжение после синхронной публикации новой цели.
	const QPointer<FallbackPolicyEditor> alive(this);
	if (impl_->active_)
		impl_->synchronizeTarget();
	if (alive)
		impl_->refresh();
}

// Выключение не закрывает цель и не отменяет локальное ожидание принятой подготовки.
void FallbackPolicyEditor::setActive(bool active)
{
	if (impl_->active_ == active)
		return;
	impl_->active_ = active;
	// alive защищает представление при синхронном уведомлении контроллера.
	const QPointer<FallbackPolicyEditor> alive(this);
	if (active)
		impl_->synchronizeTarget();
	if (alive)
		impl_->refresh();
}
} // namespace gost
