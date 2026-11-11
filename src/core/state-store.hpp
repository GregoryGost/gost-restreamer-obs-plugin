#pragma once

#include "api/models.hpp"
#include "core/dock-server-state.hpp"
#include "obs/obs-statistics.hpp"

#include <QObject>
#include <QString>

#include <optional>

namespace gost {

enum class ConnectionStatus { Disconnected, Connecting, Connected, Reconnecting, Error };

struct ConfigurationState {
	QMap<QString, api::Stream> streams;
	QMap<QString, api::Destination> destinations;
	QMap<QString, api::Group> groups;
	QMap<QString, api::Profile> profiles;
	QMap<QString, api::Asset> assets;
	QMap<QString, api::Generation> generations;
	QMap<QString, api::Variant> variants;
	ReadStatus streamsStatus;
	ReadStatus destinationsStatus;
	ReadStatus groupsStatus;
	ReadStatus profilesStatus;
	ReadStatus assetsStatus;
	ReadStatus generationsStatus;
	ReadStatus variantsStatus;
};

struct HistoryState {
	QMap<QString, api::Session> sessions;
	QMap<QString, api::Output> outputs;
	QMap<QString, api::Receipt> receipts;
	ReadStatus sessionsStatus;
	ReadStatus outputsStatus;
	ReadStatus receiptsStatus;
};

struct DetailState {
	QMap<QString, api::SessionMetrics> sessionMetrics;
	QMap<QString, api::OutputDetail> outputs;
	QMap<QString, ReadStatus> sessionMetricsStatus;
	QMap<QString, ReadStatus> outputsStatus;
};

struct RuntimeState {
	QMap<QString, api::RuntimeSession> sessions;
	api::JsonField<api::GPU> gpu;
	QString cursor;
	RuntimeAvailability availability = RuntimeAvailability::Unknown;
};

class StateStore final : public QObject {
	Q_OBJECT

public:
	explicit StateStore(QObject *parent = nullptr);
	const std::optional<QString> &profileName() const noexcept;
	std::optional<bool> streamingActive() const noexcept;
	void setObsState(QString profileName, bool streamingActive);
	const ObsStatistics &obsStatistics() const noexcept;
	void setObsStatistics(ObsStatistics statistics);
	const QString &serverOrigin() const noexcept;
	ConnectionStatus connectionStatus() const noexcept;
	const QVector<api::Stream> &streams() const noexcept;
	bool streamsComplete() const noexcept;
	const QString &selectedStreamId() const noexcept;
	QString selectedStreamName() const;
	bool selectionAvailable() const noexcept;
	bool profileResolved() const noexcept;
	bool autoConnect() const noexcept;
	bool reconnect() const noexcept;
	int sseRejectionRetries() const noexcept;
	const QString &safeError() const noexcept;
	const QString &safeErrorKey() const noexcept;
	bool settingsUnsaved() const noexcept;
	const QString &profileNotice() const noexcept;
	const QString &profileNoticeKey() const noexcept;
	const ConfigurationState &configuration() const noexcept;
	const HistoryState &history() const noexcept;
	const DetailState &details() const noexcept;
	const RuntimeState &runtime() const noexcept;
	const DockContext &dockContext() const noexcept;
	const DockServerState &dockServer() const noexcept;
	RuntimeEvidence runtimeEvidence(const QString &instanceId) const;
	const std::optional<api::Health> &ready() const noexcept;
	const std::optional<api::Capabilities> &capabilities() const noexcept;
	void setConnection(QString origin, ConnectionStatus status, QString safeError = {}, QString errorKey = {});
	void setPreferences(bool autoConnect, bool reconnect, int sseRejectionRetries, bool unsaved,
			    QString settingsError = {}, QString errorKey = {});
	void setStreams(QVector<api::Stream> streams, bool complete);
	void setSelection(QString streamId, bool profileResolved, QString notice = {}, QString noticeKey = {},
			  QString profileUuid = {});
	void setDockServer(DockServerState metadata);
	void setReady(api::Health ready);
	void setCapabilities(api::Capabilities capabilities);
	void setConfiguration(ConfigurationState configuration);
	void setHistory(HistoryState history);
	void setDetails(DetailState details);
	bool installAnchor(const api::Anchor &anchor, bool awaitSubscription = false);
	bool applyEvent(const api::RuntimeEvent &event, bool *dockTopologyChanged = nullptr);
	void markRuntimeStale();
	void clearServerState(bool preserveDock = false);
	void clearRuntime();

signals:
	void obsStateChanged();
	void obsStatisticsChanged();
	void serverStateChanged();
	void streamsChanged();
	void runtimeAboutToBecomeStale();
	void runtimeStateChanged();

private:
	std::optional<QString> profileName_;
	std::optional<bool> streamingActive_;
	ObsStatistics obsStatistics_;
	QString origin_;
	ConnectionStatus connectionStatus_ = ConnectionStatus::Disconnected;
	QVector<api::Stream> streams_;
	bool streamsComplete_ = false;
	QString selectedStreamId_;
	bool profileResolved_ = false;
	bool autoConnect_ = true;
	bool reconnect_ = true;
	int sseRejectionRetries_;
	bool settingsUnsaved_ = false;
	QString connectionError_;
	QString connectionErrorKey_;
	QString settingsError_;
	QString settingsErrorKey_;
	QString profileNotice_;
	QString profileNoticeKey_;
	ConfigurationState configuration_;
	HistoryState history_;
	DetailState details_;
	RuntimeState runtime_;
	DockContext dockContext_;
	DockServerState dockServer_;
	bool pendingAnchor_ = false;
	QString gpuInstanceId_;
	std::optional<quint64> gpuRevision_;
	std::optional<api::Health> ready_;
	std::optional<api::Capabilities> capabilities_;
	struct Watermarks {
		std::optional<quint64> state;
		std::optional<quint64> metrics;
		RuntimeEvidence evidence;
	};
	QMap<QString, Watermarks> watermarks_;
};

} // namespace gost
