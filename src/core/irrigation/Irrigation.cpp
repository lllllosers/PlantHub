
#include "core/irrigation/Irrigation.h"

#include <limits>
#include <utility>

namespace planthub::core {
namespace {

bool finishesBefore(MonotonicMs plannedMs, std::uint32_t secondsRemaining) {
    // UTC 只有秒精度，扣掉当前秒中无法确认的部分。
    return secondsRemaining > 0 && plannedMs < (secondsRemaining - 1) * 1'000u;
}

template <typename Entries>
auto findReceipt(Entries& entries, RequestId id) {
    return std::find_if(entries.begin(), entries.end(), [id](const auto& entry) { return entry.requestId == id; });
}

bool sameDose(const ZoneDose& a, const ZoneDose& b) {
    return a.zoneId == b.zoneId && a.volumeMl == b.volumeMl;
}

bool sameRequest(const IrrigationRequest& a, const IrrigationRequest& b) {
    if (a.source != b.source || a.requestId != b.requestId || !sameDose(a.dose, b.dose) ||
        a.trigger.has_value() != b.trigger.has_value()) {
        return false;
    }
    return !a.trigger || (a.trigger->day == b.trigger->day && a.trigger->minute == b.trigger->minute &&
                          a.trigger->sampleMs == b.trigger->sampleMs);
}

bool samePlan(const IrrigationPlan& a, const IrrigationPlan& b) {
    return a.quotaDay == b.quotaDay && a.pumpId == b.pumpId && a.channel == b.channel &&
           a.flowMlPerMin == b.flowMlPerMin && a.plannedMs == b.plannedMs && a.maximumMs == b.maximumMs;
}

bool sameGroup(const SystemConfig& config, const ManualReceipt& receipt) {
    const auto* group = findById(config.groups, receipt.groupId);
    return group && std::equal(group->zoneIds.begin(), group->zoneIds.end(), receipt.doses.begin(),
                               receipt.doses.end(), [](EntityId zone, const auto& dose) { return zone == dose.zoneId; });
}

}  // namespace

std::optional<IrrigationPlan> Irrigation::activePlan() const {
    return state_.pending;
}

std::optional<VolumeMl> Irrigation::remaining(EntityId zoneId, VolumeMl dailyLimitMl) const {
    if (!dateReady() || phase_ == Phase::Settling || saveFailed_) {
        return std::nullopt;
    }
    const auto usage = std::find_if(state_.usage.begin(), state_.usage.end(),
                                    [zoneId](const auto& entry) { return entry.zoneId == zoneId; });
    std::uint64_t charged = usage == state_.usage.end() ? 0 : usage->usedMl;
    if (state_.pending && state_.pending->request.dose.zoneId == zoneId) {
        charged += state_.pending->request.dose.volumeMl;
    }
    return charged >= dailyLimitMl ? 0 : dailyLimitMl - static_cast<VolumeMl>(charged);
}

Decision Irrigation::check(const SystemConfig& config, const IrrigationRequest& request, IrrigationPlan& plan,
                           bool beforeStart) const {
    const auto readiness = checkZoneReadiness(config, channels_, request.dose.zoneId, request.source);
    if (readiness != ZoneReadiness::Ready) {
        return {readiness == ZoneReadiness::InvalidConfig ? Rejection::InvalidConfig : Rejection::NotReady, readiness};
    }
    if ((request.source == IrrigationSource::Manual && (!request.requestId || request.trigger)) ||
        (request.source != IrrigationSource::Manual && (request.requestId || !request.trigger)) ||
        request.dose.volumeMl == 0) {
        return {Rejection::InvalidRequest};
    }
    if (!available()) {
        return {Rejection::CriticalState};
    }
    if (saveFailed_) {
        return {Rejection::SaveFailed};
    }
    if (state_.fault != Fault::None) {
        return {Rejection::FaultLocked};
    }
    if (phase_ != (beforeStart ? Phase::Preparing : Phase::Idle)) {
        return {Rejection::Busy};
    }
    if (!dateReady() || config.utcOffsetMinutes != offsetMinutes_) {
        return {Rejection::InvalidTime};
    }

    const auto& zone = *findById(config.zones, request.dose.zoneId);
    const auto& rule = zone.rule;
    const auto& pump = *findById(config.pumps, zone.pumpId);
    const bool automatic = request.source != IrrigationSource::Manual;
    const auto& window = config.autoIrrigationWindow;
    if (automatic) {
        if (state_.autoPaused) {
            return {Rejection::AutoPaused};
        }
        if (!window.allows(local_->minute)) {
            return {Rejection::OutsideAutoWindow};
        }
    }
    if (request.source == IrrigationSource::Scheduled) {
        if (rule.mode != AutoMode::Scheduled) {
            return {Rejection::ModeMismatch};
        }
        const auto minute = request.trigger->minute;
        const auto item = std::find_if(rule.schedule.begin(), rule.schedule.end(),
                                       [minute](const auto& dose) { return dose.minuteOfDay == minute; });
        if (!scheduleWindow_ || state_.date->schedulesSuspended || request.trigger->day != scheduleWindow_->day ||
            minute != scheduleWindow_->minute || item == rule.schedule.end() ||
            item->targetVolumeMl != request.dose.volumeMl) {
            return {Rejection::StaleTrigger};
        }
    }
    if (request.source == IrrigationSource::Smart) {
        if (rule.mode != AutoMode::Smart) {
            return {Rejection::ModeMismatch};
        }
        if (cooling(zone, *pump.channel, now_)) {
            return {Rejection::Cooling};
        }
        const auto evidence = std::find_if(soil_.begin(), soil_.end(),
                                           [&zone](const auto& entry) { return entry.zoneId == zone.id; });
        const auto stamp = request.trigger->sampleMs;
        if (evidence == soil_.end() || !evidence->latestLow || evidence->lastSample != stamp ||
            stamp > now_ || now_ - stamp > config.readingValidityMs() ||
            rule.smartDoseMl != request.dose.volumeMl || request.trigger->day != local_->day) {
            return {Rejection::StaleTrigger};
        }
    }
    const auto usage = std::find_if(state_.usage.begin(), state_.usage.end(), [&](const auto& entry) {
        return entry.zoneId == zone.id || entry.channel == *pump.channel;
    });
    if (usage != state_.usage.end() && (usage->zoneId != zone.id || usage->channel != *pump.channel)) {
        return {Rejection::BindingChanged};
    }
    if (usage == state_.usage.end() && state_.usage.size() >= CriticalState::maxTrackedZones) {
        return {Rejection::Capacity};
    }
    if (state_.recentWatering.size() >= CriticalState::maxTrackedZones &&
        std::none_of(state_.recentWatering.begin(), state_.recentWatering.end(),
                     [&pump](const auto& entry) { return entry.channel == *pump.channel; })) {
        return {Rejection::Capacity};
    }
    if (request.dose.volumeMl > *rule.singleLimitMl) {
        return {Rejection::SingleLimit};
    }
    // 启动前复查时，当前执行的预留仍属于本次请求。
    const auto used = usage == state_.usage.end() ? 0 : usage->usedMl;
    if (used >= *rule.dailyLimitMl || request.dose.volumeMl > *rule.dailyLimitMl - used) {
        return {Rejection::DailyLimit};
    }
    const auto duration = calculateTargetDuration(request.dose.volumeMl, pump);
    if (duration.error != DurationError::None) {
        return {Rejection::Duration, ZoneReadiness::Ready, duration.error};
    }
    if (!finishesBefore(*duration.durationMs, local_->secondsRemaining)) {
        return {Rejection::CrossesMidnight};
    }
    if (automatic && window.enabled &&
        !finishesBefore(*duration.durationMs, local_->secondsRemaining - (24 * 60 - window.endMinute) * 60u)) {
        return {Rejection::OutsideAutoWindow};
    }
    if (*pump.maxDurationMs > std::numeric_limits<MonotonicMs>::max() - now_) {
        return {Rejection::ClockRange};
    }
    plan = {0, request, pump.id, *pump.channel, *pump.flowMlPerMin, *duration.durationMs,
            *pump.maxDurationMs, state_.date->day};
    return {};
}

Decision Irrigation::reserve(IrrigationPlan plan) {
    if (state_.lastExecutionId == std::numeric_limits<ExecutionId>::max()) {
        state_.fault = Fault::IdentityExhausted;
        dirty_ = true;
        return {Rejection::FaultLocked};
    }
    const auto zoneId = plan.request.dose.zoneId;
    if (std::none_of(state_.usage.begin(), state_.usage.end(),
                     [zoneId](const auto& entry) { return entry.zoneId == zoneId; })) {
        state_.usage.push_back({zoneId, plan.channel, 0});
    }
    plan.executionId = ++state_.lastExecutionId;
    state_.pending = plan;
    execution_.emplace();
    phase_ = Phase::Preparing;
    dirty_ = true;
    return {};
}

Decision Irrigation::duplicate(RequestId id, EntityId groupId, const std::vector<ZoneDose>& doses) const {
    const auto found = findReceipt(state_.manualReceipts, id);
    if (found == state_.manualReceipts.end()) {
        return {};
    }
    if (found->groupId != groupId ||
        !std::equal(found->doses.begin(), found->doses.end(), doses.begin(), doses.end(), sameDose)) {
        return {Rejection::RequestConflict};
    }
    const auto result = found->stage == RequestStage::Ended ? std::optional{found->result} : std::nullopt;
    return {Rejection::Duplicate, ZoneReadiness::Ready, DurationError::None, found->stage, result};
}

Decision Irrigation::accept(const SystemConfig& config, const IrrigationRequest& request) {
    if (request.source == IrrigationSource::Manual) {
        if (request.trigger || !request.requestId) {
            return {Rejection::InvalidRequest};
        }
        if (const auto previous = duplicate(request.requestId, 0, {request.dose}); !previous) {
            return previous;
        }
    }
    if (group_) {
        return {Rejection::Busy};
    }

    IrrigationPlan plan;
    const auto decision = check(config, request, plan);
    if (request.source != IrrigationSource::Manual) {
        const auto found = std::find_if(candidates_.begin(), candidates_.end(),
                                       [&](const auto& candidate) { return sameRequest(candidate, request); });
        if (decision.error != Rejection::Busy && found != candidates_.end()) {
            candidates_.erase(found);
        } else if (decision && found == candidates_.end()) {
            return {Rejection::StaleTrigger};
        }
    }
    if (!decision) {
        return decision;
    }
    if (request.source == IrrigationSource::Manual && state_.manualReceipts.size() >= CriticalState::maxManualReceipts) {
        return {Rejection::Capacity};
    }
    const auto reserved = reserve(plan);
    if (reserved && request.source == IrrigationSource::Manual) {
        state_.manualReceipts.push_back({request.requestId, 0, {request.dose}});
    }
    return reserved;
}

Decision Irrigation::acceptGroup(const SystemConfig& config, RequestId id, EntityId groupId,
                                 const std::vector<VolumeMl>& orderedDoses) {
    if (!id || !groupId || orderedDoses.size() > CriticalState::maxTrackedZones) {
        return {Rejection::InvalidRequest};
    }
    if (validateConfig(config, channels_).error != ConfigError::None) {
        return {Rejection::InvalidConfig};
    }
    const auto* group = findById(config.groups, groupId);
    if (!group || group->zoneIds.size() != orderedDoses.size()) {
        return {Rejection::InvalidRequest};
    }
    std::vector<ZoneDose> doses;
    for (std::size_t index = 0; index < orderedDoses.size(); ++index) {
        doses.push_back({group->zoneIds[index], orderedDoses[index]});
    }
    if (const auto previous = duplicate(id, groupId, doses); !previous) {
        return previous;
    }
    if (group_) {
        return {Rejection::Busy};
    }
    if (state_.manualReceipts.size() >= CriticalState::maxManualReceipts) {
        return {Rejection::Capacity};
    }

    static_assert(CriticalState::maxTrackedZones <=
                  std::numeric_limits<MonotonicMs>::max() / std::numeric_limits<DurationMs>::max());
    IrrigationPlan first;
    MonotonicMs groupDurationMs = 0;
    std::size_t additionalZones = 0;
    std::size_t additionalWateringChannels = 0;
    for (const auto& dose : doses) {
        IrrigationPlan plan;
        if (const auto decision = check(config, {id, IrrigationSource::Manual, dose, {}}, plan); !decision) {
            return decision;
        }
        if (first.request.dose.zoneId == 0) {
            first = plan;
        }
        groupDurationMs += plan.plannedMs;
        if (std::none_of(state_.usage.begin(), state_.usage.end(),
                         [&dose](const auto& entry) { return entry.zoneId == dose.zoneId; })) {
            ++additionalZones;
        }
        if (std::none_of(state_.recentWatering.begin(), state_.recentWatering.end(),
                         [&plan](const auto& entry) { return entry.channel == plan.channel; })) {
            ++additionalWateringChannels;
        }
    }
    if (!finishesBefore(groupDurationMs, local_->secondsRemaining)) {
        return {Rejection::CrossesMidnight};
    }
    if (additionalZones + state_.usage.size() > CriticalState::maxTrackedZones ||
        additionalWateringChannels + state_.recentWatering.size() > CriticalState::maxTrackedZones) {
        return {Rejection::Capacity};
    }
    const auto reserved = reserve(first);
    if (reserved) {
        state_.manualReceipts.push_back({id, groupId, std::move(doses)});
        group_ = GroupRun{id, 0, first.quotaDay};
    }
    return reserved;
}

Decision Irrigation::advanceGroup(const SystemConfig& config) {
    if (!group_) {
        return {Rejection::InvalidRequest};
    }
    if (phase_ != Phase::Idle || dirty_ || inFlight_) {
        return {Rejection::Busy};
    }
    if (state_.date->day != group_->quotaDay) {
        finishGroup(IrrigationResult::Failed);
        return {Rejection::CrossesMidnight};
    }
    const auto& receipt = *findReceipt(state_.manualReceipts, group_->id);
    IrrigationPlan next;
    auto decision = sameGroup(config, receipt) ?
        check(config, {receipt.requestId, IrrigationSource::Manual, receipt.doses[group_->member], {}}, next) :
        Decision{Rejection::InvalidRequest};
    if (decision) {
        decision = reserve(next);
    }
    if (!decision) {
        finishGroup(IrrigationResult::Failed);
    }
    return decision;
}

void Irrigation::finishGroup(IrrigationResult result) {
    if (group_) {
        auto& receipt = *findReceipt(state_.manualReceipts, group_->id);
        receipt.stage = RequestStage::Ended;
        receipt.result = result;
        group_.reset();
        dirty_ = true;
    }
}

bool Irrigation::forgetManualRequest(RequestId id) {
    if (!available() || phase_ != Phase::Idle) {
        return false;
    }
    const auto receipt = findReceipt(state_.manualReceipts, id);
    if (receipt == state_.manualReceipts.end() || receipt->stage != RequestStage::Ended) {
        return false;
    }
    state_.manualReceipts.erase(receipt);
    dirty_ = true;
    return true;
}

std::optional<StartPermit> Irrigation::issueStart(const SystemConfig& config) {
    if (phase_ != Phase::Preparing || saveFailed_) {
        return std::nullopt;
    }
    const auto& existing = *state_.pending;
    IrrigationPlan rechecked;
    if ((group_ && !sameGroup(config, *findReceipt(state_.manualReceipts, group_->id))) ||
        !check(config, existing.request, rechecked, true) || !samePlan(existing, rechecked)) {
        requestStop(StopReason::Interrupted);
        return std::nullopt;
    }
    if (dirty_ || inFlight_) {
        return std::nullopt;
    }
    // 从发出许可时就固定截止时间。启动确认迟到不会重置时限。
    execution_->permit = StartPermit{existing, now_, now_ + existing.plannedMs, now_ + existing.maximumMs};
    phase_ = Phase::AwaitingStart;
    return execution_->permit;
}

bool Irrigation::confirmStarted(ExecutionId id, MonotonicMs atMs, std::optional<UtcSeconds> utc) {
    if ((phase_ != Phase::AwaitingStart && phase_ != Phase::Stopping) || !state_.pending ||
        id != state_.pending->executionId || !execution_->permit || execution_->startedMs ||
        atMs < execution_->permit->issuedAtMs) {
        return false;
    }
    execution_->startedMs = atMs;
    execution_->startedUtc = utc;
    if (state_.pending->request.source == IrrigationSource::Manual) {
        const auto receipt = findReceipt(state_.manualReceipts, state_.pending->request.requestId);
        if (receipt->stage == RequestStage::Accepted) {
            receipt->stage = RequestStage::InProgress;
            dirty_ = true;
        }
    }
    if (phase_ == Phase::AwaitingStart) {
        phase_ = Phase::Running;
        enforceDeadline(std::max(now_, atMs));
    }
    return true;
}

void Irrigation::enforceDeadline(MonotonicMs now) {
    if (phase_ != Phase::AwaitingStart && phase_ != Phase::Running && phase_ != Phase::Stopping) {
        return;
    }
    const auto& permit = *execution_->permit;
    if (now > permit.hardStopMs || (now == permit.hardStopMs && permit.plannedStopMs < permit.hardStopMs)) {
        requestStop(StopReason::MaximumDuration);
    } else if (now >= permit.plannedStopMs && phase_ != Phase::Stopping) {
        requestStop(phase_ == Phase::Running ? StopReason::Planned : StopReason::DeviceFault);
    }
}

std::optional<StopRequest> Irrigation::pendingStop() const {
    if (phase_ == Phase::Stopping) {
        return StopRequest{state_.pending->executionId, execution_->stopReason};
    }
    return std::nullopt;
}

void Irrigation::requestStop(StopReason reason) {
    if (phase_ == Phase::Preparing) {
        execution_->stopReason = reason;
        settle(StopEvidence::NeverStarted, {}, now_);
    } else if (phase_ == Phase::Running || phase_ == Phase::AwaitingStart || phase_ == Phase::Stopping) {
        if (execution_->stopReason == StopReason::None || execution_->stopReason == StopReason::Planned ||
            (execution_->stopReason == StopReason::Manual && reason != StopReason::Planned)) {
            execution_->stopReason = reason;
        }
        phase_ = Phase::Stopping;
    }
}

void Irrigation::stopAll() {
    if (!state_.autoPaused) {
        state_.autoPaused = true;
        dirty_ = true;
    }
    clearAutomatic();
    if (phase_ == Phase::Idle || phase_ == Phase::Settling) {
        finishGroup(IrrigationResult::Stopped);
    }
    requestStop(StopReason::Manual);
}

bool Irrigation::reportDeviceFault(ExecutionId id) {
    if (!state_.pending || id != state_.pending->executionId) {
        return false;
    }
    if (state_.fault == Fault::Device && phase_ == Phase::Stopping) {
        return false;
    }
    state_.fault = Fault::Device;
    dirty_ = true;
    clearAutomatic();
    requestStop(StopReason::DeviceFault);
    return true;
}

bool Irrigation::confirmStopped(PumpStopped event) {
    if ((phase_ != Phase::AwaitingStart && phase_ != Phase::Running && phase_ != Phase::Stopping) ||
        event.executionId != state_.pending->executionId) {
        return false;
    }
    const auto& permit = *execution_->permit;
    const auto earliestStop = execution_->startedMs.value_or(permit.issuedAtMs);
    if (event.stoppedAtMs < earliestStop) {
        return false;
    }
    if (event.evidence == StopEvidence::NeverStarted &&
        (execution_->startedMs || event.durationMs.value_or(0) != 0)) {
        requestStop(StopReason::DeviceFault);
        event.evidence = StopEvidence::Unknown;
    }
    MonotonicMs confirmedElapsedMs = 0;
    if (event.evidence == StopEvidence::Measured) {
        const auto elapsedMs = event.stoppedAtMs - earliestStop;
        if (event.durationMs.value_or(0) == 0 || *event.durationMs > elapsedMs ||
            (execution_->startedMs && *event.durationMs != elapsedMs)) {
            requestStop(StopReason::ClockFault);
            event.evidence = StopEvidence::Unknown;
            // 已确认的启停时刻仍可证明超量。只有许可时刻时，时间差只能作为上界。
            if (execution_->startedMs) {
                confirmedElapsedMs = elapsedMs;
            }
        } else if (*event.durationMs > permit.plan.maximumMs || event.stoppedAtMs > permit.hardStopMs) {
            requestStop(StopReason::MaximumDuration);
        } else if (execution_->stopReason == StopReason::Planned && *event.durationMs < permit.plan.plannedMs) {
            requestStop(StopReason::DeviceFault);
        } else if (execution_->stopReason == StopReason::None) {
            requestStop(*event.durationMs >= permit.plan.plannedMs ? StopReason::Planned : StopReason::DeviceFault);
        }
    }
    settle(event.evidence, event.durationMs, std::max(now_, event.stoppedAtMs), confirmedElapsedMs);
    return true;
}

void Irrigation::settle(StopEvidence evidence, std::optional<DurationMs> duration, MonotonicMs endMs,
                        MonotonicMs confirmedElapsedMs) {
    const auto plan = *state_.pending;
    const auto& request = plan.request;
    const auto reason = execution_->stopReason;
    auto result = IrrigationResult::Failed;
    VolumeMl charged = request.dose.volumeMl;
    auto fault = state_.fault;
    if (reason == StopReason::MaximumDuration || reason == StopReason::ClockFault || reason == StopReason::DeviceFault) {
        fault = Fault::Device;
    }

    std::optional<double> volume;
    if (evidence == StopEvidence::NeverStarted) {
        duration = 0;
        volume = 0.0;
        charged = 0;
        result = reason == StopReason::Manual ? IrrigationResult::Stopped : IrrigationResult::Failed;
    } else {
        const bool measured = evidence == StopEvidence::Measured;
        if (measured) {
            confirmedElapsedMs = *duration;
        } else {
            duration.reset();
            result = IrrigationResult::Unknown;
            fault = Fault::UnknownDelivery;
        }
        if (confirmedElapsedMs > std::numeric_limits<DurationMs>::max()) {
            charged = std::numeric_limits<VolumeMl>::max();
            fault = Fault::AccountingOverflow;
        } else if (confirmedElapsedMs > 0) {
            const auto delivery = *estimateDelivery(plan.flowMlPerMin, static_cast<DurationMs>(confirmedElapsedMs));
            if (measured) {
                volume = delivery.volumeMl;
            }
            if (!delivery.chargeMl) {
                charged = std::numeric_limits<VolumeMl>::max();
                fault = Fault::AccountingOverflow;
            } else if (measured || *delivery.chargeMl > charged) {
                charged = *delivery.chargeMl;
                if (charged > request.dose.volumeMl) {
                    fault = Fault::ExcessDelivery;
                }
            }
        }
        if (fault == Fault::None) {
            result = reason == StopReason::Planned ? IrrigationResult::Completed :
                     reason == StopReason::Manual ? IrrigationResult::Stopped : IrrigationResult::Failed;
        }
    }

    auto& usage = *std::find_if(state_.usage.begin(), state_.usage.end(),
                                [&](const auto& entry) { return entry.zoneId == request.dose.zoneId; });
    if (charged > std::numeric_limits<VolumeMl>::max() - usage.usedMl) {
        usage.usedMl = std::numeric_limits<VolumeMl>::max();
        fault = Fault::AccountingOverflow;
    } else {
        usage.usedMl += charged;
    }
    if (fault != Fault::None) {
        state_.fault = fault;
        clearAutomatic();
        if (result == IrrigationResult::Completed) {
            result = IrrigationResult::Failed;
        }
    }
    if (evidence != StopEvidence::NeverStarted) {
        rememberWatering(plan, endMs);
    }

    if (group_) {
        const auto& receipt = *findReceipt(state_.manualReceipts, group_->id);
        if (result != IrrigationResult::Completed || ++group_->member == receipt.doses.size()) {
            finishGroup(result);
        }
    } else if (request.source == IrrigationSource::Manual) {
        auto& receipt = *findReceipt(state_.manualReceipts, request.requestId);
        if (receipt.stage != RequestStage::Ended) {
            receipt.stage = RequestStage::Ended;
            receipt.result = result;
        }
    }
    settlingRecord_ = IrrigationRecord{request.dose.zoneId, plan.pumpId, request.source, request.dose.volumeMl,
                                       plan.flowMlPerMin, duration, volume, execution_->startedUtc,
                                       result, plan.executionId, reason};
    state_.pending.reset();
    execution_.reset();
    phase_ = Phase::Settling;
    dirty_ = true;
}

bool Irrigation::resumeAutomatic() {
    if (!available() || !state_.autoPaused || state_.fault != Fault::None || saveFailed_ ||
        phase_ != Phase::Idle || inFlight_ || dirty_) {
        return false;
    }
    clearAutomatic();
    state_.autoPaused = false;
    dirty_ = true;
    return true;
}

bool Irrigation::unlockFault(bool pumpsVerifiedOff) {
    if (!pumpsVerifiedOff || !available() || phase_ != Phase::Idle || inFlight_ || dirty_ ||
        state_.fault == Fault::None || state_.fault == Fault::IdentityExhausted) {
        return false;
    }
    clearAutomatic();
    state_.fault = Fault::None;
    dirty_ = true;
    return true;
}

}  // namespace planthub::core
