#pragma once
#include "api/models.hpp"
#include <QQueue>
namespace gost::api {
// Полный кадр без завершающего разделителя в data. bytes — учётный размер полей и разделителей
// для бюджетов очереди/доставки, не размер JSON и не число символов QString. Начальная BOM
// не учитывается, а LF после CR не учитывается второй раз; это не точный объём сетевого трафика.
struct SSEFrame {
	QString event;
	QString id;
	QByteArray data;
	qsizetype bytes = 0;
};
/**
 * Разбирает поток байтов выбранного серверного контракта независимо от границ сетевых чтений.
 * Накапливает незавершённую строку и кадр, распознаёт CR/LF/CRLF и одну начальную UTF-8 BOM.
 * Полные строки должны быть корректным UTF-8; JSON проверяет последующий слой.
 *
 * Это парсер кадров API, а не полный браузерный EventSource: id относится к одному кадру,
 * NUL в id отклоняется, неизвестные поля и retry игнорируются. Подтверждённый cursor и политика
 * восстановления находятся в EventClient/ConnectionController. Кадры без data не выдаются;
 * data с пустым значением выдаётся. Служебный сигнал не становится прикладным событием.
 *
 * Размер текущего кадра и суммарной очереди ограничен Limits. Ошибка сохраняется до reset().
 * Класс не синхронизирован; feed и извлечение кадров должны выполняться последовательно.
 */
class SSEParser {
public:
	explicit SSEParser(Limits limits = {});
	// Добавляет произвольный фрагмент; false означает сохранённую ошибку. Ранее готовые кадры могут остаться в очереди.
	bool feed(const QByteArray &bytes);
	bool hasFrame() const noexcept;
	// Извлекает первый кадр и освобождает его бюджет. При пустой очереди возвращает пустой SSEFrame.
	SSEFrame takeFrame();
	// Размер первого готового кадра либо ноль; позволяет потребителю проверить бюджет до извлечения.
	qsizetype nextFrameBytes() const noexcept;
	bool hasError() const noexcept;
	// Вызывать после hasError(); без ошибки возвращает резервное значение Decode, а не признак успеха.
	Error error() const;
	// Учитывает готовые кадры и текущий незаконченный кадр; pendingFrames считает только готовые.
	qsizetype pendingBytes() const noexcept;
	int pendingFrames() const noexcept;
	// Удаляет все данные, очередь, ошибку и состояние разделителей, сохраняя Limits.
	void reset();
	// Отбрасывает незавершённый хвост без выдачи события. Готовые кадры и прежняя ошибка сохраняются.
	// Возвращает отсутствие ошибки; для нового независимого потока после EOF требуется reset().
	bool finish();

private:
	// Обрабатывает одну завершённую строку; пустая строка закрывает кадр и сбрасывает его поля.
	bool line();
	bool fail(ErrorKind kind);
	Limits limits_;
	QByteArray line_;
	QByteArray bom_;
	QByteArray data_;
	QString event_;
	QString id_;
	QQueue<SSEFrame> frames_;
	qsizetype frameBytes_ = 0;
	qsizetype queuedBytes_ = 0;
	// initial_ допускает BOM лишь в начале потока; afterCR_ поглощает LF даже в следующем feed.
	bool initial_ = true;
	bool afterCR_ = false;
	bool dataSeen_ = false;
	std::optional<Error> error_;
};
} // namespace gost::api
