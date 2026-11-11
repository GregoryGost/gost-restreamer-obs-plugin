#include "state-store.hpp"
#include "settings-store.hpp"
#include "api/json-decoder.hpp"

#include <QThread>
#include <QSet>

#include <algorithm>
#include <utility>

namespace gost {
namespace {
constexpr qsizetype MaxRuntimeObservations = 10000;

qsizetype observationCount(const api::RuntimeSession &session)
{
	// Учитывается и контейнер Session без observation: создание пустых родителей тоже расходует бюджет.
	return 1 + (session.observation ? 1 : 0) + session.groups.size() + session.outputs.size() +
	       session.ffmpeg.size();
}

template<class T> api::JsonField<T> present(T value)
{
	return {api::FieldState::Value, std::move(value)};
}

bool validIdentity(const QString &value)
{
	// Ограничение относится к непрозрачному ключу дерева, а не к формату UUID/числа сервера.
	return !value.isEmpty() && value.size() <= 4096 && !value.contains(QChar::Null);
}

void markStale(RuntimeAvailability &availability)
{
	if (availability == RuntimeAvailability::Fresh)
		availability = RuntimeAvailability::Stale;
}

template<class T> auto findInstance(QVector<T> &nodes, const QString &instance)
{
	return std::find_if(nodes.begin(), nodes.end(),
			    [&instance](const auto &node) { return node.instanceId == instance; });
}

template<class T> void eraseInstances(QVector<T> &nodes, const QSet<QString> &instances)
{
	nodes.erase(std::remove_if(nodes.begin(), nodes.end(),
				   [&instances](const auto &node) { return instances.contains(node.instanceId); }),
		    nodes.end());
}
} // namespace

StateStore::StateStore(QObject *parent)
	: QObject(parent),
	  sseRejectionRetries_(SettingsStore::DefaultSseRejectionRetries)
{
}

const std::optional<QString> &StateStore::profileName() const noexcept
{
	return profileName_;
}

std::optional<bool> StateStore::streamingActive() const noexcept
{
	return streamingActive_;
}

void StateStore::setObsState(QString profileName, bool streamingActive)
{
	Q_ASSERT(QThread::currentThread() == thread());
	if (profileName_ == profileName && streamingActive_ == streamingActive) {
		return;
	}
	profileName_ = std::move(profileName);
	streamingActive_ = streamingActive;
	emit obsStateChanged();
}

const ObsStatistics &StateStore::obsStatistics() const noexcept
{
	return obsStatistics_;
}

void StateStore::setObsStatistics(ObsStatistics statistics)
{
	Q_ASSERT(QThread::currentThread() == thread());
	if (obsStatistics_ == statistics)
		return;
	obsStatistics_ = std::move(statistics);
	emit obsStatisticsChanged();
}

const QString &StateStore::serverOrigin() const noexcept
{
	return origin_;
}
ConnectionStatus StateStore::connectionStatus() const noexcept
{
	return connectionStatus_;
}
const QVector<api::Stream> &StateStore::streams() const noexcept
{
	return streams_;
}
bool StateStore::streamsComplete() const noexcept
{
	return streamsComplete_;
}
const QString &StateStore::selectedStreamId() const noexcept
{
	return selectedStreamId_;
}
bool StateStore::autoConnect() const noexcept
{
	return autoConnect_;
}
bool StateStore::reconnect() const noexcept
{
	return reconnect_;
}
int StateStore::sseRejectionRetries() const noexcept
{
	return sseRejectionRetries_;
}
const QString &StateStore::safeError() const noexcept
{
	return connectionError_.isEmpty() ? settingsError_ : connectionError_;
}

const QString &StateStore::safeErrorKey() const noexcept
{
	return connectionError_.isEmpty() ? settingsErrorKey_ : connectionErrorKey_;
}
bool StateStore::settingsUnsaved() const noexcept
{
	return settingsUnsaved_;
}
bool StateStore::profileResolved() const noexcept
{
	return profileResolved_;
}
const QString &StateStore::profileNotice() const noexcept
{
	return profileNotice_;
}

const QString &StateStore::profileNoticeKey() const noexcept
{
	return profileNoticeKey_;
}
const ConfigurationState &StateStore::configuration() const noexcept
{
	return configuration_;
}
const HistoryState &StateStore::history() const noexcept
{
	return history_;
}
const DetailState &StateStore::details() const noexcept
{
	return details_;
}
const RuntimeState &StateStore::runtime() const noexcept
{
	return runtime_;
}
const DockContext &StateStore::dockContext() const noexcept
{
	return dockContext_;
}
const DockServerState &StateStore::dockServer() const noexcept
{
	return dockServer_;
}
RuntimeEvidence StateStore::runtimeEvidence(const QString &instanceId) const
{
	const auto found = watermarks_.constFind(instanceId);
	return pendingAnchor_ || found == watermarks_.cend() ? RuntimeEvidence{} : found->evidence;
}
const std::optional<api::Health> &StateStore::ready() const noexcept
{
	return ready_;
}
const std::optional<api::Capabilities> &StateStore::capabilities() const noexcept
{
	return capabilities_;
}

QString StateStore::selectedStreamName() const
{
	for (const auto &stream : streams_)
		if (stream.id == selectedStreamId_)
			return stream.name;
	return {};
}

bool StateStore::selectionAvailable() const noexcept
{
	return profileResolved_ && !selectedStreamId_.isEmpty() &&
	       std::any_of(streams_.cbegin(), streams_.cend(),
			   [this](const auto &stream) { return stream.id == selectedStreamId_; });
}

void StateStore::setConnection(QString origin, ConnectionStatus status, QString safeError, QString errorKey)
{
	Q_ASSERT(QThread::currentThread() == thread());
	if (origin_ != origin) {
		dockContext_ = {};
		dockServer_ = {};
	}
	origin_ = std::move(origin);
	connectionStatus_ = status;
	connectionError_ = std::move(safeError);
	connectionErrorKey_ = std::move(errorKey);
	if (status == ConnectionStatus::Connected && pendingAnchor_) {
		pendingAnchor_ = false;
		runtime_.availability = RuntimeAvailability::Fresh;
		emit runtimeStateChanged();
	}
	emit serverStateChanged();
}

void StateStore::setPreferences(bool autoConnect, bool reconnect, int sseRejectionRetries, bool unsaved,
				QString settingsError, QString errorKey)
{
	autoConnect_ = autoConnect;
	reconnect_ = reconnect;
	sseRejectionRetries_ = sseRejectionRetries;
	settingsUnsaved_ = unsaved;
	settingsError_ = std::move(settingsError);
	settingsErrorKey_ = std::move(errorKey);
	emit serverStateChanged();
}

void StateStore::setStreams(QVector<api::Stream> streams, bool complete)
{
	streams_ = std::move(streams);
	streamsComplete_ = complete;
	configuration_.streams.clear();
	for (const auto &stream : streams_)
		configuration_.streams.insert(stream.id, stream);
	configuration_.streamsStatus = {RuntimeAvailability::Fresh, complete, {}};
	emit streamsChanged();
	emit serverStateChanged();
}

void StateStore::setSelection(QString streamId, bool profileResolved, QString notice, QString noticeKey,
			      QString profileUuid)
{
	DockContext next{origin_, std::move(profileUuid), streamId};
	if (dockContext_ != next) {
		dockContext_ = std::move(next);
		dockServer_ = {};
	}
	selectedStreamId_ = std::move(streamId);
	profileResolved_ = profileResolved;
	profileNotice_ = std::move(notice);
	profileNoticeKey_ = std::move(noticeKey);
	emit serverStateChanged();
}

void StateStore::setDockServer(DockServerState metadata)
{
	dockServer_ = std::move(metadata);
	emit serverStateChanged();
}

void StateStore::setReady(api::Health ready)
{
	ready_ = std::move(ready);
	emit serverStateChanged();
}

void StateStore::setCapabilities(api::Capabilities capabilities)
{
	capabilities_ = std::move(capabilities);
	emit serverStateChanged();
}

void StateStore::setConfiguration(ConfigurationState configuration)
{
	configuration_ = std::move(configuration);
	streams_.clear();
	streams_.reserve(configuration_.streams.size());
	for (const auto &stream : configuration_.streams)
		streams_.append(stream);
	streamsComplete_ = configuration_.streamsStatus.complete;
	emit streamsChanged();
	emit serverStateChanged();
}

void StateStore::setHistory(HistoryState history)
{
	history_ = std::move(history);
	emit serverStateChanged();
}

void StateStore::setDetails(DetailState details)
{
	details_ = std::move(details);
	emit serverStateChanged();
}

void StateStore::markRuntimeStale()
{
	// Сначала даём Dock сохранить последние принятые значения. Если пометить их устаревшими раньше
	// уведомления, отложенное обновление виджета уже не сможет принять их как последнее свежее измерение.
	emit runtimeAboutToBecomeStale();
	markStale(runtime_.availability);
	if (!pendingAnchor_)
		for (auto &lanes : watermarks_) {
			markStale(lanes.evidence.state);
			markStale(lanes.evidence.input);
			markStale(lanes.evidence.metrics);
		}
	auto stale = [](ReadStatus &status) {
		markStale(status.availability);
	};
	stale(dockServer_.sessionStatus);
	stale(dockServer_.outputsStatus);
	markStale(dockServer_.fallback.availability);
	stale(configuration_.streamsStatus);
	stale(configuration_.destinationsStatus);
	stale(configuration_.groupsStatus);
	stale(configuration_.profilesStatus);
	stale(configuration_.assetsStatus);
	stale(configuration_.generationsStatus);
	stale(configuration_.variantsStatus);
	stale(history_.sessionsStatus);
	stale(history_.outputsStatus);
	stale(history_.receiptsStatus);
	for (auto &status : details_.sessionMetricsStatus)
		stale(status);
	for (auto &status : details_.outputsStatus)
		stale(status);
	emit runtimeStateChanged();
	emit serverStateChanged();
}

void StateStore::clearRuntime()
{
	runtime_ = {};
	pendingAnchor_ = false;
	gpuInstanceId_.clear();
	gpuRevision_.reset();
	watermarks_.clear();
	emit runtimeStateChanged();
	emit serverStateChanged();
}

void StateStore::clearServerState(bool preserveDock)
{
	streams_.clear();
	streamsComplete_ = false;
	configuration_ = {};
	history_ = {};
	details_ = {};
	ready_.reset();
	capabilities_.reset();
	if (!preserveDock)
		dockServer_ = {};
	emit streamsChanged();
	clearRuntime();
}

bool StateStore::installAnchor(const api::Anchor &anchor, bool awaitSubscription)
{
	// next/watermarks/instances остаются локальными до конца проверки: отказ последнего потомка
	// не должен оставить наполовину заменённый runtime. instanceId уникален во всём дереве,
	// outputId — внутри Session; повтор конфигурационного groupId допускается для разных исполнений.
	Q_ASSERT(QThread::currentThread() == thread());
	if (!validIdentity(anchor.cursor))
		return false;
	RuntimeState next;
	QMap<QString, Watermarks> watermarks;
	QSet<QString> instances;
	auto addObservation = [&](const api::Observation &observation) {
		if (!validIdentity(observation.instanceId) || instances.contains(observation.instanceId))
			return false;
		instances.insert(observation.instanceId);
		auto &lanes = watermarks[observation.instanceId];
		// У ревизии состояния из снимка нет обещания совпадать с ревизией последующего измерения.
		lanes.state = observation.revision;
		if (!observation.state.isEmpty())
			lanes.evidence.state = RuntimeAvailability::Fresh;
		return true;
	};
	for (const auto &session : anchor.sessions) {
		if (!validIdentity(session.sessionId) || next.sessions.contains(session.sessionId))
			return false;
		if (session.observation) {
			if (!addObservation(*session.observation))
				return false;
			if (session.input) {
				auto &evidence = watermarks[session.observation->instanceId].evidence;
				evidence.input = RuntimeAvailability::Fresh;
				if (session.input->metrics)
					evidence.metrics = RuntimeAvailability::Fresh;
			}
		}
		QSet<QString> groups;
		QSet<QString> outputIds;
		for (const auto &group : session.groups) {
			if (!validIdentity(group.groupId) || !validIdentity(group.profileId) || !addObservation(group))
				return false;
			groups.insert(group.instanceId);
			if (group.metrics)
				watermarks[group.instanceId].evidence.metrics = RuntimeAvailability::Fresh;
		}
		for (const auto &output : session.outputs) {
			if (!validIdentity(output.outputId) || outputIds.contains(output.outputId) ||
			    !addObservation(output))
				return false;
			outputIds.insert(output.outputId);
			if (output.metrics)
				watermarks[output.instanceId].evidence.metrics = RuntimeAvailability::Fresh;
		}
		for (const auto &ffmpeg : session.ffmpeg) {
			if (!groups.contains(ffmpeg.groupInstanceId) || ffmpeg.role.size() > 4096 ||
			    ffmpeg.role.contains(QChar::Null) || !addObservation(ffmpeg))
				return false;
			if (ffmpeg.metrics)
				watermarks[ffmpeg.instanceId].evidence.metrics = RuntimeAvailability::Fresh;
		}
		next.sessions.insert(session.sessionId, session);
		if (instances.size() + next.sessions.size() > MaxRuntimeObservations)
			return false;
	}
	next.cursor = anchor.cursor;
	next.gpu = anchor.gpu;
	next.availability = awaitSubscription ? RuntimeAvailability::Unknown : RuntimeAvailability::Fresh;
	runtime_ = std::move(next);
	pendingAnchor_ = awaitSubscription;
	gpuInstanceId_.clear();
	gpuRevision_.reset();
	watermarks_ = std::move(watermarks);
	emit runtimeStateChanged();
	emit serverStateChanged();
	return true;
}

bool StateStore::applyEvent(const api::RuntimeEvent &event, bool *dockTopologyChanged)
{
	// Для Session и её потомков сначала проверяем форму события и принадлежность исполнения.
	// Изменения Session и ревизий готовим в копиях, затем проверяем размер всего дерева и только
	// после этого заменяем рабочие данные, продвигаем cursor и уведомляем UI. Ревизии сравниваются
	// лишь у одного instanceId и отдельно для state/metrics; порядок непрозрачных cursor не сравнивается.
	Q_ASSERT(QThread::currentThread() == thread());
	if (dockTopologyChanged)
		*dockTopologyChanged = false;
	if (runtime_.cursor.isEmpty() || !validIdentity(event.cursor) || !api::validateRuntimeEvent(event))
		return false;
	if (event.cursor == runtime_.cursor)
		return true;
	// Курсор непрозрачен: равенство обнаруживает повтор, но числовое или лексическое сравнение недопустимо.
	if (event.owner == QStringLiteral("system")) {
		// GPU имеет отдельную идентичность/шкалу. Запоздалое removed старого экземпляра не стирает новый.
		if (!validIdentity(event.observation.instanceId))
			return false;
		if (event.kind == QStringLiteral("removed")) {
			if (gpuInstanceId_.isEmpty() || gpuInstanceId_ == event.observation.instanceId) {
				runtime_.gpu = {};
				gpuInstanceId_.clear();
				gpuRevision_.reset();
			}
		} else if (gpuInstanceId_ != event.observation.instanceId || !gpuRevision_ ||
			   event.observation.revision > *gpuRevision_) {
			runtime_.gpu = event.gpu;
			gpuInstanceId_ = event.observation.instanceId;
			gpuRevision_ = event.observation.revision;
		}
		runtime_.cursor = event.cursor;
		runtime_.availability = RuntimeAvailability::Fresh;
		emit runtimeStateChanged();
		emit serverStateChanged();
		return true;
	}
	const bool stateEvent = event.kind == QStringLiteral("state");
	const bool metricsEvent = event.kind == QStringLiteral("metrics");
	const bool inputEvent = event.kind == QStringLiteral("input");
	const bool attemptEvent = event.kind == QStringLiteral("attempt");
	const bool removedEvent = event.kind == QStringLiteral("removed");
	if ((!stateEvent && !metricsEvent && !inputEvent && !attemptEvent && !removedEvent) ||
	    !validIdentity(event.sessionId) || !validIdentity(event.observation.instanceId))
		return false;
	const bool sessionOwner = event.owner == QStringLiteral("session");
	const bool groupOwner = event.owner == QStringLiteral("group");
	const bool outputOwner = event.owner == QStringLiteral("output");
	const bool ffmpegOwner = event.owner == QStringLiteral("ffmpeg");
	if ((!sessionOwner && !groupOwner && !outputOwner && !ffmpegOwner) ||
	    (inputEvent && (!sessionOwner || !event.input)) || (attemptEvent && !ffmpegOwner) ||
	    (groupOwner && (!validIdentity(event.groupId) || !validIdentity(event.profileId))) ||
	    (outputOwner && !validIdentity(event.outputId)) ||
	    (ffmpegOwner &&
	     (!validIdentity(event.groupInstanceId) || event.role.size() > 4096 || event.role.contains(QChar::Null))) ||
	    (metricsEvent &&
	     ((sessionOwner && (!event.input || !event.input->metrics)) || (groupOwner && !event.group) ||
	      (outputOwner && !event.output) || (ffmpegOwner && !event.ffmpeg))))
		return false;
	if (stateEvent && event.observation.state.isEmpty())
		return false;
	const auto &instance = event.observation.instanceId;
	if (ffmpegOwner && event.groupInstanceId == instance)
		return false;
	for (const auto &existingSession : runtime_.sessions) {
		// Уже известное исполнение не может сменить тип/Session/родителя под тем же instanceId.
		// Проверка выполняется до создания недостающих узлов, включая временного родителя FFmpeg.
		if (existingSession.observation && existingSession.observation->instanceId == instance &&
		    (!sessionOwner || existingSession.sessionId != event.sessionId))
			return false;
		for (const auto &group : existingSession.groups)
			if (group.instanceId == instance &&
			    (!groupOwner || existingSession.sessionId != event.sessionId ||
			     (!group.groupId.isEmpty() && group.groupId != event.groupId) ||
			     (!group.profileId.isEmpty() && group.profileId != event.profileId)))
				return false;
		for (const auto &output : existingSession.outputs)
			if (output.instanceId == instance &&
			    (!outputOwner || existingSession.sessionId != event.sessionId ||
			     output.outputId != event.outputId))
				return false;
		for (const auto &ffmpeg : existingSession.ffmpeg)
			if (ffmpeg.instanceId == instance &&
			    (!ffmpegOwner || existingSession.sessionId != event.sessionId ||
			     ffmpeg.groupInstanceId != event.groupInstanceId || ffmpeg.role != event.role))
				return false;
		if (ffmpegOwner) {
			if (existingSession.observation &&
			    existingSession.observation->instanceId == event.groupInstanceId)
				return false;
			for (const auto &existing : existingSession.outputs)
				if (existing.instanceId == event.groupInstanceId)
					return false;
			for (const auto &existing : existingSession.ffmpeg)
				if (existing.instanceId == event.groupInstanceId)
					return false;
			for (const auto &existing : existingSession.groups)
				if (existing.instanceId == event.groupInstanceId &&
				    existingSession.sessionId != event.sessionId)
					return false;
		}
	}
	// Изменения сессии и её ревизий устанавливаются только после полной проверки события.
	auto session = runtime_.sessions.value(event.sessionId);
	session.sessionId = event.sessionId;
	if (groupOwner && !event.groupInstanceId.isEmpty() && session.observation &&
	    event.groupInstanceId != session.observation->instanceId)
		return false;
	auto lanes = watermarks_.value(instance);
	bool lanesChanged = false;
	QSet<QString> removed;
	auto removeSession = [&] {
		// Удаление или замена исполнения Session также удаляет записи ревизий всех его потомков.
		if (session.observation)
			removed.insert(session.observation->instanceId);
		for (const auto &group : session.groups)
			removed.insert(group.instanceId);
		for (const auto &output : session.outputs)
			removed.insert(output.instanceId);
		for (const auto &ffmpeg : session.ffmpeg)
			removed.insert(ffmpeg.instanceId);
		session = {};
		session.sessionId = event.sessionId;
	};
	auto removeGroup = [&](const QString &groupInstance) {
		// Удаляется точный экземпляр группы и его FFmpeg, а не все группы с тем же конфигурационным ID.
		removed.insert(groupInstance);
		for (const auto &ffmpeg : session.ffmpeg)
			if (ffmpeg.groupInstanceId == groupInstance)
				removed.insert(ffmpeg.instanceId);
	};
	bool eraseSession = false;
	bool ignored = false;
	bool topologyChanged = false;
	api::Observation *observation = nullptr;
	api::RuntimeGroup *group = nullptr;
	api::RuntimeOutput *output = nullptr;
	api::RuntimeFFmpeg *ffmpeg = nullptr;
	if (sessionOwner) {
		if (removedEvent) {
			if (session.observation && session.observation->instanceId == instance) {
				removeSession();
				eraseSession = true;
				topologyChanged = true;
			} else if (!session.observation && runtime_.sessions.contains(event.sessionId)) {
				return false;
			}
			ignored = true;
		} else {
			if (session.observation && session.observation->instanceId != instance)
				removeSession();
			if (!session.observation) {
				api::Observation unknown;
				unknown.instanceId = instance;
				session.observation = present(std::move(unknown));
				topologyChanged = true;
			}
			observation = &*session.observation;
		}
	} else if (groupOwner) {
		auto found = findInstance(session.groups, instance);
		if (removedEvent) {
			if (found != session.groups.end()) {
				removeGroup(instance);
				topologyChanged = true;
			}
			ignored = true;
		} else {
			if (found == session.groups.end()) {
				api::RuntimeGroup unknown;
				unknown.instanceId = instance;
				session.groups.push_back(std::move(unknown));
				topologyChanged = true;
			}
			group = &*findInstance(session.groups, instance);
			group->groupId = event.groupId;
			group->profileId = event.profileId;
			observation = group;
		}
	} else if (outputOwner) {
		auto found = findInstance(session.outputs, instance);
		if (removedEvent) {
			if (found != session.outputs.end()) {
				removed.insert(instance);
				topologyChanged = true;
			}
			ignored = true;
		} else {
			if (found == session.outputs.end()) {
				// Новое исполнение того же Output вытесняет старое вместе с его ревизиями и подтверждениями свежести.
				for (const auto &old : session.outputs)
					if (old.outputId == event.outputId)
						removed.insert(old.instanceId);
				eraseInstances(session.outputs, removed);
				api::RuntimeOutput unknown;
				unknown.instanceId = instance;
				unknown.outputId = event.outputId;
				session.outputs.push_back(std::move(unknown));
				topologyChanged = true;
			}
			output = &*findInstance(session.outputs, instance);
			observation = output;
		}
	} else {
		auto found = findInstance(session.ffmpeg, instance);
		if (removedEvent) {
			if (found != session.ffmpeg.end())
				removed.insert(instance);
			ignored = true;
		} else {
			auto parent = findInstance(session.groups, event.groupInstanceId);
			if (parent == session.groups.end()) {
				// Событие дочернего FFmpeg может прийти раньше родителя; известен только ID экземпляра группы.
				api::RuntimeGroup unknown;
				unknown.instanceId = event.groupInstanceId;
				session.groups.push_back(std::move(unknown));
			}
			if (found == session.ffmpeg.end()) {
				api::RuntimeFFmpeg unknown;
				unknown.instanceId = instance;
				unknown.groupInstanceId = event.groupInstanceId;
				unknown.role = event.role;
				session.ffmpeg.push_back(std::move(unknown));
			}
			ffmpeg = &*findInstance(session.ffmpeg, instance);
			observation = ffmpeg;
		}
	}
	if (!ignored) {
		// Принятое событие может не менять поле: старая ревизия state/metrics всё равно потребляется,
		// но не обновляет evidence. Input и attempt являются границами жизненного цикла и сбрасывают
		// соответствующую телеметрию независимо от ранее большой ревизии metrics.
		lanesChanged = true;
		if (attemptEvent) {
			// Новая попытка сбрасывает наблюдения FFmpeg независимо от прежней численной ревизии.
			ffmpeg->metrics = {};
			ffmpeg->state.clear();
			ffmpeg->observedAt = {};
			ffmpeg->failure = {};
			lanes.metrics.reset();
			lanes.evidence.metrics = RuntimeAvailability::Unknown;
			lanes.evidence.state = RuntimeAvailability::Unknown;
		} else if (inputEvent) {
			// Жизненный цикл входа использует revision=0 и начинает новую последовательность измерений.
			if (event.observation.revision != 0)
				return false;
			session.input = event.input;
			session.input->metrics = {};
			lanes.metrics.reset();
			lanes.evidence.input = RuntimeAvailability::Fresh;
			lanes.evidence.metrics = RuntimeAvailability::Unknown;
		} else if (stateEvent) {
			if (!lanes.state || event.observation.revision > *lanes.state) {
				if (sessionOwner || (outputOwner && observation->state != event.observation.state))
					topologyChanged = true;
				*observation = event.observation;
				lanes.state = event.observation.revision;
				lanes.evidence.state = RuntimeAvailability::Fresh;
				if (group) {
					group->source = event.source;
					group->sourceRevision = event.sourceRevision;
				}
			}
		} else if (metricsEvent && (!lanes.metrics || event.observation.revision > *lanes.metrics)) {
			lanes.metrics = event.observation.revision;
			lanes.evidence.metrics = RuntimeAvailability::Fresh;
			if (sessionOwner) {
				if (!session.input)
					session.input = event.input;
				else
					session.input->metrics = event.input->metrics;
			} else if (group) {
				group->metrics = event.group;
			} else if (output) {
				output->metrics = event.output;
			} else {
				ffmpeg->metrics = event.ffmpeg;
			}
		}
	}
	eraseInstances(session.groups, removed);
	eraseInstances(session.outputs, removed);
	eraseInstances(session.ffmpeg, removed);
	qsizetype count = eraseSession ? 0 : observationCount(session);
	for (const auto &existingSession : runtime_.sessions)
		if (existingSession.sessionId != event.sessionId)
			count += observationCount(existingSession);
	if (count > MaxRuntimeObservations)
		return false;
	// Только полностью проверенная копия заменяет Session; отказ выше не продвигает курсор и ревизии.
	if (eraseSession)
		runtime_.sessions.remove(event.sessionId);
	else if (!ignored || runtime_.sessions.contains(event.sessionId))
		runtime_.sessions.insert(event.sessionId, std::move(session));
	for (const auto &id : removed)
		watermarks_.remove(id);
	if (lanesChanged)
		watermarks_.insert(instance, std::move(lanes));
	runtime_.cursor = event.cursor;
	runtime_.availability = RuntimeAvailability::Fresh;
	if (topologyChanged && dockServer_.session && dockServer_.session->id == event.sessionId) {
		if (sessionOwner)
			markStale(dockServer_.sessionStatus.availability);
		markStale(dockServer_.outputsStatus.availability);
	}
	if (dockTopologyChanged)
		*dockTopologyChanged = topologyChanged;
	emit runtimeStateChanged();
	emit serverStateChanged();
	return true;
}

} // namespace gost
