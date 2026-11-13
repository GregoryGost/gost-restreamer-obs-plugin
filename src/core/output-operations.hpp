#pragma once

#include "core/operations-state.hpp"

namespace gost {

// Формирует план Output; отправка, квитанции и таймеры принадлежат одному RestreamController.
class OutputOperations final {
public:
	// Один неизменяемый шаг; request_id назначается исполнителем перед первой отправкой.
	struct Step {
		// Команда одного Output: start, stop либо detach.
		OperationAction action = OperationAction::None;
		// outputId адресует точную сохранённую цель исходного пакета.
		QString outputId;
		// resultIndex связывает stop/start шаги с одним публичным итогом элемента.
		qsizetype resultIndex = 0;
	};
	// Проверяет полный свежий снимок, точный состав и допустимость действия; false не отправляет POST.
	static bool makePlan(OperationAction action, const QStringList &outputIds, const DockServerState &snapshot,
			     QVector<Step> &steps, QVector<OutputOperationResult> &results);
};

} // namespace gost
