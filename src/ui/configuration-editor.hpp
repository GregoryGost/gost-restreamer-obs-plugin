#pragma once
#include "core/configuration-state.hpp"
#include "text-lookup.hpp"
#include <QWidget>
#include <memory>

namespace gost {
class StateStore;
class ConfigurationController;
/** Представляет список и одну конфигурационную карточку внутри существующего окна.
 * Серверный кеш и RAM-черновик читает из StateStore; все намерения передаёт контроллеру. */
class ConfigurationEditor final : public QWidget {
	Q_OBJECT
public:
	// Создаёт страницу kind; state и controller заимствованы и должны пережить виджет.
	ConfigurationEditor(StateStore &state, ConfigurationController &controller, TextLookup text,
			    ConfigurationKind kind, QWidget *parent = nullptr);
	// Освобождает привязки формы; дочерние элементы удаляет QObject.
	~ConfigurationEditor() override;
	// Открывает собственный вид сущности и обновляет коллекции, сохраняя текущий несохранённый черновик.
	void activate();
	// При preserveDraft сохраняет ввод в RAM; иначе явно подтверждает его потерю перед созданием новой формы.
	bool canLeave(bool preserveDraft = false);
	// Сбрасывает раскрытие всех секретов при смене страницы и закрытии/открытии окна.
	void hideSecrets();
	// Возвращает точный ID просматриваемого Destination, не ID рабочего Stream.
	QString selectedDestinationId() const;
	// Ограничивает прикрепление внешней проверкой текущей Session; сохранённость карточки проверяется отдельно.
	void setAttachAvailable(bool available);
signals:
	// Передаёт точный ID назначения; родитель фиксирует Session и подтверждает прикрепление.
	void requestAttachDestination(QString destinationId);
	// Сообщает об изменении просматриваемой сущности, не меняя рабочую привязку OBS.
	void selectionChanged();

private:
	// Реализация содержит элементы формы и синхронизирует их с общей RAM-моделью.
	class Impl;
	// impl_ владеет описаниями формы и заимствует общие сервисы.
	std::unique_ptr<Impl> impl_;
};
} // namespace gost
