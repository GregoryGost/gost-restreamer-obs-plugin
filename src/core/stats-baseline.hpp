#pragma once

#include <QString>

#include <optional>

namespace gost {

/**
 * Вычисляет видимые накопления относительно локальной точки отсчёта, не меняя исходный источник.
 * Один объект обслуживает один счётчик либо пару count/total одного этапа. Identity задаёт исполнение
 * и эпоху непрерывности; новый источник никогда не наследует прежний baseline.
 *
 * До сброса показывает текущие накопления. reset фиксирует одновременно числитель и знаменатель.
 * Регресс, смена наличия знаменателя или насыщение начинают новый период. Несвежие данные того же
 * источника сохраняют последнее чтение, но не дают пользователю сбросить неизвестное текущее значение.
 * Класс не синхронизирован; UI последовательно передаёт наблюдения и читает результат.
 */
class StatsBaseline {
public:
	struct Identity {
		QString instance;
		quint64 epoch = 0;
		bool operator==(const Identity &) const = default;
	};
	struct Counters {
		quint64 count = 0;
		// Отсутствующий знаменатель допускает отображение количества, но исключает вычисление процента.
		std::optional<quint64> total;
	};
	// SinceNewSample обозначает автоматическую границу после разрыва/предела, а не пользовательский сброс.
	enum class Period { CurrentRun, SinceReset, SinceNewSample };
	struct Reading {
		std::optional<Counters> counters;
		std::optional<double> percent;
		Period period = Period::CurrentRun;
		bool limited = false;
	};

	// Несвежая выборка того же источника сохраняет последнее значение, но запрещает сброс.
	// Смена identity отбрасывает старые данные даже при отсутствии новой свежей выборки.
	// При fresh=true отсутствие counters делает чтение недоступным; limited запрещает точный прирост
	// и удаляет baseline. Следующая пригодная выборка становится началом нового периода.
	void observe(Identity identity, std::optional<Counters> counters, bool fresh, bool limited = false);
	bool canReset() const noexcept;
	// Без canReset не действует. После сброса доступные дельты равны нулю до следующего наблюдения.
	void reset();
	// Возвращает копию результата. percent отсутствует без положительного общего знаменателя
	// или при count>total; отсутствие процента не превращает доступный count в недоступный.
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
