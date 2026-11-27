#pragma once

#include <deque>
#include <utility>

#include <unity.h>

#include "application/services/IrrigationScheduler.h"

namespace simulation {
using namespace planthub::core;
using namespace planthub::application;

inline constexpr UtcSeconds referenceDay = 20'000 * UtcSeconds{86'400};

struct TestClock : Clock {
    MonotonicMs ms = 10;
    UtcSeconds epoch = referenceDay - 3 * 3'600;
    bool trusted = true;

    ClockSample now() const override {
        return {ms, trusted ? std::optional{epoch + static_cast<UtcSeconds>(ms / 1'000)} : std::nullopt};
    }
};

struct TestSensors : SensorSource {
    explicit TestSensors(const TestClock& source) : clock(source) {}

    std::optional<SoilSample> readSoil(LogicalChannel channel) override {
        const auto index = channel == 1 ? 0 : 1;
        ++soilReads;
        if (!freeze[index]) {
            cached[index] = SoilSample{clock.now(), soilStatus[index],
                soilStatus[index] == Acquisition::Success ? std::optional{raw[index]} : std::nullopt};
        }
        return cached[index];
    }

    std::optional<AirSample> readAir() override {
        ++airReads;
        return AirSample{clock.now(), Acquisition::Success, 25.0f, 50.0f};
    }

    const TestClock& clock;
    AdcCount raw[2] = {200, 800};
    Acquisition soilStatus[2] = {Acquisition::Success, Acquisition::Success};
    bool freeze[2] = {false, false};
    std::optional<SoilSample> cached[2];
    int soilReads = 0;
    int airReads = 0;
};

struct TestConfigStore : ConfigStore {
    LoadResult<ConfigSnapshot> load() override {
        ++reads;
        return stored;
    }
    bool save(const ConfigSnapshot& snapshot) override {
        ++writes;
        if (fail) {
            return false;
        }
        stored = {LoadStatus::Loaded, snapshot};
        return true;
    }

    LoadResult<ConfigSnapshot> stored{LoadStatus::FirstUse, {}};
    bool fail = false;
    int reads = 0;
    int writes = 0;
};

struct TestRuntimeStore : RuntimeStateStore {
    LoadResult<RuntimeSnapshot> load() override {
        ++reads;
        return stored;
    }
    void startSave(SaveId id, const RuntimeSnapshot& snapshot) override {
        TEST_ASSERT_FALSE(inProgress.has_value());
        inProgress = {id, snapshot};
        ++writes;
    }
    std::optional<SaveCompletion> pollSave() override {
        if (!results.empty()) {
            const auto result = results.front();
            results.pop_front();
            return result;
        }
        if (automatic && inProgress) {
            complete(true);
            return pollSave();
        }
        return {};
    }
    SaveId complete(bool reliable) {
        TEST_ASSERT_TRUE(inProgress.has_value());
        const auto id = inProgress->first;
        if (reliable) {
            stored = {LoadStatus::Loaded, std::move(inProgress->second)};
        }
        inProgress.reset();
        results.push_back({id, reliable});
        return id;
    }

    LoadResult<RuntimeSnapshot> stored{LoadStatus::FirstUse, {}};
    std::optional<std::pair<SaveId, RuntimeSnapshot>> inProgress;
    std::deque<SaveCompletion> results;
    bool automatic = true;
    int reads = 0;
    int writes = 0;
};

struct TestPump : PumpController {
    explicit TestPump(const TestRuntimeStore& store) : runtime(store) {}

    void start(const StartPermit& permit) override {
        TEST_ASSERT_TRUE(allPumpsVerifiedOff());
        TEST_ASSERT_TRUE(runtime.stored.snapshot->critical.pending.has_value());
        TEST_ASSERT_EQUAL_UINT64(permit.plan.executionId, runtime.stored.snapshot->critical.pending->executionId);
        TEST_ASSERT_EQUAL_UINT64(permit.issuedAtMs + permit.plan.plannedMs, permit.plannedStopMs);
        TEST_ASSERT_EQUAL_UINT64(permit.issuedAtMs + permit.plan.maximumMs, permit.hardStopMs);
        TEST_ASSERT_TRUE(permits.empty() || permit.plan.executionId > permits.back().plan.executionId);
        permits.push_back(permit);
        queued = permit;
    }
    void priorityStop(StopRequest request) override {
        stops.push_back(request);
        if (queued && (!request.executionId || queued->plan.executionId == request.executionId)) {
            revoked = std::exchange(queued, std::nullopt);
        }
    }
    bool allPumpsVerifiedOff() const override {
        return verified && !queued && !running && !revoked;
    }
    std::optional<PumpEvent> pollEvent() override {
        if (events.empty()) {
            return {};
        }
        const auto event = events.front();
        events.pop_front();
        return event;
    }
    bool activate(ClockSample when, bool report = true) {
        if (!queued) {
            return false;
        }
        TEST_ASSERT_FALSE(running.has_value());
        running = PumpStarted{queued->plan.executionId, when};
        queued.reset();
        if (report) {
            events.push_back(*running);
        }
        return true;
    }
    PumpStopped measured(MonotonicMs actualOff, bool duplicate = false) {
        TEST_ASSERT_TRUE(running.has_value());
        const PumpStopped event{running->executionId, actualOff, StopEvidence::Measured,
                                static_cast<DurationMs>(actualOff - running->started.monotonicMs)};
        running.reset();
        events.push_back(event);
        if (duplicate) {
            events.push_back(event);
        }
        return event;
    }
    void confirmRevoked(MonotonicMs at) {
        TEST_ASSERT_TRUE(revoked.has_value());
        events.push_back(PumpStopped{revoked->plan.executionId, at, StopEvidence::NeverStarted, {}});
        revoked.reset();
    }

    const TestRuntimeStore& runtime;
    bool verified = true;
    std::vector<StartPermit> permits;
    std::vector<StopRequest> stops;
    std::deque<PumpEvent> events;
    std::optional<StartPermit> queued;
    std::optional<StartPermit> revoked;
    std::optional<PumpStarted> running;
};

struct TestHistory : HistorySink {
    bool writeMonitoring(const MonitoringSnapshot& snapshot) override {
        monitoring.push_back(snapshot);
        return !failMonitoring;
    }
    bool writeIrrigation(const IrrigationRecord& record) override {
        TEST_ASSERT_TRUE(irrigation.empty() || record.executionId > irrigation.back().executionId);
        irrigation.push_back(record);
        return !failIrrigation;
    }

    bool failMonitoring = false;
    bool failIrrigation = false;
    std::vector<MonitoringSnapshot> monitoring;
    std::vector<IrrigationRecord> irrigation;
};

inline SystemConfig twoPlants() {
    SystemConfig config;
    config.plants = {{1, "Mint", true, 1, 1}, {2, "Basil", true, 2, 2}};
    config.soilSensors = {{1, "Mint", 1, SoilCalibration{0, 1'000}},
                          {2, "Basil", 2, SoilCalibration{1'000, 0}}};
    config.pumps = {{1, "Mint", 1, 600.0f, 100, 10'000},
                    {2, "Basil", 2, 600.0f, 100, 10'000}};
    config.zones = {{1, "Mint", 1, 1, {}}, {2, "Basil", 2, 2, {}}};
    for (auto& zone : config.zones) {
        zone.rule.singleLimitMl = 50;
        zone.rule.dailyLimitMl = 100;
        zone.rule.lowMoisturePercent = 30.0f;
        zone.rule.smartDoseMl = 10;
        zone.rule.cooldownMs = 900'000;
    }
    config.groups = {{1, "Balcony", {2, 1}}};
    return config;
}

inline ChannelCapabilities channels() {
    return {{1, 2}, {1, 2}};
}

struct Rig {
    TestClock clock;
    TestSensors sensors{clock};
    TestConfigStore configs;
    TestRuntimeStore runtime;
    TestPump pump{runtime};
    TestHistory history;
    IrrigationScheduler app{channels(), clock, sensors, pump, configs, runtime, history};

    IrrigationService& irrigation() { return app.irrigation(); }
    const Irrigation& engine() { return *irrigation().coreStatus(); }
    void tick(int count = 1) {
        for (int i = 0; i < count; ++i) {
            app.tick();
        }
    }
    void advance(MonotonicMs delta, int count = 1) {
        clock.ms += delta;
        tick(count);
    }
    void configure(const SystemConfig& config = twoPlants()) {
        tick(6);
        TEST_ASSERT_EQUAL_INT(static_cast<int>(BootStatus::Ready), static_cast<int>(irrigation().bootStatus()));
        TEST_ASSERT_EQUAL_INT(static_cast<int>(ConfigChange::Applied),
                              static_cast<int>(app.replaceConfiguration(config).status));
        advance(1, 4);
    }
    void finishPlanned() {
        TEST_ASSERT_TRUE(pump.activate(clock.now()));
        tick(2);
        const auto deadline = pump.permits.back().plannedStopMs;
        clock.ms = deadline;
        tick();
        TEST_ASSERT_EQUAL_UINT64(pump.permits.back().plan.executionId, pump.stops.back().executionId);
        pump.measured(deadline, true);
        tick(4);
    }
};

}  // namespace simulation
