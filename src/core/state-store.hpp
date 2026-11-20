#pragma once

#include "api/models.hpp"
#include "core/dock-server-state.hpp"
#include "core/operations-state.hpp"
#include "core/configuration-state.hpp"
#include "core/fallback-state.hpp"
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
	// Создаёт пустое состояние с настройками по умолчанию; parent управляет временем жизни QObject.
	explicit StateStore(QObject *parent = nullptr);
	// До первого наблюдения optional пусты. Имя OBS Profile — отображение, а не устойчивая идентичность.
	const std::optional<QString> &profileName() const noexcept;
	// Возвращает последний флаг OBS либо отсутствие наблюдения до первой выборки.
	std::optional<bool> streamingActive() const noexcept;
	// Заменяет локальное имя профиля и флаг трансляции до уведомления obsStateChanged.
	void setObsState(QString profileName, bool streamingActive);
	// Возвращает независимую от сервера выборку локальной статистики OBS.
	const ObsStatistics &obsStatistics() const noexcept;
	// Публикует готовую выборку statistics и уведомляет obsStatisticsChanged.
	void setObsStatistics(ObsStatistics statistics);
	// Возвращает канонический адрес выбранного сервера.
	const QString &serverOrigin() const noexcept;
	// Возвращает фазу подключения API, независимо от OBS и наличия серверного эфира.
	ConnectionStatus connectionStatus() const noexcept;
	// Возвращает полученную часть списка Streams для выбора в интерфейсе.
	const QVector<api::Stream> &streams() const noexcept;
	// Показывает, прочитан ли весь список Streams, без вывода о свежести runtime.
	bool streamsComplete() const noexcept;
	// Возвращает сохранённый ID выбора, даже если его наличие на сервере ещё не подтверждено.
	const QString &selectedStreamId() const noexcept;
	// Ищет имя выбранного Stream в известном списке; при отсутствии возвращает пустую строку.
	QString selectedStreamName() const;
	// Подтверждает UUID профиля и наличие выбранного Stream в полученной части списка.
	bool selectionAvailable() const noexcept;
	// UUID текущего профиля может быть известен, даже когда сохранённый Stream ещё не найден на сервере.
	// Неполный список оставляет его наличие неподтверждённым. selectionAvailable возвращает true,
	// только когда профиль подтверждён и выбранный Stream есть в полученном списке.
	bool profileResolved() const noexcept;
	// Возвращает предпочтение автоматического подключения API при загрузке OBS.
	bool autoConnect() const noexcept;
	// Возвращает разрешение автоматического восстановления API.
	bool reconnect() const noexcept;
	// Возвращает бюджет восстановления после SSE 400/409; -1 означает отсутствие конечного лимита.
	int sseRejectionRetries() const noexcept;
	// Возвращает безопасный текст ошибки настроек или подключения для интерфейса.
	const QString &safeError() const noexcept;
	// Возвращает соответствующий safeError ключ локализации, если он известен.
	const QString &safeErrorKey() const noexcept;
	// Показывает, не удалось ли сохранить текущее предпочтение на диск.
	bool settingsUnsaved() const noexcept;
	// Возвращает безопасное пояснение привязки профиля к Stream.
	const QString &profileNotice() const noexcept;
	// Возвращает ключ локализации пояснения привязки профиля.
	const QString &profileNoticeKey() const noexcept;
	// Возвращает конфигурационные REST-коллекции с их полнотой и свежестью.
	const ConfigurationState &configuration() const noexcept;
	// Возвращает единственную RAM-форму отдельно от серверного кеша.
	const ConfigurationEditorState &configurationEditor() const noexcept;
	// Публикует новое значение формы до синхронного сигнала интерфейсу.
	void setConfigurationEditor(ConfigurationEditorState editor);
	// Возвращает отдельную RAM-модель подготовки и назначения заглушек.
	const FallbackState &fallback() const noexcept;
	// Публикует готовое значение заглушек перед синхронным уведомлением.
	void setFallback(FallbackState fallback);
	// Проверяет барьер origin и прежние editor fixtures, не связывая запрет с текущей карточкой.
	bool configurationSettled(const QString &origin) const;
	// Атомарно занимает канал до сигналов; owner/token адресуют только одно намерение.
	bool beginConfigurationWrite(const QString &origin, const QString &owner, quint64 token);
	// Проверяет исходное владение непосредственно перед передачей HTTP.
	bool ownsConfigurationWrite(const QString &origin, const QString &owner, quint64 token) const;
	// Освобождает только указанное намерение; Unknown регистрируется вызывающим кодом раньше.
	void endConfigurationWrite(const QString &origin, const QString &owner, quint64 token);
	// Устанавливает признак несверенных обязательств владельца независимо от видимой карточки.
	void setConfigurationUnknown(const QString &origin, const QString &owner, bool unknown);
	// Возвращает эпоху конфигурации для защиты чтений и предварительной проверки.
	quint64 configurationGeneration() const noexcept;
	// Отзывает пригодность прежних чтений перед записью и после её завершения.
	void invalidateConfiguration();
	// Возвращает общий срок новых обычных REST-заданий в секундах.
	int httpTimeoutSeconds() const noexcept;
	// Возвращает выбранный уровень штатного журнала OBS.
	bool detailedLogging() const noexcept;
	// Проецирует настройки из SettingsStore, не записывая их на диск.
	void setClientPreferences(int seconds, bool detailed);
	// Возвращает исторические записи без подмены текущих наблюдений исполнения.
	const HistoryState &history() const noexcept;
	// Возвращает детальные REST-чтения и статусы их доступности.
	const DetailState &details() const noexcept;
	// Возвращает дерево исполнений установленного snapshot и принятых SSE-событий.
	const RuntimeState &runtime() const noexcept;
	// Возвращает тройку сервер/UUID профиля/Stream, к которой относятся метаданные Dock.
	const DockContext &dockContext() const noexcept;
	// Возвращает последние REST-метаданные Session, выходов и fallback выбранного контекста.
	const DockServerState &dockServer() const noexcept;
	// Возвращает ограниченную RAM-модель намерений; она не является серверным runtime.
	const OperationsState &operations() const noexcept;
	// Заменяет готовое значение до отдельного сигнала, не вызывая runtime reducer.
	void setOperations(OperationsState operations);
	// Возвращает независимые признаки подтверждения состояния, входа и метрик instanceId.
	RuntimeEvidence runtimeEvidence(const QString &instanceId) const;
	// Возвращает подтверждение GPU независимо от других событий; ожидающий SSE снимок ещё не подтверждён.
	RuntimeAvailability gpuAvailability() const noexcept;
	// Возвращает последнюю проверку ready; до первого успешного чтения значение отсутствует.
	const std::optional<api::Health> &ready() const noexcept;
	// Возвращает последние объявленные возможности сервера либо отсутствие чтения.
	const std::optional<api::Capabilities> &capabilities() const noexcept;
	// Смена origin очищает DockContext и серверный кеш Dock. Connected снимает ожидание подтверждения
	// установленного снимка, но при возобновлении без нового снимка не освежает прежние наблюдения.
	// Текст ошибки и ключ локализации должны быть безопасны для UI.
	void setConnection(QString origin, ConnectionStatus status, QString safeError = {}, QString errorKey = {});
	// Заменяет предпочтения API и безопасную ошибку сохранения; unsaved отдельно обозначает несохранённость.
	void setPreferences(bool autoConnect, bool reconnect, int sseRejectionRetries, bool unsaved,
			    QString settingsError = {}, QString errorKey = {});
	// Заменяет известный список и configuration.streams, сохраняя явный complete. Выбор не удаляет.
	void setStreams(QVector<api::Stream> streams, bool complete);
	// Новый контекст сбрасывает REST-метаданные Dock; переименование с прежним UUID не меняет контекст.
	void setSelection(QString streamId, bool profileResolved, QString notice = {}, QString noticeKey = {},
			  QString profileUuid = {});
	// Принадлежность/актуальность пакета уже должен проверить ConnectionController до этого вызова.
	void setDockServer(DockServerState metadata);
	// Сохраняет результат ready и уведомляет serverStateChanged.
	void setReady(api::Health ready);
	// Сохраняет capabilities и уведомляет serverStateChanged.
	void setCapabilities(api::Capabilities capabilities);
	// Методы замены принимают готовые значения целиком. setConfiguration также обновляет список выбора;
	// setHistory и setDetails не меняют runtime. Все уведомляют потребителей через serverStateChanged.
	void setConfiguration(ConfigurationState configuration);
	// Заменяет историю целиком и уведомляет UI, не меняя оперативное дерево.
	void setHistory(HistoryState history);
	// Заменяет детальные чтения целиком и уведомляет UI, не меняя оперативное дерево.
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
	// Сообщает об изменении каталога, передачи, поколения или policy заглушки.
	void fallbackChanged();
	// Сообщает о занятии или освобождении общего конфигурационного барьера.
	void configurationActivityChanged();
	// Сообщает о новом вводе, конфликте или результате конфигурационной записи.
	void configurationEditorChanged();
	// Отзывает старую предварительную проверку и адресные чтения указанной эпохи.
	void configurationInvalidated(quint64 generation);
	// Уведомляет о результате операции независимо от REST/SSE телеметрии.
	void operationsChanged();
	// Сообщает об изменении имени профиля или флага штатной трансляции OBS.
	void obsStateChanged();
	// Сообщает о новой локальной статистике OBS.
	void obsStatisticsChanged();
	// Сообщает об изменении подключения, REST-данных или серверных предпочтений.
	void serverStateChanged();
	// Сообщает об изменении списка Streams или выбора для его повторного отображения.
	void streamsChanged();
	// Даёт UI сохранить последние значения перед снятием подтверждения свежести.
	void runtimeAboutToBecomeStale();
	// Сообщает о замене дерева, применении события или изменении доступности runtime.
	void runtimeStateChanged();

