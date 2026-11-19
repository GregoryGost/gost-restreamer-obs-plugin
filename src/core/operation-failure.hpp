#pragma once

#include "api/models.hpp"
#include "core/dock-server-state.hpp"
#include <QMetaType>

namespace gost {

// Различает независимые поколения runtime-команд, прежнего CRUD и подготовки/назначения заглушек.
enum class OperationFailureSource {
	// Явные команды Session/Output используют поколения RestreamController.
	Runtime,
	// Прежний CRUD использует независимые поколения ConfigurationController.
	Configuration,
	// Asset, policy и собственная Generation используют поколения FallbackController.
	Fallback
};

// Однократное уведомление об отказе или отсутствии подтверждения явной записи. Диагностика передаётся
// presenter синхронно и не сохраняется в StateStore или истории строк операций.
struct OperationFailure {
	// source определяет пространство generation и stepIdentity.
	OperationFailureSource source = OperationFailureSource::Runtime;
	// context фиксирует сервер, профиль OBS и рабочий выбор; Stream может отсутствовать у CRUD.
	DockContext context;
	// generation отличает пользовательские намерения одного контроллера.
	quint64 generation = 0;
	// stepIdentity содержит request_id с номером попытки команды либо адрес конфигурационного шага.
	QString stepIdentity;
	// code сохраняет серверную классификацию без вывода поведения для неизвестных кодов.
	QString code;
	// reason выводится только как обычный текст; пустота требует локального общего пояснения.
	QString reason;
	// action содержит полученное от сервера действие, без догадок клиента.
	QString action;
	// reasonKey задаёт доверенное локальное пояснение, когда подтверждение записи не получено.
	QString reasonKey;
	// actionKey задаёт известный локальный способ сверки перед следующим сохранением.
	QString actionKey;
	// field адресует поле отказа, ids содержит проверенные декодером контекстные идентификаторы.
	QString field;
	QMap<QString, QString> ids;
	// diagnosticBody содержит ограниченное исходное тело отказа, включая присутствующие секреты.
	QByteArray diagnosticBody;
};

// Возвращает наличие HTTP-отказа, отличая его от потери ответа и ошибки успешного документа.
inline bool mutationRejected(const api::Error &error)
{
	return error.httpStatus >= 400 && error.httpStatus <= 599;
}

// Только явный машинный код разрешает сравнение ревизий; поздний GET не устанавливает причину 409.
inline bool revisionConflict(const api::Error &error)
{
	return error.httpStatus == 409 && error.problem && error.problem->code == QStringLiteral("revision_conflict");
}

// Копирует структурированный отказ и краткоживущую диагностику; адрес и поколение заполняет владелец операции.
inline OperationFailure operationFailure(const api::Error &error)
{
	// failure передаёт только серверные данные; общее локализованное пояснение выбирает presenter.
	OperationFailure failure;
	failure.diagnosticBody = error.diagnosticBody;
	if (!error.problem)
		return failure;
	failure.code = error.problem->code;
	failure.reason = error.problem->detail.isEmpty() ? error.problem->title : error.problem->detail;
	failure.action = error.problem->action;
	failure.field = error.problem->field;
	failure.ids = error.problem->context;
	// issue дополняет причину деталями полей, которые больше не дублируются в редакторе.
	for (const auto &issue : error.problem->errors) {
		if (issue.message.isEmpty())
			continue;
		if (!failure.reason.isEmpty())
			failure.reason += QLatin1Char('\n');
		failure.reason += issue.location.isEmpty() ? issue.message
							   : issue.location + QStringLiteral(": ") + issue.message;
	}
	return failure;
}

} // namespace gost

Q_DECLARE_METATYPE(gost::OperationFailure)
