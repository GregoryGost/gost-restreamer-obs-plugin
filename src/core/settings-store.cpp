#include "settings-store.hpp"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSaveFile>
#include <QSet>
#include <QThread>
#include <QUuid>
#include <QUrl>

#include <utility>

namespace gost {
namespace {
constexpr qint64 MaxSettingsBytes = 1024 * 1024;
constexpr qsizetype MaxProfiles = 1024;
constexpr qsizetype MaxServers = 128;
constexpr qsizetype MaxBindings = 10000;

bool validText(const QString &value, bool allowEmpty = false)
{
	return (allowEmpty || !value.isEmpty()) && value.size() <= 4096 && !value.contains(QChar::Null);
}

bool exactKeys(const QJsonObject &object, const QStringList &keys)
{
	if (object.size() != keys.size())
		return false;
	for (const auto &key : keys)
		if (!object.contains(key))
			return false;
	return true;
}
} // namespace

SettingsStore::SettingsStore(QString filePath, QObject *parent) : QObject(parent), filePath_(std::move(filePath))
{
	load();
}

std::optional<QString> SettingsStore::canonicalOrigin(const QString &text)
{
	if (!validText(text) || text != text.trimmed())
		return std::nullopt;
	QUrl url(text, QUrl::StrictMode);
	const auto scheme = url.scheme().toLower();
	if (!url.isValid() || (scheme != QStringLiteral("http") && scheme != QStringLiteral("https")) ||
	    url.host().isEmpty() || !url.userInfo().isEmpty() || text.contains(QLatin1Char('@')) || url.hasQuery() ||
	    url.hasFragment() || (!url.path().isEmpty() && url.path() != QStringLiteral("/")))
		return std::nullopt;
	url.setScheme(scheme);
	url.setHost(url.host().toLower());
	if ((scheme == QStringLiteral("http") && url.port() == 80) ||
	    (scheme == QStringLiteral("https") && url.port() == 443))
		url.setPort(-1);
	url.setPath({});
	return url.toString(QUrl::FullyEncoded);
}

bool SettingsStore::load()
{
	Q_ASSERT(QThread::currentThread() == thread());
	if (filePath_.isEmpty()) {
		writeAllowed_ = false;
		error_ = QStringLiteral("Путь настроек не задан.");
		errorKey_ = QStringLiteral("Settings.PathUnavailable");
		emit changed();
		return false;
	}
	QFile file(filePath_);
	if (!file.exists())
		return true;
	auto reject = [this](QString key = QStringLiteral("Settings.UnsupportedFormat")) {
		writeAllowed_ = false;
		error_ = QStringLiteral("Формат настроек повреждён или не поддерживается; файл сохранён.");
		errorKey_ = std::move(key);
		emit changed();
		return false;
	};
	if (!file.open(QIODevice::ReadOnly))
		return reject(QStringLiteral("Settings.ReadError"));
	if (file.size() > MaxSettingsBytes)
		return reject();
	const auto bytes = file.read(MaxSettingsBytes + 1);
	if (file.error() != QFileDevice::NoError)
		return reject(QStringLiteral("Settings.ReadError"));
	if (bytes.size() > MaxSettingsBytes || !bytes.isValidUtf8())
		return reject();
	QJsonParseError parseError;
	const auto document = QJsonDocument::fromJson(bytes, &parseError);
	if (parseError.error != QJsonParseError::NoError || !document.isObject())
		return reject();
	const auto root = document.object();
	const int schemaVersion = root.value(QStringLiteral("schema_version")).toInt(-1);
	if (schemaVersion != 1 && schemaVersion != 2)
		return reject();
	QStringList keys{QStringLiteral("schema_version"), QStringLiteral("server_origin"),
			 QStringLiteral("auto_connect"),   QStringLiteral("reconnect"),
			 QStringLiteral("profiles"),       QStringLiteral("bindings")};
	int retries = DefaultSseRejectionRetries;
	if (schemaVersion == 2) {
		const auto key = QStringLiteral("sse_rejection_retries");
		keys.append(key);
		retries = root.value(key).toInt(UnlimitedSseRejectionRetries - 1);
	}
	if (!exactKeys(root, keys) || retries < UnlimitedSseRejectionRetries ||
	    !root.value(QStringLiteral("server_origin")).isString() ||
	    !root.value(QStringLiteral("auto_connect")).isBool() || !root.value(QStringLiteral("reconnect")).isBool() ||
	    !root.value(QStringLiteral("profiles")).isArray() || !root.value(QStringLiteral("bindings")).isArray())
		return reject();
	const auto origin = root.value(QStringLiteral("server_origin")).toString();
	if (!origin.isEmpty() && canonicalOrigin(origin) != std::optional<QString>(origin))
		return reject();
	QMap<QString, ProfileRecord> profiles;
	QSet<QString> locators;
	const auto records = root.value(QStringLiteral("profiles")).toArray();
	if (records.size() > MaxProfiles)
		return reject();
	for (const auto &entry : records) {
		if (!entry.isObject())
			return reject();
		const auto record = entry.toObject();
		if (!exactKeys(record, {QStringLiteral("uuid"), QStringLiteral("name"), QStringLiteral("locator")}))
			return reject();
		ProfileRecord profile{record.value(QStringLiteral("uuid")).toString(),
				      record.value(QStringLiteral("name")).toString(),
				      record.value(QStringLiteral("locator")).toString()};
		if (QUuid(profile.uuid).isNull() ||
		    QUuid(profile.uuid).toString(QUuid::WithoutBraces) != profile.uuid || !validText(profile.name) ||
		    !validText(profile.locator) || profiles.contains(profile.uuid) ||
		    locators.contains(profile.locator))
			return reject();
		profiles.insert(profile.uuid, profile);
		locators.insert(profile.locator);
	}
	QMap<QString, QMap<QString, QString>> bindings;
	const auto entries = root.value(QStringLiteral("bindings")).toArray();
	if (entries.size() > MaxBindings)
		return reject();
	for (const auto &entry : entries) {
		if (!entry.isObject())
			return reject();
		const auto binding = entry.toObject();
		if (!exactKeys(binding,
			       {QStringLiteral("origin"), QStringLiteral("profile_uuid"), QStringLiteral("stream_id")}))
			return reject();
		const auto server = binding.value(QStringLiteral("origin")).toString();
		const auto uuid = binding.value(QStringLiteral("profile_uuid")).toString();
		const auto stream = binding.value(QStringLiteral("stream_id")).toString();
		if (canonicalOrigin(server) != std::optional<QString>(server) || !profiles.contains(uuid) ||
		    !validText(stream) || bindings.value(server).contains(uuid))
			return reject();
		bindings[server].insert(uuid, stream);
		if (bindings.size() > MaxServers)
			return reject();
	}
	origin_ = origin;
	autoConnect_ = root.value(QStringLiteral("auto_connect")).toBool();
	reconnect_ = root.value(QStringLiteral("reconnect")).toBool();
	sseRejectionRetries_ = retries;
	profiles_ = std::move(profiles);
	bindings_ = std::move(bindings);
	currentUuid_.clear();
	changing_ = false;
	transitionUncertain_ = false;
	writeAllowed_ = true;
	unsaved_ = false;
	error_.clear();
	errorKey_.clear();
	emit changed();
	return true;
}

bool SettingsStore::save()
{
	Q_ASSERT(QThread::currentThread() == thread());
	unsaved_ = true;
	if (!writeAllowed_) {
		emit changed();
		return false;
	}
	QJsonArray profiles;
	for (const auto &profile : profiles_)
		profiles.append(QJsonObject{{QStringLiteral("uuid"), profile.uuid},
					    {QStringLiteral("name"), profile.name},
					    {QStringLiteral("locator"), profile.locator}});
	QJsonArray bindings;
	for (auto server = bindings_.cbegin(); server != bindings_.cend(); ++server)
		for (auto binding = server->cbegin(); binding != server->cend(); ++binding)
			bindings.append(QJsonObject{{QStringLiteral("origin"), server.key()},
						    {QStringLiteral("profile_uuid"), binding.key()},
						    {QStringLiteral("stream_id"), binding.value()}});
	const auto bytes = QJsonDocument(QJsonObject{{QStringLiteral("schema_version"), 2},
						     {QStringLiteral("server_origin"), origin_},
						     {QStringLiteral("auto_connect"), autoConnect_},
						     {QStringLiteral("reconnect"), reconnect_},
						     {QStringLiteral("sse_rejection_retries"), sseRejectionRetries_},
						     {QStringLiteral("profiles"), profiles},
						     {QStringLiteral("bindings"), bindings}})
				   .toJson(QJsonDocument::Compact);
	if (bytes.size() > MaxSettingsBytes || !QDir().mkpath(QFileInfo(filePath_).absolutePath())) {
		error_ = QStringLiteral("Не удалось сохранить настройки; изменения доступны в памяти.");
		errorKey_ = QStringLiteral("Settings.WriteError");
		emit changed();
		return false;
	}
	QSaveFile file(filePath_);
	file.setDirectWriteFallback(false);
	if (!file.open(QIODevice::WriteOnly) || file.write(bytes) != bytes.size() || !file.commit()) {
		error_ = QStringLiteral("Не удалось сохранить настройки; изменения доступны в памяти.");
		errorKey_ = QStringLiteral("Settings.WriteError");
		emit changed();
		return false;
	}
	unsaved_ = false;
	error_.clear();
	errorKey_.clear();
	emit changed();
	return true;
}

const QString &SettingsStore::serverOrigin() const noexcept
{
	return origin_;
}
bool SettingsStore::autoConnect() const noexcept
{
	return autoConnect_;
}
bool SettingsStore::reconnect() const noexcept
{
	return reconnect_;
}
int SettingsStore::sseRejectionRetries() const noexcept
{
	return sseRejectionRetries_;
}
bool SettingsStore::unsaved() const noexcept
{
	return unsaved_;
}
const QString &SettingsStore::error() const noexcept
{
	return error_;
}

const QString &SettingsStore::errorKey() const noexcept
{
	return errorKey_;
}
const QString &SettingsStore::profileUuid() const noexcept
{
	return currentUuid_;
}
const QMap<QString, ProfileRecord> &SettingsStore::profiles() const noexcept
{
	return profiles_;
}

QString SettingsStore::selectedStream(const QString &origin, const QString &uuid) const
{
	return bindings_.value(origin).value(uuid);
}

void SettingsStore::persist()
{
	save();
}

bool SettingsStore::setServerOrigin(const QString &origin)
{
	const auto canonical = canonicalOrigin(origin);
	if (!canonical)
		return false;
	if (origin_ != *canonical) {
		origin_ = *canonical;
		persist();
	}
	return true;
}

void SettingsStore::setAutoConnect(bool enabled)
{
	if (autoConnect_ != enabled) {
		autoConnect_ = enabled;
		persist();
	}
}

void SettingsStore::setReconnect(bool enabled)
{
	if (reconnect_ != enabled) {
		reconnect_ = enabled;
		persist();
	}
}

void SettingsStore::setSseRejectionRetries(int retries)
{
	if (retries >= UnlimitedSseRejectionRetries && retries != sseRejectionRetries_) {
		sseRejectionRetries_ = retries;
		persist();
	}
}

bool SettingsStore::setSelectedStream(const QString &origin, const QString &uuid, const QString &streamId)
{
	if (canonicalOrigin(origin) != std::optional<QString>(origin) || !profiles_.contains(uuid) ||
	    !validText(streamId, true))
		return false;
	if (selectedStream(origin, uuid) == streamId)
		return true;
	if (streamId.isEmpty()) {
		bindings_[origin].remove(uuid);
		if (bindings_[origin].isEmpty())
			bindings_.remove(origin);
	} else {
		qsizetype count = 0;
		for (const auto &server : bindings_)
			count += server.size();
		if ((!bindings_.contains(origin) && bindings_.size() >= MaxServers) ||
		    (!bindings_.value(origin).contains(uuid) && count >= MaxBindings))
			return false;
		bindings_[origin].insert(uuid, streamId);
	}
	persist();
	return true;
}

void SettingsStore::retireAbsent(const QStringList &inventory)
{
	for (auto profile = profiles_.begin(); profile != profiles_.end();) {
		if (profile.key() != currentUuid_ && !inventory.contains(profile->name)) {
			const auto uuid = profile.key();
			profile = profiles_.erase(profile);
			for (auto server = bindings_.begin(); server != bindings_.end();) {
				server->remove(uuid);
				if (server->isEmpty())
					server = bindings_.erase(server);
				else
					++server;
			}
		} else {
			++profile;
		}
	}
}

ProfileResolution SettingsStore::observeProfile(FrontendEvent event, const ProfileDescriptor &descriptor)
{
	Q_ASSERT(QThread::currentThread() == thread());
	if (event == FrontendEvent::ProfileChanging) {
		changing_ = true;
		return {currentUuid_, false, true, false};
	}
	if (!descriptor.valid || !validText(descriptor.name) || !validText(descriptor.path) ||
	    !descriptor.inventory.contains(descriptor.name)) {
		transitionUncertain_ = true;
		return {{}, false, true, false};
	}
	const auto previous = currentUuid_;
	const auto previousRecord = profiles_.value(previous);
	QString located;
	for (const auto &profile : profiles_)
		if (profile.locator == descriptor.path)
			located = profile.uuid;
	bool renamed = false;
	if (!previous.isEmpty() && !changing_ && !transitionUncertain_ && previousRecord.locator != descriptor.path &&
	    !descriptor.inventory.contains(previousRecord.name) && located.isEmpty() &&
	    (event == FrontendEvent::ProfileChanged || event == FrontendEvent::ProfileRenamed)) {
		located = previous;
		renamed = true;
	}
	if (located.isEmpty()) {
		if (transitionUncertain_ || event == FrontendEvent::ProfileListChanged ||
		    profiles_.size() >= MaxProfiles)
			return {{}, false, true, false};
		located = QUuid::createUuid().toString(QUuid::WithoutBraces);
	}
	auto record = ProfileRecord{located, descriptor.name, descriptor.path};
	const auto old = profiles_.value(located);
	const bool registryChanged = old.name != record.name || old.locator != record.locator;
	profiles_.insert(located, std::move(record));
	currentUuid_ = located;
	if (event != FrontendEvent::ProfileListChanged)
		changing_ = false;
	transitionUncertain_ = false;
	const auto oldSize = profiles_.size();
	retireAbsent(descriptor.inventory);
	if (registryChanged || profiles_.size() != oldSize)
		persist();
	return {located, true, previous != located, renamed};
}

} // namespace gost
