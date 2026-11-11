#pragma once

#include <QMap>
#include <QString>

#include <optional>

namespace gost {

enum class RuntimeAvailability { Unknown, Fresh, Stale };

struct ReadStatus {
	RuntimeAvailability availability = RuntimeAvailability::Unknown;
	bool complete = false;
	QString safeError;
};

struct DockContext {
	QString origin;
	QString profileUuid;
	QString streamId;
	bool valid() const noexcept { return !origin.isEmpty() && !profileUuid.isEmpty() && !streamId.isEmpty(); }
	bool operator==(const DockContext &) const = default;
};

struct DockSession {
	QString id;
	QString state;
	bool desiredPaused = false;
};

struct DockOutput {
	QString name;
	bool attached = false;
	bool desiredRunning = false;
};

struct DockFallback {
	RuntimeAvailability availability = RuntimeAvailability::Unknown;
	std::optional<bool> assigned;
	bool assetKnown = false;
	QString currentGenerationId;
	QString pendingGenerationId;
	QString currentStatus;
};

struct DockServerState {
	ReadStatus sessionStatus;
	std::optional<DockSession> session;
	ReadStatus outputsStatus;
	QMap<QString, DockOutput> outputs;
	DockFallback fallback;
};

struct RuntimeEvidence {
	RuntimeAvailability state = RuntimeAvailability::Unknown;
	RuntimeAvailability input = RuntimeAvailability::Unknown;
	RuntimeAvailability metrics = RuntimeAvailability::Unknown;
};

} // namespace gost
