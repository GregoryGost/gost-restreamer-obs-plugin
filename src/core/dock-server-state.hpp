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

// Содержит сохранённые намерения выхода и ID снимка назначения; исполнение наблюдается отдельно.
struct DockOutput {
	// name — серверное имя снимка назначения, которое используется только для отображения.
	QString name;
	// attached показывает подтверждённое прикрепление выхода к Session.
	bool attached = false;
	// desiredRunning задаёт сохранённое намерение запуска, не подтверждая публикацию.
	bool desiredRunning = false;
	// destinationId позволяет обнаружить уже прикреплённое назначение по точному публичному ID.
	QString destinationId;
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

// Содержит безопасное имя сохранённого назначения; его ID задаётся ключом карты Dock.
struct DockDestination {
	// name отображается даже без Session; секретные поля полного Destination здесь не хранятся.
	QString name;
};

// REST-метаданные Dock отделены от оперативных SSE-наблюдений; свежесть и полнота задаются для каждой части.
struct DockServerState {
	// sessionStatus описывает свежесть и полноту поиска единственной активной Session.
	ReadStatus sessionStatus;
	// session содержит подтверждённую серверную запись, независимо от наличия runtime.
	std::optional<DockSession> session;
	// outputsStatus относится к составу Outputs выбранной Session.
	ReadStatus outputsStatus;
	// outputs содержит сохранённые записи по Output ID, включая подтверждённый detach.
	QMap<QString, DockOutput> outputs;
	// fallback содержит независимо прочитанное назначение и метаданные заглушки.
	DockFallback fallback;
	// destinationsStatus описывает отдельное чтение каталога выбранного Stream, без зависимости от Session.
	ReadStatus destinationsStatus;
	// destinations хранит только безопасные имена по Destination ID, включая выключенные назначения.
	QMap<QString, DockDestination> destinations;
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
