
#include <limits>

#include <unity.h>

#include "core/irrigation/Irrigation.h"

using namespace planthub::core;

namespace {

const ChannelCapabilities channels{{0, 1}, {0, 1}};
constexpr UtcSeconds day = 86'400;

SystemConfig configuration() {
    SystemConfig config;
    config.utcOffsetMinutes = 0;
    config.sampleIntervalMs = 1'000;
    // 既有回归以午夜附近为时间基准，时段规则使用独立用例验证。
    config.autoIrrigationWindow.enabled = false;
    config.pumps = {{21, "Pump 1", 0, 60'000, 1, 5'000}, {22, "Pump 2", 1, 60'000, 1, 5'000}};
    config.soilSensors = {{11, "Soil 1", 0, SoilCalibration{0, 100}}, {12, "Soil 2", 1, SoilCalibration{0, 100}}};
    config.plants = {{1, "Plant 1", true, 11, 31}, {2, "Plant 2", true, 12, 32}};
    config.zones = {{32, "Zone 2", 22, 12, {}}, {31, "Zone 1", 21, 11, {}}};
    for (auto& zone : config.zones) {
        zone.rule.singleLimitMl = 100;
        zone.rule.dailyLimitMl = 200;
        zone.rule.lowMoisturePercent = 30;
        zone.rule.smartDoseMl = 30;
        zone.rule.cooldownMs = 5'000;
        zone.rule.schedule = {{1, 30}, {2, 30}, {3, 30}};
    }
    config.groups = {{41, "Group", {31, 32}}, {42, "Reverse", {32, 31}}};
    return config;
}

IrrigationRequest manual(RequestId id, EntityId zone = 31, VolumeMl dose = 30) {
    return {id, IrrigationSource::Manual, {zone, dose}, {}};
}

void expect(Rejection expected, Decision actual) {
    TEST_ASSERT_EQUAL_INT(static_cast<int>(expected), static_cast<int>(actual.error));
}

CriticalState save(Irrigation& engine) {
    const auto draft = engine.beginSave();
    TEST_ASSERT_TRUE(draft.has_value());
    TEST_ASSERT_FALSE(engine.beginSave().has_value());
    TEST_ASSERT_FALSE(engine.finishSave(draft->id + 1, true));
    TEST_ASSERT_TRUE(engine.finishSave(draft->id, true));
    TEST_ASSERT_FALSE(engine.finishSave(draft->id, true));
    return draft->state;
}

void clock(Irrigation& engine, const SystemConfig& config, MonotonicMs ms, UtcSeconds utc) {
    engine.observeTime(config, {ms, utc});
}

void recordSoil(Irrigation& engine, const SystemConfig& config, MonotonicMs stamp, float moisture = 20,
                ReadingStatus status = ReadingStatus::Valid, EntityId sensor = 11) {
    const SensorReading reading{stamp, {}, SoilReading{sensor, 20, moisture, status}};
    engine.observeSoil(config, 31, &reading);
}

void soil(Irrigation& engine, const SystemConfig& config, MonotonicMs now, MonotonicMs stamp, float moisture = 20,
          ReadingStatus status = ReadingStatus::Valid, EntityId sensor = 11) {
    clock(engine, config, now, day + static_cast<UtcSeconds>(now / 1'000));
    recordSoil(engine, config, stamp, moisture, status, sensor);
}

StartPermit start(Irrigation& engine, const SystemConfig& config) {
    TEST_ASSERT_FALSE(engine.issueStart(config).has_value());
    save(engine);
    const auto permission = engine.issueStart(config);
    TEST_ASSERT_TRUE(permission.has_value());
    TEST_ASSERT_FALSE(engine.issueStart(config).has_value());
    TEST_ASSERT_TRUE(engine.confirmStarted(permission->plan.executionId, permission->issuedAtMs, day));
    TEST_ASSERT_FALSE(engine.confirmStarted(permission->plan.executionId, permission->issuedAtMs));
    return *permission;
}

IrrigationRecord complete(Irrigation& engine, const StartPermit& permit) {
    TEST_ASSERT_TRUE(engine.confirmStopped(
        {permit.plan.executionId, permit.plannedStopMs, StopEvidence::Measured, permit.plan.plannedMs}));
    TEST_ASSERT_FALSE(engine.takeRecord().has_value());
    save(engine);
    const auto record = engine.takeRecord();
    TEST_ASSERT_TRUE(record.has_value());
    TEST_ASSERT_TRUE(record->result == IrrigationResult::Completed);
    return *record;
}

void test_full_config_readiness_and_manual_with_no_soil() {
    auto config = configuration();
    Irrigation engine(channels, Recovery::FirstInitialization, 0);
    expect(Rejection::InvalidTime, engine.accept(config, manual(1)));
    clock(engine, config, 0, day);
    config.pumps[0].channel.reset();
    expect(Rejection::InvalidConfig, engine.accept(config, manual(1)));
    config.pumps[0].channel = 0;
    config.pumps[0].flowMlPerMin.reset();
    TEST_ASSERT_TRUE(engine.accept(config, manual(1)).readiness == ZoneReadiness::MissingPumpCalibration);
    config.pumps[0].flowMlPerMin = 60'000;
    config.pumps[0].minDurationMs.reset();
    TEST_ASSERT_TRUE(engine.accept(config, manual(1)).readiness == ZoneReadiness::MissingPumpLimits);
    config.pumps[0].minDurationMs = 1;
    config.pumps[0].maxDurationMs = 10;
    TEST_ASSERT_TRUE(engine.accept(config, manual(1)).durationError == DurationError::TooLong);
    config.pumps[0].maxDurationMs = 5'000;

    config.zones[1].representativeSensorId.reset();
    config.plants[0].enabled = false;
    TEST_ASSERT_TRUE(validateConfig(config, channels).error == ConfigError::None);
    for (const auto source : {IrrigationSource::Manual, IrrigationSource::Scheduled, IrrigationSource::Smart}) {
        TEST_ASSERT_TRUE(checkZoneReadiness(config, channels, 31, source) == ZoneReadiness::NoEnabledPlants);
    }
    config.plants.erase(config.plants.begin());
    TEST_ASSERT_TRUE(engine.accept(config, manual(1)).readiness == ZoneReadiness::NoEnabledPlants);
    config.plants.push_back({1, "Plant 1", true, {}, 31});
    expect(Rejection::InvalidRequest, engine.accept(config, manual(1, 31, 0)));
    expect(Rejection::SingleLimit, engine.accept(config, manual(1, 31, 101)));
    expect(Rejection::None, engine.accept(config, manual(1)));
    TEST_ASSERT_TRUE(engine.activePlan()->channel == 0);
    expect(Rejection::Busy, engine.accept(config, manual(2, 32)));
}

void test_reservation_and_settlement_are_separate_reliable_saves() {
    auto config = configuration();
    Irrigation engine(channels, Recovery::FirstInitialization, 0);
    clock(engine, config, 0, day);
    expect(Rejection::None, engine.accept(config, manual(1)));
    TEST_ASSERT_TRUE(engine.remaining(31, 200) == 170);
    const auto rejected = engine.beginSave();
    TEST_ASSERT_TRUE(rejected->state.pending->request.dose.volumeMl == 30);
    TEST_ASSERT_EQUAL_UINT32(0, rejected->state.usage[0].usedMl);
    TEST_ASSERT_FALSE(engine.issueStart(config).has_value());
    TEST_ASSERT_TRUE(engine.finishSave(rejected->id, false));
    expect(Rejection::SaveFailed, engine.accept(config, manual(2)));
    TEST_ASSERT_TRUE(engine.accept(config, manual(1)).previousStage == RequestStage::Accepted);
    const auto permit = start(engine, config);
    TEST_ASSERT_TRUE(engine.accept(config, manual(1)).previousStage == RequestStage::InProgress);
    clock(engine, config, 30, day);
    TEST_ASSERT_TRUE(engine.pendingStop()->reason == StopReason::Planned);
    expect(Rejection::Busy, engine.accept(config, manual(2, 32)));
    TEST_ASSERT_FALSE(engine.confirmStopped({permit.plan.executionId + 1, 30, StopEvidence::Measured, 30}));
    TEST_ASSERT_TRUE(engine.confirmStopped({permit.plan.executionId, 30, StopEvidence::Measured, 30}));
    TEST_ASSERT_FALSE(engine.confirmStopped({permit.plan.executionId, 30, StopEvidence::Measured, 30}));
    const auto settlement = engine.beginSave();
    TEST_ASSERT_TRUE(engine.finishSave(settlement->id, false));
    TEST_ASSERT_TRUE(engine.phase() == Phase::Settling);
    TEST_ASSERT_FALSE(engine.remaining(31, 200).has_value());
    TEST_ASSERT_FALSE(engine.takeRecord().has_value());
    expect(Rejection::SaveFailed, engine.accept(config, manual(2)));
    const auto durable = save(engine);
    TEST_ASSERT_FALSE(durable.pending.has_value());
    TEST_ASSERT_EQUAL_UINT32(30, durable.usage[0].usedMl);
    TEST_ASSERT_TRUE(engine.takeRecord()->result == IrrigationResult::Completed);
    TEST_ASSERT_FALSE(engine.takeRecord().has_value());
    expect(Rejection::Duplicate, engine.accept(config, manual(1)));
    expect(Rejection::RequestConflict, engine.accept(config, manual(1, 32)));
    config.zones[1].rule.dailyLimitMl = 20;
    TEST_ASSERT_TRUE(engine.remaining(31, 20) == 0);
    expect(Rejection::DailyLimit, engine.accept(config, manual(2, 31, 1)));
}

void test_stop_during_reservation_save_revokes_start_and_requires_new_save() {
    const auto config = configuration();
    Irrigation engine(channels, Recovery::FirstInitialization, 0);
    clock(engine, config, 0, day);
    expect(Rejection::None, engine.accept(config, manual(1)));
    const auto reserve = engine.beginSave();
    engine.stopAll();
    TEST_ASSERT_TRUE(engine.state().autoPaused);
    TEST_ASSERT_TRUE(engine.phase() == Phase::Settling);
    TEST_ASSERT_TRUE(engine.finishSave(reserve->id, true));
    TEST_ASSERT_FALSE(engine.issueStart(config).has_value());
    TEST_ASSERT_FALSE(engine.confirmStarted(reserve->state.pending->executionId, 0));
    TEST_ASSERT_FALSE(engine.resumeAutomatic());
    const auto cancelled = engine.beginSave();
    TEST_ASSERT_TRUE(engine.finishSave(cancelled->id, false));
    TEST_ASSERT_TRUE(engine.state().autoPaused);
    expect(Rejection::SaveFailed, engine.accept(config, manual(2)));
    save(engine);
    const auto record = engine.takeRecord();
    TEST_ASSERT_TRUE(record->actualDurationMs == 0 && record->estimatedVolumeMl == 0.0);
    TEST_ASSERT_TRUE(record->result == IrrigationResult::Stopped);
    TEST_ASSERT_TRUE(engine.state().recentlyWateredZones.empty());
    TEST_ASSERT_TRUE(engine.state().recentlyUsedChannels.empty());
    TEST_ASSERT_TRUE(engine.resumeAutomatic());
    save(engine);
}

void test_late_start_after_stop_keeps_stopping_and_accounts_for_real_delivery() {
    auto config = configuration();
    Irrigation engine(channels, Recovery::FirstInitialization, 0);
    clock(engine, config, 0, day);
    config.pumps[0].flowMlPerMin = 90'000;
    expect(Rejection::None, engine.accept(config, manual(1)));
    save(engine);
    const auto permit = engine.issueStart(config);
    engine.observeTime(config, {1, {}});
    engine.stopAll();
    const auto id = permit->plan.executionId;
    TEST_ASSERT_TRUE(engine.pendingStop()->reason == StopReason::Manual);
    TEST_ASSERT_TRUE(engine.confirmStarted(id, 1));
    TEST_ASSERT_TRUE(engine.phase() == Phase::Stopping);
    TEST_ASSERT_FALSE(engine.confirmStopped({id, 0, StopEvidence::NeverStarted, {}}));
    TEST_ASSERT_TRUE(engine.confirmStopped({id, 2, StopEvidence::Measured, 1}));
    const auto state = save(engine);
    TEST_ASSERT_EQUAL_UINT32(2, state.usage[0].usedMl);
    TEST_ASSERT_TRUE(engine.takeRecord()->stopReason == StopReason::Manual);
    TEST_ASSERT_TRUE(state.autoPaused);
}

void test_confirmed_never_started_releases_dispatched_reservation() {
    const auto config = configuration();
    Irrigation engine(channels, Recovery::FirstInitialization, 0);
    clock(engine, config, 1, day);
    expect(Rejection::None, engine.accept(config, manual(1)));
    save(engine);
    const auto permit = engine.issueStart(config);
    engine.stopAll();
    TEST_ASSERT_TRUE(engine.confirmStopped({permit->plan.executionId, 2, StopEvidence::NeverStarted, {}}));
    save(engine);
    TEST_ASSERT_TRUE(engine.remaining(31, 200) == 200);
    TEST_ASSERT_TRUE(engine.takeRecord()->actualDurationMs == 0);
    TEST_ASSERT_FALSE(engine.confirmStarted(permit->plan.executionId, 2));
}

void test_clock_deadlines_and_snapshot_survive_calibration_and_wall_changes() {
    auto config = configuration();
    Irrigation engine(channels, Recovery::FirstInitialization, 1'000);
    clock(engine, config, 1'000, day);
    expect(Rejection::None, engine.accept(config, manual(1)));
    const auto permit = start(engine, config);
    TEST_ASSERT_EQUAL_UINT64(1'030, permit.plannedStopMs);
    TEST_ASSERT_EQUAL_UINT64(6'000, permit.hardStopMs);
    config.pumps[0].flowMlPerMin = 120'000;
    config.pumps[0].maxDurationMs = 50'000;
    clock(engine, config, 1'029, 3 * day);
    TEST_ASSERT_FALSE(engine.pendingStop().has_value());
    engine.observeTime(config, {1'030, {}});
    TEST_ASSERT_TRUE(engine.pendingStop()->reason == StopReason::Planned);
    clock(engine, config, 6'000, day);
    TEST_ASSERT_TRUE(engine.pendingStop()->reason == StopReason::MaximumDuration);
    // 事件迟到，实际启停时刻仍与确认时长一致。
    TEST_ASSERT_TRUE(engine.confirmStopped({permit.plan.executionId, 1'040, StopEvidence::Measured, 40}));
    const auto state = save(engine);
    const auto record = engine.takeRecord();
    TEST_ASSERT_TRUE(record->result == IrrigationResult::Failed);
    TEST_ASSERT_TRUE(record->estimatedVolumeMl == 40.0);
    TEST_ASSERT_EQUAL_UINT32(40, state.usage[0].usedMl);
    TEST_ASSERT_TRUE(state.fault == Fault::ExcessDelivery);
}

void test_monotonic_regression_protects_pending_start_and_overflow_is_rejected() {
    const auto config = configuration();
    Irrigation engine(channels, Recovery::FirstInitialization, 100);
    clock(engine, config, 100, day);
    expect(Rejection::None, engine.accept(config, manual(1)));
    save(engine);
    const auto permit = engine.issueStart(config);
    clock(engine, config, 99, day);
    TEST_ASSERT_TRUE(engine.pendingStop()->reason == StopReason::ClockFault);
    TEST_ASSERT_TRUE(engine.confirmStarted(permit->plan.executionId, 100));
    TEST_ASSERT_TRUE(engine.phase() == Phase::Stopping);
    TEST_ASSERT_TRUE(engine.confirmStopped({permit->plan.executionId, 101, StopEvidence::Unknown, {}}));
    save(engine);
    TEST_ASSERT_TRUE(engine.state().fault == Fault::UnknownDelivery);
    TEST_ASSERT_TRUE(engine.takeRecord()->result == IrrigationResult::Unknown);

    Irrigation edge(channels, Recovery::FirstInitialization, 0);
    clock(edge, config, std::numeric_limits<MonotonicMs>::max() - 4'999, day);
    expect(Rejection::ClockRange, edge.accept(config, manual(1)));
}

void test_settlement_saturates_conversion_and_addition() {
    auto config = configuration();
    config.pumps[0].flowMlPerMin = 120'000;
    config.zones[1].rule.singleLimitMl = std::numeric_limits<VolumeMl>::max();
    config.zones[1].rule.dailyLimitMl = std::numeric_limits<VolumeMl>::max();
    for (const auto used : {0u, std::numeric_limits<VolumeMl>::max() - 31}) {
        CriticalState baseline;
        baseline.date = QuotaDate{1, 0, {}, false};
        baseline.usage = {{31, 0, used}};
        Irrigation engine(channels, Recovery::Loaded, 0, baseline);
        clock(engine, config, 0, day);
        expect(Rejection::None, engine.accept(config, manual(1)));
        const auto permit = start(engine, config);
        const auto duration = used == 0 ? std::numeric_limits<DurationMs>::max() : 20u;
        TEST_ASSERT_TRUE(engine.confirmStopped({permit.plan.executionId, duration, StopEvidence::Measured, duration}));
        const auto state = save(engine);
        TEST_ASSERT_TRUE(state.fault == Fault::AccountingOverflow);
        TEST_ASSERT_EQUAL_UINT32(std::numeric_limits<VolumeMl>::max(), state.usage[0].usedMl);
        TEST_ASSERT_TRUE(engine.remaining(31, std::numeric_limits<VolumeMl>::max()) == 0);
    }
}

void test_contradictory_stop_evidence_cannot_release_reservation_or_hide_excess() {
    struct Case {
        bool started;
        StopEvidence evidence;
        std::optional<DurationMs> reported;
        MonotonicMs elapsed;
        VolumeMl charged;
        Fault fault;
    };
    const Case cases[] = {
        {false, StopEvidence::NeverStarted, 5, 10, 30, Fault::UnknownDelivery},
        {true, StopEvidence::NeverStarted, 0, 10, 30, Fault::UnknownDelivery},
        {false, StopEvidence::Measured, 5, 4, 30, Fault::UnknownDelivery},
        {true, StopEvidence::Measured, 5, 4, 30, Fault::UnknownDelivery},
        {true, StopEvidence::Measured, 2, 10, 30, Fault::UnknownDelivery},
        {true, StopEvidence::Measured, 2, 45, 45, Fault::ExcessDelivery},
        {true, StopEvidence::Measured, {}, 45, 45, Fault::ExcessDelivery},
        {true, StopEvidence::Unknown, 50, 60, 30, Fault::UnknownDelivery},
        {true, StopEvidence::Measured, 2, MonotonicMs{1} + std::numeric_limits<DurationMs>::max(),
         std::numeric_limits<VolumeMl>::max(), Fault::AccountingOverflow},
    };
    const auto config = configuration();
    for (const auto& item : cases) {
        Irrigation engine(channels, Recovery::FirstInitialization, 0);
        clock(engine, config, 100, day);
        expect(Rejection::None, engine.accept(config, manual(1)));
        save(engine);
        const auto permit = engine.issueStart(config);
        TEST_ASSERT_TRUE(permit.has_value());
        const auto id = permit->plan.executionId;
        if (item.started) {
            TEST_ASSERT_TRUE(engine.confirmStarted(id, 100));
        }
        engine.stopAll();
        const PumpStopped event{id, 100 + item.elapsed, item.evidence, item.reported};
        TEST_ASSERT_TRUE(engine.confirmStopped(event));
        TEST_ASSERT_FALSE(engine.confirmStopped(event));
        expect(Rejection::FaultLocked, engine.accept(config, manual(2)));
        const auto durable = save(engine);
        TEST_ASSERT_EQUAL_UINT32(item.charged, durable.usage[0].usedMl);
        TEST_ASSERT_TRUE(durable.fault == item.fault);
        const auto record = engine.takeRecord();
        TEST_ASSERT_TRUE(record->result == IrrigationResult::Unknown);
        TEST_ASSERT_FALSE(record->actualDurationMs.has_value());
        TEST_ASSERT_FALSE(record->estimatedVolumeMl.has_value());
    }
}

void test_smart_uses_latest_valid_contiguous_sample_only_once() {
    auto config = configuration();
    config.zones[1].rule.mode = AutoMode::Smart;
    Irrigation engine(channels, Recovery::FirstInitialization, 0);
    clock(engine, config, 0, day);
    soil(engine, config, 1'000, 1'000);
    soil(engine, config, 1'000, 1'000);
    soil(engine, config, 2'000, 2'000);
    expect(Rejection::StaleTrigger, engine.acceptAutomatic(config));
    soil(engine, config, 3'000, 3'000);
    expect(Rejection::None, engine.acceptAutomatic(config));
    TEST_ASSERT_TRUE(engine.activePlan()->request.source == IrrigationSource::Smart);
    save(engine);
    soil(engine, config, 3'500, 3'000, 30);
    TEST_ASSERT_FALSE(engine.issueStart(config).has_value());
    save(engine);
    TEST_ASSERT_TRUE(engine.takeRecord()->actualDurationMs == 0);
    expect(Rejection::StaleTrigger, engine.acceptAutomatic(config));
}

void test_bad_sample_values_and_gaps_reset_smart_streak() {
    auto config = configuration();
    config.zones[1].rule.mode = AutoMode::Smart;
    for (const auto defect : {0, 1, 2, 3, 4, 5, 6, 7, 8}) {
        Irrigation engine(channels, Recovery::FirstInitialization, 0);
        clock(engine, config, 0, day);
        soil(engine, config, 1'000, 1'000);
        soil(engine, config, 2'000, 2'000);
        switch (defect) {
            case 0: soil(engine, config, 3'000, 3'000, 30); break;
            case 1: soil(engine, config, 3'000, 3'000, 0, ReadingStatus::OutOfRange); break;
            case 2: soil(engine, config, 3'000, 3'000, 20, ReadingStatus::Invalid); break;
            case 3: soil(engine, config, 5'001, 3'000); break;
            case 4: engine.observeSoil(config, 31, nullptr); break;
            case 5: soil(engine, config, 3'000, 1'500); break;
            case 6: soil(engine, config, 3'000, 3'001); break;
            case 7: soil(engine, config, 3'000, 3'000, std::numeric_limits<float>::quiet_NaN()); break;
            case 8: soil(engine, config, 4'000, 4'000); break;
        }
        expect(Rejection::StaleTrigger, engine.acceptAutomatic(config));
        soil(engine, config, 5'500, 5'500);
        soil(engine, config, 6'500, 6'500);
        expect(Rejection::StaleTrigger, engine.acceptAutomatic(config));
        soil(engine, config, 7'500, 7'500);
        expect(Rejection::None, engine.acceptAutomatic(config));
    }
}

void test_smart_continuity_tolerates_bounded_jitter_and_detects_a_missed_cycle() {
    struct Case {
        DurationMs interval;
        MonotonicMs firstGap;
        MonotonicMs secondGap;
        bool triggered;
    };
    const Case cases[] = {{1'000, 1'000, 1'000, true}, {1'000, 920, 1'090, true},
                          {1'000, 1'100, 1'100, true}, {1'000, 1'101, 1'000, false},
                          {1'000, 1'900, 1'000, false}, {600'000, 630'000, 600'000, true},
                          {600'000, 630'001, 600'000, false}};
    for (const auto& item : cases) {
        auto config = configuration();
        config.sampleIntervalMs = item.interval;
        config.zones[1].rule.mode = AutoMode::Smart;
        Irrigation engine(channels, Recovery::FirstInitialization, 0);
        clock(engine, config, 0, day);
        const auto second = 1'000 + item.firstGap;
        const auto third = second + item.secondGap;
        soil(engine, config, 1'000, 1'000);
        soil(engine, config, second, second);
        soil(engine, config, third, third);
        expect(item.triggered ? Rejection::None : Rejection::StaleTrigger, engine.acceptAutomatic(config));
    }
}

void test_schedules_open_at_start_and_consume_outside_slots_without_backfill() {
    auto config = configuration();
    config.autoIrrigationWindow.enabled = true;
    auto& rule = config.zones[1].rule;
    rule.mode = AutoMode::Scheduled;
    rule.schedule = {{299, 25}, {300, 30}, {1380, 35}};
    TEST_ASSERT_TRUE(validateConfig(config, channels).error == ConfigError::None);
    Irrigation engine(channels, Recovery::FirstInitialization, 0);
    clock(engine, config, 0, day + 299 * 60 - 1);
    clock(engine, config, 1'000, day + 299 * 60);
    TEST_ASSERT_TRUE(engine.state().date->consumedThroughMinute == 299);
    expect(Rejection::StaleTrigger, engine.acceptAutomatic(config));
    clock(engine, config, 60'000, day + 300 * 60 - 1);
    clock(engine, config, 61'000, day + 300 * 60);
    expect(Rejection::None, engine.acceptAutomatic(config));
    TEST_ASSERT_EQUAL_UINT32(30, engine.activePlan()->request.dose.volumeMl);
    complete(engine, start(engine, config));
    clock(engine, config, 100'000, day + 1380 * 60 - 1);
    clock(engine, config, 101'000, day + 1380 * 60);
    TEST_ASSERT_TRUE(engine.state().date->consumedThroughMinute == 1380);
    expect(Rejection::StaleTrigger, engine.acceptAutomatic(config));
    const IrrigationRequest outside{0, IrrigationSource::Scheduled, {31, 35}, Trigger{1, 1380, 0}};
    expect(Rejection::OutsideAutoWindow, engine.accept(config, outside));
    expect(Rejection::None, engine.accept(config, manual(1)));
    complete(engine, start(engine, config));
    expect(Rejection::None, engine.acceptGroup(config, 2, 41, {10, 10}));
}

void test_automatic_duration_must_fit_and_is_rechecked_after_saving() {
    for (const VolumeMl dose : {58, 59}) {
        auto config = configuration();
        config.autoIrrigationWindow.enabled = true;
        config.pumps[0].flowMlPerMin = 60;
        config.pumps[0].maxDurationMs = 60'000;
        auto& rule = config.zones[1].rule;
        rule.mode = AutoMode::Scheduled;
        rule.schedule = {{1379, dose}};
        Irrigation engine(channels, Recovery::FirstInitialization, 0);
        clock(engine, config, 0, day + 1379 * 60 - 1);
        clock(engine, config, 1'000, day + 1379 * 60);
        if (dose == 59) {
            expect(Rejection::OutsideAutoWindow, engine.acceptAutomatic(config));
            TEST_ASSERT_FALSE(engine.activePlan().has_value());
        } else {
            expect(Rejection::None, engine.acceptAutomatic(config));
            const auto reservation = engine.beginSave();
            TEST_ASSERT_TRUE(reservation.has_value());
            clock(engine, config, 3'000, day + 1379 * 60 + 2);
            TEST_ASSERT_TRUE(engine.finishSave(reservation->id, true));
            TEST_ASSERT_FALSE(engine.issueStart(config).has_value());
            TEST_ASSERT_TRUE(engine.phase() == Phase::Settling);
            const auto durable = save(engine);
            TEST_ASSERT_EQUAL_UINT32(0, durable.usage[0].usedMl);
            TEST_ASSERT_TRUE(engine.takeRecord()->actualDurationMs == 0);
        }
    }
}

void test_smart_window_restarts_sampling_and_config_reset_drops_old_candidate() {
    auto config = configuration();
    config.autoIrrigationWindow.enabled = true;
    config.zones[1].rule.mode = AutoMode::Smart;
    Irrigation engine(channels, Recovery::FirstInitialization, 0);
    const auto opening = day + 5 * 3600;
    const auto sample = [&](MonotonicMs ms) {
        clock(engine, config, ms, opening - 4 + static_cast<UtcSeconds>(ms / 1'000));
        recordSoil(engine, config, ms);
    };
    clock(engine, config, 0, opening - 4);
    sample(1'000);
    sample(2'000);
    sample(3'000);
    expect(Rejection::StaleTrigger, engine.acceptAutomatic(config));
    clock(engine, config, 4'000, opening);
    recordSoil(engine, config, 3'000);
    sample(5'000);
    sample(6'000);
    expect(Rejection::StaleTrigger, engine.acceptAutomatic(config));
    sample(7'000);
    config.autoIrrigationWindow.endMinute = 1379;
    engine.resetAutomatic();
    expect(Rejection::StaleTrigger, engine.acceptAutomatic(config));
    recordSoil(engine, config, 7'000);
    sample(8'000);
    sample(9'000);
    expect(Rejection::StaleTrigger, engine.acceptAutomatic(config));
    sample(10'000);
    expect(Rejection::None, engine.acceptAutomatic(config));
    TEST_ASSERT_TRUE(complete(engine, start(engine, config)).source == IrrigationSource::Smart);
}

void test_window_close_cancels_unstarted_auto_and_preserves_running_stop_deadline() {
    for (const auto phase : {Phase::Preparing, Phase::AwaitingStart, Phase::Running}) {
        auto config = configuration();
        config.autoIrrigationWindow = {true, 0, 1};
        config.zones[1].rule.mode = AutoMode::Smart;
        Irrigation engine(channels, Recovery::FirstInitialization, 0);
        clock(engine, config, 0, day + 55);
        for (MonotonicMs second = 1; second <= 3; ++second) {
            clock(engine, config, second * 1'000, day + 55 + static_cast<UtcSeconds>(second));
            recordSoil(engine, config, second * 1'000);
        }
        expect(Rejection::None, engine.acceptAutomatic(config));
        std::optional<StartPermit> permit;
        if (phase != Phase::Preparing) {
            save(engine);
            permit = engine.issueStart(config);
            TEST_ASSERT_TRUE(permit.has_value());
            if (phase == Phase::Running) {
                TEST_ASSERT_TRUE(engine.confirmStarted(permit->plan.executionId, 3'000));
            }
        }
        clock(engine, config, 3'010, day + 60);
        TEST_ASSERT_FALSE(engine.issueStart(config).has_value());
        if (phase == Phase::Running) {
            TEST_ASSERT_TRUE(engine.phase() == Phase::Running);
            clock(engine, config, permit->plannedStopMs, day + 60);
            TEST_ASSERT_TRUE(engine.pendingStop()->reason == StopReason::Planned);
            complete(engine, *permit);
        } else if (phase == Phase::AwaitingStart) {
            TEST_ASSERT_TRUE(engine.pendingStop()->reason == StopReason::Interrupted);
            TEST_ASSERT_TRUE(engine.confirmStopped({permit->plan.executionId, 3'010, StopEvidence::NeverStarted, {}}));
            save(engine);
        } else {
            TEST_ASSERT_TRUE(engine.phase() == Phase::Settling);
            save(engine);
        }
        expect(Rejection::StaleTrigger, engine.acceptAutomatic(config));
    }
}

void test_disabled_auto_window_still_enforces_midnight() {
    auto config = configuration();
    config.zones[1].rule.mode = AutoMode::Smart;
    Irrigation engine(channels, Recovery::FirstInitialization, 0);
    clock(engine, config, 0, day * 2 - 4);
    for (MonotonicMs second = 1; second <= 3; ++second) {
        clock(engine, config, second * 1'000, day * 2 - 4 + static_cast<UtcSeconds>(second));
        recordSoil(engine, config, second * 1'000);
    }
    expect(Rejection::CrossesMidnight, engine.acceptAutomatic(config));
}

void test_automatic_modes_share_manual_quota_and_respect_pause() {
    auto config = configuration();
    auto& rule = config.zones[1].rule;
    rule.dailyLimitMl = 75;
    rule.mode = AutoMode::Scheduled;
    Irrigation engine(channels, Recovery::FirstInitialization, 0);
    clock(engine, config, 0, day + 59);
    expect(Rejection::None, engine.accept(config, manual(1)));
    complete(engine, start(engine, config));
    clock(engine, config, 1'000, day + 60);
    expect(Rejection::None, engine.acceptAutomatic(config));
    complete(engine, start(engine, config));
    TEST_ASSERT_TRUE(engine.remaining(31, 75) == 15);
    rule.mode = AutoMode::Smart;
    engine.resetAutomatic();
    clock(engine, config, 2'000, day + 2);
    soil(engine, config, 7'000, 7'000);
    soil(engine, config, 8'000, 8'000);
    soil(engine, config, 9'000, 9'000);
    expect(Rejection::DailyLimit, engine.acceptAutomatic(config));
    expect(Rejection::StaleTrigger, engine.acceptAutomatic(config));
    engine.stopAll();
    save(engine);
    const IrrigationRequest smart{0, IrrigationSource::Smart, {31, 30}, Trigger{1, 0, 9'000}};
    expect(Rejection::AutoPaused, engine.accept(config, smart));
    expect(Rejection::None, engine.accept(config, manual(2, 31, 10)));
    TEST_ASSERT_TRUE(rule.mode == AutoMode::Smart);
}

void test_cooling_from_manual_and_scheduled_survives_mode_change_and_reboot() {
    for (const auto source : {IrrigationSource::Manual, IrrigationSource::Scheduled}) {
        auto config = configuration();
        auto& rule = config.zones[1].rule;
        rule.cooldownMs.reset();
        rule.mode = source == IrrigationSource::Scheduled ? AutoMode::Scheduled : AutoMode::Off;
        Irrigation engine(channels, Recovery::FirstInitialization, 0);
        clock(engine, config, 0, day + 59);
        if (source == IrrigationSource::Scheduled) {
            clock(engine, config, 1'000, day + 60);
            expect(Rejection::None, engine.acceptAutomatic(config));
        } else {
            expect(Rejection::None, engine.accept(config, manual(1)));
        }
        complete(engine, start(engine, config));
        auto durable = engine.state();
        rule.mode = AutoMode::Smart;
        rule.cooldownMs = 5'000;
        engine.resetAutomatic();
        clock(engine, config, 2'000, day + 2);
        soil(engine, config, 3'000, 3'000);
        soil(engine, config, 4'000, 4'000);
        soil(engine, config, 5'000, 5'000);
        expect(Rejection::StaleTrigger, engine.acceptAutomatic(config));
        soil(engine, config, 7'000, 5'000);
        soil(engine, config, 7'000, 7'000);
        soil(engine, config, 8'000, 8'000);
        expect(Rejection::StaleTrigger, engine.acceptAutomatic(config));
        engine.resetAutomatic();
        soil(engine, config, 9'000, 9'000);
        soil(engine, config, 10'000, 10'000);
        expect(Rejection::StaleTrigger, engine.acceptAutomatic(config));
        soil(engine, config, 11'000, 11'000);
        expect(Rejection::None, engine.acceptAutomatic(config));

        Irrigation rebooted(channels, Recovery::Loaded, 0, durable);
        clock(rebooted, config, 0, day);
        soil(rebooted, config, 1'000, 1'000);
        soil(rebooted, config, 2'000, 2'000);
        soil(rebooted, config, 3'000, 3'000);
        expect(Rejection::StaleTrigger, rebooted.acceptAutomatic(config));
        soil(rebooted, config, 5'001, 5'001);
        soil(rebooted, config, 6'001, 6'001);
        soil(rebooted, config, 7'001, 7'001);
        expect(Rejection::None, rebooted.acceptAutomatic(config));
        const auto record = complete(rebooted, start(rebooted, config));
        TEST_ASSERT_TRUE(record.source == IrrigationSource::Smart);
    }
}

void test_scheduled_conflicts_use_zone_id_and_expire_without_catch_up() {
    auto config = configuration();
    for (auto& zone : config.zones) {
        zone.rule.mode = AutoMode::Scheduled;
    }
    Irrigation engine(channels, Recovery::FirstInitialization, 0);
    clock(engine, config, 0, day + 59);
    clock(engine, config, 1'000, day + 60);
    expect(Rejection::None, engine.acceptAutomatic(config));
    TEST_ASSERT_EQUAL_UINT32(31, engine.activePlan()->request.dose.zoneId);
    expect(Rejection::Busy, engine.acceptAutomatic(config));
    complete(engine, start(engine, config));
    clock(engine, config, 1'031, day + 60);
    expect(Rejection::None, engine.acceptAutomatic(config));
    TEST_ASSERT_EQUAL_UINT32(32, engine.activePlan()->request.dose.zoneId);
    save(engine);
    clock(engine, config, 61'000, day + 123);
    TEST_ASSERT_FALSE(engine.issueStart(config).has_value());
    save(engine);
    expect(Rejection::StaleTrigger, engine.acceptAutomatic(config));
    clock(engine, config, 117'000, day + 179);
    clock(engine, config, 118'000, day + 180);
    expect(Rejection::None, engine.acceptAutomatic(config));
}

void test_schedule_first_sync_jump_rewind_and_restart_do_not_repeat_fixed_minute() {
    auto config = configuration();
    config.zones[1].rule.mode = AutoMode::Scheduled;
    Irrigation engine(channels, Recovery::FirstInitialization, 0);
    clock(engine, config, 0, day + 60);
    expect(Rejection::StaleTrigger, engine.acceptAutomatic(config));
    clock(engine, config, 1'000, day + 119);
    expect(Rejection::StaleTrigger, engine.acceptAutomatic(config));
    clock(engine, config, 2'000, day + 120);
    expect(Rejection::None, engine.acceptAutomatic(config));
    complete(engine, start(engine, config));
    const auto baseline = engine.state();
    config.zones[1].rule.schedule[1].targetVolumeMl = 25;
    clock(engine, config, 3'000, day + 120);
    expect(Rejection::StaleTrigger, engine.acceptAutomatic(config));
    clock(engine, config, 4'000, day + 119);
    clock(engine, config, 5'000, day + 120);
    expect(Rejection::StaleTrigger, engine.acceptAutomatic(config));
    clock(engine, config, 6'000, day + 181);
    expect(Rejection::StaleTrigger, engine.acceptAutomatic(config));

    Irrigation rebooted(channels, Recovery::Loaded, 0, baseline);
    config.zones[1].rule.schedule.erase(config.zones[1].rule.schedule.begin() + 1);
    config.zones[1].rule.schedule.push_back({2, 20});
    clock(rebooted, config, 0, day + 119);
    clock(rebooted, config, 1'000, day + 120);
    expect(Rejection::StaleTrigger, rebooted.acceptAutomatic(config));
    config.zones[1].rule.mode = AutoMode::Off;
    const IrrigationRequest request{0, IrrigationSource::Scheduled, {31, 20}, Trigger{1, 2, 0}};
    expect(Rejection::ModeMismatch, rebooted.accept(config, request));
}

void test_date_rollover_waits_for_stable_time_and_reliable_settlement() {
    const auto config = configuration();
    Irrigation engine(channels, Recovery::FirstInitialization, 0);
    clock(engine, config, 0, day * 2 - 1);
    expect(Rejection::CrossesMidnight, engine.accept(config, manual(1)));
    clock(engine, config, 1'000, day * 2 - 2);
    expect(Rejection::None, engine.accept(config, manual(1)));
    const auto permit = start(engine, config);
    clock(engine, config, 1'029, day * 3);
    TEST_ASSERT_FALSE(engine.pendingStop().has_value());
    clock(engine, config, 1'030, day * 3);
    TEST_ASSERT_TRUE(engine.pendingStop()->reason == StopReason::Planned);
    TEST_ASSERT_TRUE(engine.confirmStopped({permit.plan.executionId, 1'030, StopEvidence::Measured, 30}));
    const auto settling = engine.beginSave();
    clock(engine, config, 61'029, day * 3 + 60);
    TEST_ASSERT_EQUAL_INT64(1, engine.state().date->day);
    TEST_ASSERT_TRUE(engine.finishSave(settling->id, true));
    clock(engine, config, 61'030, day * 3 + 60);
    TEST_ASSERT_EQUAL_INT64(3, engine.state().date->day);
    TEST_ASSERT_TRUE(engine.state().usage.empty());
    TEST_ASSERT_TRUE(engine.remaining(31, 200) == 200);
    clock(engine, config, 61'031, day * 2);
    expect(Rejection::InvalidTime, engine.accept(config, manual(2)));
    TEST_ASSERT_EQUAL_INT64(3, engine.state().date->day);
}

void test_timezone_change_cannot_reset_daily_quota_or_repeat_schedule() {
    auto config = configuration();
    config.zones[1].rule.mode = AutoMode::Scheduled;
    Irrigation engine(channels, Recovery::FirstInitialization, 0);
    clock(engine, config, 0, day + 59);
    clock(engine, config, 1'000, day + 60);
    expect(Rejection::None, engine.acceptAutomatic(config));
    complete(engine, start(engine, config));
    config.utcOffsetMinutes = 60;
    clock(engine, config, 2'000, 2 * day - 3'000);
    TEST_ASSERT_TRUE(engine.state().date->schedulesSuspended);
    TEST_ASSERT_EQUAL_UINT32(30, engine.state().usage[0].usedMl);
    expect(Rejection::InvalidTime, engine.accept(config, manual(2)));
    save(engine);
    clock(engine, config, 62'000, 2 * day - 2'940);
    TEST_ASSERT_EQUAL_INT64(1, engine.state().date->day);
    config.utcOffsetMinutes = 0;
    clock(engine, config, 63'000, day + 59);
    clock(engine, config, 64'000, day + 60);
    expect(Rejection::StaleTrigger, engine.acceptAutomatic(config));
    TEST_ASSERT_TRUE(engine.remaining(31, 200) == 170);
    config.plants[0].zoneId = 33;
    config.zones[1].id = 33;
    config.groups.clear();
    expect(Rejection::BindingChanged, engine.accept(config, manual(2, 33)));
}

void test_local_date_handles_negative_offsets_and_both_utc_limits() {
    TEST_ASSERT_TRUE(localTime(-1, 0)->day == -1);
    TEST_ASSERT_TRUE(localTime(-1, 0)->minute == 1'439);
    TEST_ASSERT_TRUE(localTime(-1, 0)->secondsRemaining == 1);
    TEST_ASSERT_TRUE(localTime(0, -1)->minute == 1'439);
    TEST_ASSERT_TRUE(localTime(0, 480)->minute == 480);
    TEST_ASSERT_FALSE(localTime(0, 1'440).has_value());
    for (const auto utc : {std::numeric_limits<UtcSeconds>::min(), std::numeric_limits<UtcSeconds>::max()}) {
        TEST_ASSERT_TRUE(localTime(utc, -1'439)->minute < 1'440);
        TEST_ASSERT_TRUE(localTime(utc, 1'439)->secondsRemaining <= day);
    }
}

void test_group_prechecks_all_and_reserves_members_in_explicit_order() {
    auto config = configuration();
    Irrigation engine(channels, Recovery::FirstInitialization, 0);
    clock(engine, config, 0, day);
    expect(Rejection::SingleLimit, engine.acceptGroup(config, 1, 42, {10, 101}));
    TEST_ASSERT_TRUE(engine.state().usage.empty());
    expect(Rejection::None, engine.acceptGroup(config, 1, 42, {10, 30}));
    expect(Rejection::Duplicate, engine.acceptGroup(config, 1, 42, {10, 30}));
    expect(Rejection::RequestConflict, engine.acceptGroup(config, 1, 42, {30, 10}));
    TEST_ASSERT_EQUAL_UINT32(32, engine.activePlan()->request.dose.zoneId);
    TEST_ASSERT_TRUE(engine.remaining(31, 200) == 200);
    expect(Rejection::Busy, engine.advanceGroup(config));
    expect(Rejection::Busy, engine.accept(config, manual(2)));
    complete(engine, start(engine, config));
    clock(engine, config, 10, day);
    expect(Rejection::None, engine.advanceGroup(config));
    TEST_ASSERT_EQUAL_UINT32(31, engine.activePlan()->request.dose.zoneId);
    TEST_ASSERT_TRUE(engine.remaining(32, 200) == 190);
    complete(engine, start(engine, config));
    TEST_ASSERT_FALSE(engine.groupActive());
    TEST_ASSERT_TRUE(engine.acceptGroup(config, 1, 42, {10, 30}).previousResult == IrrigationResult::Completed);
}

void test_group_rejects_total_at_midnight_without_creating_a_reservation() {
    auto config = configuration();
    for (auto& zone : config.zones) {
        zone.rule.singleLimitMl = 1'000;
        zone.rule.dailyLimitMl = 2'000;
    }
    Irrigation engine(channels, Recovery::FirstInitialization, 0);
    clock(engine, config, 0, day * 2 - 2);
    expect(Rejection::CrossesMidnight, engine.acceptGroup(config, 1, 41, {600, 400}));
    TEST_ASSERT_TRUE(engine.state().usage.empty());
    TEST_ASSERT_TRUE(engine.state().manualReceipts.empty());
    TEST_ASSERT_FALSE(engine.activePlan().has_value());
    expect(Rejection::None, engine.acceptGroup(config, 1, 41, {600, 399}));
}

void test_new_quota_day_cancels_group_after_the_completed_member_is_settled() {
    const auto config = configuration();
    Irrigation engine(channels, Recovery::FirstInitialization, 0);
    clock(engine, config, 0, day);
    expect(Rejection::None, engine.acceptGroup(config, 1, 41, {10, 30}));
    const auto first = complete(engine, start(engine, config));
    TEST_ASSERT_TRUE(first.result == IrrigationResult::Completed);
    TEST_ASSERT_TRUE(engine.remaining(31, 200) == 190);
    clock(engine, config, 1'000, day * 2);
    TEST_ASSERT_TRUE(engine.groupActive());
    clock(engine, config, 61'000, day * 2 + 60);
    TEST_ASSERT_EQUAL_INT64(2, engine.state().date->day);
    TEST_ASSERT_FALSE(engine.groupActive());
    TEST_ASSERT_FALSE(engine.activePlan().has_value());
    TEST_ASSERT_TRUE(engine.acceptGroup(config, 1, 41, {10, 30}).previousResult == IrrigationResult::Failed);
    TEST_ASSERT_TRUE(engine.remaining(31, 200) == 200);
    TEST_ASSERT_TRUE(engine.remaining(32, 200) == 200);
    save(engine);
}

void test_group_recheck_stop_and_reboot_cancel_remaining_members() {
    auto config = configuration();
    Irrigation engine(channels, Recovery::FirstInitialization, 0);
    clock(engine, config, 0, day);
    expect(Rejection::None, engine.acceptGroup(config, 1, 41, {10, 30}));
    complete(engine, start(engine, config));
    const auto durable = engine.state();
    config.zones[0].rule.singleLimitMl = 10;
    expect(Rejection::SingleLimit, engine.advanceGroup(config));
    TEST_ASSERT_FALSE(engine.groupActive());
    TEST_ASSERT_TRUE(engine.remaining(32, 200) == 200);
    save(engine);

    config.zones[0].rule.singleLimitMl = 100;
    Irrigation rebooted(channels, Recovery::Loaded, 0, durable);
    clock(rebooted, config, 0, day);
    TEST_ASSERT_FALSE(rebooted.groupActive());
    TEST_ASSERT_TRUE(rebooted.acceptGroup(config, 1, 41, {10, 30}).previousResult == IrrigationResult::Failed);
    expect(Rejection::None, rebooted.acceptGroup(config, 2, 41, {10, 30}));
    const auto permit = start(rebooted, config);
    rebooted.stopAll();
    expect(Rejection::Busy, rebooted.advanceGroup(config));
    TEST_ASSERT_TRUE(rebooted.confirmStopped({permit.plan.executionId, 2, StopEvidence::Measured, 2}));
    save(rebooted);
    TEST_ASSERT_FALSE(rebooted.groupActive());
    expect(Rejection::InvalidRequest, rebooted.advanceGroup(config));
    TEST_ASSERT_TRUE(rebooted.remaining(32, 200) == 200);
}

void test_recovery_charges_unknown_once_preserves_pause_and_rejects_corruption() {
    const auto config = configuration();
    Irrigation engine(channels, Recovery::FirstInitialization, 0);
    clock(engine, config, 1, day);
    engine.stopAll();
    expect(Rejection::None, engine.accept(config, manual(1)));
    const auto unresolved = save(engine);
    Irrigation rebooted(channels, Recovery::Loaded, 100, unresolved);
    clock(rebooted, config, 100, day);
    TEST_ASSERT_TRUE(rebooted.state().autoPaused);
    TEST_ASSERT_FALSE(rebooted.unlockFault(true));
    const auto settled = save(rebooted);
    TEST_ASSERT_EQUAL_UINT32(30, settled.usage[0].usedMl);
    const auto record = rebooted.takeRecord();
    TEST_ASSERT_TRUE(record->result == IrrigationResult::Unknown);
    TEST_ASSERT_FALSE(record->actualDurationMs.has_value());
    TEST_ASSERT_FALSE(record->estimatedVolumeMl.has_value());
    TEST_ASSERT_TRUE(rebooted.accept(config, manual(1)).previousResult == IrrigationResult::Unknown);
    expect(Rejection::FaultLocked, rebooted.accept(config, manual(2)));
    TEST_ASSERT_FALSE(rebooted.resumeAutomatic());
    TEST_ASSERT_FALSE(rebooted.unlockFault(false));
    TEST_ASSERT_TRUE(rebooted.unlockFault(true));
    const auto unlocked = save(rebooted);
    TEST_ASSERT_TRUE(unlocked.autoPaused);
    Irrigation secondReboot(channels, Recovery::Loaded, 0, unlocked);
    clock(secondReboot, config, 0, day);
    TEST_ASSERT_FALSE(secondReboot.beginSave().has_value());
    TEST_ASSERT_TRUE(secondReboot.remaining(31, 200) == 170);
    TEST_ASSERT_TRUE(secondReboot.resumeAutomatic());

    auto inconsistent = unresolved;
    inconsistent.usage.clear();
    Irrigation damaged(channels, Recovery::Loaded, 0, inconsistent);
    TEST_ASSERT_TRUE(damaged.recoveryStatus() == Recovery::Corrupt);
    expect(Rejection::CriticalState, damaged.accept(config, manual(5)));
    for (const auto recovery : {Recovery::Missing, Recovery::Incompatible, Recovery::Corrupt}) {
        Irrigation unavailable(channels, recovery, 0);
        clock(unavailable, config, 0, day);
        unavailable.stopAll();
        expect(Rejection::CriticalState, unavailable.accept(config, manual(1)));
        TEST_ASSERT_FALSE(unavailable.beginSave().has_value());
    }
}

void test_cooling_capacity_reclaims_deleted_zones_but_preserves_channels() {
    auto config = configuration();
    auto extendedChannels = channels;
    CriticalState state;
    state.date = QuotaDate{1, 0, {}, false};
    state.lastExecutionId = CriticalState::maxTrackedZones;
    state.recentlyUsedChannels = {1};
    for (std::size_t index = 0; index < CriticalState::maxTrackedZones; ++index) {
        const auto id = static_cast<EntityId>(100 + index);
        const auto channel = static_cast<LogicalChannel>(2 + index);
        extendedChannels.pumpChannels.push_back(channel);
        config.pumps.push_back({id, "Spare pump", channel, {}, {}, {}});
        config.zones.push_back({id, "Spare zone", id, {}, {}});
        state.recentlyWateredZones.push_back(id);
    }
    Irrigation engine(extendedChannels, Recovery::Loaded, 0, state);
    clock(engine, config, 1, day);
    expect(Rejection::Capacity, engine.accept(config, manual(1)));

    const auto deleted = config.zones.back().id;
    config.zones.pop_back();
    expect(Rejection::Capacity, engine.acceptGroup(config, 1, 41, {10, 10}));
    TEST_ASSERT_FALSE(engine.activePlan().has_value());
    expect(Rejection::None, engine.accept(config, manual(1)));
    complete(engine, start(engine, config));
    const auto& recentZones = engine.state().recentlyWateredZones;
    TEST_ASSERT_EQUAL_UINT32(CriticalState::maxTrackedZones, recentZones.size());
    TEST_ASSERT_TRUE(std::find(recentZones.begin(), recentZones.end(), deleted) == recentZones.end());
    TEST_ASSERT_TRUE(std::find(recentZones.begin(), recentZones.end(), 31) != recentZones.end());
    TEST_ASSERT_EQUAL_UINT32(2, engine.state().recentlyUsedChannels.size());
    TEST_ASSERT_EQUAL_UINT16(1, engine.state().recentlyUsedChannels.front());

    Irrigation rebooted(extendedChannels, Recovery::Loaded, 10, engine.state());
    TEST_ASSERT_TRUE(rebooted.recoveryStatus() == Recovery::Loaded);
}

void test_manual_retention_requires_explicit_expiration() {
    const auto config = configuration();
    Irrigation engine(channels, Recovery::FirstInitialization, 0);
    clock(engine, config, 0, day);
    for (RequestId id = 1; id <= CriticalState::maxManualReceipts; ++id) {
        expect(Rejection::None, engine.accept(config, manual(id)));
        TEST_ASSERT_FALSE(engine.forgetManualRequest(id));
        engine.stopAll();
        save(engine);
    }
    expect(Rejection::Capacity, engine.accept(config, manual(9)));
    expect(Rejection::Duplicate, engine.accept(config, manual(1)));
    TEST_ASSERT_TRUE(engine.forgetManualRequest(1));
    expect(Rejection::None, engine.accept(config, manual(9)));
}

}  // namespace

void setUp() {}
void tearDown() {}

int main() {
    UNITY_BEGIN();
    RUN_TEST(test_full_config_readiness_and_manual_with_no_soil);
    RUN_TEST(test_reservation_and_settlement_are_separate_reliable_saves);
    RUN_TEST(test_stop_during_reservation_save_revokes_start_and_requires_new_save);
    RUN_TEST(test_late_start_after_stop_keeps_stopping_and_accounts_for_real_delivery);
    RUN_TEST(test_confirmed_never_started_releases_dispatched_reservation);
    RUN_TEST(test_clock_deadlines_and_snapshot_survive_calibration_and_wall_changes);
    RUN_TEST(test_monotonic_regression_protects_pending_start_and_overflow_is_rejected);
    RUN_TEST(test_settlement_saturates_conversion_and_addition);
    RUN_TEST(test_contradictory_stop_evidence_cannot_release_reservation_or_hide_excess);
    RUN_TEST(test_smart_uses_latest_valid_contiguous_sample_only_once);
    RUN_TEST(test_bad_sample_values_and_gaps_reset_smart_streak);
    RUN_TEST(test_smart_continuity_tolerates_bounded_jitter_and_detects_a_missed_cycle);
    RUN_TEST(test_schedules_open_at_start_and_consume_outside_slots_without_backfill);
    RUN_TEST(test_automatic_duration_must_fit_and_is_rechecked_after_saving);
    RUN_TEST(test_smart_window_restarts_sampling_and_config_reset_drops_old_candidate);
    RUN_TEST(test_window_close_cancels_unstarted_auto_and_preserves_running_stop_deadline);
    RUN_TEST(test_disabled_auto_window_still_enforces_midnight);
    RUN_TEST(test_automatic_modes_share_manual_quota_and_respect_pause);
    RUN_TEST(test_cooling_from_manual_and_scheduled_survives_mode_change_and_reboot);
    RUN_TEST(test_scheduled_conflicts_use_zone_id_and_expire_without_catch_up);
    RUN_TEST(test_schedule_first_sync_jump_rewind_and_restart_do_not_repeat_fixed_minute);
    RUN_TEST(test_date_rollover_waits_for_stable_time_and_reliable_settlement);
    RUN_TEST(test_timezone_change_cannot_reset_daily_quota_or_repeat_schedule);
    RUN_TEST(test_local_date_handles_negative_offsets_and_both_utc_limits);
    RUN_TEST(test_group_prechecks_all_and_reserves_members_in_explicit_order);
    RUN_TEST(test_group_rejects_total_at_midnight_without_creating_a_reservation);
    RUN_TEST(test_new_quota_day_cancels_group_after_the_completed_member_is_settled);
    RUN_TEST(test_group_recheck_stop_and_reboot_cancel_remaining_members);
    RUN_TEST(test_recovery_charges_unknown_once_preserves_pause_and_rejects_corruption);
    RUN_TEST(test_cooling_capacity_reclaims_deleted_zones_but_preserves_channels);
    RUN_TEST(test_manual_retention_requires_explicit_expiration);
    return UNITY_END();
}
