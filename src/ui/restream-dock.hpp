#pragma once

#include "text-lookup.hpp"

#include <QWidget>

#include <memory>

class QLabel;
class QPushButton;
class QStringListModel;

namespace gost {

class StateStore;
class StatusLabel;

class RestreamDock final : public QWidget {
	Q_OBJECT

public:
	RestreamDock(StateStore &stateStore, TextLookup text, QWidget *parent = nullptr, QString version = {});
	~RestreamDock() override;

signals:
	void managementRequested();

private:
	void refresh();
	void refreshObservations();
	void refreshServer();
	void scheduleRefresh();
	void resetStatistics();
	struct Statistics;
	StateStore &stateStore_;
	TextLookup text_;
	std::unique_ptr<Statistics> statistics_;
	StatusLabel *server_ = nullptr;
	QLabel *stream_ = nullptr;
	QLabel *notice_ = nullptr;
	QLabel *profile_ = nullptr;
	StatusLabel *streaming_ = nullptr;
	QLabel *localStats_ = nullptr;
	QLabel *serverDataNotice_ = nullptr;
	QLabel *session_ = nullptr;
	QLabel *groups_ = nullptr;
	QLabel *fallback_ = nullptr;
	QLabel *ingest_ = nullptr;
	QLabel *outputSummary_ = nullptr;
	QStringListModel *outputs_ = nullptr;
	QPushButton *reset_ = nullptr;
	bool refreshScheduled_ = false;
};

} // namespace gost
