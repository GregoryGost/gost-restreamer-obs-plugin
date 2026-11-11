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

class ManagementWindow final : public QWidget {
	Q_OBJECT

public:
	ManagementWindow(StateStore &stateStore, TextLookup text, QWidget *parent = nullptr,
			 ConnectionController *connection = nullptr);

private:
	void refresh();
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
