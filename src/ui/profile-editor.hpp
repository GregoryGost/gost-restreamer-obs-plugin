#pragma once
#include "api/models.hpp"
#include "text-lookup.hpp"
#include <QWidget>
#include <QVariantMap>
#include <memory>

namespace gost {
/** Редактирует полный явный серверный профиль структурными полями. Исходный документ,
 * точные числа и незавершённый ввод сохраняются в RAM. HTTP не выполняет. */
class ProfileEditor final : public QWidget {
	Q_OBJECT
public:
	// Создаёт форму с переводом text внутри parent. Необязательный nameInput переходит в её первую строку;
	// виджет становится дочерним, а обработка имени остаётся у вызывающей карточки.
	explicit ProfileEditor(TextLookup text, QWidget *parent = nullptr, QWidget *nameInput = nullptr);
	// Освобождает описания привязок; дочерними виджетами владеет QObject.
	~ProfileEditor() override;
	// Отражает полный исходный DTO; creating требует явного заполнения нового медиаформата.
	void setProfile(const api::GroupProfile &profile, bool creating = false);
	// Возвращает полный профиль с изменёнными полями; error содержит безопасное пояснение валидации.
	bool readProfile(api::GroupProfile &profile, QString &error) const;
	// Сохраняет в RAM точный ввод, включая невалидные строки и состояние раскрытия раздела.
	QVariantMap captureDraft() const;
	// Восстанавливает снимок captureDraft без changed; HTTP и нормализацию данных не выполняет.
	void restoreDraft(const QVariantMap &draft);
signals:
	// Сообщает только о пользовательском изменении структурного поля.
	void changed();

private:
	// Реализация содержит исходный DTO и привязки отдельных полей без промежуточного JSON.
	class Impl;
	// impl_ владеет описаниями полей и RAM-базой редактора.
	std::unique_ptr<Impl> impl_;
};
} // namespace gost
