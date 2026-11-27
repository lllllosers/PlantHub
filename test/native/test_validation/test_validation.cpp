#include <limits>

#include <unity.h>

#include "core/validation/ConfigValidation.h"

using namespace planthub::core;

namespace {

const ChannelCapabilities twoChannels{{0, 1}, {0, 1}};

SystemConfig twoPlantConfig() {
    SystemConfig config;
    config.soilSensors = {{11, "Soil 1", 0, SoilCalibration{0, 100}}, {12, "Soil 2", 1, SoilCalibration{100, 0}}};
    config.pumps = {{21, "Pump 1", 0, 120, 100, 120'000}, {22, "Pump 2", 1, 60, 100, 120'000}};
    config.plants = {{1, "Plant 1", true, 11, 31}, {2, "Plant 2", true, 12, 32}};
    config.zones = {{31, "Zone 1", 21, 11, {}}, {32, "Zone 2", 22, 12, {}}};
    for (auto& zone : config.zones) {
        zone.rule.singleLimitMl = 100;
        zone.rule.dailyLimitMl = 300;
    }
    config.groups = {{41, "Group", {32, 31}}};
    return config;
}

void assertConfig(const SystemConfig& config, ConfigError error,
                  ConfigEntity entity = ConfigEntity::System, std::optional<EntityId> id = std::nullopt) {
    const auto result = validateConfig(config, twoChannels);
    TEST_ASSERT_EQUAL_INT(static_cast<int>(error), static_cast<int>(result.error));
    TEST_ASSERT_EQUAL_INT(static_cast<int>(entity), static_cast<int>(result.entity));
    TEST_ASSERT_TRUE(id == result.id);
}

void assertReadiness(const SystemConfig& config, IrrigationSource source, ZoneReadiness expected, EntityId id = 31) {
    const auto result = checkZoneReadiness(config, twoChannels, id, source);
    TEST_ASSERT_EQUAL_INT(static_cast<int>(expected), static_cast<int>(result));
}

void test_model_defaults_and_derived_reading_validity() {
    SystemConfig config;
    assertConfig(config, ConfigError::None);
    TEST_ASSERT_EQUAL_UINT32(SystemConfig::currentSchemaVersion, config.schemaVersion);
    TEST_ASSERT_EQUAL_UINT32(60'000, config.sampleIntervalMs);
    TEST_ASSERT_EQUAL_UINT32(5'000, config.dashboardRefreshMs);
    TEST_ASSERT_EQUAL_UINT32(300'000, config.historySampleIntervalMs);
    TEST_ASSERT_EQUAL_UINT32(14, config.monitoringRetentionDays);
    TEST_ASSERT_EQUAL_UINT32(90, config.irrigationRetentionDays);
    TEST_ASSERT_EQUAL_INT(480, config.utcOffsetMinutes);
    TEST_ASSERT_EQUAL_UINT(1, SystemConfig::maxConcurrentPumps);
    TEST_ASSERT_EQUAL_UINT64(120'000, config.readingValidityMs());
    config.sampleIntervalMs = 21'000;
    TEST_ASSERT_EQUAL_UINT64(42'000, config.readingValidityMs());
    config.sampleIntervalMs = std::numeric_limits<DurationMs>::max();
    TEST_ASSERT_EQUAL_UINT64(8'589'934'590ULL, config.readingValidityMs());
    const IrrigationRule rule;
    TEST_ASSERT_TRUE(rule.mode == AutoMode::Off);
    TEST_ASSERT_EQUAL_UINT32(3, rule.consecutiveLowReadings);
    TEST_ASSERT_FALSE(rule.smartDoseMl.has_value());
}

void test_two_plant_configuration_allows_shared_zones_and_ordered_groups() {
    auto config = twoPlantConfig();
    assertConfig(config, ConfigError::None);
    config.zones[1].representativeSensorId.reset();
    config.plants[1].name = "Renamed";
    config.plants[1].zoneId = 31;
    config.groups.push_back({42, "Other group", {31, 32}});
    assertConfig(config, ConfigError::None);
    TEST_ASSERT_EQUAL_UINT32(2, config.plants[1].id);
    TEST_ASSERT_EQUAL_UINT32(32, config.groups[0].zoneIds[0]);
    TEST_ASSERT_EQUAL_UINT32(31, config.groups[0].zoneIds[1]);
}

void test_channel_capabilities_can_be_sparse_and_support_more_than_two_plants() {
    auto config = twoPlantConfig();
    config.soilSensors.push_back({13, "Soil 3", 7, std::nullopt});
    config.pumps.push_back({23, "Pump 3", 9, std::nullopt, std::nullopt, std::nullopt});
    config.zones.push_back({33, "Zone 3", 23, 13, {}});
    config.plants.push_back({3, "Plant 3", true, 13, 33});
    assertConfig(config, ConfigError::UnavailableChannel, ConfigEntity::SoilSensor, 13);
    TEST_ASSERT_TRUE(validateConfig(config, {{0, 1, 7}, {0, 1}}).error == ConfigError::UnavailableChannel);
    TEST_ASSERT_TRUE(validateConfig(config, {{0, 1, 7}, {0, 1, 9}}).error == ConfigError::None);
}

void test_already_configured_relationships_and_legal_preparation_states() {
    auto config = twoPlantConfig();
    config.plants.push_back({3, "Unassigned", true, std::nullopt, std::nullopt});
    config.zones[0].representativeSensorId.reset();
    config.plants[0].soilSensorId.reset();
    config.soilSensors[0].calibration.reset();
    config.pumps[0].flowMlPerMin.reset();
    config.zones[0].rule.mode = AutoMode::Smart;
    assertConfig(config, ConfigError::None);
    assertReadiness(config, IrrigationSource::Manual, ZoneReadiness::MissingPumpCalibration);
    config.pumps[0].flowMlPerMin = 120;
    config.pumps[0].maxDurationMs.reset();
    assertReadiness(config, IrrigationSource::Manual, ZoneReadiness::MissingPumpLimits);
    config.pumps[0].maxDurationMs = 10'000;
    config.pumps[0].minDurationMs.reset();
    assertReadiness(config, IrrigationSource::Manual, ZoneReadiness::MissingPumpLimits);
    config.pumps[0].minDurationMs = 100;
    config.zones[0].rule.dailyLimitMl.reset();
    assertReadiness(config, IrrigationSource::Manual, ZoneReadiness::MissingDoseLimits);
    config.zones[0].rule.dailyLimitMl = 300;
    assertReadiness(config, IrrigationSource::Manual, ZoneReadiness::Ready);
    assertReadiness(config, IrrigationSource::Manual, ZoneReadiness::UnknownZone, 999);
}

void test_config_rejects_invalid_identity_and_duplicate_channels() {
    auto config = twoPlantConfig();
    config.plants[1].id = 0;
    assertConfig(config, ConfigError::InvalidId, ConfigEntity::Plant, 0);
    config.plants[1].id = 1;
    assertConfig(config, ConfigError::DuplicateId, ConfigEntity::Plant, 1);
    config.plants[1].id = 2;
    config.plants[1].name = " \t\n";
    assertConfig(config, ConfigError::EmptyName, ConfigEntity::Plant, 2);
    config.plants[1].name = "Plant 2";
    config.soilSensors[1].channel = 0;
    assertConfig(config, ConfigError::DuplicateChannel, ConfigEntity::SoilSensor, 12);
    config.soilSensors[1].channel = 1;
    config.pumps[1].channel = 0;
    assertConfig(config, ConfigError::DuplicateChannel, ConfigEntity::Pump, 22);
}

void test_config_rejects_dangling_references_and_duplicate_bindings() {
    const auto good = twoPlantConfig();
    auto config = good;
    config.plants[1].soilSensorId = 99;
    assertConfig(config, ConfigError::UnknownSoilSensor, ConfigEntity::Plant, 2);
    config.plants[1].soilSensorId = 11;
    assertConfig(config, ConfigError::SensorAlreadyBound, ConfigEntity::Plant, 2);
    config = good;
    config.plants[1].zoneId = 99;
    assertConfig(config, ConfigError::UnknownZone, ConfigEntity::Plant, 2);
    config = good;
    config.zones[1].pumpId = 99;
    assertConfig(config, ConfigError::UnknownPump, ConfigEntity::Zone, 32);
    config.zones[1].pumpId = 21;
    assertConfig(config, ConfigError::PumpAlreadyBound, ConfigEntity::Zone, 32);
}

void test_representative_sensor_requires_enabled_plant_in_same_zone() {
    auto config = twoPlantConfig();
    config.zones[0].representativeSensorId = 99;
    assertConfig(config, ConfigError::UnknownSoilSensor, ConfigEntity::Zone, 31);
    config.zones[0].representativeSensorId = 12;
    assertConfig(config, ConfigError::InvalidRepresentativeSensor, ConfigEntity::Zone, 31);
    config.zones[0].representativeSensorId = 11;
    config.plants[0].enabled = false;
    assertConfig(config, ConfigError::InvalidRepresentativeSensor, ConfigEntity::Zone, 31);
    config.zones[0].representativeSensorId.reset();
    assertConfig(config, ConfigError::None);
}

void test_groups_require_multiple_distinct_known_zones() {
    auto config = twoPlantConfig();
    config.groups[0].zoneIds.clear();
    assertConfig(config, ConfigError::TooFewGroupMembers, ConfigEntity::Group, 41);
    config.groups[0].zoneIds = {31};
    assertConfig(config, ConfigError::TooFewGroupMembers, ConfigEntity::Group, 41);
    config.groups[0].zoneIds = {31, 99};
    assertConfig(config, ConfigError::UnknownZone, ConfigEntity::Group, 41);
    config.groups[0].zoneIds = {31, 32, 31};
    assertConfig(config, ConfigError::DuplicateGroupMember, ConfigEntity::Group, 41);
}

void test_system_validation_checks_schema_interval_retention_and_offset() {
    const SystemConfig defaults;
    auto config = defaults;
    config.schemaVersion++;
    assertConfig(config, ConfigError::UnsupportedSchema);
    config = defaults;
    config.sampleIntervalMs = 0;
    assertConfig(config, ConfigError::InvalidSystemInterval);
    config = defaults;
    config.irrigationRetentionDays = 0;
    assertConfig(config, ConfigError::InvalidRetention);
    config = defaults;
    config.utcOffsetMinutes = -1'440;
    assertConfig(config, ConfigError::InvalidUtcOffset);
    config.utcOffsetMinutes = -1'439;
    assertConfig(config, ConfigError::None);
    config.utcOffsetMinutes = 1'440;
    assertConfig(config, ConfigError::InvalidUtcOffset);
}

void test_config_rejects_bad_calibration_and_pump_limits() {
    auto config = twoPlantConfig();
    config.soilSensors[0].calibration = SoilCalibration{0, 0};
    assertConfig(config, ConfigError::InvalidSoilCalibration, ConfigEntity::SoilSensor, 11);
    config.soilSensors[0].calibration.reset();
    for (const float invalid : {0.0f, -1.0f, std::numeric_limits<float>::quiet_NaN(),
                                std::numeric_limits<float>::infinity()}) {
        config.pumps[0].flowMlPerMin = invalid;
        assertConfig(config, ConfigError::InvalidPumpFlow, ConfigEntity::Pump, 21);
    }
    config.pumps[0].flowMlPerMin.reset();
    config.pumps[0].maxDurationMs = 0;
    assertConfig(config, ConfigError::InvalidPumpLimits, ConfigEntity::Pump, 21);
    config.pumps[0].maxDurationMs = 99;
    assertConfig(config, ConfigError::InvalidPumpLimits, ConfigEntity::Pump, 21);
}

void test_rule_rejects_illegal_values_even_when_off() {
    auto config = twoPlantConfig();
    auto& rule = config.zones[0].rule;
    rule.mode = static_cast<AutoMode>(99);
    assertConfig(config, ConfigError::InvalidAutoMode, ConfigEntity::Zone, 31);
    rule.mode = AutoMode::Off;
    rule.singleLimitMl = 0;
    assertConfig(config, ConfigError::InvalidDoseOrLimit, ConfigEntity::Zone, 31);
    rule.singleLimitMl.reset();
    for (const float invalid : {-1.0f, 100.01f, std::numeric_limits<float>::quiet_NaN(),
                                std::numeric_limits<float>::infinity()}) {
        rule.lowMoisturePercent = invalid;
        assertConfig(config, ConfigError::InvalidMoistureThreshold, ConfigEntity::Zone, 31);
    }
    rule.lowMoisturePercent = 0;
    assertConfig(config, ConfigError::None);
    rule.lowMoisturePercent = 100;
    assertConfig(config, ConfigError::None);
    rule.consecutiveLowReadings = 0;
    assertConfig(config, ConfigError::InvalidConsecutiveCount, ConfigEntity::Zone, 31);
    rule.consecutiveLowReadings = 3;
    rule.cooldownMs = 0;
    assertConfig(config, ConfigError::InvalidCooldown, ConfigEntity::Zone, 31);
}

void test_schedule_checks_time_boundaries_duplicate_minutes_and_zero_dose() {
    auto config = twoPlantConfig();
    auto& schedule = config.zones[0].rule.schedule;
    schedule = {{1'439, 50}, {0, 50}};
    assertConfig(config, ConfigError::None);
    schedule.push_back({1'440, 10});
    assertConfig(config, ConfigError::InvalidScheduleMinute, ConfigEntity::Zone, 31);
    schedule.back() = {1'439, 20};
    assertConfig(config, ConfigError::DuplicateScheduleMinute, ConfigEntity::Zone, 31);
    schedule.back() = {1, 0};
    assertConfig(config, ConfigError::InvalidDoseOrLimit, ConfigEntity::Zone, 31);
}

void test_scheduled_readiness_checks_every_dose_without_requiring_sensor() {
    auto config = twoPlantConfig();
    auto& zone = config.zones[0];
    zone.representativeSensorId.reset();
    assertReadiness(config, IrrigationSource::Scheduled, ZoneReadiness::IncompleteSchedule);
    zone.rule.schedule = {{0, 50}, {60, std::nullopt}};
    assertConfig(config, ConfigError::None);
    assertReadiness(config, IrrigationSource::Scheduled, ZoneReadiness::IncompleteSchedule);
    zone.rule.schedule[1].targetVolumeMl = 101;
    assertReadiness(config, IrrigationSource::Scheduled, ZoneReadiness::DoseExceedsLimit);
    zone.rule.schedule[1].targetVolumeMl = 100;
    zone.rule.dailyLimitMl = 99;
    assertReadiness(config, IrrigationSource::Scheduled, ZoneReadiness::DoseExceedsLimit);
    zone.rule.dailyLimitMl = 300;
    config.pumps[0].maxDurationMs = 49'999;
    assertReadiness(config, IrrigationSource::Scheduled, ZoneReadiness::InvalidDoseDuration);
    config.pumps[0].maxDurationMs = 50'000;
    assertReadiness(config, IrrigationSource::Scheduled, ZoneReadiness::Ready);
    TEST_ASSERT_TRUE(zone.rule.mode == AutoMode::Off);
}

void test_smart_readiness_requires_rule_and_calibrated_representative() {
    auto config = twoPlantConfig();
    auto& zone = config.zones[0];
    assertReadiness(config, IrrigationSource::Smart, ZoneReadiness::IncompleteSmartRule);
    zone.rule.lowMoisturePercent = 30.0f;
    zone.rule.smartDoseMl = 50;
    zone.rule.cooldownMs = 600'000;
    assertReadiness(config, IrrigationSource::Smart, ZoneReadiness::Ready);
    config.soilSensors[0].calibration.reset();
    assertReadiness(config, IrrigationSource::Smart, ZoneReadiness::MissingRepresentativeCalibration);
    zone.representativeSensorId.reset();
    assertReadiness(config, IrrigationSource::Smart, ZoneReadiness::MissingRepresentativeCalibration);
    assertReadiness(config, IrrigationSource::Manual, ZoneReadiness::Ready);
    zone.representativeSensorId = 99;
    assertReadiness(config, IrrigationSource::Smart, ZoneReadiness::InvalidConfig);
}

}  // namespace

void setUp() {}
void tearDown() {}

int main() {
    UNITY_BEGIN();
    RUN_TEST(test_model_defaults_and_derived_reading_validity);
    RUN_TEST(test_two_plant_configuration_allows_shared_zones_and_ordered_groups);
    RUN_TEST(test_channel_capabilities_can_be_sparse_and_support_more_than_two_plants);
    RUN_TEST(test_already_configured_relationships_and_legal_preparation_states);
    RUN_TEST(test_config_rejects_invalid_identity_and_duplicate_channels);
    RUN_TEST(test_config_rejects_dangling_references_and_duplicate_bindings);
    RUN_TEST(test_representative_sensor_requires_enabled_plant_in_same_zone);
    RUN_TEST(test_groups_require_multiple_distinct_known_zones);
    RUN_TEST(test_system_validation_checks_schema_interval_retention_and_offset);
    RUN_TEST(test_config_rejects_bad_calibration_and_pump_limits);
    RUN_TEST(test_rule_rejects_illegal_values_even_when_off);
    RUN_TEST(test_schedule_checks_time_boundaries_duplicate_minutes_and_zero_dose);
    RUN_TEST(test_scheduled_readiness_checks_every_dose_without_requiring_sensor);
    RUN_TEST(test_smart_readiness_requires_rule_and_calibrated_representative);
    return UNITY_END();
}
