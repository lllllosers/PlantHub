#include "core/validation/ConfigValidation.h"

#include <algorithm>
#include <bitset>
#include <cmath>

#include "core/calibration/Calculations.h"

namespace planthub::core {
namespace {

constexpr int minutesPerDay = 24 * 60;

template <typename Entity>
ConfigError validateIdentity(const std::vector<Entity>& entities, const Entity& entity) {
    if (entity.id == 0) {
        return ConfigError::InvalidId;
    }
    if (findById(entities, entity.id) != &entity) {
        return ConfigError::DuplicateId;
    }
    if (entity.name.find_first_not_of(" \t\r\n\f\v") == std::string::npos) {
        return ConfigError::EmptyName;
    }
    return ConfigError::None;
}

template <typename Entity>
ConfigError validateChannel(const std::vector<LogicalChannel>& available, const std::vector<Entity>& entities,
                            const Entity& entity) {
    if (!entity.channel || std::find(available.begin(), available.end(), *entity.channel) == available.end()) {
        return ConfigError::UnavailableChannel;
    }
    const auto first = std::find_if(entities.begin(), entities.end(), [&entity](const Entity& candidate) {
        return candidate.channel == entity.channel;
    });
    return &*first == &entity ? ConfigError::None : ConfigError::DuplicateChannel;
}

ConfigError validateRule(const IrrigationRule& rule) {
    switch (rule.mode) {
        case AutoMode::Off:
        case AutoMode::Scheduled:
        case AutoMode::Smart:
            break;
        default:
            return ConfigError::InvalidAutoMode;
    }
    if (rule.singleLimitMl == 0 || rule.dailyLimitMl == 0 || rule.smartDoseMl == 0) {
        return ConfigError::InvalidDoseOrLimit;
    }
    if (rule.lowMoisturePercent) {
        const float threshold = *rule.lowMoisturePercent;
        if (!std::isfinite(threshold) || threshold < 0.0f || threshold > 100.0f) {
            return ConfigError::InvalidMoistureThreshold;
        }
    }
    if (rule.consecutiveLowReadings == 0) {
        return ConfigError::InvalidConsecutiveCount;
    }
    if (rule.cooldownMs == 0) {
        return ConfigError::InvalidCooldown;
    }
    std::bitset<minutesPerDay> seenMinutes;
    for (const auto& dose : rule.schedule) {
        if (dose.minuteOfDay >= minutesPerDay) {
            return ConfigError::InvalidScheduleMinute;
        }
        if (seenMinutes[dose.minuteOfDay]) {
            return ConfigError::DuplicateScheduleMinute;
        }
        if (dose.targetVolumeMl == 0) {
            return ConfigError::InvalidDoseOrLimit;
        }
        seenMinutes.set(dose.minuteOfDay);
    }
    return ConfigError::None;
}

ZoneReadiness checkDose(const IrrigationRule& rule, VolumeMl doseMl, const PumpChannel& pump) {
    if (doseMl > *rule.singleLimitMl || doseMl > *rule.dailyLimitMl) {
        return ZoneReadiness::DoseExceedsLimit;
    }
    if (calculateTargetDuration(doseMl, pump).error != DurationError::None) {
        return ZoneReadiness::InvalidDoseDuration;
    }
    return ZoneReadiness::Ready;
}

}  // namespace

ConfigValidation validateConfig(const SystemConfig& config, const ChannelCapabilities& channels) {
    if (config.schemaVersion != SystemConfig::currentSchemaVersion) {
        return {ConfigError::UnsupportedSchema};
    }
    if (config.sampleIntervalMs == 0 || config.dashboardRefreshMs == 0 || config.historySampleIntervalMs == 0) {
        return {ConfigError::InvalidSystemInterval};
    }
    if (config.monitoringRetentionDays == 0 || config.irrigationRetentionDays == 0) {
        return {ConfigError::InvalidRetention};
    }
    if (config.utcOffsetMinutes <= -minutesPerDay || config.utcOffsetMinutes >= minutesPerDay) {
        return {ConfigError::InvalidUtcOffset};
    }
    const auto& window = config.autoIrrigationWindow;
    if (window.startMinute >= window.endMinute || window.endMinute > minutesPerDay) {
        return {ConfigError::InvalidAutoIrrigationWindow};
    }

    for (const auto& sensor : config.soilSensors) {
        if (const auto error = validateIdentity(config.soilSensors, sensor); error != ConfigError::None) {
            return {error, ConfigEntity::SoilSensor, sensor.id};
        }
        if (const auto error = validateChannel(channels.soilChannels, config.soilSensors, sensor);
            error != ConfigError::None) {
            return {error, ConfigEntity::SoilSensor, sensor.id};
        }
        if (sensor.calibration && !sensor.calibration->isValid()) {
            return {ConfigError::InvalidSoilCalibration, ConfigEntity::SoilSensor, sensor.id};
        }
    }
    for (const auto& pump : config.pumps) {
        if (const auto error = validateIdentity(config.pumps, pump); error != ConfigError::None) {
            return {error, ConfigEntity::Pump, pump.id};
        }
        if (const auto error = validateChannel(channels.pumpChannels, config.pumps, pump);
            error != ConfigError::None) {
            return {error, ConfigEntity::Pump, pump.id};
        }
        if (pump.flowMlPerMin && !isValidPumpFlow(*pump.flowMlPerMin)) {
            return {ConfigError::InvalidPumpFlow, ConfigEntity::Pump, pump.id};
        }
        if (!areValidPumpLimits(pump)) {
            return {ConfigError::InvalidPumpLimits, ConfigEntity::Pump, pump.id};
        }
    }
    for (const auto& plant : config.plants) {
        if (const auto error = validateIdentity(config.plants, plant); error != ConfigError::None) {
            return {error, ConfigEntity::Plant, plant.id};
        }
        if (plant.zoneId && !findById(config.zones, *plant.zoneId)) {
            return {ConfigError::UnknownZone, ConfigEntity::Plant, plant.id};
        }
        if (plant.soilSensorId) {
            if (!findById(config.soilSensors, *plant.soilSensorId)) {
                return {ConfigError::UnknownSoilSensor, ConfigEntity::Plant, plant.id};
            }
            const auto first = std::find_if(config.plants.begin(), config.plants.end(), [&plant](const Plant& other) {
                return other.soilSensorId == plant.soilSensorId;
            });
            if (&*first != &plant) {
                return {ConfigError::SensorAlreadyBound, ConfigEntity::Plant, plant.id};
            }
        }
    }
    for (const auto& zone : config.zones) {
        if (const auto error = validateIdentity(config.zones, zone); error != ConfigError::None) {
            return {error, ConfigEntity::Zone, zone.id};
        }
        if (!findById(config.pumps, zone.pumpId)) {
            return {ConfigError::UnknownPump, ConfigEntity::Zone, zone.id};
        }
        const auto first = std::find_if(config.zones.begin(), config.zones.end(), [&zone](const IrrigationZone& other) {
            return other.pumpId == zone.pumpId;
        });
        if (&*first != &zone) {
            return {ConfigError::PumpAlreadyBound, ConfigEntity::Zone, zone.id};
        }
        if (zone.representativeSensorId) {
            if (!findById(config.soilSensors, *zone.representativeSensorId)) {
                return {ConfigError::UnknownSoilSensor, ConfigEntity::Zone, zone.id};
            }
            const auto representsZone = [&zone](const Plant& plant) {
                return plant.enabled && plant.zoneId == zone.id && plant.soilSensorId == zone.representativeSensorId;
            };
            if (std::none_of(config.plants.begin(), config.plants.end(), representsZone)) {
                return {ConfigError::InvalidRepresentativeSensor, ConfigEntity::Zone, zone.id};
            }
        }
        if (const auto error = validateRule(zone.rule); error != ConfigError::None) {
            return {error, ConfigEntity::Zone, zone.id};
        }
    }
    for (const auto& group : config.groups) {
        if (const auto error = validateIdentity(config.groups, group); error != ConfigError::None) {
            return {error, ConfigEntity::Group, group.id};
        }
        if (group.zoneIds.size() < 2) {
            return {ConfigError::TooFewGroupMembers, ConfigEntity::Group, group.id};
        }
        for (auto member = group.zoneIds.begin(); member != group.zoneIds.end(); ++member) {
            if (!findById(config.zones, *member)) {
                return {ConfigError::UnknownZone, ConfigEntity::Group, group.id};
            }
            if (std::find(group.zoneIds.begin(), member, *member) != member) {
                return {ConfigError::DuplicateGroupMember, ConfigEntity::Group, group.id};
            }
        }
    }
    return {};
}

ZoneReadiness checkZoneReadiness(const SystemConfig& config, const ChannelCapabilities& channels,
                                 EntityId zoneId, IrrigationSource source) {
    if (validateConfig(config, channels).error != ConfigError::None) {
        return ZoneReadiness::InvalidConfig;
    }
    const auto* zone = findById(config.zones, zoneId);
    if (!zone) {
        return ZoneReadiness::UnknownZone;
    }
    const auto enabledInZone = [zoneId](const Plant& plant) { return plant.enabled && plant.zoneId == zoneId; };
    if (std::none_of(config.plants.begin(), config.plants.end(), enabledInZone)) {
        return ZoneReadiness::NoEnabledPlants;
    }

    const auto& pump = *findById(config.pumps, zone->pumpId);
    if (!pump.flowMlPerMin) {
        return ZoneReadiness::MissingPumpCalibration;
    }
    if (!pump.minDurationMs || !pump.maxDurationMs) {
        return ZoneReadiness::MissingPumpLimits;
    }
    const auto& rule = zone->rule;
    if (!rule.singleLimitMl || !rule.dailyLimitMl) {
        return ZoneReadiness::MissingDoseLimits;
    }

    switch (source) {
        case IrrigationSource::Manual:
            return ZoneReadiness::Ready;
        case IrrigationSource::Scheduled:
            if (rule.schedule.empty()) {
                return ZoneReadiness::IncompleteSchedule;
            }
            for (const auto& dose : rule.schedule) {
                if (!dose.targetVolumeMl) {
                    return ZoneReadiness::IncompleteSchedule;
                }
                const auto readiness = checkDose(rule, *dose.targetVolumeMl, pump);
                if (readiness != ZoneReadiness::Ready) {
                    return readiness;
                }
            }
            return ZoneReadiness::Ready;
        case IrrigationSource::Smart:
            if (!rule.lowMoisturePercent || !rule.smartDoseMl || !rule.cooldownMs) {
                return ZoneReadiness::IncompleteSmartRule;
            }
            if (!zone->representativeSensorId ||
                !findById(config.soilSensors, *zone->representativeSensorId)->calibration) {
                return ZoneReadiness::MissingRepresentativeCalibration;
            }
            return checkDose(rule, *rule.smartDoseMl, pump);
    }
    return ZoneReadiness::InvalidSource;
}

}  // namespace planthub::core
