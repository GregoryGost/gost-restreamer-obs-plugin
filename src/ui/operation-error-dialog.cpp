#include "operation-error-dialog.hpp"

#include <QDialogButtonBox>
#include <QLabel>
#include <QPointer>
#include <QPushButton>
#include <QScrollArea>
#include <QVBoxLayout>
#include <utility>

namespace gost {

OperationErrorDialog::OperationErrorDialog(TextLookup text, QWidget *parent) : QDialog(parent), text_(std::move(text))
{
	setObjectName(QStringLiteral("operationErrorDialog"));
	setWindowTitle(text_("Operation.ErrorTitle"));
	setWindowModality(Qt::NonModal);
	resize(540, 300);
	// layout размещает прокручиваемый текст и кнопку; все виджеты удаляются родительским окном.
	auto *layout = new QVBoxLayout(this);
	// scroll ограничивает геометрию даже при длинном серверном пояснении.
	auto *scroll = new QScrollArea(this);
	scroll->setWidgetResizable(true);
	// content и details содержат оба текстовых блока внутри единственной области прокрутки.
	auto *content = new QWidget(scroll);
	auto *details = new QVBoxLayout(content);
	// reasonTitle обозначает первый блок независимо от содержимого сервера.
	auto *reasonTitle = new QLabel(text_("Operation.ErrorReason"), content);
	reasonTitle->setTextFormat(Qt::PlainText);
	details->addWidget(reasonTitle);
	reason_ = new QLabel(content);
	reason_->setObjectName(QStringLiteral("operationErrorReason"));
	reason_->setTextFormat(Qt::PlainText);
	reason_->setWordWrap(true);
	reason_->setTextInteractionFlags(Qt::TextSelectableByMouse);
	details->addWidget(reason_);
	actionTitle_ = new QLabel(text_("Operation.ErrorAction"), content);
	actionTitle_->setTextFormat(Qt::PlainText);
	details->addWidget(actionTitle_);
	action_ = new QLabel(content);
	action_->setObjectName(QStringLiteral("operationErrorAction"));
	action_->setTextFormat(Qt::PlainText);
	action_->setWordWrap(true);
	action_->setTextInteractionFlags(Qt::TextSelectableByMouse);
	details->addWidget(action_);
	details->addStretch();
	scroll->setWidget(content);
	layout->addWidget(scroll);
	// buttons закрывает окно стандартной кнопкой без завершения сервисов или сохранения истории.
	auto *buttons = new QDialogButtonBox(QDialogButtonBox::Close, this);
	buttons->button(QDialogButtonBox::Close)->setText(text_("Operation.ErrorClose"));
	connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);
	layout->addWidget(buttons);
}

void OperationErrorDialog::present(const QString &reason, const QString &action)
{
	reason_->setText(reason.isEmpty() ? text_("Operation.ErrorUnknownReason") : reason);
	action_->setText(action);
	action_->setVisible(!action.isEmpty());
	actionTitle_->setVisible(!action.isEmpty());
	// guard учитывает удаление главным окном OBS или shutdown во вложенном событии показа.
	const QPointer<OperationErrorDialog> guard = this;
	if (isMinimized())
		setWindowState(windowState() & ~Qt::WindowMinimized);
	if (!guard)
		return;
	show();
	if (guard)
		raise();
	if (guard)
		activateWindow();
}

} // namespace gost
