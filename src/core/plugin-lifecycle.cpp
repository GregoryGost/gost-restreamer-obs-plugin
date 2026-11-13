#include "plugin-lifecycle.hpp"

#include "api/api-client.hpp"
#include "api/event-client.hpp"
#include "core/connection-controller.hpp"
#include "core/settings-store.hpp"
#include "core/state-store.hpp"
#include "core/restream-controller.hpp"
#include "obs/frontend.hpp"
#include "obs/obs-controller.hpp"
#include "ui/management-window.hpp"
#include "ui/restream-dock.hpp"

#include <QApplication>
#include <QDebug>
#include <QPointer>
#include <QThread>

#include <utility>

namespace gost {

// Runtime владеет сервисами плагина; контроллеры и окна обращаются к ним по заимствованным ссылкам.
// Поля объявлены так, чтобы контроллеры уничтожались раньше сервисов, которые они используют.
// Удаление Runtime также отменяет адресованные ему отложенные вызовы Qt. Dock принадлежит OBS
// и только отслеживается через QPointer; ManagementWindow удаляется явно до StateStore и SettingsStore.
class PluginLifecycle::Runtime final : public QObject {
public:
	// Создаёт наблюдение с бюджетом 3/24 и независимый канал команд 1/8; frontend и options заимствованы на вызов.
	explicit Runtime(Frontend &frontend, const PluginOptions &options)
	{
		stateStore = std::make_unique<StateStore>();
		settings = std::make_unique<SettingsStore>(options.settingsFile);
		// observationLimits ограничивает канал REST наблюдения, сохраняя суммарный бюджет двух клиентов.
		api::Limits observationLimits;
		observationLimits.maxActiveRequests = 3;
		observationLimits.maxQueuedRequests = 24;
		apiClient = std::make_unique<api::APIClient>(observationLimits);
		// commandLimits выделяет workflow отдельную последовательную очередь, независимую от recovery SSE.
		api::Limits commandLimits;
		commandLimits.maxActiveRequests = 1;
		commandLimits.maxQueuedRequests = 8;
		commandApi = std::make_unique<api::APIClient>(commandLimits);
		eventClient = std::make_unique<api::EventClient>();
		connection = std::make_unique<ConnectionController>(*stateStore, *settings, *apiClient, *eventClient);
		controller = std::make_unique<OBSController>(frontend, *stateStore);
		restream = std::make_unique<RestreamController>(*stateStore, *commandApi, *connection, *controller);
		connect(controller.get(), &OBSController::profileObserved, connection.get(),
			&ConnectionController::observeProfile);
	}
	// Прекращает workflow раньше OBS и сети; удаление скрытого окна предшествует удалению StateStore.
	~Runtime() override
	{
		restream->shutdown();
		connection->shutdown();
		// Окно хранит ссылки на общее состояние, поэтому удаляется до уничтожения сервисов.
		delete managementWindow.data();
	}

