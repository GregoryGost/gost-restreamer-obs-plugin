#pragma once

#include "frontend.hpp"

#include <obs-frontend-api.h>

#include <atomic>
#include <memory>

namespace gost {

/**
 * Реализует Frontend для OBS 33 через публичные Frontend/libobs API.
 * Не владеет главным окном, видеоподсистемой или штатным output. Владеет только своим состоянием
 * подписок и слабой ссылкой output. Сильная ссылка берётся на время одной выборки и освобождается
 * соответствующей функцией освобождения OBS, а не delete. Все пользовательские методы вызываются из GUI.
 *
 * Сигналы video_reset/reconnect_success могут прийти вне GUI: их обработчики меняют лишь атомарные
 * поколения. Владелец снимает основной callback и вызывает releaseStatistics до конца EXIT. Деструктор
 * повторяет идемпотентную очистку, которая после этого уже не обращается к OBS.
 */
class OBSFrontend final : public Frontend {
public:
	// Повторяет безопасную очистку подписок статистики при уничтожении адаптера.
	~OBSFrontend() override;
	// Возвращает заимствованный указатель на главное окно OBS.
	QWidget *mainWindow() const override;
	// Регистрирует виджет как Dock id/title; при успехе владение получает OBS.
	bool addDock(const char *id, const QString &title, QWidget *widget) override;
	// Удаляет Dock id через Frontend API вместе с его виджетом.
	void removeDock(const char *id) override;
	// Сохраняет callback/cookie и регистрирует единственный переходник событий OBS.
	void addEventCallback(EventCallback callback, void *cookie) override;
	// Снимает переходник только при совпадении ранее сохранённых callback/cookie.
	void removeEventCallback(EventCallback callback, void *cookie) override;
	// Возвращает копию имени текущего профиля и освобождает строку Frontend API.
	QString currentProfile() const override;
	// Возвращает копию каталога текущего профиля и освобождает строку Frontend API.
	QString currentProfilePath() const override;
	// Копирует список имён профилей и освобождает массив Frontend API.
	QStringList profiles() const override;
	// Читает штатный флаг активности без изменения streaming output.
	bool streamingActive() const override;
	// Возвращает собственные флаги; сильная ссылка штатного output освобождается до возврата.
	StreamingState streamingState() const override;
	// Вызывает только штатный obs_frontend_streaming_stop; завершение наблюдает OBSController.
	void requestStreamingStop() override;
	// Лениво подключает сигналы и возвращает независимую копию выборки. Сетевые счётчики выдаёт
	// только для активного rtmp_output вне переподключения и без смены поколения во время чтения.
	ObsStatisticsSample readStatistics() noexcept override;
	// Отключает сигналы до освобождения слабой ссылки; повтор без активных подписок безопасен.
	void releaseStatistics() noexcept override;

private:
	// Преобразует поддерживаемое событие OBS и передаёт его callback_ с cookie_.
	static void onEvent(obs_frontend_event event, void *cookie) noexcept;
	// Увеличивает поколение видео адаптера cookie после сигнала video_reset; data не используется.
	static void onVideoReset(void *cookie, calldata_t *data) noexcept;
	// Увеличивает поколение выхода адаптера cookie после reconnect_success; data не используется.
	static void onOutputReconnect(void *cookie, calldata_t *data) noexcept;
	// Обработчик output удерживается signal_handler_connect_ref и может пережить сам output; signal_handler_disconnect обязателен
	// до освобождения состояния, на которое указывает cookie.
	void releaseOutputStatistics() noexcept;

	// Заимствованный обработчик получателя; nullptr означает отсутствие подписки Frontend.
	EventCallback callback_ = nullptr;
	// Заимствованный контекст callback_, действующий до removeEventCallback.
	void *cookie_ = nullptr;
	// Обработчик глобальных сигналов видео, пока подключён video_reset.
	signal_handler_t *videoSignals_ = nullptr;
	// Обработчик сигналов текущего выхода; connect_ref удерживает его до disconnect.
	signal_handler_t *outputSignals_ = nullptr;
	// Обработчики сигналов статистики меняют только атомарные поколения, не обращаясь к объектам Qt.
	std::atomic<quint64> videoIdentity_ = 1;
	// Слабая ссылка сохраняет идентичность выхода между выборками, не удерживая работающий выход.
	std::unique_ptr<obs_weak_output_t, decltype(&obs_weak_output_release)> output_{nullptr,
										       &obs_weak_output_release};
	// Поколение выхода меняется при замене объекта и восстановлении подключения.
	std::atomic<quint64> outputIdentity_ = 0;
};

} // namespace gost
