#pragma once

#include "core/configuration-state.hpp"
#include "core/dock-server-state.hpp"
#include <optional>

namespace gost {

// Итог передачи записи независим от завершения серверной подготовки.
enum class FallbackWriteOutcome {
	// Пользователь ещё не отправлял намерение.
	None,
	// Намерение принято локально и ожидает HTTP результата.
	Pending,
	// Конфигурационная запись подтверждена 201/200; это не завершение подготовки.
	Saved,
	// Удаление подтверждено 204.
	Deleted,
	// Подготовка принята проверенным 202; её Generation наблюдается отдельно.
	Accepted,
	// Локальная валидация либо HTTP ответ достоверно отклонили запись.
	Rejected,
	// Только revision_conflict требует явного сравнения CAS.
	Conflict,
	// Запись могла исполниться, но достоверного ответа нет; требуется сверка без повтора.
	Unknown,
	// Намерение снято до передачи HTTP транспорту.
	NotSent
};
// Наблюдение может закончиться без знания результата серверного задания.
enum class FallbackObservation {
	// Собственная принятая Generation пока отсутствует.
	None,
	// Выполняются ограниченные последовательные GET точной принятой пары.
	Observing,
	// API подтвердил ready и полный набор вариантов; вывод остаётся предварительным.
	Ready,
	// Точная собственная Generation завершилась серверным error.
	Error,
	// Чтение либо его бюджет не подтвердили нынешний результат; серверный статус не меняется.
	Unknown,
	// Пользователь прекратил только местное ожидание, не отменяя backend подготовку.
	Cancelled
};

// Содержит проверенную принадлежность одного поколения и полноту его вариантов.
struct FallbackGenerationState {
	// Точная родительская сущность, зафиксированная до GET.
	QString assetId;
	// Точный ID поколения, неизменный во всём обходе страниц.
	QString generationId;
	// Серверный DTO без временного диагностического тела failure.
	std::optional<api::Generation> generation;
	// Варианты только указанной пары Asset/Generation.
	QVector<api::Variant> variants;
	// Полнота и свежесть относятся ко всему обходу вариантов этой пары.
	ReadStatus variantsStatus;
};

// Содержит RAM-форму политики одного явно адресованного Stream.
struct FallbackPolicyState {
	// Поколение цели и формы защищает подтверждение от вложенных событий.
	quint64 generation = 0;
	// Просматриваемый Stream независим от рабочего выбора Dock.
	QString streamId;
	// Подтверждённая политика и revision именно Stream; missing не означает null.
	std::optional<api::PolicyResult> base;
	// Свежая серверная версия для явного разрешения revision_conflict.
	std::optional<api::PolicyResult> serverConflict;
	// Желаемый Asset; пустая строка означает намеренное fallback:null.
	QString desiredAssetId;
	// Пользователь изменил назначение; GET не перезаписывает такой ввод.
	bool dirty = false;
	// Выполняется чтение именно policy target.
	bool loading = false;
	// Итог прежней записи не превращается в Saved после сверки.
	FallbackWriteOutcome outcome = FallbackWriteOutcome::None;
};

// Хранит каталог, ввод, точные поколения и политику без сетевого поведения.
struct FallbackState {
	// Поколение публикации защищает подтверждение выбранного Asset и ввода.
	quint64 generation = 0;
	// Логический контекст общего конфигурационного канала.
	ConfigurationContext context;
	// Известные записи каталога, индексированные непрозрачным ID.
	QMap<QString, api::Asset> assets;
	// Полнота и свежесть каталога; неполнота не доказывает отсутствие Asset.
	ReadStatus assetsStatus;
	// Выбранная карточка не определяет собственность принятого задания.
	QString selectedAssetId;
	// Адресно прочитанная база для expected_revision новых операций.
	std::optional<api::Asset> asset;
	// Свежесть адресного GET выбранного Asset.
	bool assetFresh = false;
	// Ввод имени для создания; хранится только в RAM.
	QString draftName;
	// Выбранный пользователем путь не читается до upload.
	QString draftFilePath;
	// Разрешённый тип исходного файла: image либо video.
	QString draftKind = QStringLiteral("image");
	// Опубликованное current, подтверждённое GET Asset.
	FallbackGenerationState current;
	// Отдельное серверное pending, не подменяющее current.
	FallbackGenerationState pending;
	// Собственная принятая пара сохраняется независимо от нынешних current/pending/карточки.
	QString acceptedAssetId;
	// Точный ID из проверенного Preparation202; не выводится из нового pending.
	QString acceptedGenerationId;
	// Terminal error этой собственной пары уже передан presenter; парковка не создаёт повторного уведомления.
	bool acceptedFailureNotified = false;
	// Детали собственного принятого поколения могут оказаться историческими.
	FallbackGenerationState accepted;
	// Результат последнего пользовательского намерения записи.
	FallbackWriteOutcome outcome = FallbackWriteOutcome::None;
	// Выполняется mutation, а не длительный polling после принятого HTTP 202.
	bool writing = false;
	// Есть несверенное обязательство текущего origin, переживающее навигацию.
	bool reconciliationRequired = false;
	// Загружается каталог либо выбранная карточка.
	bool loading = false;
	// Известные переданные байты; 100% не означает принятия или готовности.
	qint64 bytesSent = 0;
	// Захваченный размер файла, либо ноль до начала передачи.
	qint64 totalBytes = 0;
	// Независимый результат локального наблюдения принятого поколения.
	FallbackObservation observation = FallbackObservation::None;
	// Текущая адресная форма политики.
	FallbackPolicyState policy;
	// Доверенный ключ локального пояснения; raw failure здесь не хранится.
	QString messageKey;
};
} // namespace gost
