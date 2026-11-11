#pragma once
#include "api/models.hpp"
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonValue>
#include <cmath>
#include <limits>
#include <type_traits>
#include <QStringList>
namespace gost::api {
// Создаёт безопасное общее сообщение без включения исходного тела ответа и потенциальных секретов.
Error makeError(ErrorKind kind, int status = 0);
// Принимает только канонические ASCII-цифры без знака/ведущих нулей и с проверкой переполнения.
// При отказе value сохраняется; ноль представляется ровно строкой "0".
bool parseDecimal(const QString &text, quint64 &value);
// Проверяет календарную дату/время UTC с Z и до девяти знаков доли секунды; строку не преобразует.
bool validTimestamp(const QString &text);
// Перегрузки fromJson заполняют переданный объект и возвращают false с Error при нарушении формы.
// Они могут оставить частично изменённое value: публиковать его после false нельзя. Для внешнего
// документа предназначен decode<T>, который выдаёт только целиком успешно разобранный новый DTO.
// Неизвестные ключи обычных DTO допускаются; специальные оболочки имеют собственные ограничения.
bool fromJson(const QJsonValue &, QString &, Error &);
bool fromJson(const QJsonValue &, bool &, Error &);
bool fromJson(const QJsonValue &, quint64 &, Error &);
bool fromJson(const QJsonValue &, double &, Error &);
bool fromJson(const QJsonValue &, ProfileText &, Error &);
template<class T>
	requires(std::is_integral_v<T> && !std::is_same_v<T, bool> && !std::is_same_v<T, quint64>)
bool fromJson(const QJsonValue &json, T &value, Error &error)
{
	// Малые целые контракт хранит JSON-числами; quint64 исключён из шаблона и читается только строкой.
	if (!json.isDouble() || !std::isfinite(json.toDouble()) || std::trunc(json.toDouble()) != json.toDouble() ||
	    json.toDouble() < double(std::numeric_limits<T>::lowest()) ||
	    json.toDouble() > double(std::numeric_limits<T>::max())) {
		error = makeError(ErrorKind::Decode);
		return false;
	}
	value = T(json.toDouble());
	return true;
}
bool fromJson(const QJsonValue &, Record &, Error &);
bool fromJson(const QJsonValue &, Failure &, Error &);
bool fromJson(const QJsonValue &, Watchdog &, Error &);
bool fromJson(const QJsonValue &, Policy &, Error &);
bool fromJson(const QJsonValue &, Stream &, Error &);
bool fromJson(const QJsonValue &, Destination &, Error &);
bool fromJson(const QJsonValue &, FrameRate &, Error &);
bool fromJson(const QJsonValue &, Color &, Error &);
bool fromJson(const QJsonValue &, VideoFormat &, Error &);
bool fromJson(const QJsonValue &, AudioFormat &, Error &);
bool fromJson(const QJsonValue &, Format &, Error &);
bool fromJson(const QJsonValue &, VideoEncoding &, Error &);
bool fromJson(const QJsonValue &, AudioEncoding &, Error &);
bool fromJson(const QJsonValue &, Group &, Error &);
bool fromJson(const QJsonValue &, Receipt &, Error &);
bool fromJson(const QJsonValue &, Asset &, Error &);
bool fromJson(const QJsonValue &, Generation &, Error &);
bool fromJson(const QJsonValue &, Rate &, Error &);
bool fromJson(const QJsonValue &, Rates &, Error &);
bool fromJson(const QJsonValue &, ObservedFormat &, Error &);
bool fromJson(const QJsonValue &, MediaMetrics &, Error &);
bool fromJson(const QJsonValue &, InputMetrics &, Error &);
bool fromJson(const QJsonValue &, Input &, Error &);
bool fromJson(const QJsonValue &, GroupMetrics &, Error &);
bool fromJson(const QJsonValue &, OutputMetrics &, Error &);
bool fromJson(const QJsonValue &, FFmpegProgress &, Error &);
bool fromJson(const QJsonValue &, FFmpegMetrics &, Error &);
bool fromJson(const QJsonValue &, GPUMetric &, Error &);
bool fromJson(const QJsonValue &, GPUDevice &, Error &);
bool fromJson(const QJsonValue &, GPU &, Error &);
bool fromJson(const QJsonValue &, Observation &, Error &);
bool fromJson(const QJsonValue &, RuntimeGroup &, Error &);
bool fromJson(const QJsonValue &, RuntimeOutput &, Error &);
bool fromJson(const QJsonValue &, RuntimeFFmpeg &, Error &);
bool fromJson(const QJsonValue &, RuntimeSession &, Error &);
bool fromJson(const QJsonValue &, Anchor &, Error &);
bool fromJson(const QJsonValue &, SessionMetrics &, Error &);
bool fromJson(const QJsonValue &, RuntimeEvent &, Error &);
bool fromJson(const QJsonValue &, NVENC &, Error &);
bool fromJson(const QJsonValue &, SVTAV1 &, Error &);
bool fromJson(const QJsonValue &, Encoding &, Error &);
bool fromJson(const QJsonValue &, GroupProfile &, Error &);
bool fromJson(const QJsonValue &, Profile &, Error &);
bool fromJson(const QJsonValue &, GroupSnapshot &, Error &);
bool fromJson(const QJsonValue &, Session &, Error &);
bool fromJson(const QJsonValue &, Output &, Error &);
bool fromJson(const QJsonValue &, Variant &, Error &);
bool fromJson(const QJsonValue &, Health &, Error &);
bool fromJson(const QJsonValue &, Capability &, Error &);
bool fromJson(const QJsonValue &, Capabilities &, Error &);
bool fromJson(const QJsonValue &, PolicyResult &, Error &);
bool fromJson(const QJsonValue &, OutputDetail &, Error &);
bool fromJson(const QJsonValue &, Problem &, Error &);
bool fromJson(const QJsonValue &, Preparation &, Error &);
template<class T> bool fromJson(const QJsonValue &json, JsonField<T> &value, Error &error)
{
	// Физическое содержимое value не заменяет признак состояния; читать его можно только при FieldState::Value.
	if (json.isUndefined()) {
		value.state = FieldState::Missing;
		return true;
	}
	if (json.isNull()) {
		value.state = FieldState::Null;
		return true;
	}
	value.state = FieldState::Value;
	return fromJson(json, value.value, error);
}
// Контейнер очищается и наполняется заново. Ошибка элемента оставляет разобранный префикс,
// поэтому прямой вызов не обладает гарантией «старое значение или полностью новое».
template<class T> bool fromJson(const QJsonValue &json, QVector<T> &value, Error &error)
{
	if (!json.isArray()) {
		error = makeError(ErrorKind::Decode);
		return false;
	}
	const auto array = json.toArray();
	value.clear();
	value.reserve(array.size());
	for (const auto &entry : array) {
		T item{};
		if (!fromJson(entry, item, error))
			return false;
		value.push_back(std::move(item));
	}
	return true;
}
bool fromJson(const QJsonValue &, QMap<QString, quint64> &, Error &);
template<class T> bool fromJson(const QJsonValue &json, Page<T> &value, Error &error)
{
	if (!json.isObject()) {
		error = makeError(ErrorKind::Decode);
		return false;
	}
	const auto object = json.toObject();
	return fromJson(object.value(QStringLiteral("items")), value.items, error) &&
	       fromJson(object.value(QStringLiteral("next_cursor")), value.nextCursor, error);
}
// Требует корректный UTF-8 и корневой JSON-объект. Не ограничивает размер body самостоятельно:
// транспорт/вызывающая сторона обязаны применить свой бюджет до разбора.
template<class T> Result<T> decode(const QByteArray &body)
{
	// Новый объект публикуется только после полного разбора; частично заполненные поля наружу не выдаются.
	Error error = makeError(ErrorKind::Decode);
	if (!body.isValidUtf8())
		return Result<T>::failure(error);
	QJsonParseError parseError;
	const auto document = QJsonDocument::fromJson(body, &parseError);
	if (parseError.error != QJsonParseError::NoError || !document.isObject())
		return Result<T>::failure(error);
	T value{};
	if (!fromJson(document.object(), value, error))
		return Result<T>::failure(error);
	return Result<T>::success(std::move(value));
}
// Проверяет сочетания kind/owner, идентичностей и блока данных, но не существование родителей и порядок ревизий.
bool validateRuntimeEvent(const RuntimeEvent &event);
// Сохраняет исходный HTTP-статус даже при неверном Problem; прикладывает тело только при совпадении status.
Error decodeProblem(int status, const QByteArray &body);
} // namespace gost::api
