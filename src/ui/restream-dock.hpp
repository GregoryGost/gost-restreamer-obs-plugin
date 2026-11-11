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

/**
 * Показывает независимые состояния OBS/сервера и статистику общего StateStore в GUI-потоке.
 * StateStore заимствован и должен пережить Dock; дочерними виджетами и моделью владеет Qt.
 * HTTP не выполняется: данные приходят через сигналы, открытие управления передаётся владельцу.
 *
 * Statistics принадлежит представлению и хранит последние показанные значения и точки отсчёта.
 * Новый выбор сервера/профиля/Stream, новая Session или её новое известное исполнение очищает
 * серверный кеш. Внутри того же исполнения Session входные скорости и строки Output сбрасываются
 * независимо при смене соответствующего источника.
 * При потере свежести последние значения сохраняются с явной подписью. Пользовательский сброс меняет
 * только отсчёт накоплений; текущие скорости и источники OBS/сервера продолжают работать.
 * Все отложенные обновления привязаны к this.
 */
class RestreamDock final : public QWidget {
	Q_OBJECT

public:
	RestreamDock(StateStore &stateStore, TextLookup text, QWidget *parent = nullptr, QString version = {});
	~RestreamDock() override;

signals:
	void managementRequested();

private:
	// Забирает текущую выборку OBS, обновляет независимые baseline, серверные строки и доступность сброса.
	void refresh();
	// Быстро обновляет контекст/состояния и сообщения; выполняется непосредственно по части сигналов.
	void refreshObservations();
	// Проверяет принадлежность/свежесть каждой серверной оси и обновляет кеш до форматирования.
	// Также вызывается синхронно перед markRuntimeStale, чтобы не потерять последнее принятое измерение.
	void refreshServer();
	// Объединяет серию сигналов в один singleShot(0), не продлевая жизнь Dock.
	void scheduleRefresh();
	// Сначала принимает последнюю выборку, затем сбрасывает доступные baseline и перерисовывает.
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
