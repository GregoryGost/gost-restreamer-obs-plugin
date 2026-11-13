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

namespace gost {

class StateStore;
class ConnectionController;
class StatusLabel;
class RestreamController;

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
			 ConnectionController *connection = nullptr, RestreamController *restream = nullptr);
	// Выбирает выход только при совпадении исходного контекста и Session с нынешним отображением.
	void selectOutput(const DockContext &context, const QString &sessionId, const QString &outputId);

private:
	// Обновляет подписи/доступность/предпочтения, сохраняя пользовательский ввод и раскрытый список.
	void refresh();
	// Перестраивает список лишь при изменении состава/выбора; сохраняет неполученный сохранённый ID
	// как недоступный или неподтверждённый пункт, не меняя постоянную привязку.
	void refreshStreams();
	// Обновляет минимальные страницы текущего Stream и Output, сохраняя точные ID в itemData.
	void refreshOperations();
	// Передаёт действие для выбранного Output, фиксируя цель до подтверждения.
	void dispatchOutputAction(OperationAction action);
	// Показывает фиксированные назначения и запускает готовую предварительную проверку без OBS start.
	void startFallback();
	// stateStore_ предоставляет состояние GUI-потока, text_ переводит подписи.
	StateStore &stateStore_;
	TextLookup text_;
	// connection_ меняет подключение и привязку Stream; restream_ выполняет только явные команды.
	ConnectionController *connection_ = nullptr;
	RestreamController *restream_ = nullptr;
	// navigation_ переключает существующие страницы при переходе из меню дока.
	QListWidget *navigation_ = nullptr;
	// selectedStream_ показывает выбранный Stream; destinations_ — фиксированные назначения проверки.
	QLabel *selectedStream_ = nullptr;
	QLabel *destinations_ = nullptr;
	// fallbackStatus_ показывает предварительную пригодность отдельно от результата запуска.
	QLabel *fallbackStatus_ = nullptr;
	// checkFallback_ читает готовность; startFallback_ запускает только серверную Session.
	QPushButton *checkFallback_ = nullptr;
	QPushButton *startFallback_ = nullptr;
	// currentOutputs_ хранит точные ID текущей Session; outputStatus_ разделяет намерение и runtime.
	QComboBox *currentOutputs_ = nullptr;
	QLabel *outputStatus_ = nullptr;
	// displayedOutputContext_ и displayedSessionId_ предотвращают перенос выбора строки в другой эфир.
	DockContext displayedOutputContext_;
	QString displayedSessionId_;
	// outputStart_ запускает выбранный Output; outputStop_ останавливает; outputRestart_ выполняет stop→start.
	// outputDetach_ подтверждает и передаёт отсоединение, без скрытого повторного присоединения.
	QPushButton *outputStart_ = nullptr;
	QPushButton *outputStop_ = nullptr;
	QPushButton *outputRestart_ = nullptr;
	QPushButton *outputDetach_ = nullptr;
	// streamOperation_ и outputOperation_ показывают общую модель операций на обеих страницах.
	QLabel *streamOperation_ = nullptr;
	QLabel *outputOperation_ = nullptr;
	// apiUrl_ содержит черновик адреса; autoConnect_ задаёт автоподключение; reconnect_ — восстановление связи.
	// sseRejectionRetries_ задаёт существующий лимит отклонённых подписок SSE.
	QLineEdit *apiUrl_ = nullptr;
	QCheckBox *autoConnect_ = nullptr;
	QCheckBox *reconnect_ = nullptr;
	QSpinBox *sseRejectionRetries_ = nullptr;
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
