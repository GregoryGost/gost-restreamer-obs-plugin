#pragma once

#include <QString>

#include <optional>

namespace gost {

class StatsBaseline {
public:
	struct Identity {
		QString instance;
		quint64 epoch = 0;
		bool operator==(const Identity &) const = default;
	};
	struct Counters {
		quint64 count = 0;
		std::optional<quint64> total;
	};
	enum class Period { CurrentRun, SinceReset, SinceNewSample };
	struct Reading {
		std::optional<Counters> counters;
		std::optional<double> percent;
		Period period = Period::CurrentRun;
		bool limited = false;
	};

	void observe(Identity identity, std::optional<Counters> counters, bool fresh, bool limited = false);
	bool canReset() const noexcept;
	void reset();
	Reading reading() const;

private:
	std::optional<Identity> identity_;
	std::optional<Counters> previous_;
	std::optional<Counters> baseline_;
	Period period_ = Period::CurrentRun;
	bool current_ = false;
	bool available_ = false;
	bool limited_ = false;
};

} // namespace gost
