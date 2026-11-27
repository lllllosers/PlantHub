#include "application/services/ConfigurationService.h"

#include <utility>

namespace planthub::application {
namespace {

template <typename Entity>
bool validWatermark(const std::vector<Entity>& entities, core::EntityId watermark) {
    return std::all_of(entities.begin(), entities.end(), [watermark](const auto& item) {
        return item.id <= watermark;
    });
}

template <typename Entity>
bool allocate(const std::vector<Entity>& current, const std::vector<Entity>& next, core::EntityId& watermark) {
    std::vector<core::EntityId> added;
    for (const auto& item : next) {
        if (!core::findById(current, item.id)) {
            added.push_back(item.id);
        }
    }
    std::sort(added.begin(), added.end());
    for (const auto id : added) {
        if (id <= watermark || id - watermark != 1) {
            return false;
        }
        watermark = id;
    }
    return true;
}

}  // namespace

void ConfigurationService::load() {
    auto result = store_.load();
    status_ = result.status;
    if (status_ != LoadStatus::Loaded) {
        return;
    }
    if (!result.snapshot) {
        status_ = LoadStatus::Corrupt;
        return;
    }
    const auto& [config, allocated] = *result.snapshot;
    if (config.schemaVersion != core::SystemConfig::currentSchemaVersion) {
        status_ = LoadStatus::Incompatible;
        return;
    }
    if (core::validateConfig(config, channels_).error != core::ConfigError::None ||
        !validWatermark(config.plants, allocated.plants) ||
        !validWatermark(config.soilSensors, allocated.soilSensors) ||
        !validWatermark(config.pumps, allocated.pumps) ||
        !validWatermark(config.zones, allocated.zones) ||
        !validWatermark(config.groups, allocated.groups)) {
        status_ = LoadStatus::Corrupt;
        return;
    }
    current_ = std::move(result.snapshot);
}

bool ConfigurationService::initialize() {
    if (status_ == LoadStatus::Loaded) {
        return true;
    }
    if (status_ != LoadStatus::FirstUse || !store_.save({})) {
        return false;
    }
    current_.emplace();
    status_ = LoadStatus::Loaded;
    return true;
}

bool ConfigurationService::preservesUsage(const core::SystemConfig& config, const core::CriticalState& critical) {
    return std::all_of(critical.usage.begin(), critical.usage.end(), [&](const auto& entry) {
        const auto* zone = core::findById(config.zones, entry.zoneId);
        const auto* pump = zone ? core::findById(config.pumps, zone->pumpId) : nullptr;
        return pump && pump->channel == entry.channel;
    });
}

ConfigResult ConfigurationService::replace(core::SystemConfig next, const core::CriticalState& critical) {
    const auto validation = core::validateConfig(next, channels_);
    if (validation.error != core::ConfigError::None) {
        return {ConfigChange::Invalid, validation};
    }
    if (!preservesUsage(next, critical)) {
        return {ConfigChange::AccountingBinding};
    }

    const auto& previous = current_->config;
    auto allocated = current_->allocated;
    if (!allocate(previous.plants, next.plants, allocated.plants) ||
        !allocate(previous.soilSensors, next.soilSensors, allocated.soilSensors) ||
        !allocate(previous.pumps, next.pumps, allocated.pumps) ||
        !allocate(previous.zones, next.zones, allocated.zones) ||
        !allocate(previous.groups, next.groups, allocated.groups)) {
        return {ConfigChange::Identity};
    }

    ConfigSnapshot candidate{std::move(next), allocated};
    if (!store_.save(candidate)) {
        return {ConfigChange::SaveFailed};
    }
    current_ = std::move(candidate);
    return {ConfigChange::Applied};
}

}  // namespace planthub::application
