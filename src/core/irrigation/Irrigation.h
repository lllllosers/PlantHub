
#pragma once

#include "core/calibration/Calculations.h"
#include "core/validation/ConfigValidation.h"

namespace planthub::core {

using LocalDay = std::int64_t;
using RequestId = std::uint64_t;
using SaveId = std::uint64_t;

struct LocalTime {
    LocalDay day = 0;
    std::uint16_t minute = 0;
    std::uint32_t secondsRemaining = 0;
};

std::optional<LocalTime> localTime(UtcSeconds utc, std::int16_t offsetMinutes);

struct ClockSample {
    MonotonicMs monotonicMs = 0;
    // 仅在调用方确认日期可信时提供 UTC。
    std::optional<UtcSeconds> utc;
};

struct Trigger {
    LocalDay day = 0;
    std::uint16_t minute = 0;
    MonotonicMs sampleMs = 0;
};

struct ZoneDose {
    EntityId zoneId = 0;
    VolumeMl volumeMl = 0;
};

struct IrrigationRequest {
    RequestId requestId = 0;
    IrrigationSource source = IrrigationSource::Manual;
    ZoneDose dose;
    std::optional<Trigger> trigger;
};

// 自动请求由来源、分区和触发凭据标识。手动请求（含分组）使用 requestId。
struct IrrigationPlan {
    ExecutionId executionId = 0;
    IrrigationRequest request;
    EntityId pumpId = 0;
    LogicalChannel channel = 0;
    float flowMlPerMin = 0;
    DurationMs plannedMs = 0;
    DurationMs maximumMs = 0;
    LocalDay quotaDay = 0;
};

enum class Phase { Idle, Preparing, AwaitingStart, Running, Stopping, Settling };
enum class RequestStage { Accepted, InProgress, Ended };
enum class Fault { None, UnknownDelivery, Device, ExcessDelivery, AccountingOverflow, IdentityExhausted };
enum class Recovery { FirstInitialization, Loaded, Missing, Corrupt, Incompatible };
enum class Rejection {
    None, InvalidConfig, NotReady, InvalidRequest, ModeMismatch, InvalidTime, CrossesMidnight,
    CriticalState, SaveFailed, FaultLocked, AutoPaused, Busy, StaleTrigger, Cooling,
    SingleLimit, DailyLimit, Duration, ClockRange, BindingChanged, Capacity, Duplicate, RequestConflict,
};

struct Decision {
    Rejection error = Rejection::None;
    ZoneReadiness readiness = ZoneReadiness::Ready;
    DurationError durationError = DurationError::None;
    std::optional<RequestStage> previousStage = std::nullopt;
    std::optional<IrrigationResult> previousResult = std::nullopt;
    explicit operator bool() const { return error == Rejection::None; }
};

struct ZoneUsage {
    EntityId zoneId = 0;
    LogicalChannel channel = 0;
    VolumeMl usedMl = 0;
};

struct QuotaDate {
    LocalDay day = 0;
    std::int16_t offsetMinutes = 0;
    std::optional<std::uint16_t> consumedThroughMinute;
    bool schedulesSuspended = false;
};

struct RecentWatering {
    EntityId zoneId = 0;
    LogicalChannel channel = 0;
};

struct ManualReceipt {
    RequestId requestId = 0;
    EntityId groupId = 0;
    std::vector<ZoneDose> doses;
    RequestStage stage = RequestStage::Accepted;
    IrrigationResult result = IrrigationResult::Unknown;
};

struct CriticalState {
    static constexpr std::size_t maxTrackedZones = 16;
    static constexpr std::size_t maxManualReceipts = 8;

    std::optional<QuotaDate> date;
    std::vector<ZoneUsage> usage;
    // 计划目标量即唯一的未结算预留。usedMl 只包含已结算水量。
    std::optional<IrrigationPlan> pending;
    ExecutionId lastExecutionId = 0;
    std::vector<RecentWatering> recentWatering;
    std::vector<ManualReceipt> manualReceipts;
    bool autoPaused = false;
    Fault fault = Fault::None;
};

struct SaveDraft {
    SaveId id = 0;
    CriticalState state;
};

struct StartPermit {
    IrrigationPlan plan;
    MonotonicMs issuedAtMs = 0;
    MonotonicMs plannedStopMs = 0;
    MonotonicMs hardStopMs = 0;
};

struct StopRequest {
    ExecutionId executionId = 0;
    StopReason reason = StopReason::None;
};

enum class StopEvidence { NeverStarted, Measured, Unknown };

struct PumpStopped {
    ExecutionId executionId = 0;
    MonotonicMs stoppedAtMs = 0;
    StopEvidence evidence = StopEvidence::Unknown;
    std::optional<DurationMs> durationMs;
};

// 单实例持有全局执行资源。所有方法由同一应用上下文顺序调用。
class Irrigation {
public:
    Irrigation(ChannelCapabilities channels, Recovery recovery, MonotonicMs bootMs,
               CriticalState state = {});
    Irrigation(const Irrigation&) = delete;
    Irrigation& operator=(const Irrigation&) = delete;

