#pragma once

#include "core/state-store.hpp"
#include "text-lookup.hpp"

#include <QApplication>
#include <QEvent>
#include <QLabel>
#include <QPalette>
#include <QPainter>

#include <optional>

namespace gost {

// Показывает статус в GUI-потоке при существующем QApplication. Текст всегда сообщает смысл
// независимо от цвета. Tone хранит семантику между сменами палитры; Unknown/Stopped используют
// нейтральный цвет темы, промежуточное состояние не выдаётся за подтверждённую активность.
// Виджет не наблюдает StateStore сам: владелец явно передаёт нужную ось состояния.
class StatusLabel final : public QLabel {
public:
	// Tone задаёт смысл оформления: Neutral — нейтральный, Active — активный,
	// Pending — переходный, Error — ошибочный. Сами значения не меняют текст наблюдения.
	enum class Tone { Neutral, Active, Pending, Error };

	// Создаёт статус с родителем parent; indicator включает точку и отступ, по умолчанию они видимы.
	explicit StatusLabel(QWidget *parent, bool indicator = true) : QLabel(parent), indicator_(indicator)
	{
		setTextFormat(Qt::PlainText);
		setWordWrap(true);
		setContentsMargins(indicator_ ? 12 : 0, 0, 0, 0);
		updateStyle();
		qApp->installEventFilter(this);
	}

	// Показывает text обычным текстом и сохраняет смысл tone для цвета и свойства statusTone.
	void setStatus(const QString &text, Tone tone)
	{
		setText(text);
		// names сопоставляет значению Tone стабильное имя свойства для представлений и проверок.
		const char *names[] = {"neutral", "active", "pending", "error"};
		setProperty("statusTone", QString::fromLatin1(names[static_cast<int>(tone)]));
		if (tone_ != tone) {
			tone_ = tone;
			updateStyle();
		}
	}

	// Переводит наблюдаемый status подключения через text; nullopt явно показывает отсутствие данных.
	void setConnectionStatus(const TextLookup &text, std::optional<ConnectionStatus> status)
	{
		if (!status) {
			setStatus(text("Observation.NoData"), Tone::Neutral);
			return;
		}

		switch (*status) {
		case ConnectionStatus::Connecting:
			setStatus(text("Server.Connecting"), Tone::Pending);
			break;
		case ConnectionStatus::Connected:
			setStatus(text("Server.Connected"), Tone::Active);
			break;
		case ConnectionStatus::Reconnecting:
			setStatus(text("Server.Reconnecting"), Tone::Pending);
			break;
		case ConnectionStatus::Error:
			setStatus(text("Server.Error"), Tone::Error);
			break;
		case ConnectionStatus::Disconnected:
			setStatus(text("Server.Disconnected"), Tone::Neutral);
			break;
		}
	}

	// Показывает через text активность active, различая подтверждённое false и отсутствие наблюдения.
	void setStreamingStatus(const TextLookup &text, std::optional<bool> active)
	{
		// nullopt — отсутствие наблюдения, false — подтверждённая неактивность; подписи различаются.
		if (active.value_or(false)) {
			setStatus(text("OBS.StreamingActive"), Tone::Active);
		} else {
			setStatus(text(active ? "OBS.StreamingInactive" : "Observation.NoData"), Tone::Neutral);
		}
	}

	// Отражает независимую фазу phase штатного output OBS, используя переводы text.
	void setStreamingPhase(const TextLookup &text, ObsStreamingPhase phase)
	{
		switch (phase) {
		case ObsStreamingPhase::Unknown:
			setStatus(text("Observation.NoData"), Tone::Neutral);
			break;
		case ObsStreamingPhase::Stopped:
			setStatus(text("Dock.ObsStopped"), Tone::Neutral);
			break;
		case ObsStreamingPhase::Starting:
			setStatus(text("Dock.ObsStarting"), Tone::Pending);
			break;
		case ObsStreamingPhase::Streaming:
			setStatus(text("Dock.ObsStreaming"), Tone::Active);
			break;
		case ObsStreamingPhase::Stopping:
			setStatus(text("Dock.ObsStopping"), Tone::Pending);
			break;
		}
	}

protected:
	// Сначала рисует текст штатным QLabel для event, затем добавляет точку, если она включена.
	void paintEvent(QPaintEvent *event) override
	{
		QLabel::paintEvent(event);
		if (!indicator_)
			return;
		// painter рисует точку текущим смысловым цветом, не меняя стандартную отрисовку текста.
		QPainter painter(this);
		painter.setRenderHint(QPainter::Antialiasing);
		painter.setPen(Qt::NoPen);
		painter.setBrush(indicatorColor_);
		painter.drawEllipse(QRectF(1, (fontMetrics().height() - 6) / 2.0, 6, 6));
	}

	// Обновляет цвет при смене палитры QApplication; остальные event от watched передаёт QLabel.
	bool eventFilter(QObject *watched, QEvent *event) override
	{
		if (event->type() == QEvent::ApplicationPaletteChange && watched == qApp)
			updateStyle();
		return QLabel::eventFilter(watched, event);
	}

	// После стандартной обработки event обновляет цвет при смене палитры или стиля самого виджета.
	void changeEvent(QEvent *event) override
	{
		QLabel::changeEvent(event);
		if (event->type() == QEvent::PaletteChange || event->type() == QEvent::StyleChange)
			updateStyle();
	}

private:
	// Выбирает цвет текущего tone_ из палитры приложения и меняет stylesheet только при отличии.
	void updateStyle()
	{
		// При смене темы палитра родителя ещё может быть прежней, поэтому берём новую палитру приложения.
		// Меняем stylesheet только при отличии, чтобы повтор того же статуса не пересчитывал оформление.
		// palette содержит актуальные цвета приложения, включая смену оформления OBS.
		const auto palette = QApplication::palette();
		// dark выбирает контрастные цвета статусов по светлоте фона темы.
		const bool dark = palette.color(QPalette::Window).lightness() < 128;
		// color содержит цвет текста и необязательной точки для сохранённого смысла статуса.
		QString color;
		switch (tone_) {
		case Tone::Neutral:
			color = palette.color(QPalette::WindowText).name();
			break;
		case Tone::Active:
			color = dark ? QStringLiteral("#6bdf91") : QStringLiteral("#155d32");
			break;
		case Tone::Pending:
			color = dark ? QStringLiteral("#ffd078") : QStringLiteral("#7d5000");
			break;
		case Tone::Error:
			color = dark ? QStringLiteral("#ff8992") : QStringLiteral("#a42030");
			break;
		}

		indicatorColor_ = QColor(color);
		// style сохраняет полужирный текст независимо от наличия точки и состояния подключения.
		const auto style = QStringLiteral("font-weight: bold; color: %1;").arg(color);
		if (style != styleSheet())
			setStyleSheet(style);
	}

	// tone_ сохраняет смысл последнего статуса между сменами оформления.
	Tone tone_ = Tone::Neutral;
	// indicatorColor_ совпадает с цветом текста; используется при включённой точке.
	QColor indicatorColor_;
	// indicator_ задаётся при создании и определяет наличие точки и её левого отступа.
	bool indicator_ = true;
};

} // namespace gost
