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
	ConnectionController(StateStore &state, SettingsStore &settings, api::APIClient &api, api::EventClient &events,
			     QObject *parent = nullptr);
	~ConnectionController() override;
	// Принимает событие и снимок профиля из адаптера. Подтверждённое переименование сохраняет UUID и подписку.
	// Смена профиля или невозможность определить его UUID отменяет работу для прежнего выбора.
	// Автоподключение рассматривается один раз при FinishedLoading и не означает начала эфира.
	void observeProfile(FrontendEvent event, const ProfileDescriptor &descriptor);
	// Проверяет/канонизирует origin, сохраняет предпочтение и начинает полное согласование.
	// Неверный ввод сообщает ошибку без замены действующего адреса; тот же сервер сохраняет кеш Dock как устаревший.
	void connectToServer(QString origin);
	// Отменяет подключение и повторы, оставляя последние наблюдения устаревшими. Сервер/OBS не останавливает.
	void disconnectFromServer();
	// Выбирает только полученный Stream, когда UUID профиля определён; пустой ID снимает выбор.
	// Сохраняет привязку пары сервер/UUID и заново синхронизирует runtime; неизвестный ID игнорирует.
	void selectStream(QString streamId);
	// Меняет предпочтение следующего запуска; само по себе не создаёт соединение сейчас.
	void setAutoConnect(bool enabled);
	// Отключение восстановления отменяет уже ожидающий/выполняемый цикл переподключения.
	void setReconnect(bool enabled);
	// Сохраняет число новых циклов snapshot/SSE после отказа 400/409; -1 означает отсутствие конечного лимита.
	void setSseRejectionRetries(int retries);
	// Идемпотентно запрещает дальнейшие действия, завершает оба сетевых клиента и помечает данные устаревшими.
	void shutdown();

private:
	class Impl;
	std::unique_ptr<Impl> impl_;
};

} // namespace gost
