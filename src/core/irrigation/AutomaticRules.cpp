
#include "core/irrigation/Irrigation.h"

#include <cmath>
#include <iterator>

namespace planthub::core {
namespace {

constexpr int minutesPerDay = 1'440;
constexpr int secondsPerDay = 86'400;

}  // namespace

bool consistentClockSamples(const ClockSample& before, const ClockSample& after) {
    if (!before.utc || !after.utc || *after.utc < *before.utc || after.monotonicMs < before.monotonicMs) {
        return false;
    }
    const auto wall = static_cast<std::uint64_t>(*after.utc) - static_cast<std::uint64_t>(*before.utc);
    const auto elapsed = (after.monotonicMs - before.monotonicMs) / 1'000;
    return wall >= elapsed ? wall - elapsed <= 1 : elapsed - wall <= 1;
}

std::optional<LocalTime> localTime(UtcSeconds utc, std::int16_t offsetMinutes) {
    if (offsetMinutes <= -minutesPerDay || offsetMinutes >= minutesPerDay) {
        return std::nullopt;
    }
    auto day = utc / secondsPerDay;
    auto seconds = static_cast<int>(utc % secondsPerDay) + offsetMinutes * 60;
    while (seconds < 0) {
        --day;
        seconds += secondsPerDay;
    }
    while (seconds >= secondsPerDay) {
        ++day;
        seconds -= secondsPerDay;
    }
    return LocalTime{day, static_cast<std::uint16_t>(seconds / 60), static_cast<std::uint32_t>(secondsPerDay - seconds)};
}

bool Irrigation::dateReady() const {
    return timeAccepted_ && monotonicValid_ && local_ && state_.date && local_->day == state_.date->day;
}

void Irrigation::observeTime(const SystemConfig& config, ClockSample sample) {
    if (sample.monotonicMs < now_) {
        monotonicValid_ = false;
        timeAccepted_ = false;
        previousClock_.reset();
        continuityAnchor_.reset();
        clearAutomatic();
        requestStop(StopReason::ClockFault);
        return;
    }
    now_ = sample.monotonicMs;
    monotonicValid_ = true;
    enforceDeadline(now_);

    const auto previousLocal = local_;
    const bool offsetChanged = offsetMinutes_ != config.utcOffsetMinutes;
    offsetMinutes_ = config.utcOffsetMinutes;
    local_ = sample.utc ? localTime(*sample.utc, offsetMinutes_) : std::nullopt;
    timeAccepted_ = false;
    if (!available() || !local_) {
        previousClock_.reset();
        continuityAnchor_.reset();
        clearAutomatic();
        return;
    }
    const auto& window = config.autoIrrigationWindow;
    const bool autoAllowed = window.allows(local_->minute);
    if (previousLocal && (autoAllowed != window.allows(previousLocal->minute) ||
                          (window.enabled && previousLocal->day != local_->day))) {
        clearAutomatic();
    }
    if (!autoAllowed) {
        cancelUnstartedAutomatic();
    }
    const bool followsPrevious = !offsetChanged && previousClock_ && consistentClockSamples(*previousClock_, sample);
    const bool continuous = followsPrevious && continuityAnchor_ && consistentClockSamples(*continuityAnchor_, sample);
    if (!continuous) {
        continuityAnchor_ = sample;
        clearAutomatic();
    }

    const bool enteredNextMinute = continuous && now_ > previousClock_->monotonicMs &&
        now_ - previousClock_->monotonicMs < clockConfirmationMs && previousLocal &&
        ((previousLocal->day == local_->day && local_->minute == previousLocal->minute + 1) ||
         (local_->day == previousLocal->day + 1 && previousLocal->minute == minutesPerDay - 1 && local_->minute == 0));
    previousClock_ = sample;

    if (!state_.date) {
        state_.date = QuotaDate{local_->day, offsetMinutes_, {}, false};
        dirty_ = true;
    } else {
        auto& date = *state_.date;
        if (date.offsetMinutes != offsetMinutes_ && !date.schedulesSuspended) {
            date.schedulesSuspended = true;
            dirty_ = true;
        }
        const auto pinned = *localTime(*sample.utc, date.offsetMinutes);
        if (local_->day < date.day || pinned.day < date.day) {
            clearAutomatic();
            // 可信时间须重新建立，回拨期间的观察不能用于批准日期推进。
            continuityAnchor_ = sample;
            return;
        }
        if (local_->day > date.day) {
            if (pinned.day <= date.day || phase_ != Phase::Idle || dirty_ || inFlight_ ||
                now_ - continuityAnchor_->monotonicMs < clockConfirmationMs) {
                clearAutomatic();
                return;
            }
            // 旧账已经可靠结算，原时区和现时区均进入后续自然日。
            const bool skipMinute = date.schedulesSuspended || !enteredNextMinute;
            finishGroup(IrrigationResult::Failed);
            date = QuotaDate{local_->day, offsetMinutes_, {}, false};
            state_.usage.clear();
            dirty_ = true;
            timeAccepted_ = true;
            observeSchedule(config, !skipMinute, enteredNextMinute && !skipMinute);
            return;
        }
    }
    timeAccepted_ = true;
    if (!continuous && previousLocal && previousLocal->day == local_->day && previousLocal->minute > local_->minute) {
        auto& consumed = state_.date->consumedThroughMinute;
        if (!consumed || previousLocal->minute > *consumed) {
            consumed = previousLocal->minute;
            dirty_ = true;
        }
    }
    observeSchedule(config, continuous, enteredNextMinute);
}

void Irrigation::observeSchedule(const SystemConfig& config, bool continuous, bool enteredNextMinute) {
    const auto minute = local_->minute;
    if (scheduleWindow_ && (scheduleWindow_->day != local_->day || scheduleWindow_->minute != minute)) {
        scheduleWindow_.reset();
        candidates_.erase(std::remove_if(candidates_.begin(), candidates_.end(), [](const auto& request) {
            return request.source == IrrigationSource::Scheduled;
        }), candidates_.end());
    }
    auto& date = *state_.date;
    if (date.schedulesSuspended || (date.consumedThroughMinute && minute <= *date.consumedThroughMinute)) {
        return;
    }
    const bool hasSchedule = std::any_of(config.zones.begin(), config.zones.end(),
                                         [](const auto& zone) { return zone.rule.mode == AutoMode::Scheduled; });
    if (!hasSchedule) {
        return;
    }
    if (!continuous) {
        date.consumedThroughMinute = minute;
        dirty_ = true;
        return;
    }
    if (!enteredNextMinute) {
        return;
    }
    const bool canQueue = !state_.autoPaused && state_.fault == Fault::None && !saveFailed_ &&
                          config.autoIrrigationWindow.allows(minute);
    bool consumed = false;
    for (const auto& zone : config.zones) {
        if (zone.rule.mode != AutoMode::Scheduled) {
            continue;
        }
        const auto entry = std::find_if(zone.rule.schedule.begin(), zone.rule.schedule.end(),
                                        [minute](const auto& item) { return item.minuteOfDay == minute; });
        if (entry != zone.rule.schedule.end()) {
            consumed = true;
            if (canQueue && entry->targetVolumeMl &&
                checkZoneReadiness(config, channels_, zone.id, IrrigationSource::Scheduled) == ZoneReadiness::Ready) {
                addCandidate({0, IrrigationSource::Scheduled, {zone.id, *entry->targetVolumeMl},
                              Trigger{local_->day, minute, 0}});
            }
        }
    }
    if (consumed) {
        date.consumedThroughMinute = minute;
        if (canQueue) {
            scheduleWindow_ = Trigger{local_->day, minute, 0};
        }
        dirty_ = true;
    }
}

bool Irrigation::cooling(const IrrigationZone& zone, LogicalChannel channel, MonotonicMs atMs) const {
    for (std::size_t index = 0; index < state_.recentWatering.size(); ++index) {
        const auto& recent = state_.recentWatering[index];
        if ((recent.zoneId == zone.id || recent.channel == channel) &&
            (atMs <= recentEnds_[index] || atMs - recentEnds_[index] < zone.rule.cooldownMs.value_or(0))) {
            return true;
        }
    }
    return false;
}

void Irrigation::observeSoil(const SystemConfig& config, EntityId zoneId, const SensorReading* reading) {
    const auto* zone = findById(config.zones, zoneId);
    if (!dateReady() || config.utcOffsetMinutes != offsetMinutes_ ||
        !config.autoIrrigationWindow.allows(local_->minute) || state_.autoPaused || saveFailed_ ||
        state_.fault != Fault::None || !zone || zone->rule.mode != AutoMode::Smart ||
        checkZoneReadiness(config, channels_, zoneId, IrrigationSource::Smart) != ZoneReadiness::Ready) {
        const auto found = std::find_if(soil_.begin(), soil_.end(), [zoneId](const auto& entry) {
            return entry.zoneId == zoneId;
        });
        if (found != soil_.end()) {
            found->consecutive = 0;
            found->latestLow = false;
        }
        cancelCandidate(zoneId, IrrigationSource::Smart);
        return;
    }
    auto entry = std::find_if(soil_.begin(), soil_.end(), [zoneId](const auto& progress) {
        return progress.zoneId == zoneId;
    });
    if (entry == soil_.end()) {
        if (soil_.size() >= CriticalState::maxTrackedZones) {
            return;
        }
        soil_.push_back(SoilProgress{zoneId});
        entry = std::prev(soil_.end());
    }
    const auto* soil = reading ? std::get_if<SoilReading>(&reading->data) : nullptr;
    const auto channel = *findById(config.pumps, zone->pumpId)->channel;
    const bool timeValid = reading && reading->sampledAtMs <= now_ && reading->sampledAtMs > automaticAfterMs_;
    const auto stamp = reading ? reading->sampledAtMs : 0;
    const bool valid = timeValid && now_ - stamp <= config.readingValidityMs() &&
        soil && soil->sensorId == *zone->representativeSensorId && soil->status == ReadingStatus::Valid &&
        soil->moisturePercent && std::isfinite(*soil->moisturePercent) && *soil->moisturePercent >= 0.0f &&
        *soil->moisturePercent <= 100.0f && !cooling(*zone, channel, stamp) && !cooling(*zone, channel, now_);
    const bool ordered = !entry->lastSample || stamp >= *entry->lastSample;
    if (!valid || !ordered) {
        if (timeValid && ordered) {
            entry->lastSample = stamp;
        }
        entry->consecutive = 0;
        entry->latestLow = false;
        cancelCandidate(zoneId, IrrigationSource::Smart);
        return;
    }
    if (entry->lastSample == stamp) {
        // 已见过的采样可撤销触发，不能在失效后重新计数。
        if (*soil->moisturePercent >= *zone->rule.lowMoisturePercent) {
            entry->consecutive = 0;
            entry->latestLow = false;
            cancelCandidate(zoneId, IrrigationSource::Smart);
        }
        return;
    }
    cancelCandidate(zoneId, IrrigationSource::Smart);
    const MonotonicMs interval = config.sampleIntervalMs;
    const MonotonicMs jitter = std::min<MonotonicMs>(interval / 10, 30'000);
    if (entry->lastSample && stamp - *entry->lastSample > interval + jitter) {
        entry->consecutive = 0;
    }
    entry->lastSample = stamp;
    entry->latestLow = *soil->moisturePercent < *zone->rule.lowMoisturePercent;
    if (!entry->latestLow) {
        entry->consecutive = 0;
        return;
    }
    if (entry->consecutive >= zone->rule.consecutiveLowReadings - 1) {
        entry->consecutive = 0;
        addCandidate({0, IrrigationSource::Smart, {zoneId, *zone->rule.smartDoseMl},
                      Trigger{local_->day, 0, stamp}});
    } else {
        ++entry->consecutive;
    }
}

void Irrigation::resetAutomatic() {
    clearAutomatic();
    cancelUnstartedAutomatic();
}

void Irrigation::clearAutomatic() {
    scheduleWindow_.reset();
    automaticAfterMs_ = now_;
    soil_.clear();
    candidates_.clear();
}

void Irrigation::cancelUnstartedAutomatic() {
    if ((phase_ == Phase::Preparing || phase_ == Phase::AwaitingStart) &&
        state_.pending->request.source != IrrigationSource::Manual) {
        requestStop(StopReason::Interrupted);
    }
}

void Irrigation::cancelCandidate(EntityId zoneId, IrrigationSource source) {
    candidates_.erase(std::remove_if(candidates_.begin(), candidates_.end(), [zoneId, source](const auto& request) {
        return request.dose.zoneId == zoneId && request.source == source;
    }), candidates_.end());
}

void Irrigation::addCandidate(IrrigationRequest request) {
    const auto earlier = std::lower_bound(candidates_.begin(), candidates_.end(), request.dose.zoneId,
                                          [](const auto& item, EntityId id) { return item.dose.zoneId < id; });
    if (earlier != candidates_.end() && earlier->dose.zoneId == request.dose.zoneId) {
        *earlier = request;
    } else if (candidates_.size() < CriticalState::maxTrackedZones || earlier != candidates_.end()) {
        candidates_.insert(earlier, request);
        if (candidates_.size() > CriticalState::maxTrackedZones) {
            candidates_.pop_back();
        }
    }
}

Decision Irrigation::acceptAutomatic(const SystemConfig& config) {
    if (candidates_.empty()) {
        return {Rejection::StaleTrigger};
    }
    const auto request = candidates_.front();
    return accept(config, request);
}

}  // namespace planthub::core
