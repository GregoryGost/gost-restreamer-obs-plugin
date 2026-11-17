#pragma once

#include "ui/text-lookup.hpp"
#include <QDialog>

class QLabel;

namespace gost {

// Показывает последний отказ одним асинхронным окном; закрытие скрывает окно для следующего отказа.
class OperationErrorDialog final : public QDialog {
	Q_OBJECT
public:
	// Создаёт PlainText-блоки причины и действия с главным окном OBS в качестве parent.
	OperationErrorDialog(TextLookup text, QWidget *parent);
	// Заменяет содержимое без истории и показывает окно без вложенного event loop.
	void present(const QString &reason, const QString &action);

private:
	// text_ сохраняет переводчик до уничтожения принадлежащего Runtime окна.
	TextLookup text_;
	// reason_ показывает причину, action_ — только известное серверное действие.
	QLabel *reason_ = nullptr;
	QLabel *action_ = nullptr;
	// actionTitle_ скрывается вместе с пустым действием.
	QLabel *actionTitle_ = nullptr;
};

} // namespace gost
