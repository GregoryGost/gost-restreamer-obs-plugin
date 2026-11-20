#pragma once

#include "core/state-store.hpp"
#include "text-lookup.hpp"

#include <QWidget>

class QComboBox;
class QLabel;
class QTreeWidget;
class QTreeWidgetItem;
class QVBoxLayout;

namespace gost {

class StatusLabel;

/**
 * Показывает только текущий эфир выбранного контекста из общего StateStore.
 * Заимствованный stateStore переживает страницу; Qt владеет дочерними виджетами и таймером.
 * Все чтения и обновления происходят в GUI-потоке без запросов или управляющих действий.
 * Значения не кешируются между исполнениями; отсутствие и устаревание сохраняют свой смысл.
 */
class DiagnosticsPage final : public QWidget {
	Q_OBJECT

public:
	// Создаёт представление stateStore с переводами text и дочерним владением через parent.
	DiagnosticsPage(StateStore &stateStore, TextLookup text, QWidget *parent = nullptr);

private:
	// Синхронно отражает профиль, OBS и связь, сохраняя прежний контракт ManagementWindow.
	void refreshSummary();
	// Объединяет синхронные уведомления StateStore в одно обновление следующего оборота GUI.
	void scheduleRefresh();
	// Перестраивает наблюдения текущего контекста и сохраняет только точный выбранный Output/instance.
	void refresh();
	// Обновляет GPU по собственным серверным статусам и возрасту полей, независимо от событий Session.
	void refreshGpu();
	// Создаёт в layout таблицу с заголовком titleKey и стабильным objectName name.
	QTreeWidget *makeSection(QVBoxLayout &layout, const char *titleKey, const char *name);
	// Добавляет в tree либо parent строку key/value с отдельной доступностью availability.
	QTreeWidgetItem *addRow(QTreeWidget &tree, QTreeWidgetItem *parent, const QString &key, const QString &value,
				const QString &availability = {});
	// Переводит независимое подтверждение availability без вывода о состоянии транспорта.
	QString availabilityText(RuntimeAvailability availability) const;
	// Переводит известные серверные коды value; неизвестный код не раскрывается произвольным текстом.
	QString statusText(const QString &value) const;
	// Отображает observation и его подтверждение под parent, не раскрывая failure.
	void addObservation(QTreeWidget &tree, QTreeWidgetItem *parent, const api::Observation &observation);
	// Добавляет точные накопления, формат и окна скоростей media с независимой свежестью availability.
	void addMedia(QTreeWidget &tree, QTreeWidgetItem *parent, const api::MediaMetrics &media,
		      RuntimeAvailability availability);
	// Возвращает строку field либо отсутствие измерения; null и missing не превращаются в число.
	QString timestamp(const api::JsonField<QString> &field) const;
	// Возвращает десятичное field без промежуточного double; единицы задаёт подпись строки.
	template<class T> QString integer(const api::JsonField<T> &field) const
	{
		return field ? QString::number(*field) : text_("Observation.NoData");
	}
	// Возвращает дробное измерение field либо отсутствие данных.
	QString decimal(const api::JsonField<double> &field) const;
	// stateStore_ заимствован; text_ разрешает локализацию подписей до уничтожения страницы.
	StateStore &stateStore_;
	TextLookup text_;
	// context_, sessionId_ и sessionInstance_ ограничивают сохранение выбора точным текущим эфиром.
	DockContext context_;
	QString sessionId_;
	QString sessionInstance_;
	// refreshPending_ исключает дублирующую перестройку по runtimeStateChanged и serverStateChanged.
	bool refreshPending_ = false;
	// profile_, streaming_ и connection_ показывают независимые наблюдения OBS и API.
	QLabel *profile_ = nullptr;
	StatusLabel *streaming_ = nullptr;
	StatusLabel *connection_ = nullptr;
	// contextLabel_ и sessionLabel_ объясняют доступность привязки и выбранной Session.
	QLabel *contextLabel_ = nullptr;
	QLabel *sessionLabel_ = nullptr;
	// obs_, input_, groups_, output_, ffmpeg_ и gpu_ содержат раздельные источники измерений.
	QTreeWidget *obs_ = nullptr;
	QTreeWidget *input_ = nullptr;
	QTreeWidget *groups_ = nullptr;
	QTreeWidget *output_ = nullptr;
	QTreeWidget *ffmpeg_ = nullptr;
	QTreeWidget *gpu_ = nullptr;
	// outputs_ адресует текущий Output двумя отдельными ролями outputId и instanceId.
	QComboBox *outputs_ = nullptr;
	// ackTotal_, ackLast_, ackMax_ и ackTime_ сохраняют точное представление четырёх полей ACK.
	QLabel *ackTotal_ = nullptr;
	QLabel *ackLast_ = nullptr;
	QLabel *ackMax_ = nullptr;
	QLabel *ackTime_ = nullptr;
	// ackAvailability_ отделяет свежесть метрик и недостоверный прирост при общем насыщении.
	QLabel *ackAvailability_ = nullptr;
};

} // namespace gost
