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
	~OBSFrontend() override;
	QWidget *mainWindow() const override;
	bool addDock(const char *id, const QString &title, QWidget *widget) override;
	void removeDock(const char *id) override;
	void addEventCallback(EventCallback callback, void *cookie) override;
	void removeEventCallback(EventCallback callback, void *cookie) override;
	QString currentProfile() const override;
	QString currentProfilePath() const override;
	QStringList profiles() const override;
	bool streamingActive() const override;
	// Лениво подключает сигналы и возвращает независимую копию выборки. Сетевые счётчики выдаёт
	// только для активного rtmp_output вне переподключения и без смены поколения во время чтения.
	ObsStatisticsSample readStatistics() noexcept override;
	// Отключает сигналы до освобождения слабой ссылки; повтор без активных подписок безопасен.
	void releaseStatistics() noexcept override;

private:
	static void onEvent(obs_frontend_event event, void *cookie) noexcept;
	static void onVideoReset(void *cookie, calldata_t *data) noexcept;
	static void onOutputReconnect(void *cookie, calldata_t *data) noexcept;
	// Обработчик output удерживается signal_handler_connect_ref и может пережить сам output; signal_handler_disconnect обязателен
	// до освобождения состояния, на которое указывает cookie.
	void releaseOutputStatistics() noexcept;

	EventCallback callback_ = nullptr;
	void *cookie_ = nullptr;
	signal_handler_t *videoSignals_ = nullptr;
	signal_handler_t *outputSignals_ = nullptr;
	// Обработчики сигналов статистики меняют только атомарные поколения, не обращаясь к объектам Qt.
	std::atomic<quint64> videoIdentity_ = 1;
	// Слабая ссылка сохраняет идентичность выхода между выборками, не удерживая работающий выход.
	std::unique_ptr<obs_weak_output_t, decltype(&obs_weak_output_release)> output_{nullptr,
										       &obs_weak_output_release};
	std::atomic<quint64> outputIdentity_ = 0;
};

} // namespace gost
