#include <obs-module.h>
#include <plugin-support.h>
#include <util/bmem.h>

#include "core/plugin-lifecycle.hpp"
#include "obs/obs-frontend.hpp"

#include <QString>

#include <memory>

OBS_DECLARE_MODULE()
OBS_MODULE_USE_DEFAULT_LOCALE(PLUGIN_NAME, "en-US")

namespace {

// Адаптер OBS должен пережить PluginLifecycle: завершение последнего снимает подписки через адаптер.
std::unique_ptr<gost::OBSFrontend> frontend;
std::unique_ptr<gost::PluginLifecycle> lifecycle;

} // namespace

bool obs_module_load(void)
{
	// Модуль создаёт адаптер раньше lifecycle, а при любой ошибке уничтожает в обратном порядке.
	// Полученные строки копируются в QString. Выделенную OBS память пути освобождает bfree,
	// чтобы выделение и освобождение выполнялись одним API, без delete/free из модуля плагина.
	// Исключения из создания объектов C++ не должны пересекать границу загрузчика модулей OBS.
	try {
		if (lifecycle) {
			return true;
		}
		frontend = std::make_unique<gost::OBSFrontend>();
		// Путь выделяет OBS; QString копирует значение до освобождения через bfree.
		const std::unique_ptr<char, decltype(&bfree)> settingsPath(obs_module_config_path("settings.json"),
									   &bfree);
		gost::PluginOptions options;
		options.version = QString::fromUtf8(PLUGIN_VERSION);
		options.settingsFile = settingsPath ? QString::fromUtf8(settingsPath.get()) : QString{};
		lifecycle = std::make_unique<gost::PluginLifecycle>(
			*frontend, [](const char *key) { return QString::fromUtf8(obs_module_text(key)); },
			std::move(options));
		if (!lifecycle->load()) {
			lifecycle.reset();
			frontend.reset();
			obs_log(LOG_ERROR, "Не удалось загрузить GOST Restreamer.");
			return false;
		}
		obs_log(LOG_INFO, "GOST Restreamer загружен (версия %s).", PLUGIN_VERSION);
		return true;
	} catch (...) {
		lifecycle.reset();
		frontend.reset();
		obs_log(LOG_ERROR, "Ошибка загрузки GOST Restreamer.");
		return false;
	}
}

void obs_module_unload(void)
{
	// Здесь уничтожается и неактивная оболочка после EXIT. Прямой unload без EXIT также проходит
	// через деструктор lifecycle; frontend обязан оставаться доступным до его завершения.
	try {
		// После штатного EXIT активные ресурсы уже освобождены; повторное завершение безопасно.
		lifecycle.reset();
		frontend.reset();
	} catch (...) {
	}
}

const char *obs_module_name(void)
{
	return "GOST Restreamer";
}

const char *obs_module_description(void)
{
	return obs_module_text("Plugin.Description");
}
