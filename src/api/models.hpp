#pragma once

#include <QByteArray>
#include <QMap>
#include <QString>
#include <QVector>
#include <optional>
#include <functional>
#include <utility>

namespace gost::api {

struct RequestContext {
	quint64 connection = 0;
	quint64 selection = 0;
	bool operator==(const RequestContext &) const = default;
};
enum class FieldState { Missing, Null, Value };
template<class T> struct JsonField {
	FieldState state = FieldState::Missing;
	T value{};
	bool hasValue() const noexcept { return state == FieldState::Value; }
	bool isNull() const noexcept { return state == FieldState::Null; }
	bool isMissing() const noexcept { return state == FieldState::Missing; }
	explicit operator bool() const noexcept { return hasValue(); }
	const T &operator*() const { return value; }
	T &operator*() { return value; }
	const T *operator->() const { return &value; }
	T *operator->() { return &value; }
};
struct ProfileText {
	QByteArray bytes;
	bool operator==(const ProfileText &) const = default;
};
enum class ErrorKind { Transport, Timeout, Cancelled, Decode, Limit, Http, Contract, Tls };
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
template<class T> struct Result {
	std::optional<T> value;
	std::optional<Error> error;
	explicit operator bool() const noexcept { return value.has_value() && !error.has_value(); }
	static Result success(T value) { return {std::move(value), std::nullopt}; }
	static Result failure(Error error) { return {std::nullopt, std::move(error)}; }
};
template<class T> using Callback = std::function<void(RequestContext, Result<T>)>;
struct Limits {
	int httpDeadlineMs = 15000;
	int httpIdleMs = 10000;
	int maxActiveRequests = 4;
	int maxQueuedRequests = 32;
	int sseHandshakeMs = 15000;
	int sseIdleMs = 45000;
	qsizetype readBytes = 256 * 1024;
	qsizetype restBodyBytes = 8 * 1024 * 1024;
	qsizetype snapshotBodyBytes = 16 * 1024 * 1024;
	qsizetype frameBytes = 1024 * 1024;
	qsizetype pendingBytes = 4 * 1024 * 1024;
	int pendingEvents = 256;
	int eventsPerTurn = 32;
	qsizetype bytesPerTurn = 256 * 1024;
	int maxPages = 100;
	int maxItems = 10000;
	qsizetype pageWalkBytes = 64 * 1024 * 1024;
};
struct Record {
	QString id{};
	quint64 revision{};
	QString createdAt{};
	QString updatedAt{};
};
struct Failure {
	QString code{};
	QString message{};
};
struct Watchdog {
	quint64 videoTimeoutNs{};
	quint64 audioTimeoutNs{};
	quint64 recoveryWindowNs{};
	quint64 keyframeTimeoutNs{};
};
struct Policy {
	QString assetId{};
	Watchdog watchdog{};
};
struct Stream : Record {
	QString name{};
	JsonField<QString> publicationKey{};
	JsonField<Policy> fallback{};
};
struct Destination : Record {
	QString streamId{};
	QString groupId{};
	QString name{};
	QString platform{};
	JsonField<QString> endpoint{};
	JsonField<QString> streamKey{};
	bool enabled{};
};
struct FrameRate {
	quint32 numerator{};
	quint32 denominator{};
};
struct Color {
	ProfileText range{};
	ProfileText primaries{};
	ProfileText transfer{};
	ProfileText matrix{};
};
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
struct AudioFormat {
	QString codec{};
	ProfileText codecProfile{};
	quint32 sampleRateHz{};
	quint16 channels{};
	ProfileText channelLayout{};
};
struct Format {
	VideoFormat video{};
	AudioFormat audio{};
};
struct VideoEncoding {
	quint64 targetBitrateBps{};
	quint64 maxBitrateBps{};
	quint64 bufferBits{};
	quint32 gopFrames{};
	quint32 maxBFrames{};
	bool closedGop{};
};
struct AudioEncoding {
	quint64 bitrateBps{};
};
struct Group : Record {
	QString streamId{};
	QString profileId{};
	QString name{};
};
struct Receipt : Record {
	QString requestId{};
	QString kind{};
	QString targetId{};
	QString sessionId{};
	QString outputId{};
	QString status{};
	JsonField<Failure> failure{};
};
struct Asset : Record {
	QString name{};
	QString kind{};
	QString state{};
	QString currentGenerationId{};
	QString pendingGenerationId{};
	quint64 sizeBytes{};
};
struct Generation : Record {
	QString assetId{};
	QString kind{};
	QString status{};
	quint64 sizeBytes{};
	JsonField<Failure> failure{};
};
struct Rate {
	QString status{};
	QString reason{};
	JsonField<double> value{};
	JsonField<QString> windowStart{};
	JsonField<QString> windowEnd{};
	quint64 durationNs{};
};
struct Rates {
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
	quint64 sourceEpoch{};
};
struct Input {
	bool connected{};
	JsonField<QString> changedAt{};
	JsonField<InputMetrics> metrics{};
};
struct GroupMetrics {
	MediaMetrics liveInput{};
	MediaMetrics fallbackInput{};
	MediaMetrics output{};
};
struct OutputMetrics {
	MediaMetrics media{};
	QMap<QString, quint64> counters{};
	QMap<QString, quint64> queueDrops{};
	QMap<QString, quint64> deliveryDrops{};
	bool resync{};
	JsonField<QString> lastWrite{};
	JsonField<QString> lastMedia{};
	quint64 ackAheadEventsTotal{};
	quint64 ackAheadBytesLast{};
	quint64 ackAheadBytesMax{};
	JsonField<QString> ackLastAnomalyAt{};
};
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
struct FFmpegMetrics {
	QString status{};
	QString reason{};
	JsonField<QString> sampledAt{};
	JsonField<QString> lastProgressAt{};
	quint64 progressLosses{};
	quint64 diagnosticLosses{};
	JsonField<FFmpegProgress> progress{};
};
struct GPUMetric {
	QString status{};
	QString reason{};
	JsonField<quint64> value{};
	JsonField<QString> sampledAt{};
	JsonField<QString> lastSuccessAt{};
	quint64 samplingPeriodUs{};
};
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
struct GPU {
	QString status{};
	QString reason{};
	JsonField<QString> sampledAt{};
	quint64 staleAfterNs{};
	QVector<GPUDevice> devices{};
};
struct Observation {
	QString instanceId{};
	quint64 revision{};
	JsonField<QString> observedAt{};
	QString state{};
	JsonField<Failure> failure{};
};
struct RuntimeGroup : Observation {
	QString groupId{};
	QString profileId{};
	QString source{};
	quint64 sourceRevision{};
	JsonField<GroupMetrics> metrics{};
};
struct RuntimeOutput : Observation {
	QString outputId{};
	JsonField<OutputMetrics> metrics{};
};
struct RuntimeFFmpeg : Observation {
	QString groupInstanceId{};
	QString role{};
	JsonField<FFmpegMetrics> metrics{};
};
struct RuntimeSession {
	QString sessionId{};
	JsonField<Observation> observation{};
	JsonField<Input> input{};
	QVector<RuntimeGroup> groups{};
	QVector<RuntimeOutput> outputs{};
	QVector<RuntimeFFmpeg> ffmpeg{};
};
struct Anchor {
	QString cursor{};
	QVector<RuntimeSession> sessions{};
	JsonField<GPU> gpu{};
};
struct SessionMetrics {
	QString availability{};
	QString reason{};
	JsonField<RuntimeSession> snapshot{};
};
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
struct NVENC {
	QString preset{};
	QString tuning{};
	QString multipass{};
	int lookaheadFrames{};
	bool enableSpatialAq{};
	bool enableTemporalAq{};
	int aqStrength{};
};
struct SVTAV1 {
	int preset{};
	QString tuning{};
	int aqMode{};
	int lookaheadFrames{};
};
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
struct GroupProfile {
	QString mode{};
	JsonField<QString> liveAudioMode{};
	Format format{};
	VideoEncoding videoEncoding{};
	AudioEncoding audioEncoding{};
	JsonField<Encoding> encoding{};
};
struct Profile : Record {
	QString name{};
	GroupProfile profile{};
};
struct GroupSnapshot {
	Group group{};
	Profile profile{};
};
struct Session : Record {
	QString streamId{};
	QString state{};
	bool desiredPaused{};
	Stream stream{};
	QVector<GroupSnapshot> groups{};
	JsonField<QString> endedAt{};
	JsonField<Failure> failure{};
};
struct Output : Record {
	QString sessionId{};
	Destination destination{};
	Group group{};
	Profile profile{};
	JsonField<Policy> fallback{};
	bool desiredRunning{};
	bool attached{};
};
struct Variant : Record {
	QString assetId{};
	QString generationId{};
	QString state{};
	GroupProfile profile{};
	Format format{};
	quint64 sizeBytes{};
};
struct Health {
	QString status;
	QString reason;
};
struct Capability {
	QString status;
	QString reason;
	JsonField<QString> checkedAt;
};
struct Capabilities {
	Capability ffmpeg;
	Capability ffprobe;
	Capability helper;
	Capability nvidia;
};
struct PolicyResult {
	quint64 revision = 0;
	JsonField<Policy> fallback;
};
struct Preparation {
	QString assetId;
	QString generationId;
};
struct OutputDetail : Output {
	JsonField<RuntimeOutput> runtime;
	QString runtimeAvailability;
	QString runtimeReason;
};
template<class T> struct Page {
	QVector<T> items;
	QString nextCursor;
};
template<class T> struct Collection {
	QVector<T> items;
	bool complete = false;
	QString nextCursor;
	qsizetype bytes = 0;
};
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
