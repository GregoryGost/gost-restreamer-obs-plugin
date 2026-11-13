#include "output-operations.hpp"

#include <QSet>

namespace gost {

// Проверяет снимок и готовит план целиком до публикации: отказ не оставляет частичной очереди.
bool OutputOperations::makePlan(OperationAction action, const QStringList &outputIds, const DockServerState &snapshot,
				QVector<Step> &steps, QVector<OutputOperationResult> &results)
{
	if (snapshot.outputsStatus.availability != RuntimeAvailability::Fresh || !snapshot.outputsStatus.complete ||
	    outputIds.isEmpty() || outputIds.size() > 10000)
		return false;
	// nextSteps и nextResults сохраняют локальный кандидат до успешной проверки всех IDs.
	QVector<Step> nextSteps;
	QVector<OutputOperationResult> nextResults;
	// seen запрещает дубли одного Output в фиксированном пакете.
	QSet<QString> seen;
	// id адресует очередной Output исходного состава, а не текущую строку UI.
	for (const auto &id : outputIds) {
		// found указывает только в неизменяемый снимок в пределах этого синхронного вызова.
		const auto found = snapshot.outputs.constFind(id);
		if (id.isEmpty() || seen.contains(id) || found == snapshot.outputs.cend() || !found->attached)
			return false;
		seen.insert(id);
		// index связывает оба шага restart с одним результатом элемента.
		const auto index = nextResults.size();
		if ((action == OperationAction::StartOutput || action == OperationAction::StartStopped) &&
		    found->desiredRunning)
			return false;
		nextResults.append({id, found->name, OperationOutcome::NotSent});
		switch (action) {
		case OperationAction::StartOutput:
		case OperationAction::StartStopped:
			nextSteps.append({OperationAction::StartOutput, id, index});
			break;
		case OperationAction::StopOutput:
			nextSteps.append({OperationAction::StopOutput, id, index});
			break;
		case OperationAction::DetachOutput:
			nextSteps.append({OperationAction::DetachOutput, id, index});
			break;
		case OperationAction::RestartOutput:
		case OperationAction::RestartAll:
			nextSteps.append({OperationAction::StopOutput, id, index});
			nextSteps.append({OperationAction::StartOutput, id, index});
			break;
		default:
			return false;
		}
	}
	steps = std::move(nextSteps);
	results = std::move(nextResults);
	return true;
}

} // namespace gost
