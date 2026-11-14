#pragma once

#include "text-lookup.hpp"
#include "core/operations-state.hpp"

#include <QWidget>

class QLabel;
class QLineEdit;
class QCheckBox;
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
 * Единственное повторно используемое расширенное окно с внутренней навигацией.
 * StateStore и необязательный ConnectionController заимствованы и должны пережить окно.
 * Все виджеты/страницы принадлежат Qt, доступ происходит в GUI-потоке. Закрытие скрывает окно
 * без удаления, завершения приложения, потери общего состояния или разрыва подписки.
 *
 * Окно отражает StateStore, а изменения передаёт контроллеру. Без контроллера остаётся представлением
 * с недоступными действиями. Обновления значений блокируют сигналы элементов ввода, чтобы отражение
 * состояния не стало новым пользовательским изменением. Черновик адреса не затирается телеметрией.
 */
class ManagementWindow final : public QWidget {
	Q_OBJECT

public:
	// Создаёт одно окно представления; заимствованные контроллеры переживают его скрытие и закрытие.
	ManagementWindow(StateStore &stateStore, TextLookup text, QWidget *parent = nullptr,
			 ConnectionController *connection = nullptr, RestreamController *restream = nullptr,
			 ConfigurationController *configuration = nullptr);
	// Открывает карточку назначения из Dock только для исходного контекста и Session.
	void selectOutput(const DockContext &context, const QString &sessionId, const QString &outputId);

protected:
	// Скрывает секреты и планирует активацию карточки после обработки показа окна Qt.
	void showEvent(QShowEvent *event) override;
	// Возвращает маску всем карточкам, сохраняя RAM-черновик и отправленные операции.
	void hideEvent(QHideEvent *event) override;

private:
	// Возвращает редактор текущей страницы/вкладки либо nullptr для остальных разделов.
	ConfigurationEditor *currentEditor() const;
	// Активирует только текущий вид карточки, не меняя рабочий Stream.
	void activateCurrentEditor();
	// Возвращает маску всем секретным полям, в том числе на невидимых страницах.
	void hideSecrets();
	// Согласует переход row с изменённым черновиком и сохраняет прежний выбор при отказе.
	void changePage(int row);
	// Обновляет подписи/доступность/предпочтения, сохраняя пользовательский ввод и раскрытый список.
	void refresh();
	// Перестраивает список лишь при изменении состава/выбора; сохраняет неполученный сохранённый ID
	// как недоступный или неподтверждённый пункт, не меняя постоянную привязку.
	void refreshStreams();
	// stateStore_ предоставляет состояние GUI-потока, text_ переводит подписи.
	StateStore &stateStore_;
	TextLookup text_;
	// connection_ меняет подключение и привязку рабочего Stream.
	ConnectionController *connection_ = nullptr;
	// configuration_ координирует CRUD независимо от очередей наблюдения и команд.
	ConfigurationController *configuration_ = nullptr;
	// streamEditor_ и destinationEditor_ представляют конфигурацию, а не текущие runtime-снимки.
	ConfigurationEditor *streamEditor_ = nullptr;
	ConfigurationEditor *destinationEditor_ = nullptr;
	// groupEditor_ и profileEditor_ размещаются на самостоятельных страницах того же окна.
	ConfigurationEditor *groupEditor_ = nullptr;
	ConfigurationEditor *profileEditor_ = nullptr;
	// pages_ переключает существующие страницы без дополнительных окон.
	QStackedWidget *pages_ = nullptr;
	// currentPage_ сохраняет прежний раздел до парковки несохранённого ввода.
	int currentPage_ = 0;
	// navigationChanging_ исключает повторный переход из вложенного цикла подтверждения.
	bool navigationChanging_ = false;
	// navigation_ переключает существующие страницы при переходе из меню дока.
	QListWidget *navigation_ = nullptr;
	// apiUrl_ содержит черновик адреса; autoConnect_ задаёт автоподключение; reconnect_ — восстановление связи.
	// sseRejectionRetries_ задаёт существующий лимит отклонённых подписок SSE.
	QLineEdit *apiUrl_ = nullptr;
	QCheckBox *autoConnect_ = nullptr;
	QCheckBox *reconnect_ = nullptr;
	QSpinBox *sseRejectionRetries_ = nullptr;
	// httpTimeout_ редактирует общий срок новых REST-запросов, logMode_ — уровень штатного журнала OBS.
	QSpinBox *httpTimeout_ = nullptr;
	QComboBox *logMode_ = nullptr;
	// streams_ и displayedSelectionId_ сохраняют выбор и не пересоздают раскрытый список при телеметрии.
	QComboBox *streams_ = nullptr;
	QString displayedSelectionId_;
	// connectButton_ и disconnectButton_ передают явные изменения сетевого подключения.
	QPushButton *connectButton_ = nullptr;
	QPushButton *disconnectButton_ = nullptr;
	// connectionStatus_ показывает связь; connectionMessage_ — общие предупреждения подключения и выбора.
	// advancedSettingsMessage_ отдельно показывает результат сохранения настроек на странице дополнительных параметров.
	StatusLabel *connectionStatus_ = nullptr;
	QLabel *connectionMessage_ = nullptr;
	QLabel *advancedSettingsMessage_ = nullptr;
	// profile_ и streaming_ представляют локальные наблюдения OBS на странице диагностики.
	QLabel *profile_ = nullptr;
	StatusLabel *streaming_ = nullptr;
};

} // namespace gost
