#include "Simulation.h"

using namespace simulation;

namespace {

void expect(Rejection expected, Decision actual) {
    TEST_ASSERT_EQUAL_INT(static_cast<int>(expected), static_cast<int>(actual.error));
}

void expect(ConfigChange expected, ConfigResult actual) {
    TEST_ASSERT_EQUAL_INT(static_cast<int>(expected), static_cast<int>(actual.status));
}

void expect(BootStatus expected, IrrigationService& service) {
    TEST_ASSERT_EQUAL_INT(static_cast<int>(expected), static_cast<int>(service.bootStatus()));
}

const SoilReading& soil(const MonitoringSnapshot& snapshot, std::size_t index = 0) {
    return std::get<SoilReading>(snapshot.soil.at(index).data);
}

void test_two_plants_from_reliable_configuration_to_monitored_manual_delivery() {
    Rig device;
    device.configure();
    const auto readings = device.app.monitoring().latest();
    TEST_ASSERT_EQUAL_UINT32(2, readings.soil.size());
    TEST_ASSERT_FLOAT_WITHIN(0.01, 20.0, *soil(readings).moisturePercent);
    TEST_ASSERT_FLOAT_WITHIN(0.01, 20.0, *soil(readings, 1).moisturePercent);
    TEST_ASSERT_FLOAT_WITHIN(0.001, 1.584, *std::get<AirReading>(readings.air->data).vpdKPa);
    TEST_ASSERT_TRUE(readings.air->sampledAtUtcSeconds.has_value());

    auto& irrigation = device.irrigation();
    expect(Rejection::None, irrigation.requestManual(11, {1, 10}));
    expect(Rejection::Duplicate, irrigation.requestManual(11, {1, 10}));
    expect(Rejection::RequestConflict, irrigation.requestManual(11, {2, 10}));
    expect(Rejection::Busy, irrigation.requestManual(12, {2, 15}));
    TEST_ASSERT_TRUE(device.pump.permits.empty());
    device.tick();
    TEST_ASSERT_TRUE(device.pump.permits.empty());
    device.tick(3);
    TEST_ASSERT_EQUAL_UINT32(1, device.pump.permits.size());
    device.finishPlanned();
    expect(Rejection::None, irrigation.requestManual(12, {2, 15}));
    device.tick(2);
    device.finishPlanned();
    TEST_ASSERT_EQUAL_UINT32(2, device.history.irrigation.size());
    TEST_ASSERT_TRUE(device.history.irrigation[0].result == IrrigationResult::Completed);
    TEST_ASSERT_EQUAL_UINT32(1'000, *device.history.irrigation[0].actualDurationMs);
    TEST_ASSERT_TRUE(device.history.irrigation[1].estimatedVolumeMl == 15.0);
    TEST_ASSERT_TRUE(irrigation.requestManual(12, {2, 15}).previousResult == IrrigationResult::Completed);
    TEST_ASSERT_EQUAL_UINT32(90, *device.engine().remaining(1, 100));
    TEST_ASSERT_EQUAL_UINT32(85, *device.engine().remaining(2, 100));
}

void test_schedule_is_independent_of_collection_and_resolves_conflicts_by_zone() {
    Rig device;
    auto config = twoPlants();
    std::reverse(config.zones.begin(), config.zones.end());
    for (auto& zone : config.zones) {
        zone.rule.mode = AutoMode::Scheduled;
        zone.rule.schedule = {{301, 10}};
    }
    device.configure(config);
    const auto sampled = device.sensors.soilReads;
    const auto history = device.history.monitoring.size();
    device.advance(59'000);
    device.advance(1'000, 3);
    TEST_ASSERT_EQUAL_INT(sampled, device.sensors.soilReads);
    TEST_ASSERT_EQUAL_UINT32(history, device.history.monitoring.size());
    TEST_ASSERT_EQUAL_UINT32(1, device.pump.permits.size());
    TEST_ASSERT_EQUAL_UINT32(1, device.pump.permits[0].plan.request.dose.zoneId);
    TEST_ASSERT_TRUE(device.pump.permits[0].plan.request.source == IrrigationSource::Scheduled);
    device.finishPlanned();
    TEST_ASSERT_EQUAL_UINT32(2, device.pump.permits.size());
    TEST_ASSERT_EQUAL_UINT32(2, device.pump.permits[1].plan.request.dose.zoneId);
    device.finishPlanned();
    device.advance(30'000, 4);
    TEST_ASSERT_EQUAL_UINT32(2, device.history.irrigation.size());
}

void test_smart_rejects_cached_and_failed_samples_and_restarts_after_config_change() {
    Rig device;
    auto config = twoPlants();
    config.zones[0].rule.mode = AutoMode::Smart;
    device.configure(config);  // 第一个低湿度样本。
    device.sensors.freeze[0] = true;
    device.advance(300'000, 2);
    TEST_ASSERT_EQUAL_UINT64(11, device.app.monitoring().latest().soil[0].sampledAtMs);
    device.advance(300'000, 2);
    device.sensors.freeze[0] = false;
    device.sensors.soilStatus[0] = Acquisition::Failed;
    device.advance(300'000);
    TEST_ASSERT_TRUE(soil(device.app.monitoring().latest()).status == ReadingStatus::AcquisitionFailed);
    TEST_ASSERT_FALSE(soil(device.app.monitoring().latest()).raw.has_value());
    device.sensors.soilStatus[0] = Acquisition::Success;
    device.advance(300'000);
    device.advance(300'000);
    TEST_ASSERT_TRUE(device.pump.permits.empty());

    config.soilSensors[0].calibration->wetRaw = 1'100;
    expect(ConfigChange::Applied, device.app.replaceConfiguration(config));
    TEST_ASSERT_TRUE(device.app.monitoring().latest().soil.empty());
    device.sensors.freeze[0] = true;
    device.advance(1);
    TEST_ASSERT_EQUAL_UINT32(1, device.app.monitoring().latest().soil.size());
    TEST_ASSERT_EQUAL_UINT32(2, soil(device.app.monitoring().latest()).sensorId);
    device.sensors.freeze[0] = false;
    device.advance(300'000);
    device.advance(325'000);  // R05 允许 30 秒以内的间隔抖动。
    TEST_ASSERT_TRUE(device.pump.permits.empty());
    device.advance(300'000, 3);
    TEST_ASSERT_EQUAL_UINT32(1, device.pump.permits.size());
    TEST_ASSERT_TRUE(device.pump.permits[0].plan.request.source == IrrigationSource::Smart);
    device.finishPlanned();

    config.zones[0].rule.lowMoisturePercent = 40;
    expect(ConfigChange::Applied, device.app.replaceConfiguration(config));
    device.advance(1);
    device.advance(300'000);
    device.advance(300'000);
    TEST_ASSERT_EQUAL_UINT32(1, device.pump.permits.size());
    device.advance(300'000);
    device.advance(300'000);
    device.advance(300'000, 3);
    TEST_ASSERT_EQUAL_UINT32(2, device.pump.permits.size());
}

void test_auto_window_leaves_monitoring_history_and_manual_independent() {
    Rig device;
    device.clock.epoch = referenceDay - 8 * 3'600;
    auto config = twoPlants();
    config.historySampleIntervalMs = 30'000;
    config.zones[0].rule.mode = AutoMode::Scheduled;
    config.zones[0].rule.schedule = {{1, 10}};
    device.configure(config);
    const auto sampleCount = device.sensors.soilReads;
    device.advance(30'000);
    TEST_ASSERT_EQUAL_UINT32(2, device.history.monitoring.size());
    TEST_ASSERT_EQUAL_UINT64(11, device.history.monitoring.back().soil[0].sampledAtMs);
    device.advance(29'000);
    device.advance(1'000, 3);
    TEST_ASSERT_EQUAL_INT(sampleCount, device.sensors.soilReads);
    TEST_ASSERT_TRUE(device.pump.permits.empty());
    TEST_ASSERT_EQUAL_UINT16(1, *device.engine().state().date->consumedThroughMinute);
    expect(Rejection::None, device.irrigation().requestManual(18, {1, 10}));
    device.tick(2);
    device.finishPlanned();
    device.advance(300'000);
    TEST_ASSERT_TRUE(device.sensors.soilReads > sampleCount);
}

void test_config_validation_atomic_save_watermarks_and_accounting_bindings() {
    Rig device;
    auto config = twoPlants();
    device.configure(config);
    auto invalid = config;
    invalid.soilSensors.erase(invalid.soilSensors.begin());
    expect(ConfigChange::Invalid, device.app.replaceConfiguration(invalid));
    auto withUnbound = config;
    withUnbound.plants.push_back({3, "Seedling", true, {}, {}});
    device.configs.fail = true;
    expect(ConfigChange::SaveFailed, device.app.replaceConfiguration(withUnbound));
    TEST_ASSERT_EQUAL_UINT32(2, device.app.configuration().current()->allocated.plants);
    device.configs.fail = false;
    expect(ConfigChange::Applied, device.app.replaceConfiguration(withUnbound));
    expect(ConfigChange::Applied, device.app.replaceConfiguration(config));
    expect(ConfigChange::Identity, device.app.replaceConfiguration(withUnbound));
    withUnbound.plants.back().id = 4;
    expect(ConfigChange::Applied, device.app.replaceConfiguration(withUnbound));

    expect(Rejection::None, device.irrigation().requestManual(101, {1, 20}));
    expect(ConfigChange::Busy, device.app.replaceConfiguration(config));
    device.tick(2);
    expect(ConfigChange::Busy, device.app.replaceConfiguration(config));
    device.finishPlanned();
    TEST_ASSERT_EQUAL_UINT32(20, device.runtime.stored.snapshot->critical.usage[0].usedMl);

    auto swapped = config;
    std::swap(swapped.pumps[0].channel, swapped.pumps[1].channel);
    expect(ConfigChange::AccountingBinding, device.app.replaceConfiguration(swapped));
    auto removed = config;
    removed.groups.clear();
    removed.plants.erase(removed.plants.begin());
    removed.zones.erase(removed.zones.begin());
    expect(ConfigChange::AccountingBinding, device.app.replaceConfiguration(removed));
    removed.plants.push_back({5, "New Mint", true, 1, 3});
    removed.zones.push_back({3, "New Zone", 1, 1, config.zones[0].rule});
    expect(ConfigChange::AccountingBinding, device.app.replaceConfiguration(removed));

    config.zones[0].rule.singleLimitMl = 10;
    config.zones[0].rule.dailyLimitMl = 15;
    expect(ConfigChange::Applied, device.app.replaceConfiguration(config));
    expect(Rejection::DailyLimit, device.irrigation().requestManual(102, {1, 1}));
    config.zones[0].rule.dailyLimitMl = 25;
    expect(ConfigChange::Applied, device.app.replaceConfiguration(config));
    expect(Rejection::DailyLimit, device.irrigation().requestManual(103, {1, 6}));
    TEST_ASSERT_EQUAL_UINT32(5, *device.engine().remaining(1, 25));

    IrrigationScheduler reboot(channels(), device.clock, device.sensors, device.pump, device.configs,
                               device.runtime, device.history);
    reboot.tick();
    TEST_ASSERT_EQUAL_UINT32(4, reboot.configuration().current()->allocated.plants);
    expect(ConfigChange::Identity, reboot.replaceConfiguration(withUnbound));
    expect(Rejection::Duplicate, reboot.irrigation().requestManual(101, {1, 20}));
}

void test_stop_cancels_dispatched_permit_and_ignores_repeated_events() {
    Rig device;
    device.configure();
    expect(Rejection::None, device.irrigation().requestManual(1, {1, 10}));
    device.tick(2);
    const auto permit = device.pump.permits.back();
    device.irrigation().stopAll();
    TEST_ASSERT_EQUAL_UINT64(0, device.pump.stops.back().executionId);
    TEST_ASSERT_FALSE(device.pump.activate(device.clock.now()));
    TEST_ASSERT_TRUE(device.irrigation().automaticPaused());
    TEST_ASSERT_TRUE(device.engine().phase() == Phase::Stopping);
    device.tick(3);
    TEST_ASSERT_TRUE(device.history.irrigation.empty());
    TEST_ASSERT_FALSE(device.irrigation().resumeAutomatic());
    device.pump.confirmRevoked(device.clock.ms);
    device.tick(3);
    device.pump.events.push_back(PumpStarted{permit.plan.executionId, device.clock.now()});
    device.pump.events.push_back(PumpStopped{permit.plan.executionId, device.clock.ms, StopEvidence::Unknown, {}});
    device.tick(2);
    TEST_ASSERT_EQUAL_UINT32(1, device.history.irrigation.size());
    TEST_ASSERT_EQUAL_UINT32(0, *device.history.irrigation[0].actualDurationMs);
    TEST_ASSERT_TRUE(device.history.irrigation[0].result == IrrigationResult::Stopped);
    TEST_ASSERT_EQUAL_UINT32(1, device.pump.permits.size());
    TEST_ASSERT_TRUE(device.runtime.stored.snapshot->critical.autoPaused);
}

void test_physically_started_before_revocation_is_accounted_with_actual_stop_time() {
    Rig device;
    auto config = twoPlants();
    config.sampleIntervalMs = 5'000;
    config.zones[0].rule.mode = AutoMode::Smart;
    config.zones[0].rule.consecutiveLowReadings = 1;
    config.zones[0].rule.cooldownMs = 10'000;
    device.sensors.raw[0] = 500;
    device.configure(config);
    expect(Rejection::None, device.irrigation().requestManual(7, {1, 10}));
    device.tick(2);
    TEST_ASSERT_TRUE(device.pump.activate(device.clock.now(), false));
    device.clock.ms += 200;
    device.irrigation().stopAll();
    device.pump.events.push_back(*device.pump.running);
    device.tick();
    TEST_ASSERT_TRUE(device.engine().phase() == Phase::Stopping);
    const auto actualOff = device.clock.ms;
    device.advance(3'000);  // 可靠反馈迟于实际停泵。
    device.pump.measured(actualOff, true);
    device.tick(4);
    TEST_ASSERT_EQUAL_UINT32(200, *device.history.irrigation[0].actualDurationMs);
    TEST_ASSERT_EQUAL_UINT32(2, device.runtime.stored.snapshot->critical.usage[0].usedMl);
    TEST_ASSERT_EQUAL_UINT32(1, device.history.irrigation.size());
    TEST_ASSERT_EQUAL_UINT32(1, device.pump.permits.size());
    TEST_ASSERT_TRUE(device.irrigation().resumeAutomatic());
    device.tick(3);
    device.sensors.raw[0] = 200;
    device.advance(7'001, 3);
    TEST_ASSERT_EQUAL_UINT32(2, device.pump.permits.size());
    TEST_ASSERT_TRUE(device.pump.permits.back().plan.request.source == IrrigationSource::Smart);
}

void test_async_reservation_rechecks_schedule_when_window_closes() {
    Rig device;
    auto config = twoPlants();
    config.autoIrrigationWindow = {true, 300, 302};
    config.zones[0].rule.mode = AutoMode::Scheduled;
    config.zones[0].rule.schedule = {{301, 10}};
    device.configure(config);
    device.advance(59'000);
    device.runtime.automatic = false;
    device.advance(1'000);
    TEST_ASSERT_TRUE(device.runtime.inProgress->second.critical.pending.has_value());
    device.advance(60'000);
    TEST_ASSERT_TRUE(device.pump.permits.empty());
    device.runtime.automatic = true;
    device.tick(6);
    TEST_ASSERT_TRUE(device.pump.permits.empty());
    TEST_ASSERT_EQUAL_UINT32(1, device.history.irrigation.size());
    TEST_ASSERT_EQUAL_UINT32(0, *device.history.irrigation[0].actualDurationMs);
    TEST_ASSERT_EQUAL_UINT16(301, *device.runtime.stored.snapshot->critical.date->consumedThroughMinute);
}

void test_group_waits_for_reliable_members_cancels_on_fault_and_retries_after_removed_config() {
    Rig device;
    auto config = twoPlants();
    device.configure(config);
    expect(Rejection::None, device.irrigation().requestGroup(20, 1, {10, 15}));
    expect(Rejection::Duplicate, device.irrigation().requestGroup(20, 1, {10, 15}));
    expect(Rejection::RequestConflict, device.irrigation().requestGroup(20, 1, {15, 10}));
    device.tick(2);
    TEST_ASSERT_EQUAL_UINT32(2, device.pump.permits[0].plan.request.dose.zoneId);
    TEST_ASSERT_TRUE(device.pump.activate(device.clock.now()));
    device.tick(3);
    device.advance(1'000);
    device.runtime.automatic = false;
    device.pump.measured(device.clock.ms);
    device.tick();
    expect(ConfigChange::Busy, device.app.replaceConfiguration(config));
    device.tick(3);
    TEST_ASSERT_EQUAL_UINT32(1, device.pump.permits.size());
    device.runtime.automatic = true;
    device.tick(5);
    TEST_ASSERT_EQUAL_UINT32(2, device.pump.permits.size());
    TEST_ASSERT_EQUAL_UINT32(1, device.history.irrigation.size());
    TEST_ASSERT_EQUAL_UINT32(1, device.pump.permits[1].plan.request.dose.zoneId);
    const auto second = device.pump.permits[1].plan.executionId;
    device.pump.events.push_back(PumpFault{second});
    device.tick();
    TEST_ASSERT_EQUAL_UINT64(second, device.pump.stops.back().executionId);
    TEST_ASSERT_FALSE(device.irrigation().unlockFault());
    device.pump.confirmRevoked(device.clock.ms);
    device.tick(5);
    TEST_ASSERT_FALSE(device.engine().groupActive());
    TEST_ASSERT_TRUE(device.irrigation().fault() == Fault::Device);
    TEST_ASSERT_EQUAL_UINT32(2, device.history.irrigation.size());
    config.groups.clear();
    expect(ConfigChange::Applied, device.app.replaceConfiguration(config));
    const auto duplicate = device.irrigation().requestGroup(20, 1, {10, 15});
    expect(Rejection::Duplicate, duplicate);
    TEST_ASSERT_TRUE(duplicate.previousResult == IrrigationResult::Failed);
    expect(Rejection::RequestConflict, device.irrigation().requestGroup(20, 1, {11, 15}));
}

void test_reservation_save_failure_and_old_ack_cannot_start_pump() {
    Rig device;
    device.configure();
    device.runtime.automatic = false;
    expect(Rejection::None, device.irrigation().requestManual(1, {1, 10}));
    device.tick();
    const auto reservation = device.runtime.complete(false);
    device.tick();
    TEST_ASSERT_TRUE(device.irrigation().runtimeSaveFailed());
    TEST_ASSERT_TRUE(device.irrigation().automaticPaused());
    TEST_ASSERT_TRUE(device.pump.permits.empty());
    TEST_ASSERT_TRUE(device.history.irrigation.empty());
    const auto writes = device.runtime.writes;
    device.tick(5);
    TEST_ASSERT_EQUAL_INT(writes, device.runtime.writes);
    device.advance(1'000);
    TEST_ASSERT_FALSE(device.runtime.inProgress->second.critical.pending.has_value());
    device.runtime.results.push_back({reservation, true});
    device.tick();
    TEST_ASSERT_TRUE(device.engine().phase() == Phase::Settling);
    device.runtime.automatic = true;
    device.tick(5);
    TEST_ASSERT_TRUE(device.pump.permits.empty());
    TEST_ASSERT_EQUAL_UINT32(1, device.history.irrigation.size());
    TEST_ASSERT_EQUAL_UINT32(0, device.runtime.stored.snapshot->critical.usage[0].usedMl);
    TEST_ASSERT_TRUE(device.runtime.stored.snapshot->critical.autoPaused);
}

void test_stop_during_save_requires_new_durable_pause_and_settlement() {
    Rig device;
    device.configure();
    device.runtime.automatic = false;
    expect(Rejection::None, device.irrigation().requestManual(9, {1, 10}));
    device.tick();
    TEST_ASSERT_TRUE(device.runtime.inProgress->second.critical.pending.has_value());
    const auto stopCount = device.pump.stops.size();
    device.irrigation().stopAll();
    TEST_ASSERT_EQUAL_UINT32(stopCount + 1, device.pump.stops.size());
    const auto old = device.runtime.complete(true);
    device.tick();
    TEST_ASSERT_EQUAL_UINT64(old + 1, device.runtime.inProgress->first);
    TEST_ASSERT_FALSE(device.runtime.stored.snapshot->critical.autoPaused);
    TEST_ASSERT_TRUE(device.runtime.inProgress->second.critical.autoPaused);
    TEST_ASSERT_TRUE(device.pump.permits.empty());
    expect(Rejection::Busy, device.irrigation().requestManual(10, {2, 10}));
    device.runtime.complete(false);
    device.tick();
    TEST_ASSERT_TRUE(device.irrigation().runtimeSaveFailed());
    TEST_ASSERT_TRUE(device.irrigation().automaticPaused());
    TEST_ASSERT_TRUE(device.history.irrigation.empty());
    device.runtime.automatic = true;
    device.advance(1'000, 5);
    TEST_ASSERT_TRUE(device.runtime.stored.snapshot->critical.autoPaused);
    TEST_ASSERT_EQUAL_UINT32(1, device.history.irrigation.size());
    TEST_ASSERT_TRUE(device.pump.permits.empty());
}

void test_pause_resume_and_fault_clear_require_durable_state_and_off_confirmation() {
    Rig device;
    device.configure();
    device.irrigation().stopAll();
    device.tick(3);
    device.runtime.automatic = false;
    TEST_ASSERT_TRUE(device.irrigation().resumeAutomatic());
    TEST_ASSERT_TRUE(device.irrigation().automaticPaused());
    device.tick();
    device.runtime.complete(false);
    device.tick();
    TEST_ASSERT_TRUE(device.irrigation().automaticPaused());
    expect(Rejection::SaveFailed, device.irrigation().requestManual(1, {1, 10}));
    device.advance(1'000);
    device.irrigation().stopAll();
    device.runtime.automatic = true;
    device.tick(5);
    TEST_ASSERT_TRUE(device.irrigation().automaticPaused());
    TEST_ASSERT_TRUE(device.runtime.stored.snapshot->critical.autoPaused);

    expect(Rejection::None, device.irrigation().requestManual(1, {1, 10}));
    device.tick(2);
    device.pump.events.push_back(PumpFault{device.pump.permits.back().plan.executionId});
    device.tick();
    device.pump.confirmRevoked(device.clock.ms);
    device.tick(5);
    device.pump.verified = false;
    TEST_ASSERT_FALSE(device.irrigation().unlockFault());
    device.pump.verified = true;
    device.runtime.automatic = false;
    TEST_ASSERT_TRUE(device.irrigation().unlockFault());
    TEST_ASSERT_TRUE(device.irrigation().fault() == Fault::Device);
    device.tick();
    device.runtime.complete(false);
    device.tick();
    TEST_ASSERT_TRUE(device.irrigation().fault() == Fault::Device);
    device.runtime.automatic = true;
    device.advance(1'000, 5);
    TEST_ASSERT_TRUE(device.irrigation().fault() == Fault::None);
    TEST_ASSERT_TRUE(device.irrigation().automaticPaused());
}

void test_receipt_capacity_age_starts_at_reliable_end_and_purge_must_persist() {
    Rig device;
    device.configure();
    for (RequestId id = 1; id < 8; ++id) {
        expect(Rejection::None, device.irrigation().requestManual(id, {1, 10}));
        device.irrigation().stopAll();
        device.tick(4);
    }
    expect(Rejection::None, device.irrigation().requestManual(8, {1, 10}));
    device.irrigation().stopAll();
    device.runtime.automatic = false;
    device.tick();
    device.advance(60'000);
    device.runtime.automatic = true;
    device.tick(4);
    expect(Rejection::Capacity, device.irrigation().requestManual(9, {1, 10}));
    expect(Rejection::Duplicate, device.irrigation().requestManual(8, {1, 10}));
    device.advance(540'000, 2);
    expect(Rejection::Capacity, device.irrigation().requestManual(9, {1, 10}));

    device.clock.ms -= 2'000;
    device.tick(3);
    TEST_ASSERT_EQUAL_UINT32(8, device.runtime.stored.snapshot->critical.manualReceipts.size());
    device.runtime.automatic = false;
    device.advance(2'001);
    TEST_ASSERT_EQUAL_UINT32(1, device.runtime.inProgress->second.critical.manualReceipts.size());
    expect(Rejection::CriticalState, device.irrigation().requestManual(9, {1, 10}));
    device.runtime.complete(false);
    device.tick();
    expect(Rejection::SaveFailed, device.irrigation().requestManual(9, {1, 10}));
    expect(Rejection::Duplicate, device.irrigation().requestManual(8, {1, 10}));
    expect(Rejection::RequestConflict, device.irrigation().requestManual(8, {1, 20}));
    TEST_ASSERT_EQUAL_UINT32(8, device.runtime.stored.snapshot->critical.manualReceipts.size());
    device.runtime.automatic = true;
    device.advance(1'000, 4);
    expect(Rejection::Duplicate, device.irrigation().requestManual(8, {1, 10}));
    expect(Rejection::None, device.irrigation().requestManual(9, {1, 10}));
}

void test_reboot_preserves_receipts_when_saved_time_is_ahead() {
    Rig device;
    device.configure();
    device.advance(61'000);
    expect(Rejection::None, device.irrigation().requestManual(1, {1, 10}));
    device.irrigation().stopAll();
    device.tick(5);
    const auto savedUtc = device.runtime.stored.snapshot->receiptRetention[0].reliableEndUtc;
    TEST_ASSERT_TRUE(savedUtc.has_value());
    TestClock rollback;
    rollback.epoch = *savedUtc - 3'600;
    TestSensors sensors(rollback);
    IrrigationScheduler reboot(channels(), rollback, sensors, device.pump, device.configs,
                                device.runtime, device.history);
    reboot.tick();
    rollback.ms += 601'000;
    reboot.tick();
    expect(Rejection::Duplicate, reboot.irrigation().requestManual(1, {1, 10}));
    rollback.epoch = *savedUtc + 3'600;
    reboot.tick();
    expect(Rejection::Duplicate, reboot.irrigation().requestManual(1, {1, 10}));
    rollback.ms += 60'000;
    reboot.tick();
    reboot.tick();
    expect(Rejection::None, reboot.irrigation().requestManual(1, {1, 10}));
}

void test_initialization_waits_for_off_and_recovers_partial_first_use() {
    Rig device;
    device.pump.verified = false;
    device.tick(2);
    expect(BootStatus::WaitingForPumps, device.irrigation());
    TEST_ASSERT_EQUAL_UINT32(1, device.pump.stops.size());
    TEST_ASSERT_EQUAL_INT(0, device.configs.reads);
    TEST_ASSERT_EQUAL_INT(0, device.runtime.reads);
    device.pump.verified = true;
    device.configs.fail = true;
    device.tick(4);
    expect(BootStatus::Initializing, device.irrigation());
    TEST_ASSERT_TRUE(device.irrigation().initializationConfigSaveFailed());
    TEST_ASSERT_TRUE(device.runtime.stored.snapshot->initializing);
    TEST_ASSERT_EQUAL_INT(1, device.configs.writes);
    device.configs.fail = false;
    IrrigationScheduler reboot(channels(), device.clock, device.sensors, device.pump, device.configs,
                               device.runtime, device.history);
    for (int i = 0; i < 5; ++i) {
        reboot.tick();
    }
    expect(BootStatus::Ready, reboot.irrigation());
    TEST_ASSERT_FALSE(device.runtime.stored.snapshot->initializing);
    TEST_ASSERT_EQUAL_UINT32(0, reboot.configuration().current()->config.plants.size());
}

void test_bad_storage_locks_irrigation_and_does_not_erase_or_disable_monitoring() {
    for (const auto status : {LoadStatus::Missing, LoadStatus::Corrupt, LoadStatus::Incompatible, LoadStatus::ReadFailed}) {
        Rig device;
        device.runtime.stored.status = status;
        device.configs.stored = {LoadStatus::Loaded, ConfigSnapshot{twoPlants(), {2, 2, 2, 2, 1}}};
        device.clock.trusted = false;
        device.tick(4);
        expect(BootStatus::StorageUnavailable, device.irrigation());
        TEST_ASSERT_TRUE(device.irrigation().runtimeLoadStatus() == status);
        expect(Rejection::CriticalState, device.irrigation().requestManual(1, {1, 10}));
        TEST_ASSERT_EQUAL_INT(0, device.runtime.writes);
        TEST_ASSERT_EQUAL_INT(0, device.configs.writes);
        TEST_ASSERT_EQUAL_UINT32(2, device.app.monitoring().latest().soil.size());
        TEST_ASSERT_FALSE(device.history.monitoring[0].air->sampledAtUtcSeconds.has_value());
    }
    Rig device;
    device.configs.stored = {LoadStatus::Missing, {}};
    device.runtime.stored = {LoadStatus::Loaded, RuntimeSnapshot{}};
    device.tick(4);
    expect(BootStatus::StorageUnavailable, device.irrigation());
    TEST_ASSERT_TRUE(device.app.configuration().loadStatus() == LoadStatus::Missing);
    TEST_ASSERT_TRUE(device.app.monitoring().latest().air.has_value());
    TEST_ASSERT_TRUE(device.pump.permits.empty());
}

void test_boot_rejects_a_config_snapshot_with_old_accounting_bindings() {
    Rig device;
    device.configure();
    expect(Rejection::None, device.irrigation().requestManual(1, {1, 10}));
    device.tick(2);
    device.finishPlanned();
    auto& config = device.configs.stored.snapshot->config;
    std::swap(config.pumps[0].channel, config.pumps[1].channel);
    IrrigationScheduler reboot(channels(), device.clock, device.sensors, device.pump, device.configs,
                               device.runtime, device.history);
    reboot.tick();
    expect(BootStatus::AccountingBindingConflict, reboot.irrigation());
    expect(Rejection::CriticalState, reboot.irrigation().requestManual(2, {1, 10}));
    TEST_ASSERT_EQUAL_UINT32(10, device.runtime.stored.snapshot->critical.usage[0].usedMl);
}

void test_reboot_settles_unknown_once_without_continuing_group() {
    Rig device;
    device.configure();
    device.irrigation().stopAll();
    device.tick(3);
    expect(Rejection::None, device.irrigation().requestGroup(30, 1, {10, 20}));
    device.tick(2);
    const auto pending = device.runtime.stored.snapshot->critical.pending;
    TEST_ASSERT_TRUE(pending.has_value());

    TestPump poweredOff(device.runtime);
    IrrigationScheduler reboot(channels(), device.clock, device.sensors, poweredOff, device.configs,
                               device.runtime, device.history);
    for (int i = 0; i < 7; ++i) {
        reboot.tick();
    }
    expect(BootStatus::Ready, reboot.irrigation());
    TEST_ASSERT_TRUE(reboot.irrigation().automaticPaused());
    TEST_ASSERT_TRUE(reboot.irrigation().fault() == Fault::UnknownDelivery);
    TEST_ASSERT_TRUE(poweredOff.permits.empty());
    TEST_ASSERT_EQUAL_UINT32(10, device.runtime.stored.snapshot->critical.usage[0].usedMl);
    const auto duplicate = reboot.irrigation().requestGroup(30, 1, {10, 20});
    expect(Rejection::Duplicate, duplicate);
    TEST_ASSERT_TRUE(duplicate.previousResult == IrrigationResult::Unknown);
    TEST_ASSERT_EQUAL_UINT32(1, device.history.irrigation.size());
    TEST_ASSERT_EQUAL_UINT64(pending->executionId, device.history.irrigation[0].executionId);
    TEST_ASSERT_FALSE(device.history.irrigation[0].actualDurationMs.has_value());
    TEST_ASSERT_FALSE(device.history.irrigation[0].estimatedVolumeMl.has_value());

    IrrigationScheduler secondBoot(channels(), device.clock, device.sensors, poweredOff, device.configs,
                                   device.runtime, device.history);
    secondBoot.tick();
    TEST_ASSERT_EQUAL_UINT32(10, device.runtime.stored.snapshot->critical.usage[0].usedMl);
    TEST_ASSERT_EQUAL_UINT32(1, device.history.irrigation.size());
}

void test_history_failure_cannot_delay_stop_or_reverse_reliable_quota() {
    Rig device;
    device.configure();
    device.history.failIrrigation = true;
    device.history.failMonitoring = true;
    expect(Rejection::None, device.irrigation().requestManual(21, {1, 10}));
    device.tick(2);
    TEST_ASSERT_TRUE(device.pump.activate(device.clock.now()));
    device.tick(2);
    device.advance(200);
    device.irrigation().stopAll();
    TEST_ASSERT_EQUAL_UINT64(0, device.pump.stops.back().executionId);
    TEST_ASSERT_TRUE(device.history.irrigation.empty());
    device.pump.measured(device.clock.ms, true);
    device.tick(5);
    TEST_ASSERT_TRUE(device.irrigation().historyWriteFailed());
    TEST_ASSERT_EQUAL_UINT32(2, device.runtime.stored.snapshot->critical.usage[0].usedMl);
    device.clock.trusted = false;
    device.advance(300'000);
    TEST_ASSERT_TRUE(device.app.monitoring().historyWriteFailed());
    TEST_ASSERT_FALSE(device.app.monitoring().latest().air->sampledAtUtcSeconds.has_value());
    expect(Rejection::InvalidTime, device.irrigation().requestManual(22, {2, 10}));
    TEST_ASSERT_EQUAL_UINT32(1, device.history.irrigation.size());
}

}  // namespace

void setUp() {}
void tearDown() {}

int main() {
    UNITY_BEGIN();
    RUN_TEST(test_two_plants_from_reliable_configuration_to_monitored_manual_delivery);
    RUN_TEST(test_schedule_is_independent_of_collection_and_resolves_conflicts_by_zone);
    RUN_TEST(test_smart_rejects_cached_and_failed_samples_and_restarts_after_config_change);
    RUN_TEST(test_auto_window_leaves_monitoring_history_and_manual_independent);
    RUN_TEST(test_config_validation_atomic_save_watermarks_and_accounting_bindings);
    RUN_TEST(test_stop_cancels_dispatched_permit_and_ignores_repeated_events);
    RUN_TEST(test_physically_started_before_revocation_is_accounted_with_actual_stop_time);
    RUN_TEST(test_async_reservation_rechecks_schedule_when_window_closes);
    RUN_TEST(test_group_waits_for_reliable_members_cancels_on_fault_and_retries_after_removed_config);
    RUN_TEST(test_reservation_save_failure_and_old_ack_cannot_start_pump);
    RUN_TEST(test_stop_during_save_requires_new_durable_pause_and_settlement);
    RUN_TEST(test_pause_resume_and_fault_clear_require_durable_state_and_off_confirmation);
    RUN_TEST(test_receipt_capacity_age_starts_at_reliable_end_and_purge_must_persist);
    RUN_TEST(test_reboot_preserves_receipts_when_saved_time_is_ahead);
    RUN_TEST(test_initialization_waits_for_off_and_recovers_partial_first_use);
    RUN_TEST(test_bad_storage_locks_irrigation_and_does_not_erase_or_disable_monitoring);
    RUN_TEST(test_boot_rejects_a_config_snapshot_with_old_accounting_bindings);
    RUN_TEST(test_reboot_settles_unknown_once_without_continuing_group);
    RUN_TEST(test_history_failure_cannot_delay_stop_or_reverse_reliable_quota);
    return UNITY_END();
}
