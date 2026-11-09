#pragma once

#include <QObject>
#include <QString>

#include <optional>

namespace gost {

class StateStore final : public QObject {
	Q_OBJECT

public:
	explicit StateStore(QObject *parent = nullptr);
	const std::optional<QString> &profileName() const noexcept;
	std::optional<bool> streamingActive() const noexcept;
	void setObsState(QString profileName, bool streamingActive);

signals:
	void obsStateChanged();

private:
	std::optional<QString> profileName_;
	std::optional<bool> streamingActive_;
};

} // namespace gost
