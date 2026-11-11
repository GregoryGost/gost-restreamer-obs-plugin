#include "stats-baseline.hpp"

#include <utility>

namespace gost {

void StatsBaseline::observe(Identity identity, std::optional<Counters> counters, bool fresh, bool limited)
{
	// Сначала проверяем identity: при новом исполнении старое показание должно исчезнуть, даже
	// если свежих данных ещё нет. Несвежая выборка прежнего исполнения сохраняет последнее число и признак предела.
	if (identity_ != identity) {
		*this = {};
		identity_ = std::move(identity);
	}
	current_ = fresh;
	if (!fresh)
		return;
	// После насыщения следующая достоверная выборка станет новой точкой отсчёта, а не продолжением старого периода.
	if (limited) {
		previous_.reset();
		baseline_.reset();
		period_ = Period::SinceNewSample;
	}
	limited_ = limited;
	available_ = counters.has_value() && !limited;
	if (!available_)
		return;
	// Регресс любого счётчика или изменение наличия знаменателя разрывает общий период пары.
	if (previous_ && (counters->count < previous_->count || counters->total < previous_->total ||
			  counters->total.has_value() != previous_->total.has_value())) {
		baseline_ = counters;
		period_ = Period::SinceNewSample;
	}
	if (!baseline_ && period_ == Period::SinceNewSample)
		baseline_ = counters;
	previous_ = counters;
}

bool StatsBaseline::canReset() const noexcept
{
	return current_ && available_;
}

void StatsBaseline::reset()
{
	if (!canReset())
		return;
	baseline_ = previous_;
	period_ = Period::SinceReset;
}

StatsBaseline::Reading StatsBaseline::reading() const
{
	Reading result{.period = period_, .limited = limited_};
	if (!available_)
		return result;
	result.counters = previous_;
	auto &counters = *result.counters;
	if (baseline_) {
		// Числитель и знаменатель вычитаются за один интервал; процент от полного total здесь был бы неверным.
		counters.count -= baseline_->count;
		if (counters.total)
			*counters.total -= *baseline_->total;
	}
	if (counters.total && *counters.total > 0 && counters.count <= *counters.total)
		result.percent = 100.0 * static_cast<double>(counters.count) / static_cast<double>(*counters.total);
	return result;
}

} // namespace gost
