#pragma once

#include "core/model/Models.h"

namespace planthub::core {

// 采集状态由调用方提供，包括硬件校验和采样新鲜度。
SoilReading calculateSoilReading(const SoilSensor& sensor, std::optional<AdcCount> raw,
                                ReadingStatus inputStatus = ReadingStatus::Valid);
AirReading calculateAirReading(std::optional<float> temperatureC, std::optional<float> humidityPercent,
                               ReadingStatus inputStatus = ReadingStatus::Valid);

bool isValidPumpFlow(float flowMlPerMin);
bool areValidPumpLimits(const PumpChannel& pump);
std::optional<float> calibratePumpFlow(double collectedVolumeMl, double calibrationDurationMs);

enum class DurationError { None, InvalidTarget, Uncalibrated, InvalidFlow, InvalidLimits, TooShort, TooLong, Overflow };

struct TargetDuration {
    DurationError error;
    std::optional<DurationMs> durationMs = std::nullopt;
};

TargetDuration calculateTargetDuration(VolumeMl targetVolumeMl, const PumpChannel& pump);
std::optional<double> estimateVolumeMl(float flowMlPerMin, std::optional<DurationMs> actualDurationMs);

}  // namespace planthub::core
