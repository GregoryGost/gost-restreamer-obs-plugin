#pragma once
#include "api/models.hpp"
#include "api/json-value.hpp"

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
// Читает QString с проверкой JSON-типа; при несовместимом поле возвращает false и Error.
bool fromJson(const JsonValue &, QString &, Error &);
// Читает bool с проверкой JSON-типа; при несовместимом поле возвращает false и Error.
bool fromJson(const JsonValue &, bool &, Error &);
// Читает quint64 с проверкой JSON-типа; при несовместимом поле возвращает false и Error.
bool fromJson(const JsonValue &, quint64 &, Error &);
// Читает double с проверкой JSON-типа; при несовместимом поле возвращает false и Error.
bool fromJson(const JsonValue &, double &, Error &);
// Читает ProfileText с проверкой JSON-типа; при несовместимом поле возвращает false и Error.
bool fromJson(const JsonValue &, ProfileText &, Error &);
// Читает строковые идентификаторы только из опубликованного сервером списка контекстных полей.
// Читает QMap<QString, QString> с проверкой JSON-типа; при несовместимом поле возвращает false и Error.
bool fromJson(const JsonValue &, QMap<QString, QString> &, Error &);
// Читает точное целое в физическом диапазоне T без double; при отказе value сохраняется.
template<class T>
	requires(std::is_integral_v<T> && !std::is_same_v<T, bool> && !std::is_same_v<T, quint64>)
