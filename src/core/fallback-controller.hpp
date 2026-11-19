#pragma once

#include "core/fallback-state.hpp"
#include "core/operation-failure.hpp"
#include <QObject>
#include <memory>

namespace gost {
class StateStore;
class ConfigurationController;
class RestreamController;
namespace api {
class APIClient;
}

/** Выполняет операции Asset и policy через заимствованный конфигурационный API.
 * ConfigurationController единолично перенастраивает и закрывает канал. Контроллер
 * отменяет только собственные receivers; скрытие Widgets не завершает workflow.
 */
class FallbackController final : public QObject {
	Q_OBJECT
public:
	// Внутренние пределы наблюдения; не являются пользовательскими настройками.
	struct Limits {
		// Бюджет одного цикла наблюдения после HTTP 202 в миллисекундах.
		int observationTimeoutMs = 15 * 60 * 1000;
		// Пауза между последовательными GET queued/preparing в миллисекундах.
		int pollIntervalMs = 1000;
	};
	// Подписывается на канал, затем принимает уже действующий контекст без записей.
	FallbackController(StateStore &state, api::APIClient &api, ConfigurationController &configuration,
			   RestreamController &restream, QObject *parent = nullptr);
	// Создаёт контроллер с уменьшенными внутренними пределами для изолированных проверок.
	FallbackController(StateStore &state, api::APIClient &api, ConfigurationController &configuration,
			   RestreamController &restream, Limits limits, QObject *parent = nullptr);
	// Идемпотентно прекращает собственные callbacks до освобождения реализации.
	~FallbackController() override;
	// Проверяет общий канал и отсутствие активной/несверенной конфигурационной записи.
	bool canWrite() const;
	// Разрешает подготовку со свежим Asset без pending, если активная Session не наблюдается.
	bool canPrepare() const;
	// Требует адресную свежую policy базу и отсутствие неразрешённого CAS.
	bool canSavePolicy() const;
	// Читает каталог, выбранный Asset, policy и сверяет Unknown без повтора записей.
	void refresh();
	// Выбирает точный Asset и запускает адресное чтение без изменения принятой пары.
	bool selectAsset(QString assetId);
	// Заменяет RAM-ввод имени, пути и kind без открытия файла или HTTP.
	bool setDraft(QString name, QString filePath, QString kind);
	// Создаёт Asset с draftName одним POST.
	bool createAsset();
	// Удаляет выбранный Asset по свежей адресной revision.
	bool deleteAsset();
	// Передаёт draftFilePath как image/video по свежей revision выбранного Asset.
	bool upload();
	// Подготавливает сохранённый оригинал без повторной передачи файла.
	bool rebuild();
	// Прекращает местное ожидание; потенциально отправленная запись остаётся Unknown.
	bool cancel();
	// Адресует policy указанного Stream независимо от рабочего выбора Dock; пустой ID закрывает цель.
	bool setPolicyTarget(QString streamId);
	// Задаёт явное назначение Asset; пустая строка означает очистку fallback:null.
	bool setPolicyAsset(QString assetId);
	// Читает точную политику текущей цели, сохраняя dirty-ввод.
	void refreshPolicy();
	// Отправляет полный policy по revision подтверждённой базы без автоматического CAS-повтора.
	bool savePolicy();
	// Принимает serverConflict новой базой; следующая запись требует отдельного savePolicy.
	bool confirmPolicyConflict();
	// Переиспользует общую предварительную проверку рабочего Stream без запуска.
	bool checkReadiness();
	// Отменяет свои таймеры/receivers и запрещает новые действия; глобальный API не закрывает.
	void shutdown();
signals:
	// Доставляет только отказ собственного намерения либо точно принятого поколения один раз.
	void operationFailed(const gost::OperationFailure &failure);

private:
	// Реализация хранит собственные scope receivers и ограниченные обязательства сверки.
	class Impl;
	// Единственный владелец реализации.
	std::unique_ptr<Impl> impl_;
};
} // namespace gost
