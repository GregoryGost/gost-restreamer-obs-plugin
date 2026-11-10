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

std::unique_ptr<gost::OBSFrontend> frontend;
std::unique_ptr<gost::PluginLifecycle> lifecycle;

} // namespace

bool obs_module_load(void)
{
	try {
		if (lifecycle) {
			return true;
		}
		frontend = std::make_unique<gost::OBSFrontend>();
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
	try {
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
