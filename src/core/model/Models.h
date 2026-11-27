#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <variant>
#include <vector>

namespace planthub::core {

using EntityId = std::uint32_t;
using LogicalChannel = std::uint16_t;
using AdcCount = std::int32_t;
using VolumeMl = std::uint32_t;
using DurationMs = std::uint32_t;
using MonotonicMs = std::uint64_t;
using UtcSeconds = std::int64_t;

struct Plant {
    EntityId id;
    std::string name;
    bool enabled = true;
    std::optional<EntityId> soilSensorId;
    std::optional<EntityId> zoneId;
};

struct SoilCalibration {
    AdcCount dryRaw;
    AdcCount wetRaw;

    bool isValid() const { return dryRaw != wetRaw; }
};

struct SoilSensor {
    EntityId id;
    std::string name;
    LogicalChannel channel;
    std::optional<SoilCalibration> calibration;
};

struct PumpChannel {
    EntityId id;
    std::string name;
    LogicalChannel channel;
    std::optional<float> flowMlPerMin;
    std::optional<DurationMs> minDurationMs;
    std::optional<DurationMs> maxDurationMs;
};

enum class AutoMode { Off, Scheduled, Smart };

struct ScheduledDose {
    std::uint16_t minuteOfDay;
    std::optional<VolumeMl> targetVolumeMl;
};

struct IrrigationRule {
    AutoMode mode = AutoMode::Off;
    std::optional<VolumeMl> singleLimitMl;
    std::optional<VolumeMl> dailyLimitMl;
    std::vector<ScheduledDose> schedule;
    std::optional<float> lowMoisturePercent;
    std::uint32_t consecutiveLowReadings = 3;
    std::optional<VolumeMl> smartDoseMl;
    std::optional<DurationMs> cooldownMs;
};

struct IrrigationZone {
    EntityId id;
    std::string name;
    EntityId pumpId;
    std::optional<EntityId> representativeSensorId;
    IrrigationRule rule;
};

struct IrrigationGroup {
    EntityId id;
    std::string name;
    std::vector<EntityId> zoneIds;
};

enum class ReadingStatus { Missing, AcquisitionFailed, Invalid, Uncalibrated, OutOfRange, Stale, Valid };

struct SoilReading {
    EntityId sensorId;
    std::optional<AdcCount> raw;
    std::optional<float> moisturePercent;
    ReadingStatus status = ReadingStatus::Missing;
};

struct AirReading {
    std::optional<float> temperatureC;
    std::optional<float> humidityPercent;
    std::optional<float> vpdKPa;
    ReadingStatus status = ReadingStatus::Missing;
};

struct SensorReading {
    MonotonicMs sampledAtMs;
    std::optional<UtcSeconds> sampledAtUtcSeconds;
    std::variant<SoilReading, AirReading> data;
};

enum class IrrigationSource { Manual, Scheduled, Smart };
enum class IrrigationResult { Unknown, Completed, Stopped, Failed };

struct IrrigationRecord {
    EntityId zoneId;
    EntityId pumpId;
    IrrigationSource source;
    VolumeMl targetVolumeMl;
    float flowMlPerMin;
    std::optional<DurationMs> actualDurationMs;
    std::optional<double> estimatedVolumeMl;
    std::optional<UtcSeconds> startedAtUtcSeconds;
    IrrigationResult result = IrrigationResult::Unknown;
};

struct SystemConfig {
    static constexpr std::uint32_t currentSchemaVersion = 1;
    static constexpr std::uint8_t maxConcurrentPumps = 1;

    std::uint32_t schemaVersion = currentSchemaVersion;
    DurationMs sampleIntervalMs = 60'000;
    DurationMs dashboardRefreshMs = 5'000;
    DurationMs historySampleIntervalMs = 300'000;
    std::uint32_t monitoringRetentionDays = 14;
    std::uint32_t irrigationRetentionDays = 90;
    std::int16_t utcOffsetMinutes = 8 * 60;

    std::vector<Plant> plants;
    std::vector<SoilSensor> soilSensors;
    std::vector<PumpChannel> pumps;
    std::vector<IrrigationZone> zones;
    std::vector<IrrigationGroup> groups;

    MonotonicMs readingValidityMs() const {
        return 2 * static_cast<MonotonicMs>(sampleIntervalMs);
    }
};

}  // namespace planthub::core
