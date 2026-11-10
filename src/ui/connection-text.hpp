#pragma once

#include "core/state-store.hpp"

namespace gost {

inline const char *connectionTextKey(ConnectionStatus status)
{
	switch (status) {
	case ConnectionStatus::Connecting:
		return "Server.Connecting";
	case ConnectionStatus::Connected:
		return "Server.Connected";
	case ConnectionStatus::Reconnecting:
		return "Server.Reconnecting";
	case ConnectionStatus::Error:
		return "Server.Error";
	case ConnectionStatus::Disconnected:
		return "Server.Disconnected";
	}
	return "Observation.NoData";
}

} // namespace gost
