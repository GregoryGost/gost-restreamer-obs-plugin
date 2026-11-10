#pragma once
#include "api/models.hpp"
#include <QQueue>
namespace gost::api {
struct SSEFrame {
	QString event;
	QString id;
	QByteArray data;
	qsizetype bytes = 0;
};
class SSEParser {
public:
	explicit SSEParser(Limits limits = {});
	bool feed(const QByteArray &bytes);
	bool hasFrame() const noexcept;
	SSEFrame takeFrame();
	qsizetype nextFrameBytes() const noexcept;
	bool hasError() const noexcept;
	Error error() const;
	qsizetype pendingBytes() const noexcept;
	int pendingFrames() const noexcept;
	void reset();
	bool finish();

private:
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
	bool initial_ = true;
	bool afterCR_ = false;
	bool dataSeen_ = false;
	std::optional<Error> error_;
};
} // namespace gost::api
