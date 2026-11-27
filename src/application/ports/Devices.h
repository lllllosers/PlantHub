#pragma once

#include "core/irrigation/Irrigation.h"

namespace planthub::application {

class Clock {
public:
    virtual ~Clock() = default;
    virtual core::ClockSample now() const = 0;
};

enum class Acquisition { Success, Missing, Failed, Invalid };

struct SoilSample {
    core::ClockSample captured;
    Acquisition status = Acquisition::Missing;
    std::optional<core::AdcCount> raw;
};

struct AirSample {
    core::ClockSample captured;
    Acquisition status = Acquisition::Missing;
    std::optional<float> temperatureC;
    std::optional<float> humidityPercent;
};

class SensorSource {
public:
    virtual ~SensorSource() = default;
    // 同一通道在本启动周期以原始单调时刻标识采样。空值表示未返回采样。
    virtual std::optional<SoilSample> readSoil(core::LogicalChannel channel) = 0;
    virtual std::optional<AirSample> readAir() = 0;
};

struct PumpStarted {
    core::ExecutionId executionId;
    core::ClockSample started;
};

struct PumpFault {
    core::ExecutionId executionId;
};

using PumpEvent = std::variant<PumpStarted, PumpFault, core::PumpStopped>;

class PumpController {
public:
    virtual ~PumpController() = default;
    // 命令与事件查询均须立即返回，实际动作由设备侧执行。
    // 许可只能消费一次。启动未发生时也须提供可靠的关泵证据。
    virtual void start(const core::StartPermit& permit) = 0;
    // 始终可达，独立于启动队列。ID 为零时全局停止并撤销所有尚未执行的许可。
    // 指定 ID 时也须撤销该许可，直到确认关闭前仍可报告真实的迟到启动。
    virtual void priorityStop(core::StopRequest request) = 0;
    virtual bool allPumpsVerifiedOff() const = 0;
    // 按设备发生顺序提供事件。可靠关泵确认封闭该执行的启动路径。
    virtual std::optional<PumpEvent> pollEvent() = 0;
};

}  // namespace planthub::application
