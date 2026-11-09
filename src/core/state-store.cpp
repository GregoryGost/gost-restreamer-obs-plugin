#include "state-store.hpp"

#include <QThread>

#include <utility>

namespace gost {

StateStore::StateStore(QObject *parent) : QObject(parent) {}

const std::optional<QString> &StateStore::profileName() const noexcept
{
	return profileName_;
}

std::optional<bool> StateStore::streamingActive() const noexcept
{
	return streamingActive_;
}

void StateStore::setObsState(QString profileName, bool streamingActive)
{
	Q_ASSERT(QThread::currentThread() == thread());
	if (profileName_ == profileName && streamingActive_ == streamingActive) {
		return;
	}
	profileName_ = std::move(profileName);
	streamingActive_ = streamingActive;
	emit obsStateChanged();
}

} // namespace gost
