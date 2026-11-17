#pragma once

#include "models.hpp"

namespace gost::api::profile_math {

// Сравнивает a*b и c*d точно во всём диапазоне uint64 без расширений компилятора.
bool productLessEqual(quint64 a, quint64 b, quint64 c, quint64 d);
// Проверяет положительный FPS, int32-шкалы видео/аудио и точность серверного критерия FFmpeg av_d2q.
bool validFrameRate(const FrameRate &rate, quint32 sampleRate);

} // namespace gost::api::profile_math
