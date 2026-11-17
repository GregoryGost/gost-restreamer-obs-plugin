#include "profile-arithmetic.hpp"

#include <array>
#include <algorithm>
#include <cmath>
#include <limits>
#include <numeric>

namespace gost::api::profile_math {
namespace {

// Содержит беззнаковое 128-битное число четырьмя 32-битными словами от младшего к старшему.
// Произведение двух uint64 помещается точно; умножение разности FPS проверяет переполнение отдельно.
struct Wide {
	// words содержит основание 2^32 без зависимости от порядка байтов платформы.
	std::array<quint32, 4> words{};
};

// Возвращает точное произведение a и b; каждый промежуточный шаг не превышает uint64.max.
Wide multiply(quint64 a, quint64 b)
{
	// result накапливает четыре слова произведения.
	Wide result;
	// left и right содержат по два слова исходных множителей.
	const std::array<quint32, 2> left{quint32(a), quint32(a >> 32)};
	const std::array<quint32, 2> right{quint32(b), quint32(b >> 32)};
	// i выбирает слово первого множителя.
	for (size_t i = 0; i < 2; ++i) {
		// carry переносит старшие биты текущего столбца.
		quint64 carry = 0;
		// j выбирает слово второго множителя.
		for (size_t j = 0; j < 2; ++j) {
			// part объединяет произведение слов, накопленное слово и перенос.
			const quint64 part = quint64(left[i]) * right[j] + result.words[i + j] + carry;
			result.words[i + j] = quint32(part);
			carry = part >> 32;
		}
		result.words[i + 2] = quint32(carry);
	}
	return result;
}

// Возвращает знак сравнения a и b, начиная со старшего слова.
int compare(const Wide &a, const Wide &b)
{
	// i указывает очередное слово в порядке убывания значимости.
	for (size_t i = 4; i-- > 0;) {
		if (a.words[i] != b.words[i])
			return a.words[i] < b.words[i] ? -1 : 1;
	}
	return 0;
}

// Вычитает b из a при a>=b; заём переносится без знакового переполнения.
Wide subtract(const Wide &a, const Wide &b)
{
	// result содержит разность, borrow — заём из следующего слова.
	Wide result;
	quint64 borrow = 0;
	// i выбирает разряд вычитания от младшего к старшему.
	for (size_t i = 0; i < 4; ++i) {
		// subtrahend включает заём и может равняться 2^32.
		const quint64 subtrahend = quint64(b.words[i]) + borrow;
		result.words[i] = quint32(quint64(a.words[i]) - subtrahend);
		borrow = quint64(a.words[i]) < subtrahend ? 1 : 0;
	}
	return result;
}

// Умножает value на factor; false сообщает выход за 128 бит, который исключает требуемое сравнение FPS.
bool scale(Wide &value, quint32 factor)
{
	// carry содержит перенос между 32-битными словами.
	quint64 carry = 0;
	// word обозначает очередное изменяемое слово числа.
	for (auto &word : value.words) {
		// part содержит произведение одного слова и перенос.
		const quint64 part = quint64(word) * factor + carry;
		word = quint32(part);
		carry = part >> 32;
	}
	return carry == 0;
}

} // namespace

// Сравнивает оба произведения a*b и c*d без переполнения их 64-битных множителей.
bool productLessEqual(quint64 a, quint64 b, quint64 c, quint64 d)
{
	return compare(multiply(a, b), multiply(c, d)) <= 0;
}

// Проверяет точную rate со звуковой шкалой sampleRate тем же критерием av_d2q, что и сервер.
bool validFrameRate(const FrameRate &rate, quint32 sampleRate)
{
	if (!rate.numerator || !rate.denominator || !sampleRate)
		return false;
	// divisor сокращает исходную дробь; n и d сохраняют её математическое значение.
	const quint32 divisor = std::gcd(rate.numerator, rate.denominator);
	const quint32 n = rate.numerator / divisor;
	const quint32 d = rate.denominator / divisor;
	// limit задаёт предел нативных временных шкал FFmpeg.
	constexpr quint64 limit = std::numeric_limits<qint32>::max();
	if (n > limit || d > limit || quint64(n / std::gcd(n, sampleRate)) * sampleRate > limit)
		return false;
	if (n < 1000 && quint64(d) * (sampleRate / std::gcd(n, sampleRate)) > limit)
		return false;
	// x и exponent воспроизводят серверное float64-приближение, используемое av_d2q.
	const double x = double(n) / double(d);
	int exponent = 0;
	std::frexp(x, &exponent);
	exponent = std::max(exponent - 1, 0);
	// denominator и numerator задают двоичное приближение FPS до точного сравнения разности.
	const quint64 denominator = quint64(1) << (62 - exponent);
	const quint64 numerator = quint64(std::floor(x * double(denominator) + 0.5));
	// approximate и exact содержат перекрёстные произведения, не помещающиеся обязательно в uint64.
	const Wide approximate = multiply(numerator, d);
	const Wide exact = multiply(n, denominator);
	// difference содержит абсолютную погрешность; после умножения она занимает не более 126 бит.
	Wide difference = compare(approximate, exact) >= 0 ? subtract(approximate, exact)
							   : subtract(exact, approximate);
	return scale(difference, quint32(2 * limit)) && compare(difference, multiply(denominator, 1)) < 0;
}

} // namespace gost::api::profile_math
