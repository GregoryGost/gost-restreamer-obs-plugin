#pragma once

#include "ui/text-lookup.hpp"

#include <QObject>

#include <memory>

namespace gost {

class Frontend;
enum class FrontendEvent;
class StateStore;
class SettingsStore;
class ConnectionController;

struct PluginOptions {
	// version выводится в интерфейсе; settingsFile передаёт разрешённый владельцем путь хранилища.
	// Пустой путь не заменяется автоматически пользовательским/системным каталогом.
	QString version;
	QString settingsFile;
};

/**
 * Согласует загрузку модуля, два окна и время жизни сервисов с жизненным циклом OBS.
 * Frontend заимствован и должен пережить объект. Создание/завершение выполняются в GUI-потоке
 * при существующем QApplication. TextLookup копируется и должен оставаться вызываемым до очистки UI.
 *
 * Рабочие сервисы живут в Runtime; Dock после успешной регистрации принадлежит OBS.
 * ManagementWindow создаётся лениво, переживает закрытие и удаляется до общих сервисов.
 * shutdown синхронно освобождает Runtime, но внешняя оболочка с callback cookie сохраняется
 * до obs_module_unload. Это позволяет завершить OBS API до возврата из EXIT и отвергнуть поздние события.
 */
class PluginLifecycle final : public QObject {
	Q_OBJECT

public:
	static constexpr const char *DockId = "gost-restreamer-obs-plugin";
	PluginLifecycle(Frontend &frontend, TextLookup text, PluginOptions options = {});
	~PluginLifecycle() override;
	// Регистрирует Dock и callback до FinishedLoading без чтения профиля/статистики. Повтор при рабочем
	// Runtime возвращает true. Отказ регистрации/исключение очищает частично созданные ресурсы и закрывает
	// жизненный цикл; после такого shutdown повторная загрузка этого объекта не выполняется.
	bool load();
	// Запрещает новые действия, прекращает опрос OBS и сетевые операции плагина, снимает callback,
	// удаляет Dock и Runtime. Штатная трансляция OBS и серверная Session этим методом не останавливаются.
	// Вызывать до завершения Frontend API; из EXIT — прямо в callback, без отложенного deleteLater.
	// Идемпотентен и не выпускает исключения наружу; после него load() не возобновляет работу.
	void shutdown() noexcept;
	// Показывает/активирует единственный экземпляр, снимая минимизацию. Не создаёт новых сервисов
	// и подписок. QPointer допускает пересоздание только если прежнее окно уничтожил родитель OBS.
	void openManagementWindow();
	// Возвращаемые указатели заимствованы и действуют до shutdown(); вне рабочего периода они равны nullptr.
	StateStore *stateStore() const noexcept;
	SettingsStore *settingsStore() const noexcept;
	ConnectionController *connectionController() const noexcept;

private:
	class Runtime;
	// Граница callback C API: cookie заимствован, исключения перехватываются до выхода в OBS.
	static void frontendEvent(FrontendEvent event, void *cookie) noexcept;
	// При EXIT ресурсы освобождаются синхронно. Прочие события из чужого потока ставятся в очередь Runtime;
	// прочитанный позже профиль уже нельзя считать достоверным снимком исходного перехода.
	void handleEvent(FrontendEvent event, bool reliableProfile = true);
	Frontend &frontend_;
	TextLookup text_;
	PluginOptions options_;
	std::unique_ptr<Runtime> runtime_;
	bool callbackRegistered_ = false;
	bool dockRegistered_ = false;
	bool shuttingDown_ = false;
};

} // namespace gost
