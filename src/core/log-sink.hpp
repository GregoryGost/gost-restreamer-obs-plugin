#pragma once

#include <QString>
#include <functional>

namespace gost {

// Уровень готовой записи, передаваемой из ядра адаптеру журнала OBS.
enum class LogSeverity { Info, Warning, Error };

// Получает severity и готовое message. Для отказа явной операции message включает полученную
// диагностику без маскирования секретов; остальные категории не содержат request/config dumps.
// Адаптер использует постоянную format string и передаёт message аргументом, а не шаблоном.
// Обработчик не меняет время жизни сервисов или повторно не вызывает их методы; ядро не зависит от OBS.
using LogSink = std::function<void(LogSeverity severity, const QString &message)>;

} // namespace gost
