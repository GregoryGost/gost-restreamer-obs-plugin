#pragma once

#include <QByteArray>
#include <QMap>
#include <QString>
#include <QVector>
#include <optional>
#include <functional>
#include <utility>

namespace gost::api {

// DTO являются значениями полученного документа, а не живыми ссылками на серверные сущности.
// Поля времени хранят исходную временную метку UTC без потери наносекунд; поля *Ns/*Bytes/*Bps обозначают
// наносекунды, байты и биты/с соответственно. uint64 декодируется из десятичных строк без double.
// Само наличие структуры не доказывает свежесть наблюдений: её устанавливает StateStore.

// Локальные поколения подключения и выбора отделяют ответы прежнего сервера или Stream от текущего контекста.
struct RequestContext {
	quint64 connection = 0;
	quint64 selection = 0;
	bool operator==(const RequestContext &) const = default;
};
// Отсутствующее поле, явный null и полученное значение имеют разные контрактные значения.
enum class FieldState { Missing, Null, Value };
template<class T> struct JsonField {
	// state является источником истины. При Missing/Null физическое value может сохранять старое
	// содержимое после повторного fromJson; его нельзя читать или выдавать как полученное значение.
	FieldState state = FieldState::Missing;
	T value{};
	bool hasValue() const noexcept { return state == FieldState::Value; }
	bool isNull() const noexcept { return state == FieldState::Null; }
	bool isMissing() const noexcept { return state == FieldState::Missing; }
	explicit operator bool() const noexcept { return hasValue(); }
	// Разыменование не проверяет состояние: вызывающая сторона предварительно проверяет hasValue().
	const T &operator*() const { return value; }
	T &operator*() { return value; }
	const T *operator->() const { return &value; }
	T *operator->() { return &value; }
};
// Поля профиля могут содержать произвольные байты; UTF-8 и оболочка base64 декодируются без потери исходных данных.
struct ProfileText {
	QByteArray bytes;
	bool operator==(const ProfileText &) const = default;
};
// Decode относится к представлению данных, Contract — к недопустимому сочетанию/ответу протокола.
// Cancelled доступен модели ошибок, но явная отмена клиентов обычно подавляет обработчик целиком.
enum class ErrorKind { Transport, Timeout, Cancelled, Decode, Limit, Http, Contract, Tls };
// Структурированное тело application/problem+json. detail и instance — данные сервера;
// их наличие не разрешает автоматически выводить тело целиком в клиентский журнал.
struct Problem {
	QString type;
	QString title;
	int status = 0;
	QString detail;
	QString instance;
	QString code;
	QString field;
};
struct Error {
	ErrorKind kind = ErrorKind::Transport;
	int httpStatus = 0;
	QString message;
	std::optional<Problem> problem;
};
// Фабрики создают ровно одну альтернативу. Проверка bool требует value и отсутствия error;
// поля открыты для простых DTO, поэтому при ручном построении этот инвариант сохраняет вызывающий код.
template<class T> struct Result {
	std::optional<T> value;
	std::optional<Error> error;
	explicit operator bool() const noexcept { return value.has_value() && !error.has_value(); }
	static Result success(T value) { return {std::move(value), std::nullopt}; }
	static Result failure(Error error) { return {std::nullopt, std::move(error)}; }
};
template<class T> using Callback = std::function<void(RequestContext, Result<T>)>;
// Длительности заданы в миллисекундах, объёмы — в байтах. SSE имеет сроки ожидания заголовков и простоя,
// но не общий срок жизни подписки. Ограничения обхода страниц относятся ко всему получаемому списку.
struct Limits {
	// REST: абсолютный срок и простой начинают отсчёт при отправке; очередь в эти сроки не входит.
	int httpDeadlineMs = 15000;
	int httpIdleMs = 10000;
	int maxActiveRequests = 4;
	int maxQueuedRequests = 32;
	int sseHandshakeMs = 15000;
	int sseIdleMs = 45000;
	// readBytes ограничивает порцию чтения и буфер reply, размеры тел ограничивают полный документ.
	qsizetype readBytes = 256 * 1024;
	qsizetype restBodyBytes = 8 * 1024 * 1024;
	qsizetype snapshotBodyBytes = 16 * 1024 * 1024;
	qsizetype frameBytes = 1024 * 1024;
	qsizetype pendingBytes = 4 * 1024 * 1024;
	int pendingEvents = 256;
	// Бюджеты обработки SSE за оборот Qt; один допустимый крупный кадр может превысить bytesPerTurn.
	int eventsPerTurn = 32;
	qsizetype bytesPerTurn = 256 * 1024;
	int maxPages = 100;
	int maxItems = 10000;
	qsizetype pageWalkBytes = 64 * 1024 * 1024;
};
// Метаданные сохранённой записи. revision не является ревизией оперативных метрик и не сравнивается с ней.
struct Record {
	QString id{};
	quint64 revision{};
	QString createdAt{};
	QString updatedAt{};
};
// Серверная причина отказа. code пригоден для интерпретации известной ошибки, message остаётся
// полученным текстом и не должен автоматически попадать в журнал с остальным телом ответа.
struct Failure {
	QString code{};
	QString message{};
};
struct Watchdog {
	// Серверные интервалы контроля входа; клиент сохраняет их точно и не запускает собственный медиатаймер.
	quint64 videoTimeoutNs{};
	quint64 audioTimeoutNs{};
	quint64 recoveryWindowNs{};
	quint64 keyframeTimeoutNs{};
};
// Назначение Asset и интервалы серверного watchdog. Пустота/отсутствие политики кодируются
// внешним JsonField<Policy>, а не фиктивным ID существующего Asset.
struct Policy {
	QString assetId{};
	Watchdog watchdog{};
};
struct Stream : Record {
	QString name{};
	// Missing означает несовместимое/неполное чтение; null отклоняет декодер. Полученная строка — точный ключ.
	JsonField<QString> publicationKey{};
	// Null означает отсутствие назначенной политики; Missing сохраняет отсутствие поля в документе.
	JsonField<Policy> fallback{};
};
struct Destination : Record {
	QString streamId{};
	QString groupId{};
	QString name{};
	QString platform{};
	// Полный endpoint может содержать секретные компоненты; не нормализовать при показе/копировании.
	// Пустой полученный streamKey допустим, а Missing отличается от пустой строки. Null недопустим.
	JsonField<QString> endpoint{};
	JsonField<QString> streamKey{};
	bool enabled{}; // Участие в будущей конфигурации запуска, а не наблюдаемый статус текущего Output.
};
// Рациональный FPS профиля. Декодер отвергает ненулевой numerator при нулевом denominator;
// Декодированное 0/0 нельзя делить как обычную дробь; сам декодер допускает такое представление.
struct FrameRate {
	quint32 numerator{};
	quint32 denominator{};
};
// Точные текстовые параметры цветового представления; клиент не заменяет неизвестные имена
// предполагаемыми значениями стандартов и не теряет двоичное представление ProfileText.
struct Color {
	ProfileText range{};
	ProfileText primaries{};
	ProfileText transfer{};
	ProfileText matrix{};
};
// Ожидаемый формат профиля; отличается от фактически измеренного ObservedFormat.
// Текстовые параметры ProfileText сохраняют байты, включая неизвестные клиенту значения.
struct VideoFormat {
	QString codec{};
	quint32 width{};
	quint32 height{};
	FrameRate frameRate{};
	ProfileText codecProfile{};
	ProfileText level{};
	ProfileText tier{};
	ProfileText pixelFormat{};
	quint8 bitDepth{};
	Color color{};
};
// Ожидаемый формат AAC/аудио профиля: sampleRateHz — Гц, channels — количество каналов,
// channelLayout — точное обозначение раскладки. Это не замер реально поступившего звука.
struct AudioFormat {
	QString codec{};
	ProfileText codecProfile{};
	quint32 sampleRateHz{};
	quint16 channels{};
	ProfileText channelLayout{};
};
// Общий ожидаемый формат видео и аудио. Режимы обработки задаются отдельно в GroupProfile.
struct Format {
	VideoFormat video{};
	AudioFormat audio{};
};
struct VideoEncoding {
	// Битрейты — биты/с, bufferBits — биты, GOP/B-frames — число кадров. Это параметры рецепта,
	// которые не доказывают фактическую скорость текущего LIVE при режиме copy.
	quint64 targetBitrateBps{};
	quint64 maxBitrateBps{};
	quint64 bufferBits{};
	quint32 gopFrames{};
	quint32 maxBFrames{};
	bool closedGop{};
};
// Битрейт кодирования аудио в бит/с; применимость к LIVE зависит от liveAudioMode профиля.
struct AudioEncoding {
	quint64 bitrateBps{};
};
// Сохранённая связь Stream с Profile. Один Group может иметь несколько runtime-исполнений,
// поэтому его id не заменяет Observation.instanceId при применении событий/сбросе статистики.
struct Group : Record {
	QString streamId{};
	QString profileId{};
	QString name{};
};
// Долговечная квитанция команды. requestId связывает повторы одной операции; status описывает
// её выполнение, но не заменяет отдельное наблюдение Session/Output и приём медиапотока.
struct Receipt : Record {
	QString requestId{};
	QString kind{};
	QString targetId{};
	QString sessionId{};
	QString outputId{};
	QString status{};
	JsonField<Failure> failure{};
};
// Asset может одновременно иметь опубликованное current и готовящееся pending поколение.
// Ошибка pending не определяет состояние current; строки ID не задают порядок поколений.
struct Asset : Record {
	QString name{};
	QString kind{};
	QString state{};
	QString currentGenerationId{};
	QString pendingGenerationId{};
	quint64 sizeBytes{};
};
// Результат подготовки конкретного поколения. ready не доказывает совместимость с группами
// будущей Session; версия внутреннего серверного генератора в этом DTO не представлена.
struct Generation : Record {
	QString assetId{};
	QString kind{};
	QString status{};
	quint64 sizeBytes{};
	JsonField<Failure> failure{};
};
// Значение скорости интерпретируется вместе со status и окном измерения; отсутствующее значение не равно нулю.
// FPS измеряется в кадрах/с, битрейт — в бит/с, durationNs — в наносекундах.
struct Rate {
	QString status{};
	QString reason{};
	JsonField<double> value{};
	JsonField<QString> windowStart{};
	JsonField<QString> windowEnd{};
	quint64 durationNs{};
};
struct Rates {
	// payloadBitrate считает полезные медиабайты, writtenBitrate — записанные байты соответствующего
	// источника. Эти окна не подменяются bitrate штатного output OBS.
	Rate fps{};
	Rate payloadBitrate{};
	Rate writtenBitrate{};
};
struct ObservedFormat {
	QString videoCodec{};
	QString audioCodec{};
	quint32 width{};
	quint32 height{};
	quint32 sampleRateHz{};
	quint16 channels{};
};
// Накопительные счётчики относятся к соответствующему исполнению источника; rates описывает отдельные окна.
// saturated запрещает считать достигнутый предел достоверным нулевым приростом.
struct MediaMetrics {
	JsonField<QString> sampledAt{};
	quint64 videoFrames{};
	quint64 payloadBytes{};
	quint64 writtenBytes{};
	quint64 audioUnits{};
	bool saturated{};
	JsonField<QString> lastFrame{};
	Rates rates{};
	ObservedFormat format{};
};
struct InputMetrics {
	MediaMetrics media{};
	// Смена источника разрывает непрерывность входной телеметрии даже внутри той же Session.
	quint64 sourceEpoch{};
};
// Наблюдаемое подключение входа Session. connected и необязательные metrics обновляются
// разными событиями и имеют независимое подтверждение свежести в StateStore.
struct Input {
	bool connected{};
	JsonField<QString> changedAt{};
	JsonField<InputMetrics> metrics{};
};
// Раздельные направления одной группы; LIVE, fallback и output нельзя суммировать как один поток.
struct GroupMetrics {
	MediaMetrics liveInput{};
	MediaMetrics fallbackInput{};
	MediaMetrics output{};
};
struct OutputMetrics {
	MediaMetrics media{};
	QMap<QString, quint64> counters{};
	// Серверные потери очереди и доставки сохраняются раздельно и не являются сетевыми пропусками OBS.
	QMap<QString, quint64> queueDrops{};
	QMap<QString, quint64> deliveryDrops{};
	bool resync{};
	JsonField<QString> lastWrite{};
	JsonField<QString> lastMedia{};
	// ACK описывает достоверно измеренное опережение подтверждений RTMP. Это не RTT и не потеря кадров.
	// Только ackAheadEventsTotal накопителен; ackAheadBytesLast/ackAheadBytesMax и время не обнуляются вычитанием.
	quint64 ackAheadEventsTotal{};
	quint64 ackAheadBytesLast{};
	quint64 ackAheadBytesMax{};
	JsonField<QString> ackLastAnomalyAt{};
};
// Последние поля отчёта FFmpeg: каждое может отсутствовать независимо. speed — множитель реального
// времени, outTimeUs остаётся строкой сервера. end означает конец отчёта, а не успех серверной команды.
struct FFmpegProgress {
	JsonField<quint64> frame{};
	JsonField<quint64> totalSizeBytes{};
	JsonField<quint64> dupFrames{};
	JsonField<quint64> dropFrames{};
	JsonField<double> fps{};
	JsonField<double> bitrateBps{};
	JsonField<double> speed{};
	JsonField<QString> outTimeUs{};
	bool end{};
};
// Наблюдение одной попытки процесса. progressLosses/diagnosticLosses сохраняются раздельно
// от progress.dropFrames. Необязательный progress не подменяется нулевым отчётом.
struct FFmpegMetrics {
	QString status{};
	QString reason{};
	JsonField<QString> sampledAt{};
	JsonField<QString> lastProgressAt{};
	quint64 progressLosses{};
	quint64 diagnosticLosses{};
	JsonField<FFmpegProgress> progress{};
};
// Отдельная доступность одного серверного измерения. Единицы value определяет поле GPUDevice:
// utilization — проценты, memory*Bytes — байты; samplingPeriodUs — микросекунды.
struct GPUMetric {
	QString status{};
	QString reason{};
	JsonField<quint64> value{};
	JsonField<QString> sampledAt{};
	JsonField<QString> lastSuccessAt{};
	quint64 samplingPeriodUs{};
};
// Метрики одного серверного устройства с устойчивым uuid. Каждая GPUMetric имеет собственную
// доступность; отказ измерения encoder не делает остальные величины фиктивно нулевыми.
struct GPUDevice {
	QString uuid{};
	GPUMetric utilization{};
	GPUMetric memoryUtilization{};
	GPUMetric encoderUtilization{};
	GPUMetric decoderUtilization{};
	GPUMetric memoryUsedBytes{};
	GPUMetric memoryTotalBytes{};
	GPUMetric memoryFreeBytes{};
};
// Телеметрия GPU сервера, не локального компьютера OBS. staleAfterNs — серверный порог свежести;
// sampledAt и lastSuccessAt отдельных величин не обязаны совпадать с временем общей выборки.
struct GPU {
	QString status{};
	QString reason{};
	JsonField<QString> sampledAt{};
	quint64 staleAfterNs{};
	QVector<GPUDevice> devices{};
};
// instanceId обозначает конкретное исполнение, а не конфигурационный ID; revision относится к наблюдению.
struct Observation {
	QString instanceId{};
	quint64 revision{};
	JsonField<QString> observedAt{};
	QString state{};
	JsonField<Failure> failure{};
};
struct RuntimeGroup : Observation {
	// Несколько исполнений могут иметь одинаковый groupId/profileId. sourceRevision относится
	// к наблюдаемому источнику, а не к ревизии сохранённого Group или его профиля.
	QString groupId{};
	QString profileId{};
	QString source{};
	quint64 sourceRevision{};
	JsonField<GroupMetrics> metrics{};
};
// Текущее исполнение конфигурационного outputId; метрики принадлежат именно instanceId.
struct RuntimeOutput : Observation {
	QString outputId{};
	JsonField<OutputMetrics> metrics{};
};
// Связь задаёт точный groupInstanceId. role описывает назначение процесса и не является уникальным ID.
struct RuntimeFFmpeg : Observation {
	QString groupInstanceId{};
	QString role{};
	JsonField<FFmpegMetrics> metrics{};
};
// Дерево оперативных исполнений Session. Отсутствие observation/input/metrics сохраняется явно;
// конфигурационные имена и исторические параметры читаются отдельными REST DTO.
struct RuntimeSession {
	QString sessionId{};
	JsonField<Observation> observation{};
	JsonField<Input> input{};
	QVector<RuntimeGroup> groups{};
	QVector<RuntimeOutput> outputs{};
	QVector<RuntimeFFmpeg> ffmpeg{};
};
// Начальная согласованная точка оперативной подписки. cursor непрозрачен и не упорядочивается клиентом.
// Ревизии состояния в anchor не задают начальных границ ревизий последующих metrics-событий.
struct Anchor {
	QString cursor{};
	QVector<RuntimeSession> sessions{};
	JsonField<GPU> gpu{};
};
// Детальный ответ наблюдений Session. availability/reason определяют доступность snapshot;
// null snapshot означает отсутствие данных, а не пустую работающую Session.
struct SessionMetrics {
	QString availability{};
	QString reason{};
	JsonField<RuntimeSession> snapshot{};
};
// Оболочка одного изменения. kind и owner определяют допустимую комбинацию идентичностей и одного
// содержательного блока данных; state/removed/attempt могут не иметь такого блока. validateRuntimeEvent
// проверяет форму, StateStore — принадлежность существующему дереву и независимые ревизии.
struct RuntimeEvent {
	QString cursor{};
	QString kind{};
	QString owner{};
	QString sessionId{};
	QString outputId{};
	QString groupId{};
	QString profileId{};
	Observation observation{};
	QString groupInstanceId{};
	QString source{};
	quint64 sourceRevision{};
	QString role{};
	bool attemptActive{};
	QString reason{};
	JsonField<Input> input{};
	JsonField<GroupMetrics> group{};
	JsonField<OutputMetrics> output{};
	JsonField<FFmpegMetrics> ffmpeg{};
	JsonField<GPU> gpu{};
};
// Параметры конкретного рецепта NVENC. lookaheadFrames измеряется кадрами, preset/tuning/multipass
// сохраняют серверные имена. Поддержку параметров фактическим оборудованием подтверждает сервер.
struct NVENC {
	QString preset{};
	QString tuning{};
	QString multipass{};
	int lookaheadFrames{};
	bool enableSpatialAq{};
	bool enableTemporalAq{};
	int aqStrength{};
};
// Параметры SVT-AV1 для соответствующего рецепта; числовые preset/aqMode не являются процентами.
struct SVTAV1 {
	int preset{};
	QString tuning{};
	int aqMode{};
	int lookaheadFrames{};
};
// Явный рецепт encoding v2. Поля videoCustom/audioCustom представляют серверные разрешённые параметры,
// а не произвольную команду FFmpeg. Наличие DTO не является полной серверной валидацией рецепта.
struct Encoding {
	int version{};
	QString videoEncoder{};
	QString audioEncoder{};
	QString rateControl{};
	bool rescaleOutput{};
	QString videoCustom{};
	QString audioCustom{};
	JsonField<NVENC> nvenc{};
	JsonField<SVTAV1> svtAv1{};
};
// Полный снимок профиля. mode управляет видео, liveAudioMode — LIVE AAC независимо от encoding v2.
// Missing liveAudioMode сохраняет наследуемую обработку: новое copy при чтении не подставляется.
// Null недопустим; явный transcode разрешён лишь при mode=transcode. encoding=null — прежний рецепт.
// AudioEncoding/Encoding.audio* относятся к тишине fallback и к LIVE только при перекодировании AAC.
struct GroupProfile {
	QString mode{};
	JsonField<QString> liveAudioMode{};
	Format format{};
	VideoEncoding videoEncoding{};
	AudioEncoding audioEncoding{};
	JsonField<Encoding> encoding{};
};
// Именованная конфигурация полного GroupProfile с ревизией записи для контроля изменений.
struct Profile : Record {
	QString name{};
	GroupProfile profile{};
};
// Закреплённая пара Group/Profile в истории Session, независимая от поздних изменений реестра.
struct GroupSnapshot {
	Group group{};
	Profile profile{};
};
// Вложенные Stream, Group и Profile сохранены по значению и не заменяются поздней конфигурацией тех же ID.
struct Session : Record {
	QString streamId{};
	QString state{};
	bool desiredPaused{};
	Stream stream{};
	QVector<GroupSnapshot> groups{};
	JsonField<QString> endedAt{};
	JsonField<Failure> failure{};
};
// Закреплённые настройки исполнения отделены от текущих конфигурационных записей назначения и профиля.
struct Output : Record {
	QString sessionId{};
	Destination destination{};
	Group group{};
	Profile profile{};
	JsonField<Policy> fallback{};
	// Намерение сервера и прикрепление к Session. Фактический PUBLISHING находится в RuntimeOutput.
	bool desiredRunning{};
	bool attached{};
};
// Видимый вариант точного поколения с копией профиля/формата. Не содержит внутренних profile_key,
// generator_version и путей файлов; READY не позволяет клиенту доказать физическую готовность.
struct Variant : Record {
	QString assetId{};
	QString generationId{};
	QString state{};
	GroupProfile profile{};
	Format format{};
	quint64 sizeBytes{};
};
// Состояние HTTP-сервиса и безопасно интерпретируемая причина; готовность медиа из него не выводится.
struct Health {
	QString status;
	QString reason;
};
// Результат проверки серверного инструмента. checkedAt может быть null при отсутствии проверки;
// status=unverified не превращается в доказанную готовность или отказ инструмента.
struct Capability {
	QString status;
	QString reason;
	JsonField<QString> checkedAt;
};
// Независимые проверки инструментов сервера, не сведения о локальном runtime OBS.
struct Capabilities {
	Capability ffmpeg;
	Capability ffprobe;
	Capability helper;
	Capability nvidia;
};
// Политика имеет собственную revision. Явный null fallback означает отсутствие назначения;
// его нельзя заменить пустым Asset или готовностью ранее известного поколения.
struct PolicyResult {
	quint64 revision = 0;
	JsonField<Policy> fallback;
};
// Идентификаторы принятой подготовки: нужны для последующего чтения точного поколения,
// но само получение пары не подтверждает окончание генерации.
struct Preparation {
	QString assetId;
	QString generationId;
};
// REST объединяет сохранённый Output с необязательным runtime для детального чтения. Доступность
// следует проверять отдельно; этот ответ не заменяет подтверждение текущей SSE-последовательности.
struct OutputDetail : Output {
	JsonField<RuntimeOutput> runtime;
	QString runtimeAvailability;
	QString runtimeReason;
};
// Одна серверная страница. Пустой nextCursor завершает обход; непустой передаётся без интерпретации.
template<class T> struct Page {
	QVector<T> items;
	QString nextCursor;
};
// complete=false сохраняет полученную часть списка; наличие items само по себе не подтверждает полноту.
template<class T> struct Collection {
	QVector<T> items;
	bool complete = false;
	QString nextCursor;
	// Сумма исходных размеров принятых страниц, а не оценка памяти объектов items.
	qsizetype bytes = 0;
};
// Общие фильтры списков. Пустые строки опускаются; limit — размер одной страницы, не всего обхода.
// Применимость конкретного фильтра к маршруту проверяет сервер; клиент не меняет его значение.
struct ListQuery {
	int limit = 100;
	QString cursor;
	QString from;
	QString to;
	QString streamId;
	QString groupId;
	QString profileId;
	QString assetId;
	QString generationId;
	QString state;
};

} // namespace gost::api
