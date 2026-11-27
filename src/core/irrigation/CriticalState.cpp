
#include "core/irrigation/Irrigation.h"

#include <limits>
#include <utility>

namespace planthub::core {
namespace {

bool validFault(Fault fault) {
    switch (fault) {
        case Fault::None:
        case Fault::UnknownDelivery:
        case Fault::Device:
        case Fault::ExcessDelivery:
        case Fault::AccountingOverflow:
        case Fault::IdentityExhausted:
            return true;
    }
    return false;
}

bool validReceipt(const ManualReceipt& receipt) {
    const auto minimum = receipt.groupId == 0 ? 1u : 2u;
    if (!receipt.requestId || receipt.doses.size() < minimum ||
        receipt.doses.size() > (receipt.groupId == 0 ? 1u : CriticalState::maxTrackedZones)) {
        return false;
    }
    for (auto item = receipt.doses.begin(); item != receipt.doses.end(); ++item) {
        if (!item->zoneId || !item->volumeMl || std::any_of(receipt.doses.begin(), item, [&](const auto& earlier) {
                return earlier.zoneId == item->zoneId;
            })) {
            return false;
        }
    }
    switch (receipt.stage) {
        case RequestStage::Accepted:
        case RequestStage::InProgress:
            return receipt.result == IrrigationResult::Unknown;
        case RequestStage::Ended:
            return receipt.result == IrrigationResult::Completed || receipt.result == IrrigationResult::Stopped ||
                   receipt.result == IrrigationResult::Failed || receipt.result == IrrigationResult::Unknown;
    }
    return false;
}

}  // namespace

Irrigation::Irrigation(ChannelCapabilities channels, Recovery recovery, MonotonicMs bootMs, CriticalState state)
    : channels_(std::move(channels)), recovery_(recovery), now_(bootMs), automaticAfterMs_(bootMs) {
    if (recovery == Recovery::FirstInitialization) {
        dirty_ = true;
        return;
    }
    if (recovery != Recovery::Loaded) {
        return;
    }
    state_ = std::move(state);
    if (!validState()) {
        recovery_ = Recovery::Corrupt;
        state_ = {};
        return;
    }

    // 单调时钟跨重启不可比，每条记录从本次启动重新等待完整冷却。
    recentEnds_.assign(state_.recentWatering.size(), bootMs);
    recoverPending();
    for (auto& receipt : state_.manualReceipts) {
        if (receipt.stage != RequestStage::Ended) {
            // 成员之间重启时水泵已可靠结算，只取消分组余下成员。
            receipt.stage = RequestStage::Ended;
            receipt.result = IrrigationResult::Failed;
            dirty_ = true;
        }
    }
}

bool Irrigation::available() const {
    return recovery_ == Recovery::FirstInitialization || recovery_ == Recovery::Loaded;
}

bool Irrigation::validState() const {
    const auto validChannel = [this](LogicalChannel channel) {
        return std::find(channels_.pumpChannels.begin(), channels_.pumpChannels.end(), channel) !=
               channels_.pumpChannels.end();
    };
    if (!validFault(state_.fault) || state_.usage.size() > CriticalState::maxTrackedZones ||
        state_.recentWatering.size() > CriticalState::maxTrackedZones ||
        state_.manualReceipts.size() > CriticalState::maxManualReceipts) {
        return false;
    }
    if (state_.date) {
        const auto date = *state_.date;
        const auto earliest = localTime(std::numeric_limits<UtcSeconds>::min(), date.offsetMinutes);
        const auto latest = localTime(std::numeric_limits<UtcSeconds>::max(), date.offsetMinutes);
        if (!earliest || date.day < earliest->day || date.day > latest->day ||
            date.consumedThroughMinute.value_or(0) >= 1440) {
            return false;
        }
    } else if (!state_.usage.empty() || state_.pending || state_.lastExecutionId != 0 ||
               !state_.manualReceipts.empty() || !state_.recentWatering.empty()) {
        return false;
    }

    for (auto it = state_.usage.begin(); it != state_.usage.end(); ++it) {
        if (!it->zoneId || !validChannel(it->channel) ||
            std::any_of(state_.usage.begin(), it, [&](const auto& earlier) {
                return earlier.zoneId == it->zoneId || earlier.channel == it->channel;
            })) {
            return false;
        }
    }
    for (auto it = state_.recentWatering.begin(); it != state_.recentWatering.end(); ++it) {
        if (!it->zoneId || !validChannel(it->channel) ||
            std::any_of(state_.recentWatering.begin(), it, [&](const auto& earlier) {
                return earlier.channel == it->channel;
            })) {
            return false;
        }
    }

    const auto pendingManualId = state_.pending ? state_.pending->request.requestId : 0;
    std::size_t unfinished = 0;
    for (auto it = state_.manualReceipts.begin(); it != state_.manualReceipts.end(); ++it) {
        if (!validReceipt(*it) || std::any_of(state_.manualReceipts.begin(), it, [&](const auto& earlier) {
                return earlier.requestId == it->requestId;
            })) {
            return false;
        }
        if (it->stage != RequestStage::Ended) {
            if (++unfinished > 1 || (pendingManualId != it->requestId && (it->groupId == 0 || state_.pending))) {
                return false;
            }
        }
    }

    if (!state_.pending) {
        return true;
    }
    const auto& plan = *state_.pending;
    const auto& request = plan.request;
    if (!plan.executionId || plan.executionId != state_.lastExecutionId || plan.quotaDay != state_.date->day ||
        !request.dose.volumeMl || !plan.plannedMs || plan.plannedMs > plan.maximumMs || !plan.pumpId ||
        !isValidPumpFlow(plan.flowMlPerMin) ||
        std::none_of(state_.usage.begin(), state_.usage.end(), [&](const auto& entry) {
            return entry.zoneId == request.dose.zoneId && entry.channel == plan.channel;
        }) ||
        (state_.recentWatering.size() == CriticalState::maxTrackedZones &&
         std::none_of(state_.recentWatering.begin(), state_.recentWatering.end(),
                      [&](const auto& entry) { return entry.channel == plan.channel; }))) {
        return false;
    }

    if (request.source == IrrigationSource::Manual) {
        const auto receipt = std::find_if(state_.manualReceipts.begin(), state_.manualReceipts.end(),
                                          [&](const auto& item) { return item.requestId == pendingManualId; });
        return !request.trigger && receipt != state_.manualReceipts.end() &&
               receipt->stage != RequestStage::Ended &&
               std::any_of(receipt->doses.begin(), receipt->doses.end(), [&](const auto& item) {
                   return item.zoneId == request.dose.zoneId && item.volumeMl == request.dose.volumeMl;
               });
    }
    if (pendingManualId || !request.trigger || request.trigger->day != plan.quotaDay) {
        return false;
    }
    if (request.source == IrrigationSource::Scheduled) {
        return state_.date->consumedThroughMinute && request.trigger->minute <= *state_.date->consumedThroughMinute;
    }
    return request.source == IrrigationSource::Smart;
}

void Irrigation::recoverPending() {
    if (state_.pending) {
        execution_.emplace();
        execution_->stopReason = StopReason::Interrupted;
        settle(StopEvidence::Unknown, {}, now_);
    }
}

std::optional<SaveDraft> Irrigation::beginSave() {
    if (!available() || !dirty_ || inFlight_) {
        return std::nullopt;
    }
    if (lastSaveId_ == std::numeric_limits<SaveId>::max()) {
        saveFailed_ = true;
        return std::nullopt;
    }
    inFlight_ = ++lastSaveId_;
    dirty_ = false;
    return SaveDraft{*inFlight_, state_};
}

bool Irrigation::finishSave(SaveId id, bool reliable) {
    if (inFlight_ != id) {
        return false;
    }
    inFlight_.reset();
    saveFailed_ = !reliable;
    dirty_ = dirty_ || !reliable;
    if (!dirty_ && phase_ == Phase::Settling) {
        phase_ = Phase::Idle;
        record_ = std::exchange(settlingRecord_, std::nullopt);
    }
    return true;
}

std::optional<IrrigationRecord> Irrigation::takeRecord() {
    return std::exchange(record_, std::nullopt);
}

void Irrigation::rememberWatering(const IrrigationPlan& plan, MonotonicMs endMs) {
    const auto match = std::find_if(state_.recentWatering.begin(), state_.recentWatering.end(),
                                    [&](const auto& entry) { return entry.channel == plan.channel; });
    const RecentWatering recent{plan.request.dose.zoneId, plan.channel};
    if (match == state_.recentWatering.end()) {
        state_.recentWatering.push_back(recent);
        recentEnds_.push_back(endMs);
    } else {
        recentEnds_[static_cast<std::size_t>(match - state_.recentWatering.begin())] = endMs;
        *match = recent;
    }
    const auto progress = std::find_if(soil_.begin(), soil_.end(),
                                       [&](const auto& entry) { return entry.zoneId == recent.zoneId; });
    if (progress != soil_.end()) {
        *progress = SoilProgress{recent.zoneId};
    }
    cancelCandidate(recent.zoneId, IrrigationSource::Smart);
}

}  // namespace planthub::core
