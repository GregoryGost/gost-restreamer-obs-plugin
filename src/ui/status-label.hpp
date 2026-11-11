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

// Представление статуса в GUI-потоке при существующем QApplication. Текст всегда сообщает смысл
// независимо от цвета. Tone хранит семантику между сменами палитры; Unknown/Stopped используют
// нейтральный цвет темы, промежуточное состояние не выдаётся за подтверждённую активность.
// Виджет не наблюдает StateStore сам: владелец явно передаёт нужную ось состояния.
class StatusLabel final : public QLabel {
public:
	enum class Tone { Neutral, Active, Pending, Error };

	explicit StatusLabel(QWidget *parent) : QLabel(parent)
	{
		setTextFormat(Qt::PlainText);
		setWordWrap(true);
		setContentsMargins(12, 0, 0, 0);
		updateStyle();
		qApp->installEventFilter(this);
	}

	void setStatus(const QString &text, Tone tone)
	{
		setText(text);
		const char *names[] = {"neutral", "active", "pending", "error"};
		setProperty("statusTone", QString::fromLatin1(names[static_cast<int>(tone)]));
		if (tone_ != tone) {
			tone_ = tone;
			updateStyle();
		}
	}

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

	void setStreamingStatus(const TextLookup &text, std::optional<bool> active)
	{
		// nullopt — отсутствие наблюдения, false — подтверждённая неактивность; подписи различаются.
		if (active.value_or(false)) {
			setStatus(text("OBS.StreamingActive"), Tone::Active);
		} else {
			setStatus(text(active ? "OBS.StreamingInactive" : "Observation.NoData"), Tone::Neutral);
		}
	}

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
	void paintEvent(QPaintEvent *event) override
	{
		QLabel::paintEvent(event);
		QPainter painter(this);
		painter.setRenderHint(QPainter::Antialiasing);
		painter.setPen(Qt::NoPen);
		painter.setBrush(indicatorColor_);
		painter.drawEllipse(QRectF(1, (fontMetrics().height() - 6) / 2.0, 6, 6));
	}

	bool eventFilter(QObject *watched, QEvent *event) override
	{
		if (event->type() == QEvent::ApplicationPaletteChange && watched == qApp)
			updateStyle();
		return QLabel::eventFilter(watched, event);
	}

	void changeEvent(QEvent *event) override
	{
		QLabel::changeEvent(event);
		if (event->type() == QEvent::PaletteChange || event->type() == QEvent::StyleChange)
			updateStyle();
	}

private:
	void updateStyle()
	{
		// При смене темы палитра родителя ещё может быть прежней, поэтому берём новую палитру приложения.
		// Меняем stylesheet только при отличии, чтобы повтор того же статуса не пересчитывал оформление.
		const auto palette = QApplication::palette();
		const bool dark = palette.color(QPalette::Window).lightness() < 128;
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
		const auto style = QStringLiteral("font-weight: bold; color: %1;").arg(color);
		if (style != styleSheet())
			setStyleSheet(style);
	}

	Tone tone_ = Tone::Neutral;
	QColor indicatorColor_;
};

} // namespace gost
