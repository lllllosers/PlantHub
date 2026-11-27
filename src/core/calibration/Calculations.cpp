#include "core/calibration/Calculations.h"

#include <algorithm>
#include <cmath>
#include <limits>

namespace planthub::core {
namespace {

constexpr double millisecondsPerMinute = 60'000;
// 家庭环境的计算域。硬件测量范围由设备阶段验收。
constexpr float minVpdTemperatureC = -40.0f;
constexpr float maxVpdTemperatureC = 60.0f;

}  // namespace

SoilReading calculateSoilReading(const SoilSensor& sensor, std::optional<AdcCount> raw,
                                ReadingStatus inputStatus) {
    SoilReading reading{sensor.id, raw, std::nullopt, inputStatus};
    if (inputStatus != ReadingStatus::Valid) {
        return reading;
    }
    if (!raw) {
        reading.status = ReadingStatus::Missing;
        return reading;
    }
    if (!sensor.calibration) {
        reading.status = ReadingStatus::Uncalibrated;
        return reading;
    }
    if (!sensor.calibration->isValid()) {
        reading.status = ReadingStatus::Invalid;
        return reading;
    }

    const double dry = sensor.calibration->dryRaw;
    const double wet = sensor.calibration->wetRaw;
    const double fraction = (*raw - dry) / (wet - dry);
    if (fraction < 0.0 || fraction > 1.0) {
        reading.status = ReadingStatus::OutOfRange;
    }
    reading.moisturePercent = static_cast<float>(std::clamp(fraction, 0.0, 1.0) * 100.0);
    return reading;
}

AirReading calculateAirReading(std::optional<float> temperatureC, std::optional<float> humidityPercent,
                               ReadingStatus inputStatus) {
    AirReading reading{temperatureC, humidityPercent, std::nullopt, inputStatus};
    if (inputStatus != ReadingStatus::Valid) {
        return reading;
    }
    if (!temperatureC || !humidityPercent) {
        reading.status = ReadingStatus::Missing;
        return reading;
    }
    const double temperature = *temperatureC;
    const double humidity = *humidityPercent;
    if (!std::isfinite(temperature) || temperature < minVpdTemperatureC || temperature > maxVpdTemperatureC ||
        !std::isfinite(humidity) || humidity < 0.0 || humidity > 100.0) {
        reading.status = ReadingStatus::Invalid;
        return reading;
    }

    const double saturationKPa = 0.6108 * std::exp(17.27 * temperature / (temperature + 237.3));
    const double vpdKPa = saturationKPa * (1.0 - humidity / 100.0);
    if (!std::isfinite(vpdKPa) || vpdKPa < 0.0 || vpdKPa > std::numeric_limits<float>::max()) {
        reading.status = ReadingStatus::Invalid;
        return reading;
    }
    reading.vpdKPa = static_cast<float>(vpdKPa);
    return reading;
}

bool isValidPumpFlow(float flowMlPerMin) {
    return std::isfinite(flowMlPerMin) && flowMlPerMin > 0.0f;
}

bool areValidPumpLimits(const PumpChannel& pump) {
    return pump.minDurationMs != 0 && pump.maxDurationMs != 0 &&
           (!pump.minDurationMs || !pump.maxDurationMs || *pump.minDurationMs <= *pump.maxDurationMs);
}

std::optional<float> calibratePumpFlow(double collectedVolumeMl, double calibrationDurationMs) {
    if (!std::isfinite(collectedVolumeMl) || collectedVolumeMl <= 0.0 ||
        !std::isfinite(calibrationDurationMs) || calibrationDurationMs <= 0.0) {
        return std::nullopt;
    }
    const double flow = collectedVolumeMl / calibrationDurationMs * millisecondsPerMinute;
    if (!std::isfinite(flow) || flow <= 0.0 || flow > std::numeric_limits<float>::max()) {
        return std::nullopt;
    }
    const auto storedFlow = static_cast<float>(flow);
    if (!isValidPumpFlow(storedFlow)) {
        return std::nullopt;
    }
    return storedFlow;
}

TargetDuration calculateTargetDuration(VolumeMl targetVolumeMl, const PumpChannel& pump) {
    if (targetVolumeMl == 0) {
        return {DurationError::InvalidTarget};
    }
    if (!pump.flowMlPerMin) {
        return {DurationError::Uncalibrated};
    }
    if (!isValidPumpFlow(*pump.flowMlPerMin)) {
        return {DurationError::InvalidFlow};
    }
    if (!areValidPumpLimits(pump)) {
        return {DurationError::InvalidLimits};
    }

    const double numerator = targetVolumeMl * millisecondsPerMinute;
    const double flow = *pump.flowMlPerMin;
    // 直接比较未取整时长。融合乘加保留整毫秒边界上的比较精度。
    if (pump.maxDurationMs && std::fma(-flow, *pump.maxDurationMs, numerator) > 0.0) {
        return {DurationError::TooLong};
    }
    constexpr double endOfIntegerRange = static_cast<double>(std::numeric_limits<DurationMs>::max()) + 1.0;
    if (std::fma(-flow, endOfIntegerRange, numerator) >= 0.0) {
        return {DurationError::Overflow};
    }
    double wholeMs = std::floor(numerator / flow);
    if (std::fma(-flow, wholeMs, numerator) < 0.0) {
        --wholeMs;
    }
    if (wholeMs < pump.minDurationMs.value_or(1)) {
        return {DurationError::TooShort};
    }
    return {DurationError::None, static_cast<DurationMs>(wholeMs)};
}

std::optional<double> estimateVolumeMl(float flowMlPerMin, std::optional<DurationMs> actualDurationMs) {
    if (!isValidPumpFlow(flowMlPerMin) || !actualDurationMs) {
        return std::nullopt;
    }
    return static_cast<double>(flowMlPerMin) * *actualDurationMs / millisecondsPerMinute;
}

}  // namespace planthub::core
