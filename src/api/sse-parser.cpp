#include "api/sse-parser.hpp"
#include "api/json-decoder.hpp"
#include <utility>
namespace gost::api {
SSEParser::SSEParser(Limits limits) : limits_(limits) {}
bool SSEParser::fail(ErrorKind kind)
{
	error_ = makeError(kind);
	return false;
}
bool SSEParser::feed(const QByteArray &bytes)
{
	if (error_)
		return false;
	for (const char c : bytes) {
		if (initial_) {
			bom_.append(c);
			const QByteArray marker("\xEF\xBB\xBF", 3);
			if (marker.startsWith(bom_)) {
				if (bom_.size() == 3) {
					initial_ = false;
					bom_.clear();
				}
				continue;
			}
			initial_ = false;
			const auto prefix = std::exchange(bom_, QByteArray{});
			if (!feed(prefix))
				return false;
			continue;
		}
		if (afterCR_) {
			afterCR_ = false;
			if (c == '\n')
				continue;
		}
		if (++frameBytes_ > limits_.frameBytes || frameBytes_ + queuedBytes_ > limits_.pendingBytes)
			return fail(ErrorKind::Limit);
		if (c == '\r' || c == '\n') {
			if (!line())
				return false;
			afterCR_ = c == '\r';
		} else
			line_.append(c);
	}
	return true;
}
bool SSEParser::line()
{
	if (!line_.isValidUtf8())
		return fail(ErrorKind::Decode);
	const auto raw = std::exchange(line_, QByteArray{});
	if (raw.isEmpty()) {
		if (dataSeen_) {
			if (frames_.size() >= limits_.pendingEvents)
				return fail(ErrorKind::Limit);
			if (!data_.isEmpty())
				data_.chop(1);
			frames_.enqueue({event_, id_, data_, frameBytes_});
			queuedBytes_ += frameBytes_;
		}
		data_.clear();
		event_.clear();
		id_.clear();
		dataSeen_ = false;
		frameBytes_ = 0;
		return true;
	}
	if (raw.startsWith(':'))
		return true;
	const auto colon = raw.indexOf(':');
	const auto key = colon < 0 ? raw : raw.first(colon);
	auto value = colon < 0 ? QByteArray{} : raw.sliced(colon + 1);
	if (value.startsWith(' '))
		value.remove(0, 1);
	if (key == "data") {
		dataSeen_ = true;
		data_.append(value);
		data_.append('\n');
	} else if (key == "event")
		event_ = QString::fromUtf8(value);
	else if (key == "id") {
		if (value.contains('\0'))
			return fail(ErrorKind::Decode);
		id_ = QString::fromUtf8(value);
	}
	return true;
}
bool SSEParser::hasFrame() const noexcept
{
	return !frames_.isEmpty();
}
qsizetype SSEParser::nextFrameBytes() const noexcept
{
	return frames_.isEmpty() ? 0 : frames_.head().bytes;
}
SSEFrame SSEParser::takeFrame()
{
	if (frames_.isEmpty())
		return {};
	auto frame = frames_.dequeue();
	queuedBytes_ -= frame.bytes;
	return frame;
}
bool SSEParser::hasError() const noexcept
{
	return error_.has_value();
}
Error SSEParser::error() const
{
	return error_.value_or(makeError(ErrorKind::Decode));
}
qsizetype SSEParser::pendingBytes() const noexcept
{
	return queuedBytes_ + frameBytes_;
}
int SSEParser::pendingFrames() const noexcept
{
	return int(frames_.size());
}
void SSEParser::reset()
{
	*this = SSEParser(limits_);
}
bool SSEParser::finish()
{
	if (!line_.isValidUtf8() || !bom_.isEmpty())
		return fail(ErrorKind::Decode);
	line_.clear();
	data_.clear();
	event_.clear();
	id_.clear();
	frameBytes_ = 0;
	dataSeen_ = false;
	return !error_;
}
} // namespace gost::api
