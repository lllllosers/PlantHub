#pragma once

#include "core/irrigation/Irrigation.h"

namespace planthub::application {

enum class LoadStatus { Loaded, FirstUse, Missing, Corrupt, Incompatible, ReadFailed };

template <typename Snapshot>
struct LoadResult {
    LoadStatus status;
    std::optional<Snapshot> snapshot;
};

struct EntityWatermarks {
    core::EntityId plants = 0;
    core::EntityId soilSensors = 0;
    core::EntityId pumps = 0;
    core::EntityId zones = 0;
    core::EntityId groups = 0;
};

struct ConfigSnapshot {
    core::SystemConfig config;
    EntityWatermarks allocated;
};

struct ReceiptRetention {
    core::RequestId requestId;
    std::optional<core::UtcSeconds> reliableEndUtc;
};

struct RuntimeSnapshot {
    core::CriticalState critical;
    std::vector<ReceiptRetention> receiptRetention;
    bool initializing = false;
};

class ConfigStore {
public:
    virtual ~ConfigStore() = default;
    // FirstUse 必须来自可信的首次初始化标志。读取异常或已有数据丢失须单独报告。
    virtual LoadResult<ConfigSnapshot> load() = 0;
    virtual bool save(const ConfigSnapshot& snapshot) = 0;
};

struct SaveCompletion {
    core::SaveId id;
    bool reliable;
};

class RuntimeStateStore {
public:
    virtual ~RuntimeStateStore() = default;
    virtual LoadResult<RuntimeSnapshot> load() = 0;
    // 提交与轮询均须立即返回。复制快照并报告本启动周期内的完成 ID。
    // 失败保留最后可靠的完整快照。
    virtual void startSave(core::SaveId id, const RuntimeSnapshot& snapshot) = 0;
    virtual std::optional<SaveCompletion> pollSave() = 0;
};

struct MonitoringSnapshot {
    std::vector<core::SensorReading> soil;
    std::optional<core::SensorReading> air;
};

class HistorySink {
public:
    virtual ~HistorySink() = default;
    virtual bool writeMonitoring(const MonitoringSnapshot& snapshot) = 0;
    virtual bool writeIrrigation(const core::IrrigationRecord& record) = 0;
};

}  // namespace planthub::application
