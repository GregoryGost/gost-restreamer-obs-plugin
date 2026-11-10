#pragma once

#include "api/models.hpp"

#include <QObject>
#include <QString>

#include <optional>

namespace gost {

enum class ConnectionStatus { Disconnected, Connecting, Connected, Reconnecting, Error };
enum class RuntimeAvailability { Unknown, Fresh, Stale };

struct ReadStatus {
	RuntimeAvailability availability = RuntimeAvailability::Unknown;
	bool complete = false;
	QString safeError;
};

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
	const QString &safeError() const noexcept;
	const QString &safeErrorKey() const noexcept;
	bool settingsUnsaved() const noexcept;
	const QString &profileNotice() const noexcept;
	const QString &profileNoticeKey() const noexcept;
	const ConfigurationState &configuration() const noexcept;
	const HistoryState &history() const noexcept;
	const DetailState &details() const noexcept;
	const RuntimeState &runtime() const noexcept;
	const std::optional<api::Health> &ready() const noexcept;
	const std::optional<api::Capabilities> &capabilities() const noexcept;
	void setConnection(QString origin, ConnectionStatus status, QString safeError = {}, QString errorKey = {});
	void setPreferences(bool autoConnect, bool reconnect, bool unsaved, QString settingsError = {},
			    QString errorKey = {});
	void setStreams(QVector<api::Stream> streams, bool complete);
	void setSelection(QString streamId, bool profileResolved, QString notice = {}, QString noticeKey = {});
	void setReady(api::Health ready);
	void setCapabilities(api::Capabilities capabilities);
	void setConfiguration(ConfigurationState configuration);
	void setHistory(HistoryState history);
	void setDetails(DetailState details);
	bool installAnchor(const api::Anchor &anchor);
	bool applyEvent(const api::RuntimeEvent &event);
	void markRuntimeStale();
	void clearServerState();
	void clearRuntime();

signals:
	void obsStateChanged();
	void serverStateChanged();
	void streamsChanged();
	void runtimeStateChanged();

private:
	std::optional<QString> profileName_;
	std::optional<bool> streamingActive_;
	QString origin_;
	ConnectionStatus connectionStatus_ = ConnectionStatus::Disconnected;
	QVector<api::Stream> streams_;
	bool streamsComplete_ = false;
	QString selectedStreamId_;
	bool profileResolved_ = false;
	bool autoConnect_ = true;
	bool reconnect_ = true;
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
	QString gpuInstanceId_;
	std::optional<quint64> gpuRevision_;
	std::optional<api::Health> ready_;
	std::optional<api::Capabilities> capabilities_;
	struct Watermarks {
		std::optional<quint64> state;
		std::optional<quint64> metrics;
	};
	QMap<QString, Watermarks> watermarks_;
};

} // namespace gost
