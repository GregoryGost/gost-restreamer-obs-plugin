#pragma once

#include "core/configuration-state.hpp"
#include "core/operation-failure.hpp"
#include <QObject>
#include <memory>

namespace gost {
class StateStore;
class ConnectionController;
class RestreamController;
namespace api {
class APIClient;
}

/** Координирует CRUD и активную RAM-форму в GUI-потоке через отдельный APIClient.
 * При навигации хранит оставленные карточки в RAM по точному контексту, виду и ID.
 * Заимствованные сервисы переживают контроллер. Закрытие окна не отменяет запись;
 * смена контекста и shutdown запрещают старые callbacks и сохраняют честный Unknown.
 */
class ConfigurationController final : public QObject {
	Q_OBJECT
public:
	// Связывает состояние, независимый config API и контроллеры рабочего выбора/операций.
	ConfigurationController(StateStore &state, api::APIClient &api, ConnectionController &connection,
				RestreamController &restream, QObject *parent = nullptr);
	// Завершает принадлежащие запросы до освобождения реализации.
	~ConfigurationController() override;
	// Читает коллекцию; частичный список не доказывает отсутствие пропущенных записей.
	void refresh(ConfigurationKind kind);
	// Открывает точный ID; dirty-форма требует явного discardDirty=true, запись не прерывается.
	bool open(ConfigurationKind kind, QString id, bool discardDirty = false);
	// Создаёт пустую типизированную форму; новый Profile явно использует LIVE AAC copy.
	bool beginCreate(ConfigurationKind kind, bool discardDirty = false);
	// Заменяет ввод и точный набор изменённых JSON-полей без HTTP и смены рабочего Stream.
	bool setDraft(ConfigurationEntity draft, QSet<QString> dirtyFields);
	// Возвращает поколение опубликованной RAM-формы для подтверждения точного состояния до диалога.
	quint64 editorGeneration() const;
	// Отбрасывает ввод поколения expectedGeneration без HTTP, включая устаревший контекст.
	// Загрузка/запись запрещают сброс; Unknown текущего контекста сначала требует сверки.
	bool discardDraft(quint64 expectedGeneration);
	// Сохраняет текущую свободную карточку в RAM по полному контексту, виду и ID без её отправки.
	bool parkDraft();
	// Восстанавливает припаркованный ввод только для нынешнего контекста; пустой ID обозначает создание.
	bool restoreDraft(ConfigurationKind kind, const QString &id);
	// Меняет участие собственного назначения открытого Stream, запоминая его исходную revision.
	bool setDestinationEnabled(const QString &id, bool enabled);
	// Отправляет create/PATCH один раз; не повторяет неоднозначный результат автоматически.
	bool save();
	// Удаляет текущую сущность по исходной revision; подтверждение пользователя принадлежит UI.
	bool remove();
	// Перечитывает карточку, сохраняя dirty ввод; после Unknown разрешает новое явное намерение.
	bool reload();
	// Явно принимает разрешённый пользователем полный draft относительно новой серверной базы.
	bool confirmConflict(ConfigurationEntity resolvedDraft, QSet<QString> dirtyFields);
	// Завершает канал конфигурации; уже отправленная запись получает Unknown.
	void shutdown();

signals:
	// Передаёт отказ текущего CRUD-намерения после проверки его контекста и поколения.
	void operationFailed(const gost::OperationFailure &failure);

private:
	// Реализация хранит поколения чтений, активный ввод и припаркованные карточки.
	class Impl;
	// Единственный владелец реализации.
	std::unique_ptr<Impl> impl_;
};
} // namespace gost
