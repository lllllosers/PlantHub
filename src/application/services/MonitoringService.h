#pragma once

#include "application/ports/Devices.h"
#include "application/ports/Storage.h"

namespace planthub::application {

class MonitoringService {
public:
    MonitoringSnapshot latest() const;
    bool historyWriteFailed() const { return historyFailed_; }

private:
    friend class IrrigationScheduler;

    MonitoringService(SensorSource& sensors, const Clock& clock, HistorySink& history)
        : sensors_(sensors), clock_(clock), history_(history) {}

    void reset(std::optional<core::MonotonicMs> atMs);
    bool collect(const core::SystemConfig& config, core::MonotonicMs now);
    const core::SensorReading* freshSoil(core::EntityId id) const;
    void writeHistory(const core::SystemConfig& config, core::MonotonicMs now);

    SensorSource& sensors_;
    const Clock& clock_;
    HistorySink& history_;
    MonitoringSnapshot latest_;
    std::vector<core::EntityId> freshSoilIds_;
    core::MonotonicMs validityMs_ = 0;
    std::optional<core::MonotonicMs> resetAt_;
    std::optional<core::MonotonicMs> lastCollection_;
    std::optional<core::MonotonicMs> lastHistory_;
    bool historyFailed_ = false;
};

}  // namespace planthub::application
