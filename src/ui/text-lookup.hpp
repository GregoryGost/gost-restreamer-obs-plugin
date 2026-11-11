#pragma once

#include <QString>

#include <functional>

namespace gost {

// Синхронно возвращает собственный QString по точному ключу локализации. key заимствован лишь на
// время вызова; реализация не должна хранить указатель на временный UTF-8 буфер вызывающей стороны.
// UI копирует функцию владельца плагина; она должна оставаться вызываемой до уничтожения виджетов.
using TextLookup = std::function<QString(const char *key)>;

} // namespace gost
