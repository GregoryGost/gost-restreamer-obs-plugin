#pragma once
#include "api/models.hpp"
#include <QObject>
#include <QUrl>
#include <memory>
namespace gost::api {
class EventClient : public QObject {
public:
	using EventHandler = std::function<bool(RequestContext, const RuntimeEvent &)>;
	using ErrorHandler = std::function<void(RequestContext, Error, bool)>;
	explicit EventClient(Limits limits = {}, QObject *parent = nullptr);
	~EventClient() override;
	void subscribe(QUrl origin, QString cursor, RequestContext context, QObject *receiver,
		       EventHandler eventHandler, ErrorHandler errorHandler);
	void close();
	void shutdown();
	QString confirmedCursor() const;

private:
	class Impl;
	std::unique_ptr<Impl> impl_;
};
} // namespace gost::api
