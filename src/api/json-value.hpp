#pragma once

#include <QByteArray>
#include <QMap>
#include <QString>
#include <QVector>
#include <cstddef>
#include <type_traits>
#include <utility>

namespace gost::api {
/** Значение JSON сохраняет тип и исходную числовую лексему без преобразования через double. */
class JsonValue {
public:
	// Семь различимых состояний включают отсутствие поля вне JSON-документа.
	enum class Type { Undefined, Null, Bool, String, Number, Object, Array };
	// Object содержит значения по именам.
	using Object = QMap<QString, JsonValue>;
	// Array сохраняет порядок элементов.
	using Array = QVector<JsonValue>;
	/** Создаёт отсутствующее значение. */
	JsonValue() = default;
	/** Создаёт явный JSON null. */
	JsonValue(std::nullptr_t) : type_(Type::Null) {}
	/** Создаёт логическое значение, не число. */
	JsonValue(bool value) : type_(Type::Bool), boolean_(value) {}
	/** Сохраняет строку отдельно от числовой лексемы. */
	JsonValue(QString value) : type_(Type::String), string_(std::move(value)) {}
	/** Создаёт объект со значениями по именам. */
	JsonValue(Object value) : type_(Type::Object), object_(std::move(value)) {}
	/** Создаёт массив с исходным порядком элементов. */
	JsonValue(Array value) : type_(Type::Array), array_(std::move(value)) {}
	/** Записывает целое число в десятичной форме, сохраняя весь диапазон типа. */
	template<class T>
		requires(std::is_integral_v<T> && !std::is_same_v<T, bool>)
	JsonValue(T value) : type_(Type::Number)
	{
		if constexpr (std::is_signed_v<T>)
			number_ = QByteArray::number(qint64(value));
		else
			number_ = QByteArray::number(quint64(value));
	}
	/** Принимает только целую корректную числовую лексему JSON; иначе возвращает Undefined. */
	static JsonValue number(QByteArray lexeme);
	/** Возвращает сохранённый тип значения. */
	Type type() const { return type_; }
	/** Проверяет отсутствие поля. */
	bool isUndefined() const { return type_ == Type::Undefined; }
	/** Проверяет явный null. */
	bool isNull() const { return type_ == Type::Null; }
	/** Проверяет логический тип. */
	bool isBool() const { return type_ == Type::Bool; }
	/** Проверяет строковый тип без интерпретации содержимого. */
	bool isString() const { return type_ == Type::String; }
	/** Проверяет числовой тип без округления. */
	bool isNumber() const { return type_ == Type::Number; }
	/** Проверяет тип объекта. */
	bool isObject() const { return type_ == Type::Object; }
	/** Проверяет тип массива. */
	bool isArray() const { return type_ == Type::Array; }
	/** Возвращает логическое содержимое; вызывающая сторона проверяет тип. */
	bool toBool() const { return boolean_; }
	/** Возвращает строковое содержимое; вызывающая сторона проверяет тип. */
	const QString &toString() const { return string_; }
	/** Возвращает объект без создания второго дерева. */
	const Object &toObject() const { return object_; }
	/** Возвращает массив без создания второго дерева. */
	const Array &toArray() const { return array_; }
	/** Возвращает исходную числовую лексему. */
	const QByteArray &numberLexeme() const { return number_; }
	/** Читает математически целое U64; любой знак минус, в том числе -0, недопустим. */
	bool toUnsigned(quint64 &value) const;
	/** Читает математически целое I64; -0 становится нулём. */
	bool toSigned(qint64 &value) const;
	/** Умножает число на 10^places без округления и возвращает точное I64. */
	bool toScaledInteger(int places, qint64 &value) const;
	/** Сравнивает типы и сохранённые значения, включая числовые лексемы. */
	bool operator==(const JsonValue &) const = default;

private:
	// type_ определяет активное содержимое; остальные поля не изменяют тип значения.
	Type type_ = Type::Undefined;
	// boolean_ содержит логическое значение только для Bool.
	bool boolean_ = false;
	// string_ хранит декодированную Unicode-строку.
	QString string_;
	// number_ хранит синтаксически проверенный числовой токен.
	QByteArray number_;
	// object_ содержит дочерние значения объекта с независимым временем жизни.
	Object object_;
	// array_ содержит дочерние значения массива с независимым временем жизни.
	Array array_;
};

/** Разбирает UTF-8 JSON с пределом 64 MiB, глубины 128 и миллиона значений; при ошибке не меняет value. */
bool parseJson(const QByteArray &bytes, JsonValue &value);
/** Сериализует типизированное дерево с теми же ограничениями; при ошибке возвращает пустые байты. */
QByteArray serializeJson(const JsonValue &value);
} // namespace gost::api
