#pragma once

#include "text-lookup.hpp"
#include "core/operations-state.hpp"

#include <QWidget>

#include <memory>

class QLabel;
class QPushButton;
class QStandardItemModel;
class QListView;

namespace gost {

class StateStore;
class StatusLabel;
class RestreamController;

// Формирует безопасный текст результатов для двух окон; квитанция и состояние площадки не смешиваются.
QString formatOperations(const OperationsState &operations, const TextLookup &text);

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
	// Создаёт док с заимствованными состоянием и необязательным фасадом команд; parent управляет временем жизни.
	RestreamDock(StateStore &stateStore, TextLookup text, QWidget *parent = nullptr, QString version = {},
		     RestreamController *restream = nullptr);
	// Освобождает локальные точки отсчёта; Qt удаляет дочерние виджеты.
	~RestreamDock() override;

signals:
	// Просит владельца показать единственное расширенное окно.
	void managementRequested();
	// Просит открыть тот же экземпляр расширенного окна на точном выходе исходной Session.
	void outputDetailsRequested(DockContext context, QString sessionId, QString outputId);

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
	// Отражает доступность намерений и отдельные результаты команд без изменения телеметрии.
	void refreshOperations();
	// Фиксирует цель строки до открытия меню; подтверждённое действие не переносится на новый выбор.
	void outputActions(DockContext context, QString sessionId, QString outputId);
	// Показывает подтверждение прерывающей операции для зафиксированной Session или пакета.
	void dispatchSessionAction(OperationAction action);
	// Локальный кеш статистики, независимый от модели команд.
	struct Statistics;
	// stateStore_ предоставляет общие наблюдения; text_ переводит подписи текущей локали.
	StateStore &stateStore_;
	TextLookup text_;
	// restream_ заимствован у Runtime и переживает док; nullptr оставляет действия недоступными.
	RestreamController *restream_ = nullptr;
	// statistics_ хранит только состояние представления и точки отсчёта счётчиков.
	std::unique_ptr<Statistics> statistics_;
	// server_ показывает подключение; stream_ — выбранный Stream; notice_ — безопасные предупреждения.
	// profile_ показывает профиль OBS, streaming_ — независимое состояние штатной трансляции OBS.
	StatusLabel *server_ = nullptr;
	QLabel *stream_ = nullptr;
	QLabel *notice_ = nullptr;
	QLabel *profile_ = nullptr;
	StatusLabel *streaming_ = nullptr;
	// localStats_ и serverDataNotice_ отделяют локальные измерения от недоступных серверных данных.
	QLabel *localStats_ = nullptr;
	QLabel *serverDataNotice_ = nullptr;
	// session_ показывает долговечное состояние эфира; groups_ — фактические источники групп.
	// fallback_ показывает подготовленность файла; input_ — подключение входа; ingest_ — его формат и скорости.
	StatusLabel *session_ = nullptr;
	StatusLabel *groups_ = nullptr;
	StatusLabel *fallback_ = nullptr;
	StatusLabel *input_ = nullptr;
	QLabel *ingest_ = nullptr;
	// outputSummary_ содержит подтверждённую сводку, outputs_ — модель строк, outputList_ — их виджеты и прокрутку.
	QLabel *outputSummary_ = nullptr;
	QStandardItemModel *outputs_ = nullptr;
	QListView *outputList_ = nullptr;
	// operationStatus_ показывает текущий и последний прерванный результат вне статистики площадок.
	QLabel *operationStatus_ = nullptr;
	// nativeStart_ объясняет штатный запуск OBS только после полного подтверждения отсутствия эфира.
	QLabel *nativeStart_ = nullptr;
	// pause_ включает серверную паузу; resume_ снимает её; stop_ завершает OBS и точную серверную Session.
	QPushButton *pause_ = nullptr;
	QPushButton *resume_ = nullptr;
	QPushButton *stop_ = nullptr;
	// startOutputs_ и restartOutputs_ создают фиксированные пакеты выходов.
	QPushButton *startOutputs_ = nullptr;
	QPushButton *restartOutputs_ = nullptr;
	// retry_ повторяет только живой шаг с неизвестным принятием по явному действию пользователя.
	QPushButton *retry_ = nullptr;
	// reset_ меняет только локальные точки отсчёта накоплений.
	QPushButton *reset_ = nullptr;
	// refreshScheduled_ объединяет серию уведомлений в одно отложенное обновление.
	bool refreshScheduled_ = false;
};

} // namespace gost