bool fromJson(const JsonValue &json, T &value, Error &error)
{
	// parsedSigned принимает знаковый результат, parsedUnsigned — беззнаковый, без округления границ.
	qint64 parsedSigned = 0;
	quint64 parsedUnsigned = 0;
	if constexpr (std::is_signed_v<T>) {
		if (json.toSigned(parsedSigned) && parsedSigned >= std::numeric_limits<T>::lowest() &&
		    parsedSigned <= std::numeric_limits<T>::max()) {
			value = T(parsedSigned);
			return true;
		}
	} else {
		if (json.toUnsigned(parsedUnsigned) && parsedUnsigned <= std::numeric_limits<T>::max()) {
			value = T(parsedUnsigned);
			return true;
		}
	}
	error = makeError(ErrorKind::Decode);
	return false;
}
// Читает Record с проверкой JSON-типа; при несовместимом поле возвращает false и Error.
bool fromJson(const JsonValue &, Record &, Error &);
// Читает Failure с проверкой JSON-типа; при несовместимом поле возвращает false и Error.
bool fromJson(const JsonValue &, Failure &, Error &);
// Читает Watchdog с проверкой JSON-типа; при несовместимом поле возвращает false и Error.
bool fromJson(const JsonValue &, Watchdog &, Error &);
// Читает Policy с проверкой JSON-типа; при несовместимом поле возвращает false и Error.
bool fromJson(const JsonValue &, Policy &, Error &);
// Читает Stream с проверкой JSON-типа; при несовместимом поле возвращает false и Error.
bool fromJson(const JsonValue &, Stream &, Error &);
// Читает Destination с проверкой JSON-типа; при несовместимом поле возвращает false и Error.
bool fromJson(const JsonValue &, Destination &, Error &);
// Читает FrameRate с проверкой JSON-типа; при несовместимом поле возвращает false и Error.
bool fromJson(const JsonValue &, FrameRate &, Error &);
// Читает Color с проверкой JSON-типа; при несовместимом поле возвращает false и Error.
bool fromJson(const JsonValue &, Color &, Error &);
// Читает VideoFormat с проверкой JSON-типа; при несовместимом поле возвращает false и Error.
bool fromJson(const JsonValue &, VideoFormat &, Error &);
// Читает AudioFormat с проверкой JSON-типа; при несовместимом поле возвращает false и Error.
bool fromJson(const JsonValue &, AudioFormat &, Error &);
// Читает Format с проверкой JSON-типа; при несовместимом поле возвращает false и Error.
bool fromJson(const JsonValue &, Format &, Error &);
// Читает VideoEncoding с проверкой JSON-типа; при несовместимом поле возвращает false и Error.
bool fromJson(const JsonValue &, VideoEncoding &, Error &);
// Читает AudioEncoding с проверкой JSON-типа; при несовместимом поле возвращает false и Error.
bool fromJson(const JsonValue &, AudioEncoding &, Error &);
// Читает Group с проверкой JSON-типа; при несовместимом поле возвращает false и Error.
bool fromJson(const JsonValue &, Group &, Error &);
// Читает Receipt с проверкой JSON-типа; при несовместимом поле возвращает false и Error.
bool fromJson(const JsonValue &, Receipt &, Error &);
// Читает Asset с проверкой JSON-типа; при несовместимом поле возвращает false и Error.
bool fromJson(const JsonValue &, Asset &, Error &);
// Читает Generation с проверкой JSON-типа; при несовместимом поле возвращает false и Error.
bool fromJson(const JsonValue &, Generation &, Error &);
// Читает Rate с проверкой JSON-типа; при несовместимом поле возвращает false и Error.
bool fromJson(const JsonValue &, Rate &, Error &);
// Читает Rates с проверкой JSON-типа; при несовместимом поле возвращает false и Error.
bool fromJson(const JsonValue &, Rates &, Error &);
// Читает ObservedFormat с проверкой JSON-типа; при несовместимом поле возвращает false и Error.
bool fromJson(const JsonValue &, ObservedFormat &, Error &);
// Читает MediaMetrics с проверкой JSON-типа; при несовместимом поле возвращает false и Error.
bool fromJson(const JsonValue &, MediaMetrics &, Error &);
// Читает InputMetrics с проверкой JSON-типа; при несовместимом поле возвращает false и Error.
bool fromJson(const JsonValue &, InputMetrics &, Error &);
// Читает Input с проверкой JSON-типа; при несовместимом поле возвращает false и Error.
bool fromJson(const JsonValue &, Input &, Error &);
// Читает GroupMetrics с проверкой JSON-типа; при несовместимом поле возвращает false и Error.
bool fromJson(const JsonValue &, GroupMetrics &, Error &);
// Читает OutputMetrics с проверкой JSON-типа; при несовместимом поле возвращает false и Error.
bool fromJson(const JsonValue &, OutputMetrics &, Error &);
// Читает FFmpegProgress с проверкой JSON-типа; при несовместимом поле возвращает false и Error.
bool fromJson(const JsonValue &, FFmpegProgress &, Error &);
// Читает FFmpegMetrics с проверкой JSON-типа; при несовместимом поле возвращает false и Error.
bool fromJson(const JsonValue &, FFmpegMetrics &, Error &);
// Читает GPUMetric с проверкой JSON-типа; при несовместимом поле возвращает false и Error.
bool fromJson(const JsonValue &, GPUMetric &, Error &);
// Читает GPUDevice с проверкой JSON-типа; при несовместимом поле возвращает false и Error.
bool fromJson(const JsonValue &, GPUDevice &, Error &);
// Читает GPU с проверкой JSON-типа; при несовместимом поле возвращает false и Error.
bool fromJson(const JsonValue &, GPU &, Error &);
// Читает Observation с проверкой JSON-типа; при несовместимом поле возвращает false и Error.
bool fromJson(const JsonValue &, Observation &, Error &);
// Читает RuntimeGroup с проверкой JSON-типа; при несовместимом поле возвращает false и Error.
bool fromJson(const JsonValue &, RuntimeGroup &, Error &);
// Читает RuntimeOutput с проверкой JSON-типа; при несовместимом поле возвращает false и Error.
bool fromJson(const JsonValue &, RuntimeOutput &, Error &);
// Читает RuntimeFFmpeg с проверкой JSON-типа; при несовместимом поле возвращает false и Error.
bool fromJson(const JsonValue &, RuntimeFFmpeg &, Error &);
// Читает RuntimeSession с проверкой JSON-типа; при несовместимом поле возвращает false и Error.
bool fromJson(const JsonValue &, RuntimeSession &, Error &);
// Читает Anchor с проверкой JSON-типа; при несовместимом поле возвращает false и Error.
bool fromJson(const JsonValue &, Anchor &, Error &);
// Читает SessionMetrics с проверкой JSON-типа; при несовместимом поле возвращает false и Error.
bool fromJson(const JsonValue &, SessionMetrics &, Error &);
// Читает RuntimeEvent с проверкой JSON-типа; при несовместимом поле возвращает false и Error.
bool fromJson(const JsonValue &, RuntimeEvent &, Error &);
// Читает NVENC с проверкой JSON-типа; при несовместимом поле возвращает false и Error.
bool fromJson(const JsonValue &, NVENC &, Error &);
// Читает SVTAV1 с проверкой JSON-типа; при несовместимом поле возвращает false и Error.
bool fromJson(const JsonValue &, SVTAV1 &, Error &);
// Читает Encoding с проверкой JSON-типа; при несовместимом поле возвращает false и Error.
bool fromJson(const JsonValue &, Encoding &, Error &);
// Читает GroupProfile с проверкой JSON-типа; при несовместимом поле возвращает false и Error.
bool fromJson(const JsonValue &, GroupProfile &, Error &);
// Читает Profile с проверкой JSON-типа; при несовместимом поле возвращает false и Error.
bool fromJson(const JsonValue &, Profile &, Error &);
// Читает GroupSnapshot с проверкой JSON-типа; при несовместимом поле возвращает false и Error.
bool fromJson(const JsonValue &, GroupSnapshot &, Error &);
// Читает Session с проверкой JSON-типа; при несовместимом поле возвращает false и Error.
bool fromJson(const JsonValue &, Session &, Error &);
// Читает Output с проверкой JSON-типа; при несовместимом поле возвращает false и Error.
bool fromJson(const JsonValue &, Output &, Error &);
// Читает Variant с проверкой JSON-типа; при несовместимом поле возвращает false и Error.
bool fromJson(const JsonValue &, Variant &, Error &);
// Читает Health с проверкой JSON-типа; при несовместимом поле возвращает false и Error.
bool fromJson(const JsonValue &, Health &, Error &);
// Читает Capability с проверкой JSON-типа; при несовместимом поле возвращает false и Error.
bool fromJson(const JsonValue &, Capability &, Error &);
// Читает Capabilities с проверкой JSON-типа; при несовместимом поле возвращает false и Error.
bool fromJson(const JsonValue &, Capabilities &, Error &);
// Читает PolicyResult с проверкой JSON-типа; при несовместимом поле возвращает false и Error.
bool fromJson(const JsonValue &, PolicyResult &, Error &);
// Читает OutputDetail с проверкой JSON-типа; при несовместимом поле возвращает false и Error.
bool fromJson(const JsonValue &, OutputDetail &, Error &);
// Читает Problem с проверкой JSON-типа; при несовместимом поле возвращает false и Error.
bool fromJson(const JsonValue &, Problem &, Error &);
// Читает только location/message ошибки поля; серверное value намеренно игнорируется.
// Читает FieldError с проверкой JSON-типа; при несовместимом поле возвращает false и Error.
bool fromJson(const JsonValue &, FieldError &, Error &);
// Читает Preparation с проверкой JSON-типа; при несовместимом поле возвращает false и Error.
bool fromJson(const JsonValue &, Preparation &, Error &);
// Различает отсутствующее поле, null и значение, не материализуя значения по умолчанию.
template<class T> bool fromJson(const JsonValue &json, JsonField<T> &value, Error &error)
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
template<class T> bool fromJson(const JsonValue &json, QVector<T> &value, Error &error)
{
	if (!json.isArray()) {
		error = makeError(ErrorKind::Decode);
		return false;
	}
	// array заимствует проверенный массив; entry задаёт очередной элемент.
	const auto &array = json.toArray();
	value.clear();
	value.reserve(array.size());
	for (const auto &entry : array) {
		// item не выдаётся вызывающей стороне до полного чтения элемента.
		T item{};
		if (!fromJson(entry, item, error))
			return false;
		value.push_back(std::move(item));
	}
	return true;
}
// Читает QMap<QString, quint64> с проверкой JSON-типа; при несовместимом поле возвращает false и Error.
bool fromJson(const JsonValue &, QMap<QString, quint64> &, Error &);
// Читает массив элементов и непрозрачный курсор страницы; ошибка элемента отвергает страницу.
template<class T> bool fromJson(const JsonValue &json, Page<T> &value, Error &error)
{
	if (!json.isObject()) {
		error = makeError(ErrorKind::Decode);
		return false;
	}
	// object заимствует проверенную оболочку страницы.
	const auto &object = json.toObject();
	return fromJson(object.value(QStringLiteral("items")), value.items, error) &&
	       fromJson(object.value(QStringLiteral("next_cursor")), value.nextCursor, error);
}
// Требует корректный UTF-8 и корневой JSON-объект. Не ограничивает размер body самостоятельно:
// транспорт/вызывающая сторона обязаны применить свой бюджет до разбора.
template<class T> Result<T> decode(const QByteArray &body)
{
	// error содержит исходную Decode-классификацию до уточнения конкретной ошибкой поля.
	Error error = makeError(ErrorKind::Decode);
	// document содержит типизированное дерево с исходными числовыми лексемами.
	JsonValue document;
	if (!parseJson(body, document) || !document.isObject())
		return Result<T>::failure(error);
	// value заполняется локально, чтобы частичная ошибка не публиковала неполный DTO.
	T value{};
	if (!fromJson(document, value, error))
		return Result<T>::failure(error);
	return Result<T>::success(std::move(value));
}
// Проверяет сочетания kind/owner, идентичностей и блока данных, но не существование родителей и порядок ревизий.
bool validateRuntimeEvent(const RuntimeEvent &event);
// Сохраняет исходный HTTP-статус даже при неверном Problem; прикладывает тело только при совпадении status.
Error decodeProblem(int status, const QByteArray &body);
} // namespace gost::api
