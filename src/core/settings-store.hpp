#pragma once

#include "obs/frontend.hpp"

#include <QMap>
#include <QObject>
#include <QString>

#include <optional>

namespace gost {

struct ProfileRecord {
	QString uuid;
	QString name;
	QString locator;
};

struct ProfileResolution {
	QString uuid;
	bool resolved = false;
	bool changed = false;
	bool renamed = false;
};

class SettingsStore final : public QObject {
	Q_OBJECT

public:
	explicit SettingsStore(QString filePath, QObject *parent = nullptr);
	static std::optional<QString> canonicalOrigin(const QString &text);
	bool load();
	bool save();
	const QString &serverOrigin() const noexcept;
	bool autoConnect() const noexcept;
	bool reconnect() const noexcept;
	bool unsaved() const noexcept;
	const QString &error() const noexcept;
	const QString &errorKey() const noexcept;
	const QString &profileUuid() const noexcept;
	const QMap<QString, ProfileRecord> &profiles() const noexcept;
	QString selectedStream(const QString &origin, const QString &profileUuid) const;
	bool setServerOrigin(const QString &origin);
	void setAutoConnect(bool enabled);
	void setReconnect(bool enabled);
	bool setSelectedStream(const QString &origin, const QString &profileUuid, const QString &streamId);
	ProfileResolution observeProfile(FrontendEvent event, const ProfileDescriptor &descriptor);

signals:
	void changed();

private:
	void persist();
	void retireAbsent(const QStringList &inventory);
	QString filePath_;
	QString origin_;
	bool autoConnect_ = true;
	bool reconnect_ = true;
	bool unsaved_ = false;
	bool writeAllowed_ = true;
	QString error_;
	QString errorKey_;
	QMap<QString, ProfileRecord> profiles_;
	QMap<QString, QMap<QString, QString>> bindings_;
	QString currentUuid_;
	bool changing_ = false;
	bool transitionUncertain_ = false;
};

} // namespace gost
