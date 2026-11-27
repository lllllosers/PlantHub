#include <limits>

#include <unity.h>

#include "core/calibration/Calculations.h"

using namespace planthub::core;

namespace {

using Status = ReadingStatus;
constexpr float infinity = std::numeric_limits<float>::infinity();
constexpr float nan = std::numeric_limits<float>::quiet_NaN();

void assertSoil(const SoilSensor& sensor, AdcCount raw, float percent, Status status = Status::Valid) {
    const auto reading = calculateSoilReading(sensor, raw);
    TEST_ASSERT_EQUAL_UINT32(sensor.id, reading.sensorId);
    TEST_ASSERT_EQUAL_INT(static_cast<int>(status), static_cast<int>(reading.status));
    TEST_ASSERT_TRUE(reading.raw.has_value());
    TEST_ASSERT_EQUAL_INT32(raw, *reading.raw);
    TEST_ASSERT_TRUE(reading.moisturePercent.has_value());
    TEST_ASSERT_FLOAT_WITHIN(0.001f, percent, *reading.moisturePercent);
}

void test_soil_calibration_supports_both_directions_and_clamping() {
    const SoilSensor forward{1, "Forward", 0, SoilCalibration{0, 200}};
    const SoilSensor reverse{2, "Reverse", 1, SoilCalibration{200, 0}};
    assertSoil(forward, 0, 0.0f);
    assertSoil(forward, 200, 100.0f);
    assertSoil(forward, 50, 25.0f);
    assertSoil(forward, -1, 0.0f, Status::OutOfRange);
    assertSoil(forward, 201, 100.0f, Status::OutOfRange);
    assertSoil(reverse, 200, 0.0f);
    assertSoil(reverse, 0, 100.0f);
    assertSoil(reverse, 50, 75.0f);
    assertSoil(reverse, 201, 0.0f, Status::OutOfRange);
    assertSoil(reverse, -1, 100.0f, Status::OutOfRange);
}

void test_soil_calibration_handles_full_adc_count_range() {
    constexpr auto minRaw = std::numeric_limits<AdcCount>::min();
    constexpr auto maxRaw = std::numeric_limits<AdcCount>::max();
    assertSoil({1, "Wide", 0, SoilCalibration{minRaw, maxRaw}}, 0, 50.0f);
    assertSoil({1, "Wide", 0, SoilCalibration{maxRaw, minRaw}}, minRaw, 100.0f);
    assertSoil({1, "Adjacent", 0, SoilCalibration{maxRaw - 1, maxRaw}}, minRaw, 0.0f, Status::OutOfRange);
}

void test_soil_reading_distinguishes_missing_invalid_and_sensor_states() {
    SoilSensor sensor{1, "Soil", 0, std::nullopt};
    auto reading = calculateSoilReading(sensor, 0);
    TEST_ASSERT_TRUE(reading.status == Status::Uncalibrated);
    TEST_ASSERT_TRUE(reading.raw == 0);
    TEST_ASSERT_FALSE(reading.moisturePercent.has_value());

    sensor.calibration = SoilCalibration{5, 5};
    reading = calculateSoilReading(sensor, 5);
    TEST_ASSERT_TRUE(reading.status == Status::Invalid);
    TEST_ASSERT_FALSE(reading.moisturePercent.has_value());

    sensor.calibration = SoilCalibration{0, 100};
    reading = calculateSoilReading(sensor, std::nullopt);
    TEST_ASSERT_TRUE(reading.status == Status::Missing);
    TEST_ASSERT_FALSE(reading.raw.has_value());
    for (const auto status : {Status::AcquisitionFailed, Status::Invalid, Status::OutOfRange, Status::Stale}) {
        reading = calculateSoilReading(sensor, 10, status);
        TEST_ASSERT_TRUE(reading.status == status);
        TEST_ASSERT_TRUE(reading.raw == 10);
        TEST_ASSERT_FALSE(reading.moisturePercent.has_value());
    }
}

void test_air_vpd_matches_reference_and_saturation() {
    const auto reading = calculateAirReading(15.0f, 50.0f);
    TEST_ASSERT_TRUE(reading.status == Status::Valid);
    TEST_ASSERT_TRUE(reading.vpdKPa.has_value());
    // FAO-56 第 3 章示例 3 给出 15 °C 的饱和水汽压约为 1.705 kPa。
    TEST_ASSERT_FLOAT_WITHIN(0.0005f, 0.8525f, *reading.vpdKPa);
    const auto saturated = calculateAirReading(25.0f, 100.0f);
    TEST_ASSERT_TRUE(saturated.status == Status::Valid);
    TEST_ASSERT_TRUE(saturated.vpdKPa.has_value());
    TEST_ASSERT_FLOAT_WITHIN(0.00001f, 0.0f, *saturated.vpdKPa);
    const auto dryAir = calculateAirReading(0.0f, 0.0f);
    TEST_ASSERT_TRUE(dryAir.vpdKPa.has_value());
    TEST_ASSERT_FLOAT_WITHIN(0.00001f, 0.6108f, *dryAir.vpdKPa);
    TEST_ASSERT_TRUE(calculateAirReading(-40.0f, 50.0f).status == Status::Valid);
    TEST_ASSERT_TRUE(calculateAirReading(60.0f, 50.0f).status == Status::Valid);
}

void test_air_vpd_rejects_invalid_input_and_preserves_missing_time() {
    for (const float temperature : {-237.3f, -273.15f, -41.0f, 61.0f, infinity, -infinity, nan}) {
        const auto reading = calculateAirReading(temperature, 50.0f);
        TEST_ASSERT_TRUE(reading.status == Status::Invalid);
        TEST_ASSERT_FALSE(reading.vpdKPa.has_value());
    }
    for (const float humidity : {-1.0f, 101.0f, infinity, nan}) {
        const auto reading = calculateAirReading(25.0f, humidity);
        TEST_ASSERT_TRUE(reading.status == Status::Invalid);
        TEST_ASSERT_FALSE(reading.vpdKPa.has_value());
    }
    const auto missing = calculateAirReading(std::nullopt, 50.0f);
    TEST_ASSERT_TRUE(missing.status == Status::Missing);
    TEST_ASSERT_FALSE(missing.vpdKPa.has_value());
    const auto failed = calculateAirReading(25.0f, 50.0f, Status::AcquisitionFailed);
    TEST_ASSERT_TRUE(failed.status == Status::AcquisitionFailed);
    TEST_ASSERT_FALSE(failed.vpdKPa.has_value());

    const SensorReading sample{0, std::nullopt, calculateAirReading(25.0f, 50.0f)};
    TEST_ASSERT_EQUAL_UINT64(0, sample.sampledAtMs);
    TEST_ASSERT_FALSE(sample.sampledAtUtcSeconds.has_value());
    TEST_ASSERT_TRUE(std::get<AirReading>(sample.data).status == Status::Valid);
}

void test_pump_flow_uses_collected_volume_and_milliseconds() {
    const auto first = calibratePumpFlow(125.5, 30'000);
    const auto second = calibratePumpFlow(300, 45'000);
    TEST_ASSERT_TRUE(first.has_value());
    TEST_ASSERT_TRUE(second.has_value());
    TEST_ASSERT_FLOAT_WITHIN(0.001f, 251.0f, *first);
    TEST_ASSERT_FLOAT_WITHIN(0.001f, 400.0f, *second);
}

void test_pump_calibration_rejects_invalid_values_and_unrepresentable_flow() {
    for (const double invalid : {0.0, -1.0, static_cast<double>(infinity), static_cast<double>(nan)}) {
        TEST_ASSERT_FALSE(calibratePumpFlow(invalid, 1'000).has_value());
        TEST_ASSERT_FALSE(calibratePumpFlow(100, invalid).has_value());
    }
    TEST_ASSERT_FALSE(calibratePumpFlow(std::numeric_limits<double>::max(), 1).has_value());
    TEST_ASSERT_FALSE(calibratePumpFlow(1e39, 60'000).has_value());
    TEST_ASSERT_FALSE(calibratePumpFlow(1e-50, 60'000).has_value());
}

void assertDuration(VolumeMl target, const PumpChannel& pump, DurationError error,
                    std::optional<DurationMs> expected = std::nullopt) {
    const auto result = calculateTargetDuration(target, pump);
    TEST_ASSERT_EQUAL_INT(static_cast<int>(error), static_cast<int>(result.error));
    TEST_ASSERT_TRUE(result.durationMs == expected);
}

void test_durations_use_each_pumps_flow_and_round_down() {
    const PumpChannel slow{1, "Slow", 0, calibratePumpFlow(100, 60'000), 1, 90'000};
    const PumpChannel fast{2, "Fast", 1, calibratePumpFlow(200, 60'000), 1, 90'000};
    assertDuration(100, slow, DurationError::None, 60'000);
    assertDuration(100, fast, DurationError::None, 30'000);

    PumpChannel pump{3, "Fractional", 2, 7, 8'571, 8'572};
    assertDuration(1, pump, DurationError::None, 8'571);
    pump.maxDurationMs = 8'571;
    assertDuration(1, pump, DurationError::TooLong);
    pump.minDurationMs = 8'572;
    pump.maxDurationMs = 20'000;
    assertDuration(1, pump, DurationError::TooShort);
    pump.flowMlPerMin = 60;
    pump.minDurationMs = 1'000;
    pump.maxDurationMs = 1'000;
    assertDuration(1, pump, DurationError::None, 1'000);
}

void test_duration_rejects_invalid_input_and_checks_integer_boundary() {
    PumpChannel pump{1, "Pump", 0, 60'000, std::nullopt, std::nullopt};
    constexpr auto maxDuration = std::numeric_limits<DurationMs>::max();
    assertDuration(0, pump, DurationError::InvalidTarget);
    assertDuration(maxDuration, pump, DurationError::None, maxDuration);
    pump.maxDurationMs = maxDuration;
    assertDuration(maxDuration, pump, DurationError::None, maxDuration);
    pump.maxDurationMs.reset();
    pump.flowMlPerMin = 30'000;
    assertDuration(maxDuration, pump, DurationError::Overflow);
    pump.flowMlPerMin = std::numeric_limits<float>::denorm_min();
    assertDuration(1, pump, DurationError::Overflow);
    pump.flowMlPerMin = std::numeric_limits<float>::max();
    assertDuration(1, pump, DurationError::TooShort);
    pump.flowMlPerMin.reset();
    assertDuration(1, pump, DurationError::Uncalibrated);
    for (const float invalid : {0.0f, -1.0f, nan, infinity}) {
        pump.flowMlPerMin = invalid;
        assertDuration(1, pump, DurationError::InvalidFlow);
    }
    pump.flowMlPerMin = 100;
    pump.minDurationMs = 0;
    assertDuration(1, pump, DurationError::InvalidLimits);
}

void test_duration_respects_limits_when_division_rounds_to_integer() {
    constexpr VolumeMl target = 4'283'333'600;
    constexpr DurationMs roundedMs = 4'111'999'999;
    // 理论时长略大于 roundedMs，double 除法会舍入为该整毫秒。
    PumpChannel pump{1, "Pump", 0, 62'500.00390625f, 1, roundedMs};
    assertDuration(target, pump, DurationError::TooLong);
    pump.maxDurationMs.reset();
    assertDuration(target, pump, DurationError::None, roundedMs);
    // 此处理论时长略小于整毫秒，先舍入再向下取整会多运行 1 ms。
    assertDuration(4'292'666'936, pump, DurationError::None, 4'120'960'000);
}

void test_estimates_distinguish_zero_from_unknown() {
    const auto estimate = estimateVolumeMl(150.0f, 10'000);
    const auto zero = estimateVolumeMl(150.0f, 0);
    TEST_ASSERT_TRUE(estimate.has_value());
    TEST_ASSERT_TRUE(zero.has_value());
    TEST_ASSERT_FLOAT_WITHIN(0.001f, 25.0f, static_cast<float>(*estimate));
    TEST_ASSERT_FLOAT_WITHIN(0.001f, 0.0f, static_cast<float>(*zero));
    TEST_ASSERT_FALSE(estimateVolumeMl(infinity, 10'000).has_value());
    TEST_ASSERT_FALSE(estimateVolumeMl(0, 0).has_value());
    IrrigationRecord record{10, 20, IrrigationSource::Manual, 100, 150, std::nullopt, std::nullopt, std::nullopt};
    TEST_ASSERT_FALSE(estimateVolumeMl(record.flowMlPerMin, record.actualDurationMs).has_value());
    TEST_ASSERT_FALSE(record.estimatedVolumeMl.has_value());
    TEST_ASSERT_TRUE(record.result == IrrigationResult::Unknown);
}

}  // namespace

void setUp() {}
void tearDown() {}

int main() {
    UNITY_BEGIN();
    RUN_TEST(test_soil_calibration_supports_both_directions_and_clamping);
    RUN_TEST(test_soil_calibration_handles_full_adc_count_range);
    RUN_TEST(test_soil_reading_distinguishes_missing_invalid_and_sensor_states);
    RUN_TEST(test_air_vpd_matches_reference_and_saturation);
    RUN_TEST(test_air_vpd_rejects_invalid_input_and_preserves_missing_time);
    RUN_TEST(test_pump_flow_uses_collected_volume_and_milliseconds);
    RUN_TEST(test_pump_calibration_rejects_invalid_values_and_unrepresentable_flow);
    RUN_TEST(test_durations_use_each_pumps_flow_and_round_down);
    RUN_TEST(test_duration_rejects_invalid_input_and_checks_integer_boundary);
    RUN_TEST(test_duration_respects_limits_when_division_rounds_to_integer);
    RUN_TEST(test_estimates_distinguish_zero_from_unknown);
    return UNITY_END();
}
