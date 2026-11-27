#pragma once

#include "core/model/Models.h"

namespace planthub::core {

struct ChannelCapabilities {
    std::vector<LogicalChannel> soilChannels;
    std::vector<LogicalChannel> pumpChannels;
};

enum class ConfigError {
    None,
    UnsupportedSchema,
    InvalidSystemInterval,
    InvalidRetention,
    InvalidUtcOffset,
    InvalidId,
    DuplicateId,
    EmptyName,
    UnavailableChannel,
    DuplicateChannel,
    InvalidSoilCalibration,
    InvalidPumpFlow,
    InvalidPumpLimits,
    UnknownSoilSensor,
    UnknownZone,
    UnknownPump,
    SensorAlreadyBound,
    PumpAlreadyBound,
    InvalidRepresentativeSensor,
    TooFewGroupMembers,
    DuplicateGroupMember,
    InvalidAutoMode,
    InvalidDoseOrLimit,
    InvalidScheduleMinute,
    DuplicateScheduleMinute,
    InvalidMoistureThreshold,
    InvalidConsecutiveCount,
    InvalidCooldown,
};

enum class ConfigEntity { System, Plant, SoilSensor, Pump, Zone, Group };

struct ConfigValidation {
    ConfigError error = ConfigError::None;
    ConfigEntity entity = ConfigEntity::System;
    std::optional<EntityId> id = std::nullopt;
};

ConfigValidation validateConfig(const SystemConfig& config, const ChannelCapabilities& channels);

enum class ZoneReadiness {
    Ready,
    InvalidConfig,
    UnknownZone,
    MissingPumpCalibration,
    NoEnabledPlants,
    MissingPumpLimits,
    MissingDoseLimits,
    IncompleteSchedule,
    IncompleteSmartRule,
    MissingRepresentativeCalibration,
    DoseExceedsLimit,
    InvalidDoseDuration,
    InvalidSource,
};

// 评估指定来源的配置就绪条件。模式切换、请求参数及运行时保护由后续用例判断。
ZoneReadiness checkZoneReadiness(const SystemConfig& config, const ChannelCapabilities& channels,
                                 EntityId zoneId, IrrigationSource source);

}  // namespace planthub::core
