#pragma once

#include "text-lookup.hpp"

#include <QWidget>

class QLabel;
class QLineEdit;
class QCheckBox;
class QComboBox;
class QPushButton;
class QSpinBox;

namespace gost {

class StateStore;
class ConnectionController;
class StatusLabel;

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
	ManagementWindow(StateStore &stateStore, TextLookup text, QWidget *parent = nullptr,
			 ConnectionController *connection = nullptr);

private:
	// Обновляет подписи/доступность/предпочтения, сохраняя пользовательский ввод и раскрытый список.
	void refresh();
	// Перестраивает список лишь при изменении состава/выбора; сохраняет неполученный сохранённый ID
	// как недоступный или неподтверждённый пункт, не меняя постоянную привязку.
	void refreshStreams();
	StateStore &stateStore_;
	TextLookup text_;
	ConnectionController *connection_ = nullptr;
	QLineEdit *apiUrl_ = nullptr;
	QCheckBox *autoConnect_ = nullptr;
	QCheckBox *reconnect_ = nullptr;
	QSpinBox *sseRejectionRetries_ = nullptr;
	QComboBox *streams_ = nullptr;
	QString displayedSelectionId_;
	QPushButton *connectButton_ = nullptr;
	QPushButton *disconnectButton_ = nullptr;
	StatusLabel *connectionStatus_ = nullptr;
	QLabel *connectionMessage_ = nullptr;
	QLabel *advancedSettingsMessage_ = nullptr;
	QLabel *profile_ = nullptr;
	StatusLabel *streaming_ = nullptr;
};

} // namespace gost
