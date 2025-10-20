#include "alarm_clock.h"

#include <cstdlib>
#include <sstream>
#include <vector>

#include <esp_log.h>

namespace {
constexpr const char* kNamespace = "alarm_clock";
constexpr const char* kAlarmIdsKey = "alarm_ids";
constexpr const char* kNextIdKey = "next_alarm_id";
const char* TAG = "AlarmManager";

std::string MakeKey(const char* prefix, int id) {
    std::ostringstream oss;
    oss << prefix << id;
    return oss.str();
}

std::vector<int> ParseIdList(const std::string& id_list) {
    std::vector<int> ids;
    std::stringstream ss(id_list);
    std::string item;
    while (std::getline(ss, item, ',')) {
        if (item.empty()) {
            continue;
        }
        char* end_ptr = nullptr;
        long value = std::strtol(item.c_str(), &end_ptr, 10);
        if (end_ptr != item.c_str()) {
            ids.push_back(static_cast<int>(value));
        } else {
            ESP_LOGW(TAG, "Invalid alarm id entry: %s", item.c_str());
        }
    }
    return ids;
}

std::string JoinIds(const std::map<int, Alarm>& alarms) {
    std::ostringstream oss;
    bool first = true;
    for (const auto& entry : alarms) {
        if (!first) {
            oss << ',';
        }
        first = false;
        oss << entry.first;
    }
    return oss.str();
}

}  // namespace

AlarmManager::AlarmManager() : settings_(kNamespace, true) {
    LoadAlarms();
}

AlarmManager::~AlarmManager() {
    for (auto& [id, alarm] : alarms_) {
        if (alarm.timer_handle != nullptr) {
            esp_timer_stop(alarm.timer_handle);
            esp_timer_delete(alarm.timer_handle);
            alarm.timer_handle = nullptr;
        }
    }
    if (scheduler_timer_ != nullptr) {
        esp_timer_stop(scheduler_timer_);
        esp_timer_delete(scheduler_timer_);
        scheduler_timer_ = nullptr;
    }
}

int AlarmManager::AddAlarm(const Alarm& alarm) {
    Alarm stored = alarm;
    if (stored.id <= 0) {
        stored.id = GenerateId();
    } else if (stored.id >= next_alarm_id_) {
        next_alarm_id_ = stored.id + 1;
    }

    alarms_[stored.id] = stored;
    PersistAlarm(stored);
    PersistAlarmIds();
    settings_.SetInt(kNextIdKey, next_alarm_id_);
    return stored.id;
}

bool AlarmManager::RemoveAlarm(int id) {
    auto it = alarms_.find(id);
    if (it == alarms_.end()) {
        return false;
    }

    if (it->second.timer_handle != nullptr) {
        esp_timer_stop(it->second.timer_handle);
        esp_timer_delete(it->second.timer_handle);
    }

    alarms_.erase(it);
    RemoveAlarmFromStorage(id);
    PersistAlarmIds();
    return true;
}

bool AlarmManager::UpdateAlarm(const Alarm& alarm) {
    auto it = alarms_.find(alarm.id);
    if (it == alarms_.end()) {
        return false;
    }

    if (it->second.timer_handle != alarm.timer_handle && it->second.timer_handle != nullptr) {
        esp_timer_stop(it->second.timer_handle);
        esp_timer_delete(it->second.timer_handle);
    }

    alarms_[alarm.id] = alarm;
    PersistAlarm(alarm);
    PersistAlarmIds();
    return true;
}

std::optional<Alarm> AlarmManager::GetAlarm(int id) const {
    auto it = alarms_.find(id);
    if (it == alarms_.end()) {
        return std::nullopt;
    }
    return it->second;
}

std::vector<Alarm> AlarmManager::GetAlarms() const {
    std::vector<Alarm> results;
    results.reserve(alarms_.size());
    for (const auto& entry : alarms_) {
        results.push_back(entry.second);
    }
    return results;
}

void AlarmManager::LoadAlarms() {
    std::string ids = settings_.GetString(kAlarmIdsKey, "");
    if (!ids.empty()) {
        for (int id : ParseIdList(ids)) {
            Alarm alarm;
            alarm.id = id;
            alarm.name = settings_.GetString(MakeKey("alarm_", id), "");

            std::string time_key = MakeKey("alarm_time_", id);
            std::string time_str = settings_.GetString(time_key, "0");
            char* end_ptr = nullptr;
            long long time_value = std::strtoll(time_str.c_str(), &end_ptr, 10);
            if (end_ptr != time_str.c_str()) {
                alarm.time = static_cast<time_t>(time_value);
            } else {
                ESP_LOGW(TAG, "Invalid time for alarm %d", id);
            }

            std::string repeat_key = MakeKey("alarm_repeat_", id);
            alarm.repeat = settings_.GetBool(repeat_key, false);

            std::string interval_key = MakeKey("alarm_interval_", id);
            alarm.interval = settings_.GetInt(interval_key, 0);

            alarms_.emplace(id, std::move(alarm));
        }
    }

    next_alarm_id_ = settings_.GetInt(kNextIdKey, 1);
    if (next_alarm_id_ <= 0) {
        next_alarm_id_ = 1;
    }
    if (!alarms_.empty()) {
        int max_id = alarms_.rbegin()->first;
        if (next_alarm_id_ <= max_id) {
            next_alarm_id_ = max_id + 1;
        }
    }
}

void AlarmManager::PersistAlarm(const Alarm& alarm) {
    settings_.SetString(MakeKey("alarm_", alarm.id), alarm.name);
    settings_.SetString(MakeKey("alarm_time_", alarm.id), std::to_string(static_cast<long long>(alarm.time)));
    settings_.SetBool(MakeKey("alarm_repeat_", alarm.id), alarm.repeat);
    settings_.SetInt(MakeKey("alarm_interval_", alarm.id), alarm.interval);
}

void AlarmManager::PersistAlarmIds() {
    settings_.SetString(kAlarmIdsKey, JoinIds(alarms_));
}

void AlarmManager::RemoveAlarmFromStorage(int id) {
    settings_.EraseKey(MakeKey("alarm_", id));
    settings_.EraseKey(MakeKey("alarm_time_", id));
    settings_.EraseKey(MakeKey("alarm_repeat_", id));
    settings_.EraseKey(MakeKey("alarm_interval_", id));
}

int AlarmManager::GenerateId() {
    return next_alarm_id_++;
}
