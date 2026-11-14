#pragma once

#include <QString>
#include <functional>

namespace gost {

// Уровень готовой безопасной записи, передаваемой из ядра адаптеру журнала OBS.
enum class LogSeverity { Info, Warning, Error };

// Получает severity и готовое message без секретов. Обработчик не должен менять
// время жизни сервисов плагина или повторно вызывать их методы; ядро не зависит от OBS.
using LogSink = std::function<void(LogSeverity severity, const QString &message)>;

} // namespace gost
