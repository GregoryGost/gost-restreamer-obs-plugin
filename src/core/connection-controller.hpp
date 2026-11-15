#pragma once

#include "obs/frontend.hpp"

#include <QObject>
#include <memory>

namespace gost {

class StateStore;
class SettingsStore;
namespace api {
class APIClient;
class EventClient;
} // namespace api

/**
 * Координирует подключение: ready -> capabilities -> список Streams -> оперативный снимок -> SSE.
 * Входные сервисы заимствованы и должны пережить контроллер. Все методы вызываются в GUI-потоке;
 * сетевые операции и паузы асинхронны. Деструктор завершает работу через shutdown().
 *
 * Поколения подключения/выбора запрещают применение запоздалых результатов. После обрыва возможны
 * возобновление подтверждённой последовательности или новый снимок; повторы ограничены паузами,
 * частотой стартов и отдельным бюджетом отказов SSE 400/409. Connected устанавливается по заголовкам
 * подписки, а не по удачному REST. Метаданные Dock читаются отдельно и проверяются по точному выбору.
 * Контроллер наблюдает сервер и сохраняет локальные предпочтения; медиаоперации не выполняет.
 */
class ConnectionController final : public QObject {
	Q_OBJECT

public:
	// Связывает заимствованные state/settings/api/events и таймер восстановления с родителем parent.
	ConnectionController(StateStore &state, SettingsStore &settings, api::APIClient &api, api::EventClient &events,
			     QObject *parent = nullptr);
	// Завершает наблюдение и отменяет принадлежащие контроллеру запросы до освобождения реализации.
	~ConnectionController() override;
	// Принимает событие и снимок профиля из адаптера. Подтверждённое переименование сохраняет UUID и подписку.
	// Смена профиля или невозможность определить его UUID отменяет работу для прежнего выбора.
	// При FinishedLoading один раз подключается к сохранённому серверу; эфир при этом не начинается.
	void observeProfile(FrontendEvent event, const ProfileDescriptor &descriptor);
	// Проверяет/канонизирует origin, сохраняет предпочтение и начинает полное согласование.
	// Неверный ввод сообщает ошибку без замены действующего адреса; тот же сервер сохраняет кеш Dock как устаревший.
	void connectToServer(QString origin);
	// Отменяет подключение и повторы, оставляя последние наблюдения устаревшими. Сервер/OBS не останавливает.
	void disconnectFromServer();
	// Объединяет запрос с выполняющимися чтениями выбранной Session/Outputs/fallback и каталога назначений; POST не выполняет.
	void refreshState();
	// Выбирает только полученный Stream, когда UUID профиля определён; пустой ID снимает выбор.
	// Сохраняет привязку пары сервер/UUID и заново синхронизирует runtime; неизвестный ID игнорирует.
	void selectStream(QString streamId);
	// Сохраняет число новых циклов snapshot/SSE после отказа 400/409; -1 означает отсутствие конечного лимита.
	void setSseRejectionRetries(int retries);
	// Сохраняет общий срок новых обычных REST-запросов в пределах 5–60 секунд.
	void setHttpTimeoutSeconds(int seconds);
	// Сохраняет выбор подробных безопасных записей штатного журнала OBS.
	void setDetailedLogging(bool detailed);
	// Применяет retries/seconds/detailed одной записью локальных предпочтений без переподключения.
	// Возвращает подтверждение записи; после shutdown отклоняет действие без изменения настроек.
	bool saveAdvancedPreferences(int retries, int seconds, bool detailed);
	// Идемпотентно запрещает дальнейшие действия, завершает оба сетевых клиента и помечает данные устаревшими.
	void shutdown();

signals:
	// Выдаётся только явным отключением пользователя, отдельно от 204 или ошибки SSE.
	void explicitlyDisconnected();

private:
	// Реализует последовательность подключения и чтения метаданных в GUI-потоке.
	class Impl;
	// Единственная принадлежащая контроллеру реализация; входные сервисы она заимствует.
	std::unique_ptr<Impl> impl_;
};

} // namespace gost
