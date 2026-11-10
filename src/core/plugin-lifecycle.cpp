#include "plugin-lifecycle.hpp"

#include "api/api-client.hpp"
#include "api/event-client.hpp"
#include "core/connection-controller.hpp"
#include "core/settings-store.hpp"
#include "core/state-store.hpp"
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

class PluginLifecycle::Runtime final : public QObject {
public:
	explicit Runtime(Frontend &frontend, const PluginOptions &options)
	{
		stateStore = std::make_unique<StateStore>();
		settings = std::make_unique<SettingsStore>(options.settingsFile);
		apiClient = std::make_unique<api::APIClient>();
		eventClient = std::make_unique<api::EventClient>();
		connection = std::make_unique<ConnectionController>(*stateStore, *settings, *apiClient, *eventClient);
		controller = std::make_unique<OBSController>(frontend, *stateStore);
		connect(controller.get(), &OBSController::profileObserved, connection.get(),
			&ConnectionController::observeProfile);
	}
	~Runtime() override
	{
		connection->shutdown();
		delete managementWindow.data();
	}

	std::unique_ptr<StateStore> stateStore;
	std::unique_ptr<SettingsStore> settings;
	std::unique_ptr<api::APIClient> apiClient;
	std::unique_ptr<api::EventClient> eventClient;
	std::unique_ptr<ConnectionController> connection;
	std::unique_ptr<OBSController> controller;
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
	auto *application = qobject_cast<QApplication *>(QCoreApplication::instance());
	if (shuttingDown_ || !application || QThread::currentThread() != application->thread()) {
		return false;
	}
	if (runtime_) {
		return true;
	}
	try {
		runtime_ = std::make_unique<Runtime>(frontend_, options_);
		auto dock = std::make_unique<RestreamDock>(*runtime_->stateStore, text_, nullptr, options_.version);
		runtime_->dock = dock.get();
		connect(dock.get(), &RestreamDock::managementRequested, runtime_.get(),
			[this] { openManagementWindow(); });
		if (!frontend_.addDock(DockId, text_("Plugin.Name"), dock.get())) {
			shutdown();
			return false;
		}
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
	shuttingDown_ = true;
	Q_ASSERT(!runtime_ || QThread::currentThread() == thread());
	if (runtime_) {
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
	runtime_.reset();
}

void PluginLifecycle::openManagementWindow()
{
	Q_ASSERT(QThread::currentThread() == thread());
	if (shuttingDown_ || !runtime_) {
		return;
	}
	try {
		if (!runtime_->managementWindow) {
			runtime_->managementWindow = new ManagementWindow(
				*runtime_->stateStore, text_, frontend_.mainWindow(), runtime_->connection.get());
		}
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

void PluginLifecycle::frontendEvent(FrontendEvent event, void *cookie) noexcept
{
	auto *self = static_cast<PluginLifecycle *>(cookie);
	try {
		self->handleEvent(event);
	} catch (...) {
		qWarning("Не удалось обработать событие OBS в GOST Restreamer.");
	}
}

void PluginLifecycle::handleEvent(FrontendEvent event, bool reliableProfile)
{
	if (shuttingDown_ || !runtime_) {
		return;
	}
	if (event == FrontendEvent::Exit) {
		// OBS отправляет EXIT из GUI-потока до уничтожения Frontend API.
		shutdown();
		return;
	}
	if (QThread::currentThread() != thread()) {
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
