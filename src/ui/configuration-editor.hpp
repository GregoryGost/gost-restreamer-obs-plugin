#pragma once
#include "core/configuration-state.hpp"
#include "text-lookup.hpp"
#include <QWidget>
#include <memory>

namespace gost {
class StateStore;
class ConfigurationController;
class FallbackController;
/** Представляет список и одну конфигурационную карточку внутри существующего окна.
 * Серверный кеш и RAM-черновик читает из StateStore; все намерения передаёт контроллеру. */
class ConfigurationEditor final : public QWidget {
	Q_OBJECT
public:
	// Создаёт страницу kind; state, controller и необязательный fallback заимствованы и переживают виджет.
	ConfigurationEditor(StateStore &state, ConfigurationController &controller, TextLookup text,
			    ConfigurationKind kind, QWidget *parent = nullptr, FallbackController *fallback = nullptr);
	// Освобождает привязки формы; дочерние элементы удаляет QObject.
	~ConfigurationEditor() override;
	// Открывает собственный вид сущности и обновляет коллекции, сохраняя текущий несохранённый черновик.
	void activate();
	// Разрешает адресацию policy только активной карточке Stream; выключение сохраняет RAM-ввод.
	void setActive(bool active);
	// При preserveDraft сохраняет ввод в RAM; иначе явно подтверждает его потерю перед созданием новой формы.
	bool canLeave(bool preserveDraft = false);
	// Сбрасывает раскрытие всех секретов при смене страницы и закрытии/открытии окна.
	void hideSecrets();
	// Возвращает точный ID просматриваемого Destination, не ID рабочего Stream.
	QString selectedDestinationId() const;
signals:
	// Сообщает об изменении просматриваемой сущности, не меняя рабочую привязку OBS.
	void selectionChanged();

private:
	// Реализация содержит элементы формы и синхронизирует их с общей RAM-моделью.
	class Impl;
	// impl_ владеет описаниями формы и заимствует общие сервисы.
	std::unique_ptr<Impl> impl_;
};
} // namespace gost
