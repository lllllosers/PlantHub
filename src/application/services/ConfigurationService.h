#pragma once

#include "application/ports/Storage.h"

namespace planthub::application {

enum class ConfigChange { Applied, Unavailable, Busy, Invalid, Identity, AccountingBinding, SaveFailed };

struct ConfigResult {
    ConfigChange status;
    core::ConfigValidation validation = {};
};

class ConfigurationService {
public:
    LoadStatus loadStatus() const { return status_; }
    const ConfigSnapshot* current() const { return current_ ? &*current_ : nullptr; }

private:
    friend class IrrigationScheduler;
    friend class IrrigationService;

    ConfigurationService(ConfigStore& store, const core::ChannelCapabilities& channels)
        : store_(store), channels_(channels) {}

    void load();
    bool initialize();
    ConfigResult replace(core::SystemConfig next, const core::CriticalState& critical);
    static bool preservesUsage(const core::SystemConfig& config, const core::CriticalState& critical);

    ConfigStore& store_;
    const core::ChannelCapabilities& channels_;
    LoadStatus status_ = LoadStatus::ReadFailed;
    std::optional<ConfigSnapshot> current_;
};

}  // namespace planthub::application
