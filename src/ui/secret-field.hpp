#pragma once
#include "api/models.hpp"
#include "text-lookup.hpp"
#include <QWidget>

class QLineEdit;
class QToolButton;

namespace gost {
/** Показывает и редактирует секрет в одном маскируемом поле. Отслеживает изменение
 * относительно серверной строки; отсутствие чтения и пустой ввод различаются. */
class SecretField final : public QWidget {
	Q_OBJECT
public:
	// Создаёт поле; text переводит подписи, parent владеет дочерними элементами Qt.
	explicit SecretField(TextLookup text, QWidget *parent = nullptr);
	// Отражает серверное значение и скрывает его; emptyMeansUnset обозначает пустой ключ.
	void setReceived(const api::JsonField<QString> &value, bool emptyMeansUnset = false);
	// Разрешает пользовательский ввод; само включение редактирования не создаёт PATCH-намерение.
	void setEditable(bool editable);
	// Возвращает явную замену, включая пустую строку для очистки, либо отсутствие намерения.
	std::optional<QString> replacement() const;
	// Отражает RAM-намерение без пользовательского changed.
	void setReplacement(const std::optional<QString> &value);
	// Возвращает полученную строку вместе с признаком доступности.
	const api::JsonField<QString> &received() const noexcept;
	// Сбрасывает ввод замены без изменения серверного значения.
	void clearReplacement();
	// Возвращает полю режим пароля, сохраняя точную строку и несохранённый ввод.
	void hideSecret();
signals:
	// Сообщает о новом намерении замены; показ и Copy не меняют draft.
	void changed();

protected:
	// Обновляет собственные иконки при смене палитры OBS.
	void changeEvent(QEvent *event) override;

private:
	// Отражает точный ввод с нативным маскированием и доступностью действий.
	void refresh();
	// Рисует глаз и копирование цветом ButtonText текущей темы.
	void refreshIcons();
	// text_ переводит подписи; received_ хранит точную серверную строку в RAM.
	TextLookup text_;
	api::JsonField<QString> received_;
	// emptyMeansUnset_ отличает пустой ключ; revealed_ управляет только видимостью.
	bool emptyMeansUnset_ = false;
	bool revealed_ = false;
	// value_ хранит точный текущий ввод; eye_ раскрывает его, copy_ копирует без раскрытия.
	QLineEdit *value_ = nullptr;
	QToolButton *eye_ = nullptr;
	QToolButton *copy_ = nullptr;
	// editable_ разрешает ввод; replacement_ содержит только явное изменение, включая очистку.
	bool editable_ = false;
	std::optional<QString> replacement_;
};
} // namespace gost
