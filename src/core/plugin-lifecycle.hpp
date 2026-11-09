#pragma once

#include "ui/text-lookup.hpp"

#include <QObject>

#include <memory>

namespace gost {

class Frontend;
enum class FrontendEvent;
class StateStore;

class PluginLifecycle final : public QObject {
	Q_OBJECT

public:
	static constexpr const char *DockId = "gost-restreamer-obs-plugin";
	PluginLifecycle(Frontend &frontend, TextLookup text);
	~PluginLifecycle() override;
	bool load();
	void shutdown() noexcept;
	void openManagementWindow();
	StateStore *stateStore() const noexcept;

private:
	class Runtime;
	static void frontendEvent(FrontendEvent event, void *cookie) noexcept;
	void handleEvent(FrontendEvent event);
	Frontend &frontend_;
	TextLookup text_;
	std::unique_ptr<Runtime> runtime_;
	bool callbackRegistered_ = false;
	bool dockRegistered_ = false;
	bool shuttingDown_ = false;
};

} // namespace gost
