#pragma once

#include "application/ports/Devices.h"
#include "application/services/ConfigurationService.h"

namespace planthub::application {

enum class BootStatus { WaitingForPumps, Initializing, Ready, StorageUnavailable, AccountingBindingConflict };

class IrrigationService {
public:
    static constexpr core::MonotonicMs receiptRetentionMs = 600'000;

    core::Decision requestManual(core::RequestId id, core::ZoneDose dose);
    core::Decision requestGroup(core::RequestId id, core::EntityId groupId,
                                const std::vector<core::VolumeMl>& orderedDoses);
    void stopAll();
    bool resumeAutomatic();
    bool unlockFault();

    BootStatus bootStatus() const { return boot_; }
    LoadStatus runtimeLoadStatus() const { return runtimeStatus_; }
    const core::Irrigation* coreStatus() const { return engine_ ? &*engine_ : nullptr; }
    bool automaticPaused() const;
    core::Fault fault() const;
    bool runtimeSaveFailed() const { return saveFailed_; }
    bool initializationConfigSaveFailed() const { return initConfigSaveFailed_; }
    bool historyWriteFailed() const { return historyFailed_; }

private:
    friend class IrrigationScheduler;

    struct RetainedReceipt {
        ReceiptRetention persisted;
        core::MonotonicMs sinceMs;
        bool recovered;
    };

    struct PendingSave {
        core::SaveId id;
        RuntimeSnapshot snapshot;
    };

    IrrigationService(const core::ChannelCapabilities& channels, ConfigurationService& configuration,
                      const Clock& clock, PumpController& pump, RuntimeStateStore& store, HistorySink& history)
        : channels_(channels), configuration_(configuration), clock_(clock), pump_(pump), store_(store),
          history_(history) {}

    void refresh(core::ClockSample now);
    void load(core::ClockSample now);
    void observeRetentionClock(core::ClockSample now);
    void forwardStop();
    void drive(core::ClockSample now);
    void beginSave(core::MonotonicMs now);
    void finishSave();
    void trackReliableEnds(const RuntimeSnapshot& snapshot, core::ClockSample now);
    void expireReceipts(core::ClockSample now);
    core::Decision requestGate(core::RequestId id) const;
    bool canConfigure() const;
    bool retryDue(core::MonotonicMs now) const;

    const core::ChannelCapabilities& channels_;
    ConfigurationService& configuration_;
    const Clock& clock_;
    PumpController& pump_;
    RuntimeStateStore& store_;
    HistorySink& history_;
    std::optional<core::Irrigation> engine_;
    BootStatus boot_ = BootStatus::WaitingForPumps;
    LoadStatus runtimeStatus_ = LoadStatus::ReadFailed;
    bool startupStopSent_ = false;
    bool stoppedDuringBoot_ = false;
    bool initializingSnapshot_ = false;
    bool metadataChanged_ = false;
    bool purging_ = false;
    bool resumePending_ = false;
    std::optional<core::Fault> unlockPending_;
    bool eventsDrained_ = false;
    std::optional<core::StopRequest> stopSent_;
    std::optional<PendingSave> saving_;
    std::vector<RetainedReceipt> retained_;
    bool saveFailed_ = false;
    bool initConfigSaveFailed_ = false;
    bool historyFailed_ = false;
    std::optional<core::MonotonicMs> lastFailureMs_;
    std::optional<core::ClockSample> lastClock_;
    std::optional<core::ClockSample> utcAnchor_;
    bool monotonicValid_ = true;
};

}  // namespace planthub::application