	// Обратный порядок уничтожения полей освобождает контроллеры раньше их зависимостей.
	// stateStore хранит общие наблюдения, settings читает и записывает разрешённый файл предпочтений.
	std::unique_ptr<StateStore> stateStore;
	std::unique_ptr<SettingsStore> settings;
	// apiClient владеет очередью наблюдения 3/24; commandApi — отдельной очередью явных команд 1/8.
	std::unique_ptr<api::APIClient> apiClient;
	std::unique_ptr<api::APIClient> commandApi;
	// eventClient обслуживает единственную подписку SSE.
	std::unique_ptr<api::EventClient> eventClient;
	// connection управляет наблюдением, controller читает OBS, restream координирует явные намерения.
	std::unique_ptr<ConnectionController> connection;
	std::unique_ptr<OBSController> controller;
	std::unique_ptr<RestreamController> restream;
	// dock отслеживает переданный OBS виджет; managementWindow отслеживает скрываемое расширенное окно.
	// Оба окна могут быть удалены родителем OBS; QPointer не продлевает их жизнь.
	QPointer<RestreamDock> dock;
	QPointer<ManagementWindow> managementWindow;
};

PluginLifecycle::PluginLifecycle(Frontend &frontend, TextLookup text, PluginOptions options)
	: frontend_(frontend),
	  text_(std::move(text)),
	  options_(std::move(options))
{
}

PluginLifecycle::~PluginLifecycle()
{
	shutdown();
}

bool PluginLifecycle::load()
{
	// application проверяет существование GUI и принадлежность вызова его потоку.
	// Локальный unique_ptr удерживает Dock до подтверждения передачи OBS. Если addDock вернул false,
	// shutdown очищает Runtime, а локальный указатель освобождает ещё не переданный виджет.
	auto *application = qobject_cast<QApplication *>(QCoreApplication::instance());
	if (shuttingDown_ || !application || QThread::currentThread() != application->thread()) {
		return false;
	}
	if (runtime_) {
		return true;
	}
	try {
		runtime_ = std::make_unique<Runtime>(frontend_, options_);
		// dock удерживает виджет до успешной передачи Frontend; отказ регистрации не оставляет утечки.
		auto dock = std::make_unique<RestreamDock>(*runtime_->stateStore, text_, nullptr, options_.version,
							   runtime_->restream.get());
		runtime_->dock = dock.get();
		// Callback шестерёнки показывает только одно окно и прекращается вместе с Runtime.
		connect(dock.get(), &RestreamDock::managementRequested, runtime_.get(),
			[this] { openManagementWindow(); });
		// Меню выхода открывает тот же экземпляр окна и передаёт точные IDs исходного нажатия.
		connect(dock.get(), &RestreamDock::outputDetailsRequested, runtime_.get(),
			[this](DockContext context, QString sessionId, QString outputId) {
				openManagementWindow();
				if (runtime_ && runtime_->managementWindow)
					runtime_->managementWindow->selectOutput(context, sessionId, outputId);
			});
		if (!frontend_.addDock(DockId, text_("Plugin.Name"), dock.get())) {
			shutdown();
			return false;
		}
		// Только успешная регистрация передаёт виджет OBS; при отказе его удаляет локальный unique_ptr.
		dock.release();
		dockRegistered_ = true;
		frontend_.addEventCallback(&PluginLifecycle::frontendEvent, this);
		callbackRegistered_ = true;
		return true;
	} catch (...) {
		qWarning("Не удалось создать интерфейс GOST Restreamer.");
		shutdown();
		return false;
	}
}

void PluginLifecycle::shutdown() noexcept
{
	if (shuttingDown_) {
		return;
	}
	// Запрет новых действий устанавливается до очистки, которая сама может вызвать сигналы Qt.
	shuttingDown_ = true;
	Q_ASSERT(!runtime_ || QThread::currentThread() == thread());
	if (runtime_) {
		// Останавливаем чтение OBS и сетевые обновления, пока общее состояние ещё существует.
		runtime_->restream->shutdown();
		runtime_->controller->shutdown();
		runtime_->connection->shutdown();
	}
	if (callbackRegistered_) {
		callbackRegistered_ = false;
		try {
			frontend_.removeEventCallback(&PluginLifecycle::frontendEvent, this);
		} catch (...) {
			qWarning("Не удалось снять обработчик событий GOST Restreamer.");
		}
	}
	// Обработчик OBS снят до удаления Dock; его сигналы также не должны повторно открыть окно.
	if (runtime_ && runtime_->dock) {
		QObject::disconnect(runtime_->dock.data(), nullptr, nullptr, nullptr);
	}
	if (dockRegistered_) {
		dockRegistered_ = false;
		try {
			frontend_.removeDock(DockId);
		} catch (...) {
			qWarning("Не удалось удалить Dock GOST Restreamer.");
		}
	}
	// Освобождаем рабочие ресурсы синхронно; оболочка остаётся неактивной до выгрузки модуля.
	runtime_.reset();
}

void PluginLifecycle::openManagementWindow()
{
	Q_ASSERT(QThread::currentThread() == thread());
	if (shuttingDown_ || !runtime_) {
		return;
	}
	try {
		// Закрытие ManagementWindow скрывает его; QPointer дополнительно учитывает удаление главным окном OBS.
		if (!runtime_->managementWindow) {
			runtime_->managementWindow =
				new ManagementWindow(*runtime_->stateStore, text_, frontend_.mainWindow(),
						     runtime_->connection.get(), runtime_->restream.get());
		}
		// window заимствован у Runtime на время синхронного показа и активации.
		auto *window = runtime_->managementWindow.data();
		if (window->isMinimized()) {
			window->setWindowState(window->windowState() & ~Qt::WindowMinimized);
		}
		window->show();
		window->raise();
		window->activateWindow();
	} catch (...) {
		qWarning("Не удалось открыть окно управления GOST Restreamer.");
	}
}

StateStore *PluginLifecycle::stateStore() const noexcept
{
	return runtime_ ? runtime_->stateStore.get() : nullptr;
}

SettingsStore *PluginLifecycle::settingsStore() const noexcept
{
	return runtime_ ? runtime_->settings.get() : nullptr;
}

ConnectionController *PluginLifecycle::connectionController() const noexcept
{
	return runtime_ ? runtime_->connection.get() : nullptr;
}

// Возвращает фасад текущего Runtime без передачи владения; закрытие ManagementWindow его не уничтожает.
RestreamController *PluginLifecycle::restreamController() const noexcept
{
	return runtime_ ? runtime_->restream.get() : nullptr;
}

void PluginLifecycle::frontendEvent(FrontendEvent event, void *cookie) noexcept
{
	auto *self = static_cast<PluginLifecycle *>(cookie);
	// OBS хранит заимствованный cookie; оболочка остаётся жива до снятия обработчика и выгрузки модуля.
	// Исключения обработчика перехватываются здесь, чтобы не пересечь границу C API.
	try {
		self->handleEvent(event);
	} catch (...) {
		qWarning("Не удалось обработать событие OBS в GOST Restreamer.");
	}
}

void PluginLifecycle::handleEvent(FrontendEvent event, bool reliableProfile)
{
	// EXIT является последней разрешённой фазой Frontend API. Остальные события можно поставить
	// в очередь GUI, но тогда имя/путь/inventory уже нельзя выдавать за снимок момента исходного callback.
	if (shuttingDown_ || !runtime_) {
		return;
	}
	if (event == FrontendEvent::Exit) {
		// OBS отправляет EXIT из GUI-потока до уничтожения Frontend API.
		// Откладывать очистку нельзя: OBS отключает Frontend API сразу после рассылки EXIT.
		shutdown();
		return;
	}
	if (QThread::currentThread() != thread()) {
		// Контекст Runtime исключает доставку после его удаления. Перенесённое событие уже не даёт
		// достоверного снимка профиля на момент исходного вызова, поэтому reliableProfile сбрасывается.
		QMetaObject::invokeMethod(
			runtime_.get(),
			[this, event] {
				try {
					handleEvent(event, false);
				} catch (...) {
					qWarning("Не удалось обработать отложенное событие OBS.");
				}
			},
			Qt::QueuedConnection);
		return;
	}
	runtime_->controller->handleEvent(event, reliableProfile);
}

} // namespace gost
