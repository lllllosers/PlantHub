#include "application/services/IrrigationScheduler.h"

#include <utility>

namespace planthub::application {

IrrigationScheduler::IrrigationScheduler(core::ChannelCapabilities channels, const Clock& clock, SensorSource& sensors,
                                         PumpController& pump, ConfigStore& configStore, RuntimeStateStore& runtimeStore,
                                         HistorySink& history)
    : channels_(std::move(channels)), clock_(clock), configuration_(configStore, channels_),
      monitoring_(sensors, clock, history), irrigation_(channels_, configuration_, clock, pump, runtimeStore, history) {}

void IrrigationScheduler::tick() {
    static const core::SystemConfig monitoringDefaults;
    auto now = clock_.now();
    irrigation_.refresh(now);
    const auto* snapshot = configuration_.current();
    if (snapshot && !configurationAvailable_) {
        configurationAvailable_ = true;
        monitoring_.reset(std::nullopt);
    }
    const auto& config = snapshot ? snapshot->config : monitoringDefaults;
    const bool sampled = monitoring_.collect(config, now.monotonicMs);
    if (sampled) {
        now = clock_.now();
        irrigation_.refresh(now);
        if (irrigation_.bootStatus() == BootStatus::Ready) {
            for (const auto& zone : config.zones) {
                if (zone.rule.mode == core::AutoMode::Smart && zone.representativeSensorId) {
                    irrigation_.engine_->observeSoil(config, zone.id,
                                                     monitoring_.freshSoil(*zone.representativeSensorId));
                }
            }
        }
    }
    irrigation_.drive(clock_.now());
    // 普通历史写入安排在本轮执行保护、设备反馈及关键保存之后。
    monitoring_.writeHistory(config, now.monotonicMs);
}

ConfigResult IrrigationScheduler::replaceConfiguration(core::SystemConfig next) {
    irrigation_.refresh(clock_.now());
    if (irrigation_.bootStatus() != BootStatus::Ready) {
        return {ConfigChange::Unavailable};
    }
    if (!irrigation_.canConfigure()) {
        return {ConfigChange::Busy};
    }
    const auto result = configuration_.replace(std::move(next), irrigation_.engine_->state());
    if (result.status == ConfigChange::Applied) {
        irrigation_.engine_->resetAutomatic();
        monitoring_.reset(clock_.now().monotonicMs);
    }
    return result;
}

}  // namespace planthub::application
