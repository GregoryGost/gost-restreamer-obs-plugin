#pragma once

#include "core/state-store.hpp"
#include "text-lookup.hpp"

#include <QApplication>
#include <QEvent>
#include <QLabel>
#include <QPalette>

#include <optional>

namespace gost {

// Представление статуса в GUI-потоке при существующем QApplication. Текст всегда сообщает смысл
// независимо от цвета. Tone хранит семантику между сменами палитры; Unknown/Stopped используют
// нейтральный цвет темы, промежуточное состояние не выдаётся за подтверждённую активность.
// Виджет не наблюдает StateStore сам: владелец явно передаёт нужную ось состояния.
class StatusLabel final : public QLabel {
public:
	explicit StatusLabel(QWidget *parent) : QLabel(parent)
	{
		setTextFormat(Qt::PlainText);
		setWordWrap(true);
		updateStyle();
		qApp->installEventFilter(this);
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
	bool eventFilter(QObject *watched, QEvent *event) override
	{
		if (event->type() == QEvent::ApplicationPaletteChange && watched == qApp)
			updateStyle();
		return QLabel::eventFilter(watched, event);
	}

private:
	enum class Tone { Neutral, Active, Pending, Error };

	void setStatus(const QString &text, Tone tone)
	{
		setText(text);
		if (tone_ != tone) {
			tone_ = tone;
			updateStyle();
		}
	}

	void updateStyle()
	{
		// Используется палитра приложения OBS. Меняем stylesheet только при отличии, чтобы повторное
		// поступление того же статуса не запускало лишние перерасчёты оформления/геометрии.
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

		const auto style = QStringLiteral("font-weight: bold; color: %1;").arg(color);
		if (style != styleSheet())
			setStyleSheet(style);
	}

	Tone tone_ = Tone::Neutral;
};

} // namespace gost
