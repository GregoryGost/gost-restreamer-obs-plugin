#pragma once

#include <QMap>
#include <QString>

#include <optional>

namespace gost {

// Unknown означает отсутствие подтверждения; Stale сохраняет известное ранее, но уже не текущее наблюдение.
enum class RuntimeAvailability { Unknown, Fresh, Stale };

struct ReadStatus {
	// complete показывает полноту прочитанных данных, availability — их свежесть. Fresh/complete=false
	// означает достоверно полученную часть, а не полный список; safeError не должен содержать секретов.
	RuntimeAvailability availability = RuntimeAvailability::Unknown;
	bool complete = false;
	QString safeError;
};

// Кеш Dock относится к точной тройке сервера, устойчивого UUID профиля OBS и выбранного Stream.
struct DockContext {
	QString origin;
	QString profileUuid;
	QString streamId;
	bool valid() const noexcept { return !origin.isEmpty() && !profileUuid.isEmpty() && !streamId.isEmpty(); }
	bool operator==(const DockContext &) const = default;
};

struct DockSession {
	// Сохранённое состояние выбранной Session, не подтверждение текущей телеметрии её исполнений.
	QString id;
	QString state;
	bool desiredPaused = false;
};

struct DockOutput {
	// Имя и намерения из REST; наблюдаемый статус/битрейт берутся отдельно из RuntimeOutput.
	QString name;
	bool attached = false;
	bool desiredRunning = false;
};

struct DockFallback {
	// assigned=nullopt означает неизвестность чтения, false — подтверждённое отсутствие политики.
	// assetKnown отделяет доступность Asset от назначения; pending не подменяет статус current.
	RuntimeAvailability availability = RuntimeAvailability::Unknown;
	std::optional<bool> assigned;
	bool assetKnown = false;
	QString currentGenerationId;
	QString pendingGenerationId;
	QString currentStatus;
};

// REST-метаданные Dock отделены от оперативных SSE-наблюдений; свежесть и полнота задаются для каждой части.
struct DockServerState {
	ReadStatus sessionStatus;
	std::optional<DockSession> session;
	ReadStatus outputsStatus;
	QMap<QString, DockOutput> outputs;
	DockFallback fallback;
};

// Независимые подтверждения для одного исполнения: state — его состояние, input — подключение входа
// Session, metrics — измерения. Свежая метрика не подтверждает состояние, а событие подключения входа
// не подтверждает свежесть его скоростей. Ось наблюдения — одна из этих трёх независимых частей.
struct RuntimeEvidence {
	RuntimeAvailability state = RuntimeAvailability::Unknown;
	RuntimeAvailability input = RuntimeAvailability::Unknown;
	RuntimeAvailability metrics = RuntimeAvailability::Unknown;
};

} // namespace gost
