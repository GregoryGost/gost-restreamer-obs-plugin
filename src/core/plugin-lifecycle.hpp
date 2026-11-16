#pragma once

#include "ui/text-lookup.hpp"
#include "core/log-sink.hpp"

#include <QObject>

#include <memory>

namespace gost {

class Frontend;
enum class FrontendEvent;
class StateStore;
class SettingsStore;
class ConnectionController;
class RestreamController;
class ConfigurationController;

// Параметры загрузки модуля, не содержащие состояния команд.
struct PluginOptions {
	// version выводится в интерфейсе; settingsFile передаёт разрешённый владельцем путь хранилища.
	// Пустой путь не заменяется автоматически пользовательским/системным каталогом.
	QString version;
	QString settingsFile;
	// Принимает только подготовленные безопасные записи; production-адаптер направляет их в obs_log.
	LogSink logSink;
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
	// DockId задаёт стабильное имя зарегистрированного OBS Dock.
	static constexpr const char *DockId = "gost-restreamer-obs-plugin";
	// Сохраняет заимствованный Frontend, переводчик text и разрешённые параметры загрузки options.
	PluginLifecycle(Frontend &frontend, TextLookup text, PluginOptions options = {});
	// Завершает рабочие сервисы до разрушения оболочки callback.
	~PluginLifecycle() override;
	// Регистрирует Dock, пункт «Сервис» и callback до FinishedLoading без чтения профиля и статистики.
	// Повтор при рабочем Runtime возвращает true. Отказ регистрации/исключение очищает частичные ресурсы и закрывает
	// жизненный цикл; после такого shutdown повторная загрузка этого объекта не выполняется.
	bool load();
	// Запрещает новые действия, прекращает опрос OBS и сетевые операции плагина, снимает callback,
	// удаляет пункт «Сервис», Dock и Runtime. Штатная трансляция OBS и серверная Session продолжают работать.
	// Вызывать до завершения Frontend API; из EXIT — прямо в callback, без отложенного deleteLater.
	// Идемпотентен и не выпускает исключения наружу; после него load() не возобновляет работу.
	void shutdown() noexcept;
	// Показывает/активирует единственный экземпляр, снимая минимизацию. Не создаёт новых сервисов
	// и подписок. QPointer допускает пересоздание только если прежнее окно уничтожил родитель OBS.
	void openManagementWindow();
	// Возвращаемые указатели заимствованы и действуют до shutdown(); вне рабочего периода они равны nullptr.
	StateStore *stateStore() const noexcept;
	// Возвращает заимствованное хранилище предпочтений рабочего Runtime либо nullptr после shutdown.
	SettingsStore *settingsStore() const noexcept;
	// Возвращает заимствованный контроллер наблюдения рабочего Runtime либо nullptr после shutdown.
	ConnectionController *connectionController() const noexcept;
	// Возвращает заимствованный фасад явных команд; после shutdown результат равен nullptr.
	RestreamController *restreamController() const noexcept;
	// Возвращает заимствованный контроллер редакторов либо nullptr после завершения Runtime.
	ConfigurationController *configurationController() const noexcept;

private:
	// Runtime создаёт и завершает общие сервисы независимо от видимости окон.
	class Runtime;
	// Граница callback C API: cookie заимствован, исключения перехватываются до выхода в OBS.
	static void frontendEvent(FrontendEvent event, void *cookie) noexcept;
	// При EXIT ресурсы освобождаются синхронно. Прочие события из чужого потока ставятся в очередь Runtime;
	// прочитанный позже профиль уже нельзя считать достоверным снимком исходного перехода.
	void handleEvent(FrontendEvent event, bool reliableProfile = true);
	// frontend_ заимствован у OBS; text_ переводит подписи; options_ содержит версию и разрешённый файл настроек.
	Frontend &frontend_;
	TextLookup text_;
	PluginOptions options_;
	// runtime_ владеет рабочими сервисами, callbackRegistered_ и dockRegistered_ отслеживают регистрацию OBS.
	std::unique_ptr<Runtime> runtime_;
	bool callbackRegistered_ = false;
	bool dockRegistered_ = false;
	// shuttingDown_ запрещает поздние события и повторную загрузку завершённой оболочки.
	bool shuttingDown_ = false;
};

} // namespace gost
