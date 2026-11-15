#pragma once

#include "text-lookup.hpp"
#include "core/operations-state.hpp"

#include <QWidget>

#include <optional>

class QLabel;
class QLineEdit;
class QComboBox;
class QPushButton;
class QSpinBox;
class QListWidget;
class QStackedWidget;
class QShowEvent;
class QHideEvent;

namespace gost {

class StateStore;
class ConnectionController;
class StatusLabel;
class RestreamController;
class ConfigurationController;
class ConfigurationEditor;

/**
 * Предоставляет единственное расширенное окно с внутренней навигацией и формами конфигурации.
 * StateStore и необязательные контроллеры заимствованы и должны пережить окно.
 * Все виджеты/страницы принадлежат Qt, доступ происходит в GUI-потоке. Закрытие скрывает окно
 * без удаления, завершения приложения, потери общего состояния или разрыва подписки.
 *
 * Окно отражает StateStore, а изменения передаёт контроллеру. Без контроллера остаётся представлением
 * с недоступными действиями. Обновления значений блокируют сигналы элементов ввода, чтобы отражение
 * состояния не стало новым пользовательским изменением. Черновики адреса и дополнительных
 * предпочтений не затираются телеметрией и живут до явного сохранения/отмены.
 */
class ManagementWindow final : public QWidget {
	Q_OBJECT

public:
	// Создаёт страницы из stateStore с переводами text; parent связывает окно с OBS.
	// Заимствованные connection/configuration переживают окно; restream сохранён в интерфейсе конструктора.
	// Закрытие скрывает окно, а черновики настроек сохраняются до Save/Cancel или уничтожения окна.
	ManagementWindow(StateStore &stateStore, TextLookup text, QWidget *parent = nullptr,
			 ConnectionController *connection = nullptr, RestreamController *restream = nullptr,
			 ConfigurationController *configuration = nullptr);
	// Открывает точный Destination исходного контекста, сохраняя черновики и возвращая маски секретов.
	void selectDestination(const DockContext &context, const QString &destinationId);
	// Преобразует прежний маршрут Output/Session в прямой адрес сохранённого назначения.
	void selectOutput(const DockContext &context, const QString &sessionId, const QString &outputId);

protected:
	// Скрывает секреты и планирует активацию карточки после обработки показа окна Qt.
	void showEvent(QShowEvent *event) override;
	// Возвращает маску всем карточкам, сохраняя RAM-черновик и отправленные операции.
	void hideEvent(QHideEvent *event) override;

private:
	// Возвращает редактор текущей страницы либо nullptr для разделов без конфигурационной карточки.
	ConfigurationEditor *currentEditor() const;
	// Активирует только текущий вид карточки, не меняя рабочий Stream.
	void activateCurrentEditor();
	// Возвращает маску всем секретным полям, в том числе на невидимых страницах.
	void hideSecrets();
	// Согласует переход row с изменённым черновиком и сохраняет прежний выбор при отказе.
	void changePage(int row);
	// Отмечает пользовательский ввод дополнительных предпочтений, включая ещё не разобранный текст spinbox.
	void editAdvancedSettings();
	// Обновляет диагностику, доступность действий и сообщения форм, сохраняя изменённые черновики.
	void refresh();
	// stateStore_ предоставляет состояние GUI-потока, text_ переводит подписи.
	StateStore &stateStore_;
	TextLookup text_;
	// connection_ применяет сохранённый адрес и дополнительные предпочтения подключения.
	ConnectionController *connection_ = nullptr;
	// configuration_ координирует CRUD независимо от очередей наблюдения и команд.
	ConfigurationController *configuration_ = nullptr;
	// streamEditor_ редактирует конфигурацию потока; destinationEditor_ — его сохранённые назначения.
	ConfigurationEditor *streamEditor_ = nullptr;
	ConfigurationEditor *destinationEditor_ = nullptr;
	// groupEditor_ редактирует связи групп, profileEditor_ — параметры кодирования на отдельной странице.
	ConfigurationEditor *groupEditor_ = nullptr;
	ConfigurationEditor *profileEditor_ = nullptr;
	// pages_ переключает существующие страницы без дополнительных окон.
	QStackedWidget *pages_ = nullptr;
	// currentPage_ сохраняет прежний раздел до парковки несохранённого ввода.
	int currentPage_ = 0;
	// navigationChanging_ исключает повторный переход из вложенного цикла подтверждения.
	bool navigationChanging_ = false;
	// navigation_ содержит внутреннее меню разделов расширенного окна.
	QListWidget *navigation_ = nullptr;
	// apiUrl_ содержит локальный черновик адреса, применяемый только явным сохранением.
	QLineEdit *apiUrl_ = nullptr;
	// apiUrlInvalid_ отмечает отказ локальной проверки последнего сохранения и сбрасывается новым вводом/отменой.
	bool apiUrlInvalid_ = false;
	// sseRejectionRetries_ содержит черновик лимита отклонённых подписок SSE.
	QSpinBox *sseRejectionRetries_ = nullptr;
	// httpTimeout_ редактирует общий срок новых REST-запросов, logMode_ — уровень штатного журнала OBS.
	QSpinBox *httpTimeout_ = nullptr;
	QComboBox *logMode_ = nullptr;
	// settingsSaveButton_ применяет черновик адреса; settingsCancelButton_ возвращает текущий адрес без записи.
	QPushButton *settingsSaveButton_ = nullptr;
	QPushButton *settingsCancelButton_ = nullptr;
	// advancedSettingsSaveButton_ применяет три предпочтения; advancedSettingsCancelButton_ отменяет их ввод.
	QPushButton *advancedSettingsSaveButton_ = nullptr;
	QPushButton *advancedSettingsCancelButton_ = nullptr;
	// advancedSettingsDirty_ защищает ввод страницы от обновлений StateStore до явного Save/Cancel.
	bool advancedSettingsDirty_ = false;
	// advancedSettingsSaveResult_ хранит исход явного Save; nullopt означает отсутствие текущего результата.
	std::optional<bool> advancedSettingsSaveResult_;
	// settingsMessage_ показывает только локальную валидацию адреса и предупреждение о несохранённых настройках.
	QLabel *settingsMessage_ = nullptr;
	// advancedSettingsMessage_ показывает подтверждение записи или отказ смысловым цветом текущей темы.
	StatusLabel *advancedSettingsMessage_ = nullptr;
	// profile_ показывает имя профиля OBS, streaming_ — наблюдаемое состояние его штатной трансляции в диагностике.
	QLabel *profile_ = nullptr;
	StatusLabel *streaming_ = nullptr;
};

} // namespace gost
