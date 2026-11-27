#include "application/services/IrrigationService.h"

#include <utility>

namespace planthub::application {
namespace {

constexpr core::MonotonicMs retryIntervalMs = 1'000;

const core::ManualReceipt* endedReceipt(const core::CriticalState& state, core::RequestId id) {
    const auto receipt = std::find_if(state.manualReceipts.begin(), state.manualReceipts.end(), [id](const auto& item) {
        return item.requestId == id && item.stage == core::RequestStage::Ended;
    });
    return receipt == state.manualReceipts.end() ? nullptr : &*receipt;
}

bool validMetadata(const RuntimeSnapshot& snapshot) {
    const auto& retention = snapshot.receiptRetention;
    if (retention.size() > core::CriticalState::maxManualReceipts) {
        return false;
    }
    for (auto item = retention.begin(); item != retention.end(); ++item) {
        if (!endedReceipt(snapshot.critical, item->requestId) ||
            std::any_of(retention.begin(), item, [&](const auto& earlier) {
                return earlier.requestId == item->requestId;
            })) {
            return false;
        }
    }
    const auto& state = snapshot.critical;
    return !snapshot.initializing || (!state.date && state.fault == core::Fault::None);
}

}  // namespace

void IrrigationService::load(core::ClockSample now) {
    configuration_.load();
    auto loaded = store_.load();
    runtimeStatus_ = loaded.status;
    if (loaded.status == LoadStatus::Loaded) {
        if (!loaded.snapshot || !validMetadata(*loaded.snapshot)) {
            runtimeStatus_ = LoadStatus::Corrupt;
        } else {
            auto& snapshot = *loaded.snapshot;
            engine_.emplace(channels_, core::Recovery::Loaded, now.monotonicMs, snapshot.critical);
            if (engine_->recoveryStatus() != core::Recovery::Loaded) {
                runtimeStatus_ = LoadStatus::Corrupt;
                engine_.reset();
            } else {
                initializingSnapshot_ = snapshot.initializing;
                for (const auto& receipt : snapshot.critical.manualReceipts) {
                    if (receipt.stage != core::RequestStage::Ended) {
                        continue;
                    }
                    const auto saved = std::find_if(snapshot.receiptRetention.begin(), snapshot.receiptRetention.end(),
                        [&](const auto& item) { return item.requestId == receipt.requestId; });
                    const auto stamp = saved == snapshot.receiptRetention.end() ? std::nullopt : saved->reliableEndUtc;
                    retained_.push_back({{receipt.requestId, stamp}, now.monotonicMs, true});
                }
            }
        }
    } else if (loaded.status == LoadStatus::FirstUse && configuration_.loadStatus() == LoadStatus::FirstUse) {
        engine_.emplace(channels_, core::Recovery::FirstInitialization, now.monotonicMs);
        initializingSnapshot_ = true;
    }

    boot_ = BootStatus::StorageUnavailable;
    if (engine_) {
        const auto* config = configuration_.current();
        if (initializingSnapshot_ && (config || configuration_.loadStatus() == LoadStatus::FirstUse)) {
            boot_ = BootStatus::Initializing;
        } else if (!initializingSnapshot_ && config) {
            if (!ConfigurationService::preservesUsage(config->config, engine_->state())) {
                boot_ = BootStatus::AccountingBindingConflict;
            } else {
                // 恢复中的结算也须可靠保存，才开放正常灌溉。
                boot_ = engine_->stateReliable() ? BootStatus::Ready : BootStatus::Initializing;
            }
        }
        if (stoppedDuringBoot_) {
            engine_->stopAll();
        }
    }
}

void IrrigationService::observeRetentionClock(core::ClockSample now) {
    monotonicValid_ = !lastClock_ || now.monotonicMs >= lastClock_->monotonicMs;
    if (!monotonicValid_) {
        utcAnchor_.reset();
        return;
    }
    if (!lastClock_ || !utcAnchor_ || !core::consistentClockSamples(*lastClock_, now) ||
        !core::consistentClockSamples(*utcAnchor_, now)) {
        utcAnchor_ = now.utc ? std::optional{now} : std::nullopt;
    }
    lastClock_ = now;
}

void IrrigationService::refresh(core::ClockSample now) {
    observeRetentionClock(now);
    if (!startupStopSent_) {
        pump_.priorityStop({0, core::StopReason::Interrupted});
        startupStopSent_ = true;
    }
    if (boot_ == BootStatus::WaitingForPumps && pump_.allPumpsVerifiedOff()) {
        load(now);
    }

    eventsDrained_ = false;
    for (std::size_t count = 0; count < core::CriticalState::maxTrackedZones; ++count) {
        const auto event = pump_.pollEvent();
        if (!event) {
            eventsDrained_ = true;
            break;
        }
        if (!engine_) {
            continue;
        }
        if (const auto* started = std::get_if<PumpStarted>(&*event)) {
            engine_->confirmStarted(started->executionId, started->started.monotonicMs, started->started.utc);
        } else if (const auto* fault = std::get_if<PumpFault>(&*event)) {
            engine_->reportDeviceFault(fault->executionId);
        } else {
            engine_->confirmStopped(std::get<core::PumpStopped>(*event));
        }
        forwardStop();
    }
    if (boot_ == BootStatus::Ready) {
        engine_->observeTime(configuration_.current()->config, now);
        forwardStop();
    }
}

void IrrigationService::forwardStop() {
    const auto stop = engine_->pendingStop();
    if (stop && (!stopSent_ || stopSent_->executionId != stop->executionId || stopSent_->reason != stop->reason)) {
        pump_.priorityStop(*stop);
    }
    stopSent_ = stop;
}

void IrrigationService::stopAll() {
    pump_.priorityStop({0, core::StopReason::Manual});
    startupStopSent_ = true;
    if (engine_) {
        engine_->stopAll();
        stopSent_ = engine_->pendingStop();
    } else {
        stoppedDuringBoot_ = true;
    }
}

bool IrrigationService::automaticPaused() const {
    return stoppedDuringBoot_ || resumePending_ || (engine_ && engine_->state().autoPaused);
}

core::Fault IrrigationService::fault() const {
    return unlockPending_.value_or(engine_ ? engine_->state().fault : core::Fault::None);
}

core::Decision IrrigationService::requestGate(core::RequestId id) const {
    if (boot_ != BootStatus::Ready) {
        return {saveFailed_ ? core::Rejection::SaveFailed : core::Rejection::CriticalState};
    }
    const auto& receipts = engine_->state().manualReceipts;
    if (std::any_of(receipts.begin(), receipts.end(), [id](const auto& item) { return item.requestId == id; })) {
        // 已有凭据交给 Core 核对。可靠保存的限制继续约束新请求。
        return {};
    }
    if (resumePending_ || unlockPending_ || purging_) {
        return {saveFailed_ ? core::Rejection::SaveFailed : core::Rejection::CriticalState};
    }
    if (engine_->phase() == core::Phase::Idle && (!eventsDrained_ || !pump_.allPumpsVerifiedOff())) {
        return {core::Rejection::Busy};
    }
    return {};
}

core::Decision IrrigationService::requestManual(core::RequestId id, core::ZoneDose dose) {
    refresh(clock_.now());
    if (const auto gate = requestGate(id); !gate) {
        return gate;
    }
    return engine_->accept(configuration_.current()->config, {id, core::IrrigationSource::Manual, dose, {}});
}

core::Decision IrrigationService::requestGroup(core::RequestId id, core::EntityId groupId,
                                              const std::vector<core::VolumeMl>& orderedDoses) {
    refresh(clock_.now());
    if (const auto gate = requestGate(id); !gate) {
        return gate;
    }
    return engine_->acceptGroup(configuration_.current()->config, id, groupId, orderedDoses);
}

bool IrrigationService::resumeAutomatic() {
    if (!canConfigure() || resumePending_ || unlockPending_ || !engine_->resumeAutomatic()) {
        return false;
    }
    resumePending_ = true;
    stoppedDuringBoot_ = false;
    return true;
}

bool IrrigationService::unlockFault() {
    if (!canConfigure() || resumePending_ || unlockPending_) {
        return false;
    }
    const auto previous = engine_->state().fault;
    if (!engine_->unlockFault(pump_.allPumpsVerifiedOff())) {
        return false;
    }
    unlockPending_ = previous;
    return true;
}

bool IrrigationService::canConfigure() const {
    return boot_ == BootStatus::Ready && engine_->phase() == core::Phase::Idle && !engine_->groupActive() &&
           engine_->stateReliable() && !purging_ && eventsDrained_ && pump_.allPumpsVerifiedOff();
}

bool IrrigationService::retryDue(core::MonotonicMs now) const {
    return !lastFailureMs_ || (now >= *lastFailureMs_ && now - *lastFailureMs_ >= retryIntervalMs);
}

void IrrigationService::trackReliableEnds(const RuntimeSnapshot& snapshot, core::ClockSample now) {
    const bool stable = utcAnchor_ && core::consistentClockSamples(*utcAnchor_, now) &&
                        now.monotonicMs - utcAnchor_->monotonicMs >= core::clockConfirmationMs;
    for (const auto& receipt : snapshot.critical.manualReceipts) {
        if (receipt.stage != core::RequestStage::Ended ||
            std::any_of(retained_.begin(), retained_.end(), [&](const auto& entry) {
                return entry.persisted.requestId == receipt.requestId;
            })) {
            continue;
        }
        const auto since = std::max(now.monotonicMs, lastClock_ ? lastClock_->monotonicMs : now.monotonicMs);
        retained_.push_back({{receipt.requestId, stable ? now.utc : std::nullopt}, since, false});
        metadataChanged_ = true;
    }
}

void IrrigationService::expireReceipts(core::ClockSample now) {
    if (!monotonicValid_ || !lastClock_ || now.monotonicMs < lastClock_->monotonicMs || !engine_->stateReliable() ||
        engine_->phase() != core::Phase::Idle) {
        return;
    }
    const bool stable = utcAnchor_ && core::consistentClockSamples(*utcAnchor_, now) &&
                        now.monotonicMs - utcAnchor_->monotonicMs >= core::clockConfirmationMs;
    retained_.erase(std::remove_if(retained_.begin(), retained_.end(), [&](const auto& entry) {
        if (now.monotonicMs < entry.sinceMs || now.monotonicMs - entry.sinceMs <= receiptRetentionMs) {
            return false;
        }
        const auto priorUtc = entry.persisted.reliableEndUtc;
        if (entry.recovered && priorUtc &&
            (!stable || *now.utc < *priorUtc ||
             static_cast<std::uint64_t>(*now.utc) - static_cast<std::uint64_t>(*priorUtc) <= receiptRetentionMs / 1'000)) {
            return false;
        }
        if (!engine_->forgetManualRequest(entry.persisted.requestId)) {
            return false;
        }
        purging_ = true;
        return true;
    }), retained_.end());
}

void IrrigationService::beginSave(core::MonotonicMs now) {
    if (saving_ || !retryDue(now)) {
        return;
    }
    auto draft = engine_->beginSave(metadataChanged_);
    if (!draft) {
        return;
    }
    RuntimeSnapshot snapshot{std::move(draft->state), {}, initializingSnapshot_};
    for (const auto& entry : retained_) {
        if (endedReceipt(snapshot.critical, entry.persisted.requestId)) {
            snapshot.receiptRetention.push_back(entry.persisted);
        }
    }
    saving_ = PendingSave{draft->id, std::move(snapshot)};
    metadataChanged_ = false;
    store_.startSave(saving_->id, saving_->snapshot);
}

void IrrigationService::finishSave() {
    const auto completion = store_.pollSave();
    if (!completion || !saving_ || completion->id != saving_->id ||
        !engine_->finishSave(completion->id, completion->reliable)) {
        return;
    }
    const auto now = clock_.now();
    observeRetentionClock(now);
    saveFailed_ = !completion->reliable;
    if (saveFailed_) {
        lastFailureMs_ = now.monotonicMs;
        if (engine_->phase() == core::Phase::Preparing || engine_->phase() == core::Phase::AwaitingStart ||
            engine_->phase() == core::Phase::Running) {
            stopAll();
        }
    } else {
        lastFailureMs_.reset();
        purging_ = false;
        if (engine_->stateReliable()) {
            trackReliableEnds(saving_->snapshot, now);
            if (resumePending_ || unlockPending_) {
                engine_->resetAutomatic();
            }
            resumePending_ = false;
            unlockPending_.reset();
        }
    }
    saving_.reset();
}

void IrrigationService::drive(core::ClockSample now) {
    if (!engine_) {
        return;
    }
    finishSave();
    now = clock_.now();
    if (const auto record = engine_->takeRecord()) {
        // 每个可靠结算只提取一次，历史失败由状态反馈。
        historyFailed_ = !history_.writeIrrigation(*record) || historyFailed_;
        beginSave(clock_.now().monotonicMs);
        return;
    }
    if (boot_ == BootStatus::Initializing && engine_->stateReliable()) {
        if (initializingSnapshot_ && retryDue(now.monotonicMs)) {
            initConfigSaveFailed_ = !configuration_.initialize();
            if (initConfigSaveFailed_) {
                lastFailureMs_ = now.monotonicMs;
            } else {
                lastFailureMs_.reset();
                initializingSnapshot_ = false;
                metadataChanged_ = true;
            }
        } else if (!initializingSnapshot_ && !metadataChanged_) {
            boot_ = BootStatus::Ready;
            runtimeStatus_ = LoadStatus::Loaded;
        }
    }
    if (boot_ == BootStatus::Ready && !resumePending_ && !unlockPending_ && !saveFailed_) {
        expireReceipts(now);
        if (!purging_ && eventsDrained_ && pump_.allPumpsVerifiedOff()) {
            const auto& config = configuration_.current()->config;
            if (engine_->phase() == core::Phase::Preparing && engine_->stateReliable()) {
                refresh(clock_.now());
                if (eventsDrained_ && pump_.allPumpsVerifiedOff()) {
                    if (const auto permit = engine_->issueStart(config)) {
                        pump_.start(*permit);
                    }
                    forwardStop();
                }
            } else if (engine_->phase() == core::Phase::Idle) {
                if (engine_->groupActive()) {
                    engine_->advanceGroup(config);
                } else {
                    engine_->acceptAutomatic(config);
                }
            }
        }
    }
    beginSave(clock_.now().monotonicMs);
}

}  // namespace planthub::application
