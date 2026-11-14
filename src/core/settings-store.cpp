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
	// Для собственного формата неизвестные ключи означают неподдерживаемую редакцию, а не поля,
	// которые можно молча потерять при следующем save. Это строже чтения внешних REST DTO.
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

// Проверяет весь поддержанный документ перед заменой RAM; одно чтение не мигрирует файл.
bool SettingsStore::load()
{
	// Реестр и привязки сначала разбираются во временные переменные: ошибка в конце документа
	// не должна заменить часть рабочих настроек. Размер проверяется до JSON-разбора, число записей —
	// до замены данных в памяти. Повтор UUID, пути профиля или привязки одной пары считается ошибкой.
	Q_ASSERT(QThread::currentThread() == thread());
	if (filePath_.isEmpty()) {
		writeAllowed_ = false;
		error_ = QStringLiteral("Путь настроек не задан.");
		errorKey_ = QStringLiteral("Settings.PathUnavailable");
		emit changed();
		return false;
	}
	// file читает только явно переданный путь настроек.
	QFile file(filePath_);
	if (!file.exists())
		return true;
	// reject сохраняет исходный файл и сообщает key как локализуемую причину отказа.
	auto reject = [this](QString key = QStringLiteral("Settings.UnsupportedFormat")) {
		// Повреждённый или неизвестный формат блокирует последующие записи, сохраняя исходный файл владельца.
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
	// bytes ограничены до разбора, чтобы повреждённый файл не потребовал неограниченную память.
	const auto bytes = file.read(MaxSettingsBytes + 1);
	if (file.error() != QFileDevice::NoError)
		return reject(QStringLiteral("Settings.ReadError"));
	if (bytes.size() > MaxSettingsBytes || !bytes.isValidUtf8())
		return reject();
	// parseError различает синтаксический отказ, document содержит целый проверяемый JSON.
	QJsonParseError parseError;
	const auto document = QJsonDocument::fromJson(bytes, &parseError);
	if (parseError.error != QJsonParseError::NoError || !document.isObject())
		return reject();
	// root содержит корневую запись, schemaVersion выбирает её точный набор полей.
	const auto root = document.object();
	const int schemaVersion = root.value(QStringLiteral("schema_version")).toInt(-1);
	if (schemaVersion != 1 && schemaVersion != 2 && schemaVersion != 3)
		return reject();
	// keys перечисляет все допустимые ключи выбранной версии, включая обязательные новые поля.
	QStringList keys{QStringLiteral("schema_version"), QStringLiteral("server_origin"),
			 QStringLiteral("auto_connect"),   QStringLiteral("reconnect"),
			 QStringLiteral("profiles"),       QStringLiteral("bindings")};
	// retries получает прежний default только у схемы v1 без соответствующего поля.
	int retries = DefaultSseRejectionRetries;
	if (schemaVersion >= 2) {
		// key обозначает единственное добавленное поле схемы v2.
		const auto key = QStringLiteral("sse_rejection_retries");
		keys.append(key);
		retries = root.value(key).toInt(UnlimitedSseRejectionRetries - 1);
	}
	// timeoutSeconds и detailed получают совместимые значения при чтении v1/v2.
	int timeoutSeconds = DefaultHttpTimeoutSeconds;
	bool detailed = false;
	if (schemaVersion == 3) {
		keys.append({QStringLiteral("http_timeout_seconds"), QStringLiteral("log_level")});
		// timeoutValue должен быть целым числом секунд; logLevel допускает только два режима.
		const auto timeoutValue = root.value(QStringLiteral("http_timeout_seconds"));
		const auto logLevel = root.value(QStringLiteral("log_level"));
		timeoutSeconds = timeoutValue.toInt(-1);
		if (!timeoutValue.isDouble() || timeoutSeconds < MinHttpTimeoutSeconds ||
		    timeoutSeconds > MaxHttpTimeoutSeconds || !logLevel.isString() ||
		    (logLevel.toString() != QStringLiteral("normal") &&
		     logLevel.toString() != QStringLiteral("detailed")))
			return reject();
		detailed = logLevel.toString() == QStringLiteral("detailed");
	}
	if (!exactKeys(root, keys) || retries < UnlimitedSseRejectionRetries ||
	    !root.value(QStringLiteral("server_origin")).isString() ||
	    !root.value(QStringLiteral("auto_connect")).isBool() || !root.value(QStringLiteral("reconnect")).isBool() ||
	    !root.value(QStringLiteral("profiles")).isArray() || !root.value(QStringLiteral("bindings")).isArray())
		return reject();
	// origin проверяется в сохранённой канонической форме, без неявного исправления файла.
	const auto origin = root.value(QStringLiteral("server_origin")).toString();
	if (!origin.isEmpty() && canonicalOrigin(origin) != std::optional<QString>(origin))
		return reject();
	// profiles собирает временный реестр, locators исключает повтор путей, records содержит входные записи.
	QMap<QString, ProfileRecord> profiles;
	QSet<QString> locators;
	const auto records = root.value(QStringLiteral("profiles")).toArray();
	if (records.size() > MaxProfiles)
		return reject();
	// entry содержит одну ещё не принятую запись реестра.
	for (const auto &entry : records) {
		if (!entry.isObject())
			return reject();
		// record проверяется до создания типизированной записи профиля.
		const auto record = entry.toObject();
		if (!exactKeys(record, {QStringLiteral("uuid"), QStringLiteral("name"), QStringLiteral("locator")}))
			return reject();
		// profile содержит UUID и изменяемые атрибуты одной проверяемой записи.
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
	// bindings собирает временные привязки, entries содержит их исходный массив.
	QMap<QString, QMap<QString, QString>> bindings;
	const auto entries = root.value(QStringLiteral("bindings")).toArray();
	if (entries.size() > MaxBindings)
		return reject();
	// entry содержит одну привязку, которую нельзя принять частично.
	for (const auto &entry : entries) {
		if (!entry.isObject())
			return reject();
		// binding содержит точные origin/UUID/Stream одной пары.
		const auto binding = entry.toObject();
		if (!exactKeys(binding,
			       {QStringLiteral("origin"), QStringLiteral("profile_uuid"), QStringLiteral("stream_id")}))
			return reject();
		// server адресует сервер, uuid — профиль OBS, stream — выбранную на этой паре запись.
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
	// Рабочие значения заменяются только после проверки всего документа, включая реестр и привязки.
	origin_ = origin;
	autoConnect_ = root.value(QStringLiteral("auto_connect")).toBool();
	reconnect_ = root.value(QStringLiteral("reconnect")).toBool();
	sseRejectionRetries_ = retries;
	httpTimeoutSeconds_ = timeoutSeconds;
	detailedLogging_ = detailed;
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

// Атомарно сохраняет рабочие предпочтения схемы v3 без прямой перезаписи файла.
bool SettingsStore::save()
{
	// unsaved устанавливается до любой операции: даже заблокированная запись должна быть видна UI.
	// Сериализуется одна версия RAM целиком; QSaveFile commit является единственной точкой успеха.
	Q_ASSERT(QThread::currentThread() == thread());
	unsaved_ = true;
	if (!writeAllowed_) {
		emit changed();
		return false;
	}
	// profiles содержит JSON реестра; profile обозначает одну устойчивую идентичность OBS.
	QJsonArray profiles;
	for (const auto &profile : profiles_)
		profiles.append(QJsonObject{{QStringLiteral("uuid"), profile.uuid},
					    {QStringLiteral("name"), profile.name},
					    {QStringLiteral("locator"), profile.locator}});
	// bindings содержит JSON привязок; server и binding обходят origin и UUID без изменения исходных карт.
	QJsonArray bindings;
	for (auto server = bindings_.cbegin(); server != bindings_.cend(); ++server)
		for (auto binding = server->cbegin(); binding != server->cend(); ++binding)
			bindings.append(QJsonObject{{QStringLiteral("origin"), server.key()},
						    {QStringLiteral("profile_uuid"), binding.key()},
						    {QStringLiteral("stream_id"), binding.value()}});
	// bytes содержит один полный снимок RAM; в нём нет серверных ключей или endpoint.
	const auto bytes =
		QJsonDocument(QJsonObject{{QStringLiteral("schema_version"), 3},
					  {QStringLiteral("server_origin"), origin_},
					  {QStringLiteral("auto_connect"), autoConnect_},
					  {QStringLiteral("reconnect"), reconnect_},
					  {QStringLiteral("sse_rejection_retries"), sseRejectionRetries_},
					  {QStringLiteral("http_timeout_seconds"), httpTimeoutSeconds_},
					  {QStringLiteral("log_level"),
					   detailedLogging_ ? QStringLiteral("detailed") : QStringLiteral("normal")},
					  {QStringLiteral("profiles"), profiles},
					  {QStringLiteral("bindings"), bindings}})
			.toJson(QJsonDocument::Compact);
	if (bytes.size() > MaxSettingsBytes || !QDir().mkpath(QFileInfo(filePath_).absolutePath())) {
		error_ = QStringLiteral("Не удалось сохранить настройки; изменения доступны в памяти.");
		errorKey_ = QStringLiteral("Settings.WriteError");
		emit changed();
		return false;
	}
	// file заменяет прежний документ только после успешного commit.
	QSaveFile file(filePath_);
	// Запрет прямой записи сохраняет прежний файл, если атомарная замена невозможна.
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

// Возвращает рабочий пользовательский срок новых REST-запросов, в секундах.
int SettingsStore::httpTimeoutSeconds() const noexcept
{
	return httpTimeoutSeconds_;
}

// Возвращает выбор подробных безопасных записей штатного журнала OBS.
bool SettingsStore::detailedLogging() const noexcept
{
	return detailedLogging_;
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
	// Методы изменения намеренно сохраняют рабочие изменения при отказе диска. Результат сообщается через
	// changed/unsaved/error, поэтому вызывающий метод не откатывает пользовательский выбор.
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

// Принимает seconds в RAM только в разрешённом диапазоне и сохраняет фактическое изменение.
bool SettingsStore::setHttpTimeoutSeconds(int seconds)
{
	if (seconds < MinHttpTimeoutSeconds || seconds > MaxHttpTimeoutSeconds)
		return false;
	if (seconds != httpTimeoutSeconds_) {
		httpTimeoutSeconds_ = seconds;
		persist();
	}
	return true;
}

// Сохраняет detailed, не изменяя язык, оформление или параметры соединения.
void SettingsStore::setDetailedLogging(bool detailed)
{
	if (detailed != detailedLogging_) {
		detailedLogging_ = detailed;
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
	// Событие ProfileChanging означает явное переключение, поэтому последующую смену пути нельзя
	// принять за переименование прежнего профиля. Если descriptor недостоверен, запоминаем это:
	// следующий неизвестный каталог не должен случайно получить UUID и все привязки прежнего профиля.
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
	// OBS при переименовании меняет каталог и посылает ProfileChanged раньше ProfileRenamed без ProfileChanging.
	// Перенос UUID разрешён только при исчезновении старого имени и отсутствии неоднозначного перехода.
	if (!previous.isEmpty() && !changing_ && !transitionUncertain_ && previousRecord.locator != descriptor.path &&
	    !descriptor.inventory.contains(previousRecord.name) && located.isEmpty() &&
	    (event == FrontendEvent::ProfileChanged || event == FrontendEvent::ProfileRenamed)) {
		located = previous;
		renamed = true;
	}
	if (located.isEmpty()) {
		// Неизвестный каталог при неопределённом переходе не должен получить привязки существующего профиля.
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
