#pragma once

#include "text-lookup.hpp"
#include <QWidget>
#include <memory>

namespace gost {
class StateStore;
class FallbackController;

/** Показывает назначение заглушки одному явно выбранному Stream и передаёт намерения контроллеру.
 * Невидимые копии только отображают данные; цель общего контроллера задаёт активная страница. */
class FallbackPolicyEditor final : public QWidget {
	Q_OBJECT
public:
	// Создаёт карточку политики; state и controller заимствованы, Qt связывает виджет с parent.
	FallbackPolicyEditor(StateStore &state, FallbackController &controller, TextLookup text,
			     QWidget *parent = nullptr);
	// Освобождает описания формы, не отменяя общую операцию.
	~FallbackPolicyEditor() override;
	// Запоминает явно просматриваемый Stream; активная копия адресует контроллер и читает его политику.
	void setStreamId(QString streamId);
	// Разрешает только этой видимой странице менять общую цель; выключение не отменяет операции.
	void setActive(bool active);

private:
	// Реализация содержит элементы карточки и заимствованные сервисы GUI-потока.
	class Impl;
	// impl_ владеет привязками карточки, а не контроллером или RAM-черновиками.
	std::unique_ptr<Impl> impl_;
};
} // namespace gost
