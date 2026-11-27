#include "application/services/MonitoringService.h"

namespace planthub::application {
namespace {

bool due(std::optional<core::MonotonicMs> last, core::MonotonicMs now, core::DurationMs interval) {
    return !last || (now >= *last && now - *last >= interval);
}

bool isNew(core::MonotonicMs stamp, const core::SensorReading* previous,
           std::optional<core::MonotonicMs> resetAt, core::MonotonicMs now) {
    return stamp <= now && (!previous || stamp > previous->sampledAtMs) && (!resetAt || stamp > *resetAt);
}

core::ReadingStatus readingStatus(Acquisition status) {
    switch (status) {
        case Acquisition::Success: return core::ReadingStatus::Valid;
        case Acquisition::Missing: return core::ReadingStatus::Missing;
        case Acquisition::Failed: return core::ReadingStatus::AcquisitionFailed;
        case Acquisition::Invalid: return core::ReadingStatus::Invalid;
    }
    return core::ReadingStatus::Invalid;
}

auto soilReading(MonitoringSnapshot& snapshot, core::EntityId id) {
    return std::find_if(snapshot.soil.begin(), snapshot.soil.end(), [id](const auto& reading) {
        return std::get<core::SoilReading>(reading.data).sensorId == id;
    });
}

void markStale(core::SensorReading& reading, core::MonotonicMs now, core::MonotonicMs validity) {
    if (now < reading.sampledAtMs || now - reading.sampledAtMs > validity) {
        std::visit([](auto& data) { data.status = core::ReadingStatus::Stale; }, reading.data);
    }
}

}  // namespace

void MonitoringService::reset(std::optional<core::MonotonicMs> atMs) {
    latest_ = {};
    freshSoilIds_.clear();
    resetAt_ = atMs;
    lastCollection_.reset();
}

bool MonitoringService::collect(const core::SystemConfig& config, core::MonotonicMs now) {
    validityMs_ = config.readingValidityMs();
    if (!due(lastCollection_, now, config.sampleIntervalMs) || (resetAt_ && now <= *resetAt_)) {
        return false;
    }
    lastCollection_ = now;
    freshSoilIds_.clear();
    for (const auto& sensor : config.soilSensors) {
        if (!sensor.channel) {
            continue;
        }
        const auto sample = sensors_.readSoil(*sensor.channel);
        const auto old = soilReading(latest_, sensor.id);
        const auto* previous = old == latest_.soil.end() ? nullptr : &*old;
        if (!sample || !isNew(sample->captured.monotonicMs, previous, resetAt_, clock_.now().monotonicMs)) {
            continue;
        }
        const auto data = core::calculateSoilReading(sensor, sample->raw, readingStatus(sample->status));
        const core::SensorReading reading{sample->captured.monotonicMs, sample->captured.utc, data};
        if (previous) {
            *old = reading;
        } else {
            latest_.soil.push_back(reading);
        }
        freshSoilIds_.push_back(sensor.id);
    }
    const auto air = sensors_.readAir();
    if (air && isNew(air->captured.monotonicMs, latest_.air ? &*latest_.air : nullptr,
                     resetAt_, clock_.now().monotonicMs)) {
        latest_.air = core::SensorReading{air->captured.monotonicMs, air->captured.utc,
            core::calculateAirReading(air->temperatureC, air->humidityPercent, readingStatus(air->status))};
    }
    return true;
}

const core::SensorReading* MonitoringService::freshSoil(core::EntityId id) const {
    if (std::find(freshSoilIds_.begin(), freshSoilIds_.end(), id) == freshSoilIds_.end()) {
        return nullptr;
    }
    const auto item = std::find_if(latest_.soil.begin(), latest_.soil.end(), [id](const auto& reading) {
        return std::get<core::SoilReading>(reading.data).sensorId == id;
    });
    return item == latest_.soil.end() ? nullptr : &*item;
}

MonitoringSnapshot MonitoringService::latest() const {
    auto snapshot = latest_;
    const auto now = clock_.now().monotonicMs;
    for (auto& reading : snapshot.soil) {
        markStale(reading, now, validityMs_);
    }
    if (snapshot.air) {
        markStale(*snapshot.air, now, validityMs_);
    }
    return snapshot;
}

void MonitoringService::writeHistory(const core::SystemConfig& config, core::MonotonicMs now) {
    if (due(lastHistory_, now, config.historySampleIntervalMs)) {
        lastHistory_ = now;
        historyFailed_ = !history_.writeMonitoring(latest()) || historyFailed_;
    }
}

}  // namespace planthub::application
