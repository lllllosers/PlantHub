#pragma once

#include "application/services/IrrigationService.h"
#include "application/services/MonitoringService.h"

namespace planthub::application {

class IrrigationScheduler {
public:
    IrrigationScheduler(core::ChannelCapabilities channels, const Clock& clock, SensorSource& sensors,
                        PumpController& pump, ConfigStore& configStore, RuntimeStateStore& runtimeStore,
                        HistorySink& history);
    IrrigationScheduler(const IrrigationScheduler&) = delete;
    IrrigationScheduler& operator=(const IrrigationScheduler&) = delete;

    // 调用方按约一秒间隔推进，采集、历史、保护及自动调度各自检查时限。
    void tick();
    ConfigResult replaceConfiguration(core::SystemConfig next);

    const ConfigurationService& configuration() const { return configuration_; }
    const MonitoringService& monitoring() const { return monitoring_; }
    IrrigationService& irrigation() { return irrigation_; }

private:
    core::ChannelCapabilities channels_;
    const Clock& clock_;
    ConfigurationService configuration_;
    MonitoringService monitoring_;
    IrrigationService irrigation_;
    bool configurationAvailable_ = false;
};

}  // namespace planthub::application