    const CriticalState& state() const { return state_; }
    Recovery recoveryStatus() const { return recovery_; }
    Phase phase() const { return phase_; }
    bool groupActive() const { return group_.has_value(); }
    std::optional<IrrigationPlan> activePlan() const;
    std::optional<VolumeMl> remaining(EntityId zoneId, VolumeMl dailyLimitMl) const;

    // 单次只允许一个保存，确认对应快照。失败后可提交最新候选重试。
    std::optional<SaveDraft> beginSave();
    bool finishSave(SaveId id, bool reliable);
    std::optional<IrrigationRecord> takeRecord();

    void observeTime(const SystemConfig& config, ClockSample sample);
    void observeSoil(const SystemConfig& config, EntityId zoneId, const SensorReading* reading);
    // 在影响规则、探头、采集间隔或自动模式的配置变化后调用。
    void resetAutomatic();

    Decision accept(const SystemConfig& config, const IrrigationRequest& request);
    Decision acceptAutomatic(const SystemConfig& config);
    Decision acceptGroup(const SystemConfig& config, RequestId id, EntityId groupId,
                         const std::vector<VolumeMl>& orderedDoses);
    Decision advanceGroup(const SystemConfig& config);
    bool forgetManualRequest(RequestId id);

    // 调度方须先提供当前时间、最新读数，并原子消费许可与处理优先停止。
    std::optional<StartPermit> issueStart(const SystemConfig& config);
    bool confirmStarted(ExecutionId id, MonotonicMs atMs, std::optional<UtcSeconds> utc = {});
    std::optional<StopRequest> pendingStop() const;
    void stopAll();
    bool reportDeviceFault(ExecutionId id);
    bool confirmStopped(PumpStopped event);
    bool resumeAutomatic();
    bool unlockFault(bool pumpsVerifiedOff);

private:
    struct Execution {
        std::optional<StartPermit> permit;
        std::optional<MonotonicMs> startedMs;
        std::optional<UtcSeconds> startedUtc;
        StopReason stopReason = StopReason::None;
    };

    struct GroupRun {
        RequestId id = 0;
        std::size_t member = 0;
    };

    struct SoilProgress {
        EntityId zoneId = 0;
        std::optional<MonotonicMs> lastSample = std::nullopt;
        std::uint32_t consecutive = 0;
        bool latestLow = false;
    };

    bool validState() const;
    bool available() const;
    bool dateReady() const;
    bool cooling(const IrrigationZone& zone, LogicalChannel channel, MonotonicMs atMs) const;
    void recoverPending();
    void observeSchedule(const SystemConfig& config, bool continuous, bool enteredNextMinute);
    void clearAutomatic();
    void cancelCandidate(EntityId zoneId, IrrigationSource source);
    void addCandidate(IrrigationRequest request);
    Decision check(const SystemConfig& config, const IrrigationRequest& request, IrrigationPlan& plan,
                   bool beforeStart = false) const;
    Decision reserve(IrrigationPlan plan);
    Decision duplicate(RequestId id, EntityId groupId, const std::vector<ZoneDose>& doses) const;
    void finishGroup(IrrigationResult result);
    void enforceDeadline(MonotonicMs now);
    void requestStop(StopReason reason);
    void settle(StopEvidence evidence, std::optional<DurationMs> duration, MonotonicMs endMs);
    void rememberWatering(const IrrigationPlan& plan, MonotonicMs endMs);

    ChannelCapabilities channels_;
    Recovery recovery_;
    CriticalState state_;
    Phase phase_ = Phase::Idle;
    std::optional<Execution> execution_;
    std::optional<GroupRun> group_;
    std::optional<IrrigationRecord> settlingRecord_;
    std::optional<IrrigationRecord> record_;
    bool dirty_ = false;
    bool saveFailed_ = false;
    SaveId lastSaveId_ = 0;
    std::optional<SaveId> inFlight_;

    MonotonicMs now_ = 0;
    MonotonicMs automaticAfterMs_ = 0;
    bool monotonicValid_ = true;
    bool timeAccepted_ = false;
    std::optional<ClockSample> previousClock_;
    std::optional<ClockSample> continuityAnchor_;
    std::optional<LocalTime> local_;
    std::optional<Trigger> scheduleWindow_;
    std::int16_t offsetMinutes_ = 0;
    std::vector<MonotonicMs> recentEnds_;
    std::vector<SoilProgress> soil_;
    std::vector<IrrigationRequest> candidates_;
};

}  // namespace planthub::core
