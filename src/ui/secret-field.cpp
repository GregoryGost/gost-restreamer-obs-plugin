#include "secret-field.hpp"
#include <QApplication>
#include <QClipboard>
#include <QEvent>
#include <QHBoxLayout>
#include <QLineEdit>
#include <QPainter>
#include <QPainterPath>
#include <QSignalBlocker>
#include <QToolButton>
#include <utility>
#include <limits>

namespace gost {
SecretField::SecretField(TextLookup text, QWidget *parent) : QWidget(parent), text_(std::move(text))
{
	// row размещает единственный ввод и кнопки раскрытия/копирования в одной строке.
	auto *row = new QHBoxLayout(this);
	row->setContentsMargins(0, 0, 0, 0);
	value_ = new QLineEdit(this);
	value_->setMaxLength(std::numeric_limits<int>::max());
	value_->setReadOnly(true);
	value_->setObjectName(QStringLiteral("secretReceived"));
	value_->setAccessibleName(text_("Secret.Value"));
	eye_ = new QToolButton(this);
	eye_->setObjectName(QStringLiteral("secretEye"));
	copy_ = new QToolButton(this);
	copy_->setObjectName(QStringLiteral("secretCopy"));
	copy_->setToolTip(text_("Secret.Copy"));
	copy_->setAccessibleName(text_("Secret.Copy"));
	row->addWidget(value_, 1);
	row->addWidget(eye_);
	row->addWidget(copy_);
	// Обработчик глаза меняет только видимость, сохраняя серверное значение и новый ввод.
	connect(eye_, &QToolButton::clicked, this, [this] {
		revealed_ = !revealed_;
		refresh();
	});
	// Обработчик Copy берёт точную текущую строку; нативное маскирование не попадает в clipboard.
	connect(copy_, &QToolButton::clicked, this, [this] {
		if ((received_.hasValue() || replacement_.has_value()) && !value_->text().isEmpty())
			QApplication::clipboard()->setText(value_->text());
	});
	// textEdited отмечает только пользовательский ввод. Возврат исходной строки снимает изменение;
	// при недоступном чтении введённая пользователем пустота остаётся явной очисткой.
	connect(value_, &QLineEdit::textEdited, this, [this](const QString &text) {
		if (received_.hasValue() && text == *received_)
			replacement_.reset();
		else
			replacement_ = text;
		refresh();
		emit changed();
	});
	setEditable(false);
	refreshIcons();
	refresh();
}
void SecretField::setReceived(const api::JsonField<QString> &value, bool emptyMeansUnset)
{
	received_ = value;
	emptyMeansUnset_ = emptyMeansUnset;
	replacement_.reset();
	hideSecret();
}
void SecretField::setEditable(bool editable)
{
	editable_ = editable;
	refresh();
}
std::optional<QString> SecretField::replacement() const
{
	return replacement_;
}
void SecretField::setReplacement(const std::optional<QString> &value)
{
	replacement_ = value;
	if (replacement_ && received_.hasValue() && *replacement_ == *received_)
		replacement_.reset();
	refresh();
}
const api::JsonField<QString> &SecretField::received() const noexcept
{
	return received_;
}
void SecretField::clearReplacement()
{
	setReplacement(std::nullopt);
}
void SecretField::hideSecret()
{
	revealed_ = false;
	refresh();
}
void SecretField::refresh()
{
	// known отличает полученное или введённое значение от отсутствия серверного чтения.
	const bool known = received_.hasValue() || replacement_.has_value();
	// current содержит исходную строку либо явную замену; никакие пробелы и URI-части не меняются.
	const QString current = replacement_.value_or(received_.hasValue() ? *received_ : QString{});
	// blocked исключает служебные сигналы при отражении RAM; textEdited и без него остаётся пользовательским.
	const QSignalBlocker blocked(value_);
	if (value_->text() != current)
		value_->setText(current);
	value_->setReadOnly(!editable_);
	value_->setEchoMode(revealed_ ? QLineEdit::Normal : QLineEdit::Password);
	value_->setPlaceholderText(!known                                  ? text_("Secret.Unavailable")
				   : emptyMeansUnset_ && current.isEmpty() ? text_("Secret.Unset")
									   : QString{});
	eye_->setEnabled(known && !current.isEmpty());
	copy_->setEnabled(known && !current.isEmpty());
	eye_->setToolTip(text_(revealed_ ? "Secret.Hide" : "Secret.Show"));
	eye_->setAccessibleName(eye_->toolTip());
}
void SecretField::refreshIcons()
{
	// eyePixmap и copyPixmap хранят прозрачные иконки; painter использует цвет кнопок темы.
	QPixmap eyePixmap(20, 20);
	QPixmap copyPixmap(20, 20);
	eyePixmap.fill(Qt::transparent);
	copyPixmap.fill(Qt::transparent);
	QPainter painter(&eyePixmap);
	painter.setRenderHint(QPainter::Antialiasing);
	painter.setPen(QPen(palette().color(QPalette::ButtonText), 1.5));
	// outline задаёт контур глаза без внешних изображений.
	QPainterPath outline;
	outline.moveTo(2, 10);
	outline.quadTo(10, 0, 18, 10);
	outline.quadTo(10, 20, 2, 10);
	painter.drawPath(outline);
	painter.drawEllipse(QPointF(10, 10), 2.5, 2.5);
	painter.end();
	painter.begin(&copyPixmap);
	painter.setPen(QPen(palette().color(QPalette::ButtonText), 1.5));
	painter.drawRoundedRect(QRectF(6, 6, 11, 12), 1, 1);
	painter.drawLine(3, 14, 3, 3);
	painter.drawLine(3, 3, 13, 3);
	painter.end();
	eye_->setIcon(QIcon(eyePixmap));
	copy_->setIcon(QIcon(copyPixmap));
}
void SecretField::changeEvent(QEvent *event)
{
	QWidget::changeEvent(event);
	if (event->type() == QEvent::PaletteChange || event->type() == QEvent::ApplicationPaletteChange)
		refreshIcons();
}
} // namespace gost
