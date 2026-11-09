#pragma once

#include <QString>

#include <functional>

namespace gost {

using TextLookup = std::function<QString(const char *key)>;

} // namespace gost
