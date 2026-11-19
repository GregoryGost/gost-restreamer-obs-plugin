#pragma once

#include "text-lookup.hpp"
#include <QWidget>
#include <memory>

namespace gost {
class StateStore;
class FallbackController;

/** Предоставляет каталог, подготовку и наблюдение заглушек внутри единственного расширенного окна.
 * Файлы и сетевые операции выполняет контроллер; скрытие страницы сохраняет RAM-ввод и операцию. */
class FallbackEditor final : public QWidget {
	Q_OBJECT
public:
	// Создаёт страницу; state и controller заимствованы, Qt удаляет дочерние элементы вместе с parent.
	FallbackEditor(StateStore &state, FallbackController &controller, TextLookup text, QWidget *parent = nullptr);
	// Освобождает представление без отмены подготовки на сервере.
	~FallbackEditor() override;
	// Активирует явную цель политики и обновляет каталог, не изменяя рабочий Stream Dock.
	void activate();
	// Выключает адресацию общей политики у скрытой страницы, сохраняя её выбор и ввод.
	void setActive(bool active);

private:
	// Реализация строит страницу и отражает StateStore в GUI-потоке.
	class Impl;
	// impl_ хранит привязки формы и не владеет общими сервисами.
	std::unique_ptr<Impl> impl_;
};
} // namespace gost
