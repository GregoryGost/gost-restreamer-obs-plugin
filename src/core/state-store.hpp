#pragma once

#include "api/models.hpp"
#include "core/dock-server-state.hpp"
#include "obs/obs-statistics.hpp"

#include <QObject>
#include <QString>

#include <optional>

namespace gost {

enum class ConnectionStatus { Disconnected, Connecting, Connected, Reconnecting, Error };

// Последние REST-конфигурации, индексированные ID записи; статус каждой коллекции задаёт полноту
// и свежесть чтения. Отсутствие ID в неполной коллекции не доказывает удаление сущности.
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

// Сохранённые Session/Output/Receipt со своими историческими снимками параметров.
// Поздняя запись истории не изменяет ни оперативные ревизии, ни закреплённые настройки других записей.
struct HistoryState {
	QMap<QString, api::Session> sessions;
	QMap<QString, api::Output> outputs;
	QMap<QString, api::Receipt> receipts;
	ReadStatus sessionsStatus;
	ReadStatus outputsStatus;
	ReadStatus receiptsStatus;
};

// Выборочные детальные REST-чтения и отдельные статусы каждого результата. Они доступны диагностике,
// но не являются заменой текущему дереву единственной последовательной SSE-подписки.
struct DetailState {
	QMap<QString, api::SessionMetrics> sessionMetrics;
	QMap<QString, api::OutputDetail> outputs;
	QMap<QString, ReadStatus> sessionMetricsStatus;
	QMap<QString, ReadStatus> outputsStatus;
};

// Текущие исполнения из установленного снимка и последующих принятых событий SSE.
// availability описывает общую доступность дерева; свежесть состояния, входа и метрик задаёт RuntimeEvidence.
struct RuntimeState {
	QMap<QString, api::RuntimeSession> sessions;
	api::JsonField<api::GPU> gpu;
	QString cursor;
	RuntimeAvailability availability = RuntimeAvailability::Unknown;
};

/**
 * Общее состояние плагина для контроллеров и двух представлений UI.
 * Все чтения, изменения и сигналы принадлежат GUI-потоку. Ссылки из методов чтения заимствованы:
 * их нельзя удерживать через изменяющий вызов, обработку сигнала или следующий оборот событий.
 * Сигналы могут синхронно вызвать потребителей, поэтому изменения устанавливаются до уведомления.
 *
 * OBS, REST-конфигурация, история, детали и оперативные данные SSE хранятся независимо. Ошибка сети
 * не означает остановки OBS/Session, а наличие сохранённой записи не подтверждает телеметрию.
 * installAnchor задаёт базу runtime, applyEvent последовательно изменяет её с проверкой дерева,
 * идентичностей и отдельных ревизий состояния/метрик. Непрозрачный cursor подтверждается
 * только после принятия события, включая безопасный пропуск устаревшей ревизии.
 *
 * RuntimeAvailability описывает доступность, RuntimeEvidence — подтверждение каждой оси исполнения.
 * Возобновление транспорта не освежает прежние оси автоматически. Dock-метаданные дополнительно
 * привязаны к серверу, UUID профиля и Stream, чтобы последние значения не перешли к другому выбору.
 */
class StateStore final : public QObject {
	Q_OBJECT

public:
	explicit StateStore(QObject *parent = nullptr);
	// До первого наблюдения optional пусты. Имя OBS Profile — отображение, а не устойчивая идентичность.
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
	// UUID текущего профиля может быть известен, даже когда сохранённый Stream ещё не найден на сервере.
	// Неполный список оставляет его наличие неподтверждённым. selectionAvailable возвращает true,
	// только когда профиль подтверждён и выбранный Stream есть в полученном списке.
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
	// Смена origin очищает DockContext и серверный кеш Dock. Connected снимает ожидание подтверждения
	// установленного снимка, но при возобновлении без нового снимка не освежает прежние наблюдения.
	// Текст ошибки и ключ локализации должны быть безопасны для UI.
	void setConnection(QString origin, ConnectionStatus status, QString safeError = {}, QString errorKey = {});
	void setPreferences(bool autoConnect, bool reconnect, int sseRejectionRetries, bool unsaved,
			    QString settingsError = {}, QString errorKey = {});
	// Заменяет известный список и configuration.streams, сохраняя явный complete. Выбор не удаляет.
	void setStreams(QVector<api::Stream> streams, bool complete);
	// Новый контекст сбрасывает REST-метаданные Dock; переименование с прежним UUID не меняет контекст.
	void setSelection(QString streamId, bool profileResolved, QString notice = {}, QString noticeKey = {},
			  QString profileUuid = {});
	// Принадлежность/актуальность пакета уже должен проверить ConnectionController до этого вызова.
	void setDockServer(DockServerState metadata);
	void setReady(api::Health ready);
	void setCapabilities(api::Capabilities capabilities);
	// Методы замены принимают готовые значения целиком. setConfiguration также обновляет список выбора;
	// setHistory и setDetails не меняют runtime. Все уведомляют потребителей через serverStateChanged.
	void setConfiguration(ConfigurationState configuration);
	void setHistory(HistoryState history);
	void setDetails(DetailState details);
	/**
	 * Проверяет снимок целиком и атомарно заменяет runtime/ревизии. Отвергает неверные/повторные
	 * идентичности, противоречивых родителей и превышение 10000 узлов; при false старое дерево остаётся.
	 * Снимок задаёт границу ревизий state, но не metrics. awaitSubscription скрывает подтверждение
	 * всех осей до Connected по заголовкам подписки. Предыдущий cursor и GPU-поколение заменяются.
	 */
	bool installAnchor(const api::Anchor &anchor, bool awaitSubscription = false);
	/**
	 * Применяет событие к установленному anchor. true разрешает подтвердить его cursor, включая
	 * дубликат текущего cursor, старую ревизию и удаление уже отсутствующего точного исполнения.
	 * false означает недостоверное продолжение: курсор/дерево не продвигаются, нужен новый anchor.
	 * dockTopologyChanged, если задан, получает false при входе и true при изменении состава/состояния,
	 * требующем нового чтения REST-метаданных Dock. Это не признак изменения любых метрик.
	 */
	bool applyEvent(const api::RuntimeEvent &event, bool *dockTopologyChanged = nullptr);
	// Сначала даёт Dock сохранить последние значения через runtimeAboutToBecomeStale, затем помечает
	// известные оси и REST-чтения Stale. Значения сохраняются; Unknown не превращается в Stale.
	void markRuntimeStale();
	// Очищает серверные коллекции/runtime и ready/capabilities, оставляя локальное OBS-состояние.
	// preserveDock сохраняет метаданные для уже проверенного прежнего контекста; сам не проверяет origin.
	void clearServerState(bool preserveDock = false);
	// Сбрасывает только runtime, его cursor, ожидание подтверждения снимка и все границы ревизий; уведомляет UI.
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
	// Для каждого instanceId храним последние принятые ревизии state и metrics независимо друг от друга.
	// evidence отдельно хранит подтверждение свежести: потреблённое событие со старой ревизией
	// может продвинуть cursor, но не делает старое состояние или измерение снова свежим.
	struct Watermarks {
		// Снимок задаёт только ревизию состояния; первая последующая метрика имеет собственную шкалу ревизий.
		std::optional<quint64> state;
		std::optional<quint64> metrics;
		RuntimeEvidence evidence;
	};
	QMap<QString, Watermarks> watermarks_;
};

} // namespace gost
