#include "alarm_clock.h"

#include <cstdlib>
#include <sstream>
#include <utility>
#include <vector>

#include <esp_err.h>
#include <esp_log.h>

#include "application.h"
#include "assets/lang_config.h"
#include "board.h"
#include "display.h"

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
    esp_timer_create_args_t timer_args = {
        .callback = [](void* arg) {
            auto* manager = static_cast<AlarmManager*>(arg);
            manager->OnSchedulerTimer();
        },
        .arg = this,
        .dispatch_method = ESP_TIMER_TASK,
        .name = "alarm_scheduler",
        .skip_unhandled_events = true,
    };

    esp_err_t err = esp_timer_create(&timer_args, &scheduler_timer_);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to create scheduler timer: %s", esp_err_to_name(err));
        scheduler_timer_ = nullptr;
    }

    LoadAlarms();
    ScheduleNext();
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
    settings_.Commit();
    ScheduleNext();
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
    settings_.Commit();
    ScheduleNext();
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
    settings_.Commit();
    ScheduleNext();
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

void AlarmManager::SetCloudNotifier(std::function<void(const Alarm&)> notifier) {
    cloud_notifier_ = std::move(notifier);
}

void AlarmManager::ScheduleNext() {
    if (scheduler_timer_ == nullptr) {
        return;
    }

    if (esp_timer_is_active(scheduler_timer_)) {
        esp_timer_stop(scheduler_timer_);
    }

    if (alarms_.empty()) {
        return;
    }

    time_t now = std::time(nullptr);
    bool has_next = false;
    time_t next_time = 0;

    for (const auto& entry : alarms_) {
        if (!has_next || entry.second.time < next_time) {
            next_time = entry.second.time;
            has_next = true;
        }
    }

    if (!has_next) {
        return;
    }

    int64_t delay_us = 1000;
    if (next_time > now) {
        delay_us = static_cast<int64_t>(next_time - now) * 1000000LL;
        if (delay_us < 1000) {
            delay_us = 1000;
        }
    }

    esp_err_t err = esp_timer_start_once(scheduler_timer_, delay_us);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to start scheduler timer: %s", esp_err_to_name(err));
    } else {
        ESP_LOGI(TAG, "Scheduled next alarm in %lld ms", static_cast<long long>(delay_us / 1000));
    }
}

void AlarmManager::OnSchedulerTimer() {
    constexpr int kSecondsPerMinute = 60;
    time_t now = std::time(nullptr);
    std::vector<int> due_ids;
    due_ids.reserve(alarms_.size());

    for (const auto& entry : alarms_) {
        if (entry.second.time <= now) {
            due_ids.push_back(entry.first);
        }
    }

    if (due_ids.empty()) {
        ScheduleNext();
        return;
    }

    bool commit_needed = false;
    bool ids_changed = false;
    std::vector<int> remove_ids;

    for (int id : due_ids) {
        auto it = alarms_.find(id);
        if (it == alarms_.end()) {
            continue;
        }

        Alarm& stored_alarm = it->second;
        Alarm alarm_copy = stored_alarm;
        std::string notification = alarm_copy.name.empty() ? std::string("Alarm") : alarm_copy.name;
        auto notifier = cloud_notifier_;

        Application::GetInstance().Schedule([alarm_copy, notification, notifier]() {
            auto display = Board::GetInstance().GetDisplay();
            if (display != nullptr) {
                display->ShowNotification(notification.c_str(), 5000);
            }

            auto& app = Application::GetInstance();
            app.PlaySound(Lang::Sounds::OGG_POPUP);

            if (notifier) {
                notifier(alarm_copy);
            }
        });

        if (stored_alarm.repeat && stored_alarm.interval > 0) {
            int64_t interval_seconds = static_cast<int64_t>(stored_alarm.interval) * kSecondsPerMinute;
            if (interval_seconds <= 0) {
                interval_seconds = kSecondsPerMinute;
            }

            time_t next_time = stored_alarm.time + interval_seconds;
            while (next_time <= now) {
                next_time += interval_seconds;
            }

            stored_alarm.time = next_time;
            PersistAlarm(stored_alarm);
            commit_needed = true;
        } else {
            if (stored_alarm.timer_handle != nullptr) {
                esp_timer_stop(stored_alarm.timer_handle);
                esp_timer_delete(stored_alarm.timer_handle);
                stored_alarm.timer_handle = nullptr;
            }
            remove_ids.push_back(id);
        }
    }

    for (int id : remove_ids) {
        auto it = alarms_.find(id);
        if (it == alarms_.end()) {
            continue;
        }

        RemoveAlarmFromStorage(id);
        alarms_.erase(it);
        ids_changed = true;
        commit_needed = true;
    }

    if (ids_changed) {
        PersistAlarmIds();
    }

    if (commit_needed || ids_changed) {
        settings_.Commit();
    }

    ScheduleNext();
}
