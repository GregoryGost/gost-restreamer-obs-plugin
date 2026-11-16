#include <obs-module.h>
#include <plugin-support.h>
#include <gost-build-version.hpp>
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

// Создаёт адаптер раньше lifecycle и освобождает их в обратном порядке при отказе.
// Исключения C++ не пересекают границу загрузчика модулей OBS.
bool obs_module_load(void)
{
	try {
		if (lifecycle) {
			return true;
		}
		frontend = std::make_unique<gost::OBSFrontend>();
		// settingsPath удерживает выделенный OBS путь; QString копирует его до освобождения через bfree.
		const std::unique_ptr<char, decltype(&bfree)> settingsPath(obs_module_config_path("settings.json"),
									   &bfree);
		// options передаёт Runtime версию, собственный путь настроек и безопасный адаптер журнала.
		gost::PluginOptions options;
		options.version = QString::fromUtf8(GOST_BUILD_VERSION);
		options.settingsFile = settingsPath ? QString::fromUtf8(settingsPath.get()) : QString{};
		// Обработчик severity/message направляет готовую безопасную запись в штатный журнал OBS.
		// Он не устанавливает глобальный Qt-handler и не обращается к содержимому HTTP-запросов.
		options.logSink = [](gost::LogSeverity severity, const QString &message) {
			// level переводит закрытую классификацию ядра в уровень OBS, bytes держит UTF-8 до obs_log.
			const int level = severity == gost::LogSeverity::Error     ? LOG_ERROR
					  : severity == gost::LogSeverity::Warning ? LOG_WARNING
										   : LOG_INFO;
			const auto bytes = message.toUtf8();
			obs_log(level, "%s", bytes.constData());
		};
		// Переводчик получает ключ key из ядра и копирует локализованный текст OBS в QString.
		lifecycle = std::make_unique<gost::PluginLifecycle>(
			*frontend, [](const char *key) { return QString::fromUtf8(obs_module_text(key)); },
			std::move(options));
		if (!lifecycle->load()) {
			lifecycle.reset();
			frontend.reset();
			obs_log(LOG_ERROR, "Не удалось загрузить GOST Restreamer.");
			return false;
		}
		obs_log(LOG_INFO, "GOST Restreamer загружен (версия %s).", GOST_BUILD_VERSION);
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