private:
	// Содержит адрес одного активного намерения двух конфигурационных контроллеров.
	struct ConfigurationWrite {
		// Канонический сервер ограничивает действие блокировки.
		QString origin;
		// Владелец Configuration либо Fallback; не является QObject ресурсом.
		QString owner;
		// Поколение намерения защищает снятие чужой блокировки.
		quint64 token = 0;
	};
	// Одно GUI-намерение занимает канал до завершения всей записи/пакета.
	std::optional<ConfigurationWrite> configurationWrite_;
	// Наличие owner в origin означает несверенную запись; точные обязательства хранят контроллеры.
	QMap<QString, QSet<QString>> configurationUnknown_;
	// RAM-модель заглушек не смешивается с runtime и редактором остальных сущностей.
	FallbackState fallback_;
	// Последнее отображаемое имя OBS Profile; не используется как устойчивая идентичность.
	std::optional<QString> profileName_;
	// Последний флаг активности OBS; пуст до первого наблюдения.
	std::optional<bool> streamingActive_;
	// Локальные метрики OBS с собственной доступностью и эпохами.
	ObsStatistics obsStatistics_;
	// Канонический origin наблюдаемого сервера.
	QString origin_;
	// Текущая фаза подключения REST/SSE.
	ConnectionStatus connectionStatus_ = ConnectionStatus::Disconnected;
	// Полученная часть списка Streams для пользовательского выбора.
	QVector<api::Stream> streams_;
	// true только после полного чтения streams_.
	bool streamsComplete_ = false;
	// Сохранённый ID выбранного Stream для текущего origin/UUID профиля.
	QString selectedStreamId_;
	// Подтверждена ли устойчивая идентичность текущего OBS Profile.
	bool profileResolved_ = false;
	// Предпочтение подключения API при загрузке OBS.
	bool autoConnect_ = true;
	// Предпочтение автоматического восстановления API.
	bool reconnect_ = true;
	// Число разрешённых восстановлений после SSE 400/409 либо -1 для неограниченного режима.
	int sseRejectionRetries_;
	// Есть ли несохранённые на диск предпочтения.
	bool settingsUnsaved_ = false;
	// Безопасный текст последней ошибки подключения.
	QString connectionError_;
	// Ключ локализации connectionError_.
	QString connectionErrorKey_;
	// Безопасный текст ошибки сохранения настроек.
	QString settingsError_;
	// Ключ локализации settingsError_.
	QString settingsErrorKey_;
	// Пояснение текущей привязки профиля.
	QString profileNotice_;
	// Ключ локализации profileNotice_.
	QString profileNoticeKey_;
	// Конфигурационные коллекции REST; не заменяют runtime.
	ConfigurationState configuration_;
	// RAM-черновик не сериализуется в настройки и переживает скрытие окна.
	ConfigurationEditorState configurationEditor_;
	// Изменение эпохи запрещает старому GET заменить результат записи.
	quint64 configurationGeneration_ = 0;
	// Срок новых HTTP-заданий; текущие задания сохраняют исходный deadline.
	int httpTimeoutSeconds_ = 15;
	// true включает безопасную подробную диагностику штатного журнала.
	bool detailedLogging_ = false;
	// Исторические Session/Output/Receipt с закреплёнными параметрами.
	HistoryState history_;
	// Отдельные детальные REST-ответы и статусы чтения.
	DetailState details_;
	// Текущее дерево оперативных исполнений SSE.
	RuntimeState runtime_;
	// Идентичность выбора, ограничивающая применение метаданных Dock.
	DockContext dockContext_;
	// Метаданные Session/Outputs/fallback для dockContext_.
	DockServerState dockServer_;
	// Текущий и последний прерванный workflow; при перезапуске процесса пусты.
	OperationsState operations_;
	// Новый snapshot ещё ждёт подтверждения заголовками подписки SSE.
	bool pendingAnchor_ = false;
	// Идентичность GPU текущего снимка для отбрасывания чужих метрик.
	QString gpuInstanceId_;
	// Последняя принятая ревизия GPU; независима от ревизий Session/Output.
	std::optional<quint64> gpuRevision_;
	// Свежесть последнего принятого GPU-снимка; восстановление транспорта само её не возвращает.
	RuntimeAvailability gpuAvailability_ = RuntimeAvailability::Unknown;
	// Последний успешный ответ проверки ready.
	std::optional<api::Health> ready_;
	// Последний успешный ответ capabilities.
	std::optional<api::Capabilities> capabilities_;
	// Для каждого instanceId храним последние принятые ревизии state и metrics независимо друг от друга.
	// evidence отдельно хранит подтверждение свежести: потреблённое событие со старой ревизией
	// может продвинуть cursor, но не делает старое состояние или измерение снова свежим.
	struct Watermarks {
		// Снимок задаёт только ревизию состояния; первая последующая метрика имеет собственную шкалу ревизий.
		std::optional<quint64> state;
		// Последняя принятая ревизия метрик; может отсутствовать после установки snapshot.
		std::optional<quint64> metrics;
		// Подтверждение отдельных осей исполнения независимо от числовых ревизий.
		RuntimeEvidence evidence;
	};
	// Границы принятых ревизий и подтверждение наблюдения каждого instanceId.
	QMap<QString, Watermarks> watermarks_;
};

} // namespace gost
