#pragma once

#include "text-lookup.hpp"
#include "core/operations-state.hpp"

#include <QWidget>

#include <memory>

class QLabel;
class QPushButton;
class QStandardItemModel;
class QListView;
class QComboBox;
class QResizeEvent;
class QToolButton;

namespace gost {

class StateStore;
class StatusLabel;
class RestreamController;
class ConnectionController;

// Формирует безопасный текст результатов команд; квитанция и состояние площадки не смешиваются.
QString formatOperations(const OperationsState &operations, const TextLookup &text);

/**
 * Показывает независимые состояния OBS/сервера и статистику общего StateStore в GUI-потоке.
 * StateStore и контроллеры заимствованы и должны пережить Dock; дочерними виджетами и моделью владеет Qt.
 * HTTP напрямую не выполняется: данные приходят через сигналы, выбор Stream передаётся
 * ConnectionController, открытие управления — владельцу Dock.
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
	// Создаёт Dock: stateStore даёт наблюдения, text переводит подписи, version показывается внизу.
	// parent управляет временем жизни; необязательные restream и connection заимствованы у Runtime.
	RestreamDock(StateStore &stateStore, TextLookup text, QWidget *parent = nullptr, QString version = {},
		     RestreamController *restream = nullptr, ConnectionController *connection = nullptr);
	// Освобождает локальные точки отсчёта; Qt удаляет дочерние виджеты.
	~RestreamDock() override;

signals:
	// Просит владельца показать единственное расширенное окно.
	void managementRequested();
	// Просит открыть сохранённое назначение исходной строки в том же расширенном окне.
	void destinationDetailsRequested(DockContext context, QString destinationId);
	// Совместимый маршрут прежних потребителей по точному выходу исходной Session.
	void outputDetailsRequested(DockContext context, QString sessionId, QString outputId);

protected:
	// При изменении ширины event планирует обновление переносов метрик, сохраняя значения и точки отсчёта.
	void resizeEvent(QResizeEvent *event) override;

private:
	// Забирает текущую выборку OBS, обновляет независимые baseline, серверные строки и доступность сброса.
	void refresh();
	// Быстро обновляет контекст/состояния и сообщения; выполняется непосредственно по части сигналов.
	void refreshObservations();
	// Отражает список и точный сохранённый ID без обратной записи привязки; неполный список её не удаляет.
	void refreshStreams();
	// Проверяет принадлежность/свежесть каждой серверной оси и обновляет кеш до форматирования.
	// Также вызывается синхронно перед markRuntimeStale, чтобы не потерять последнее принятое измерение.
	void refreshServer();
	// Объединяет серию сигналов в один singleShot(0), не продлевая жизнь Dock.
	void scheduleRefresh();
	// Сначала принимает последнюю выборку, затем сбрасывает доступные baseline и перерисовывает.
	void resetStatistics();
	// Отражает доступность намерений и отдельные результаты команд без изменения телеметрии.
	void refreshOperations();
	// Открывает меню у anchor для раздельных outputId/destinationId исходного context/sessionId.
	// До вложенного цикла фиксирует ветвь запуска, после него проверяет ту же цель и время жизни Dock.
	void outputActions(DockContext context, QString sessionId, QString outputId, QString destinationId,
			   QToolButton *anchor);
	// Передаёт намерение исходной Session; завершение эфира отдельно подтверждает диалогом.
	void dispatchSessionAction(OperationAction action);
	// Копирует весь показанный состав до подтверждения и передаёт его одному пакетному workflow.
	void dispatchOutputAction(OperationAction action);
	// Локальный кеш статистики, независимый от модели команд.
	struct Statistics;
	// stateStore_ предоставляет общие наблюдения; text_ переводит подписи текущей локали.
	StateStore &stateStore_;
	TextLookup text_;
	// restream_ заимствован у Runtime и переживает док; nullptr оставляет действия недоступными.
	RestreamController *restream_ = nullptr;
	// connection_ принимает явный выбор Stream; Runtime обеспечивает его жизнь до удаления Dock.
	ConnectionController *connection_ = nullptr;
	// statistics_ хранит только состояние представления и точки отсчёта счётчиков.
	std::unique_ptr<Statistics> statistics_;
	// server_ показывает подключение без точки; streams_ выбирает рабочий Stream по точному itemData.
	StatusLabel *server_ = nullptr;
	QComboBox *streams_ = nullptr;
	// displayedSelectionId_ отличает смену выбора от телеметрии, которая не пересоздаёт раскрытый список.
	QString displayedSelectionId_;
	// notice_ содержит безопасные предупреждения, streaming_ — независимую фазу штатной трансляции OBS.
	QLabel *notice_ = nullptr;
	StatusLabel *streaming_ = nullptr;
	// localStats_ содержит FPS/битрейт/время рендера, localDrops_ — отдельные потери и отправленные байты OBS.
	QLabel *localStats_ = nullptr;
	QLabel *localDrops_ = nullptr;
	// localPeriod_ показывает общий период только при его совпадении у всех доступных накоплений OBS.
	QLabel *localPeriod_ = nullptr;
	// serverDataNotice_ отделяет недоступность серверных измерений от локальной статистики OBS.
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
	// nativeStart_ объясняет запуск OBS и отдельного выхода после подтверждения отсутствия эфира.
	QLabel *nativeStart_ = nullptr;
	// pause_ включает серверную паузу; resume_ снимает её; stop_ завершает OBS и точную серверную Session.
	QPushButton *pause_ = nullptr;
	QPushButton *resume_ = nullptr;
	QPushButton *stop_ = nullptr;
	// startOutputs_ запускает показанные цели, не прерывая уже запрошенный запуск.
	QToolButton *startOutputs_ = nullptr;
	// stopOutputs_ останавливает только показанные выходы, сохраняя OBS и Session.
	QToolButton *stopOutputs_ = nullptr;
	// restartOutputs_ перезапускает весь показанный состав, включая остановленные назначения.
	QToolButton *restartOutputs_ = nullptr;
	// shownOutputs_ хранит безопасную адресацию ровно тех строк, которые установлены в outputs_.
	// Значение заменяется вместе со строками; обработчики копируют его до вложенного цикла подтверждения.
	OutputBatchRequest shownOutputs_;
	// shownSessionId_ сохраняет также завершённую Session показанного списка.
	// Пакет без активной Session передаёт контроллеру пустой sessionId.
	QString shownSessionId_;
	// retry_ повторяет только живой шаг с неизвестным принятием по явному действию пользователя.
	QPushButton *retry_ = nullptr;
	// reset_ меняет только локальные точки отсчёта накоплений.
	QPushButton *reset_ = nullptr;
	// refreshScheduled_ объединяет серию уведомлений в одно отложенное обновление.
	bool refreshScheduled_ = false;
};

} // namespace gost
