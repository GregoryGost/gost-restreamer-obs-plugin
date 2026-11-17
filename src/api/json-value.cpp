#include "api/json-value.hpp"
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonParseError>
#include <algorithm>
#include <limits>

namespace gost::api {
namespace {
// Документ ограничен транспортными бюджетами; дополнительный общий предел защищает прямые вызовы.
constexpr qsizetype maxDocumentBytes = 64 * 1024 * 1024;
// Глубина ограничивает стек parser/writer независимо от числа структурных элементов.
constexpr int maxDepth = 128;
// Число значений ограничивает память дерева для документов из коротких токенов.
constexpr qsizetype maxValues = 1000000;

/** Проверяет ASCII-цифру JSON без локализованного распознавания Unicode. */
bool digit(char c)
{
	return c >= '0' && c <= '9';
}

/** Проверяет целую лексему по грамматике JSON number без числовых преобразований. */
bool validNumber(const QByteArray &text)
{
	// i последовательно проходит знак, целую часть, дробь и экспоненту.
	qsizetype i = 0;
	if (i < text.size() && text[i] == '-')
		++i;
	if (i == text.size())
		return false;
	if (text[i] == '0')
		++i;
	else {
		if (text[i] < '1' || text[i] > '9')
			return false;
		while (i < text.size() && digit(text[i]))
			++i;
	}
	if (i < text.size() && text[i] == '.') {
		// start отличает непустую дробь от точки без цифр.
		const auto start = ++i;
		while (i < text.size() && digit(text[i]))
			++i;
		if (i == start)
			return false;
	}
	if (i < text.size() && (text[i] == 'e' || text[i] == 'E')) {
		++i;
		if (i < text.size() && (text[i] == '+' || text[i] == '-'))
			++i;
		// start отмечает начало обязательных цифр экспоненты.
		const auto start = i;
		while (i < text.size() && digit(text[i]))
			++i;
		if (i == start)
			return false;
	}
	return i == text.size();
}

/** Проверяет пары UTF-16, чтобы строка могла вернуться на провод без замены суррогатов. */
bool validString(const QString &text)
{
	// i указывает текущую кодовую единицу; пара суррогатов обрабатывается вместе.
	for (qsizetype i = 0; i < text.size(); ++i) {
		if (text[i].isHighSurrogate()) {
			if (++i >= text.size() || !text[i].isLowSurrogate())
				return false;
		} else if (text[i].isLowSurrogate()) {
			return false;
		}
	}
	return true;
}

/** Читает четыре ASCII hex-цифры escape-последовательности без изменения исходного токена. */
bool hexUnit(const QByteArray &token, qsizetype start, ushort &value)
{
	if (start + 4 > token.size())
		return false;
	value = 0;
	// i указывает очередную hex-цифру, c содержит её ASCII-код, part — значение этой цифры.
	for (qsizetype i = start; i < start + 4; ++i) {
		const char c = token[i];
		const int part = c >= '0' && c <= '9'   ? c - '0'
				 : c >= 'a' && c <= 'f' ? c - 'a' + 10
				 : c >= 'A' && c <= 'F' ? c - 'A' + 10
							: -1;
		if (part < 0)
			return false;
		value = ushort(value * 16 + part);
	}
	return true;
}

/** Отклоняет изолированные Unicode-суррогаты до декодирования строкового токена средствами Qt. */
bool validEscapedString(const QByteArray &token)
{
	// i проходит токен между кавычками; удвоенный обратный слеш не начинает Unicode escape.
	for (qsizetype i = 1; i < token.size() - 1; ++i) {
		if (token[i] != '\\')
			continue;
		if (++i >= token.size() - 1)
			return false;
		if (token[i] != 'u')
			continue;
		// unit содержит одну кодовую единицу UTF-16 из escape-последовательности.
		ushort unit = 0;
		if (!hexUnit(token, i + 1, unit))
			return false;
		i += 4;
		if (QChar::isLowSurrogate(unit))
			return false;
		if (QChar::isHighSurrogate(unit)) {
			if (i + 6 >= token.size() || token[i + 1] != '\\' || token[i + 2] != 'u' ||
			    !hexUnit(token, i + 3, unit) || !QChar::isLowSurrogate(unit))
				return false;
			i += 6;
		}
	}
	return true;
}

/** Извлекает модуль точного целого с десятичным сдвигом places и заданным пределом. */
bool integerMagnitude(const QByteArray &text, int places, quint64 limit, quint64 &value)
{
	if (!validNumber(text) || places < 0 || places > 18)
		return false;
	// i пропускает знак, coefficient собирает цифры без десятичной точки.
	qsizetype i = text.startsWith('-') ? 1 : 0;
	QByteArray coefficient;
	// fraction считает цифры после точки; dotted отмечает начало дробной части.
	qint64 fraction = 0;
	bool dotted = false;
	while (i < text.size() && text[i] != 'e' && text[i] != 'E') {
		if (text[i] == '.')
			dotted = true;
		else {
			coefficient.append(text[i]);
			if (dotted)
				++fraction;
		}
		++i;
	}
	// exponent имеет ограниченный модуль: больший порядок нельзя компенсировать длиной коэффициента.
	qint64 exponent = 0;
	if (i < text.size()) {
		++i;
		// negativeExponent определяет направление сдвига; cap превышает возможную длину коэффициента.
		const bool negativeExponent = text[i] == '-';
		const qint64 cap = text.size() + 64;
		if (text[i] == '+' || text[i] == '-')
			++i;
		for (; i < text.size(); ++i)
			if (exponent < cap)
				exponent = std::min(cap, exponent * 10 + (text[i] - '0'));
		if (negativeExponent)
			exponent = -exponent;
	}
	// first отделяет нулевой коэффициент и ведущие нули без создания огромной степени десяти.
	qsizetype first = 0;
	while (first < coefficient.size() && coefficient[first] == '0')
		++first;
	if (first == coefficient.size()) {
		value = 0;
		return true;
	}
	// shift задаёт число добавляемых нулей; last исключает делимые десятичные нули коэффициента.
	qint64 shift = exponent - fraction + places;
	qsizetype last = coefficient.size();
	while (last > first && coefficient[last - 1] == '0') {
		--last;
		++shift;
	}
	if (shift < 0 || qint64(last - first) + shift > 20)
		return false;
	// result накапливает не более двадцати цифр с проверкой перед каждой операцией.
	quint64 result = 0;
	for (i = first; i < last; ++i) {
		// next является одной значащей десятичной цифрой.
		const quint64 next = coefficient[i] - '0';
		if (next > limit || result > (limit - next) / 10)
			return false;
		result = result * 10 + next;
	}
	while (shift-- > 0) {
		if (result > limit / 10)
			return false;
		result *= 10;
	}
	value = result;
	return true;
}

/** Читает один ограниченный документ; рекурсия допускается только до maxDepth. */
class Parser {
public:
	/** Заимствует неизменяемое тело на время синхронного разбора. */
	explicit Parser(const QByteArray &bytes) : bytes_(bytes) {}
	/** Публикует дерево только после последнего токена и разрешённых пробелов. */
	bool parse(JsonValue &value)
	{
		// parsed удерживает незавершённое дерево до проверки конца документа.
		JsonValue parsed;
		if (!read(parsed, 0))
			return false;
		space();
		if (position_ != bytes_.size())
			return false;
		value = std::move(parsed);
		return true;
	}

private:
	/** Пропускает только четыре вида пробела JSON. */
	void space()
	{
		while (position_ < bytes_.size() && (bytes_[position_] == ' ' || bytes_[position_] == '\t' ||
						     bytes_[position_] == '\r' || bytes_[position_] == '\n'))
			++position_;
	}
	/** Потребляет ожидаемый символ после пробелов. */
	bool take(char expected)
	{
		space();
		if (position_ >= bytes_.size() || bytes_[position_] != expected)
			return false;
		++position_;
		return true;
	}
	/** Декодирует отдельный строковый токен Qt и проверяет сохранность Unicode. */
	bool string(QString &value)
	{
		if (!take('"'))
			return false;
		// start включает открывающую кавычку; escaped различает кавычку и escape-последовательность.
		const auto start = position_ - 1;
		bool escaped = false;
		while (position_ < bytes_.size()) {
			// c является следующим байтом строки, включая закрывающую кавычку.
			const char c = bytes_[position_++];
			if (!escaped && c == '"') {
				// token сохраняет целую строку для проверки escape-пар до преобразований Qt.
				const auto token = bytes_.mid(start, position_ - start);
				if (!validEscapedString(token))
					return false;
				// wrapper позволяет Qt декодировать только строку, не касаясь числовых токенов документа.
				const auto wrapper = QByteArray("[") + token + ']';
				// error фиксирует синтаксический отказ Qt; decoded содержит только декодированную строку.
				QJsonParseError error;
				const auto decoded = QJsonDocument::fromJson(wrapper, &error);
				if (error.error != QJsonParseError::NoError || !decoded.isArray() ||
				    decoded.array().size() != 1 || !decoded.array()[0].isString())
					return false;
				value = decoded.array()[0].toString();
				return validString(value);
			}
			if (!escaped && c == '\\')
				escaped = true;
			else
				escaped = false;
		}
		return false;
	}
	/** Читает типизированное значение, ограничивая глубину и суммарное число узлов. */
	bool read(JsonValue &value, int depth)
	{
		if (depth > maxDepth || ++values_ > maxValues)
			return false;
		space();
		if (position_ == bytes_.size())
			return false;
		// c выбирает единственную ветвь грамматики по первому байту токена.
		const char c = bytes_[position_];
		if (c == '{') {
			++position_;
			// object собирает проверенные пары; повторное имя отвергается как неоднозначный документ.
			JsonValue::Object object;
			if (!take('}')) {
				do {
					// key и item содержат одну полностью прочитанную пару объекта.
					QString key;
					JsonValue item;
					if (!string(key) || object.contains(key) || !take(':') ||
					    !read(item, depth + 1))
						return false;
					object.insert(std::move(key), std::move(item));
				} while (take(','));
				if (!take('}'))
					return false;
			}
			value = JsonValue(std::move(object));
			return true;
		}
		if (c == '[') {
			++position_;
			// array накапливает элементы в исходном порядке.
			JsonValue::Array array;
			if (!take(']')) {
				do {
					// item публикуется в массиве только после успешного чтения.
					JsonValue item;
					if (!read(item, depth + 1))
						return false;
					array.append(std::move(item));
				} while (take(','));
				if (!take(']'))
					return false;
			}
			value = JsonValue(std::move(array));
			return true;
		}
		if (c == '"') {
			// text сохраняет декодированную строку без числовой интерпретации.
			QString text;
			if (!string(text))
				return false;
			value = JsonValue(std::move(text));
			return true;
		}
		// start задаёт границу примитивного токена без копирования остального тела.
		const auto start = position_;
		while (position_ < bytes_.size() && bytes_[position_] != ',' && bytes_[position_] != ']' &&
		       bytes_[position_] != '}' && bytes_[position_] != ' ' && bytes_[position_] != '\t' &&
		       bytes_[position_] != '\r' && bytes_[position_] != '\n')
			++position_;
		// token является целым литералом либо числовой лексемой.
		const auto token = bytes_.mid(start, position_ - start);
		if (token == "null")
			value = JsonValue(nullptr);
		else if (token == "true" || token == "false")
			value = JsonValue(token == "true");
		else {
			value = JsonValue::number(token);
			if (value.isUndefined())
				return false;
		}
		return true;
	}
	// bytes_ заимствуется только пока parseJson держит исходное тело.
	const QByteArray &bytes_;
	// position_ указывает первый ещё не обработанный байт.
	qsizetype position_ = 0;
	// values_ ограничивает число созданных значений, включая контейнеры.
	qsizetype values_ = 0;
};

/** Экранирует одну Unicode-строку средствами Qt без преобразования чисел. */
QByteArray quote(const QString &value)
{
	// array нужен только для получения строкового токена вместе с кавычками.
	QJsonArray array;
	array.append(value);
	// bytes содержит массив из одной строки; внешние скобки не принадлежат строковому токену.
	const auto bytes = QJsonDocument(array).toJson(QJsonDocument::Compact);
	return bytes.mid(1, bytes.size() - 2);
}

/** Пишет дерево с ограничениями глубины, узлов и общего размера; Undefined не является JSON. */
bool write(const JsonValue &value, QByteArray &bytes, int depth, qsizetype &values)
{
	if (depth > maxDepth || ++values > maxValues || bytes.size() > maxDocumentBytes)
		return false;
	switch (value.type()) {
	case JsonValue::Type::Undefined:
		return false;
	case JsonValue::Type::Null:
		bytes.append("null");
		break;
	case JsonValue::Type::Bool:
		bytes.append(value.toBool() ? "true" : "false");
		break;
	case JsonValue::Type::String:
		if (!validString(value.toString()) || value.toString().size() > maxDocumentBytes)
			return false;
		bytes.append(quote(value.toString()));
		break;
	case JsonValue::Type::Number:
		if (!validNumber(value.numberLexeme()) || value.numberLexeme().size() > maxDocumentBytes)
			return false;
		bytes.append(value.numberLexeme());
		break;
	case JsonValue::Type::Object: {
		bytes.append('{');
		// first управляет разделителями; it обходит именованные значения объекта.
		bool first = true;
		for (auto it = value.toObject().cbegin(); it != value.toObject().cend(); ++it) {
			if (!first)
				bytes.append(',');
			first = false;
			if (!validString(it.key()) || it.key().size() > maxDocumentBytes)
				return false;
			bytes.append(quote(it.key()));
			bytes.append(':');
			if (!write(it.value(), bytes, depth + 1, values))
				return false;
		}
		bytes.append('}');
		break;
	}
	case JsonValue::Type::Array: {
		bytes.append('[');
		// first управляет разделителями элементов, item заимствует очередное значение массива.
		bool first = true;
		for (const auto &item : value.toArray()) {
			if (!first)
				bytes.append(',');
			first = false;
			if (!write(item, bytes, depth + 1, values))
				return false;
		}
		bytes.append(']');
		break;
	}
	}
	return bytes.size() <= maxDocumentBytes;
}
} // namespace

/** Создаёт числовое значение только из корректного JSON-токена без преобразования его записи. */
JsonValue JsonValue::number(QByteArray lexeme)
{
	// value остаётся Undefined, если переданный токен не соответствует грамматике JSON.
	JsonValue value;
	if (lexeme.size() <= maxDocumentBytes && validNumber(lexeme)) {
		value.type_ = Type::Number;
		value.number_ = std::move(lexeme);
	}
	return value;
}
/** Возвращает точное U64 и запрещает любой исходный минус. */
bool JsonValue::toUnsigned(quint64 &value) const
{
	return isNumber() && !number_.startsWith('-') &&
	       integerMagnitude(number_, 0, std::numeric_limits<quint64>::max(), value);
}
/** Возвращает точное знаковое I64 с нормализацией -0 в ноль. */
bool JsonValue::toSigned(qint64 &value) const
{
	return toScaledInteger(0, value);
}
/** Масштабирует десятичное число без округления и меняет value только при точном попадании в I64. */
bool JsonValue::toScaledInteger(int places, qint64 &value) const
{
	if (!isNumber())
		return false;
	// negative определяет знак результата; magnitude допускает модуль I64.min только для минуса.
	const bool negative = number_.startsWith('-');
	quint64 magnitude = 0;
	if (!integerMagnitude(number_, places, quint64(std::numeric_limits<qint64>::max()) + quint64(negative),
			      magnitude))
		return false;
	if (negative && magnitude == quint64(std::numeric_limits<qint64>::max()) + 1)
		value = std::numeric_limits<qint64>::min();
	else
		value = negative ? -qint64(magnitude) : qint64(magnitude);
	return true;
}
/** Проверяет размер и UTF-8 тела, затем разбирает один полный ограниченный документ. */
bool parseJson(const QByteArray &bytes, JsonValue &value)
{
	if (bytes.size() > maxDocumentBytes || !bytes.isValidUtf8())
		return false;
	return Parser(bytes).parse(value);
}
/** Возвращает одно ограниченное JSON-тело либо пустой результат при неподходящем дереве. */
QByteArray serializeJson(const JsonValue &value)
{
	// bytes накапливает единственный результат, values ограничивает размер структуры.
	QByteArray bytes;
	qsizetype values = 0;
	if (!write(value, bytes, 0, values))
		return {};
	return bytes;
}
} // namespace gost::api
