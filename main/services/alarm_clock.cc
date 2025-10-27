#include "alarm_clock.h"

#include <array>
#include <charconv>
#include <cctype>
#include <cstdlib>
#include <limits>
#include <sstream>
#include <string_view>
#include <system_error>
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
constexpr const char* kAlarmNameKeyPrefix = "a_n_";
constexpr const char* kAlarmTimeKeyPrefix = "a_t_";
constexpr const char* kAlarmRepeatKeyPrefix = "a_r_";
constexpr const char* kAlarmIntervalKeyPrefix = "a_i_";
constexpr const char* kAlarmSoundKeyPrefix = "a_s_";
constexpr const char* kLegacyAlarmNameKeyPrefix = "alarm_";
constexpr const char* kLegacyAlarmTimeKeyPrefix = "alarm_time_";
constexpr const char* kLegacyAlarmRepeatKeyPrefix = "alarm_repeat_";
constexpr const char* kLegacyAlarmIntervalKeyPrefix = "alarm_interval_";
constexpr const char* kLegacyAlarmSoundKeyPrefix = "alarm_sound_";
const char* TAG = "AlarmManager";

constexpr std::array<std::string_view, 3> kAllowedSounds{{"ALARM1", "ALARM2", "ALARM3"}};

bool IsValidSound(std::string_view sound) {
    for (auto allowed : kAllowedSounds) {
        if (sound == allowed) {
            return true;
        }
    }
    return false;
}

std::string NormalizeSound(std::string_view sound) {
    if (IsValidSound(sound)) {
        return std::string(sound);
    }
    return std::string("ALARM1");
}

std::string_view ResolveAlarmSoundClip(std::string_view sound) {
    if (sound == "ALARM2") {
        return Lang::Sounds::OGG_ALARM2;
    }
    if (sound == "ALARM3") {
        return Lang::Sounds::OGG_ALARM3;
    }
    return Lang::Sounds::OGG_ALARM1;
}

std::string MakeKey(const char* prefix, int id) {
    std::ostringstream oss;
    oss << prefix << id;
    return oss.str();
}

std::string_view TrimStringView(std::string_view view) {
    auto is_space = [](unsigned char c) { return std::isspace(c) != 0; };
    while (!view.empty() && is_space(static_cast<unsigned char>(view.front()))) {
        view.remove_prefix(1);
    }
    while (!view.empty() && is_space(static_cast<unsigned char>(view.back()))) {
        view.remove_suffix(1);
    }
    return view;
}

template<typename T>
std::optional<T> ParseIntegral(std::string_view text) {
    text = TrimStringView(text);
    if (text.empty()) {
        return std::nullopt;
    }

    T value{};
    auto result = std::from_chars(text.data(), text.data() + text.size(), value);
    if (result.ec != std::errc{} || result.ptr != text.data() + text.size()) {
        return std::nullopt;
    }
    return value;
}

std::vector<int> ParseIdList(const std::string& id_list) {
    std::vector<int> ids;
    std::stringstream ss(id_list);
    std::string item;
    while (std::getline(ss, item, ',')) {
        std::string_view trimmed = TrimStringView(item);
        if (trimmed.empty()) {
            continue;
        }
        auto parsed = ParseIntegral<int>(trimmed);
        if (parsed.has_value()) {
            ids.push_back(parsed.value());
        } else {
            std::string invalid(trimmed);
            ESP_LOGW(TAG, "Invalid alarm id entry: %s", invalid.c_str());
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
    std::lock_guard<std::mutex> lock(alarms_mutex_);
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
    std::lock_guard<std::mutex> lock(alarms_mutex_);
    Alarm stored = alarm;
    stored.sound = NormalizeSound(stored.sound);
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
    ScheduleNextLocked();
    return stored.id;
}

bool AlarmManager::RemoveAlarm(int id) {
    std::lock_guard<std::mutex> lock(alarms_mutex_);
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
    ScheduleNextLocked();
    return true;
}

bool AlarmManager::UpdateAlarm(const Alarm& alarm) {
    std::lock_guard<std::mutex> lock(alarms_mutex_);
    auto it = alarms_.find(alarm.id);
    if (it == alarms_.end()) {
        return false;
    }

    if (it->second.timer_handle != alarm.timer_handle && it->second.timer_handle != nullptr) {
        esp_timer_stop(it->second.timer_handle);
        esp_timer_delete(it->second.timer_handle);
    }

    Alarm updated = alarm;
    updated.sound = NormalizeSound(updated.sound);
    alarms_[updated.id] = updated;
    PersistAlarm(updated);
    PersistAlarmIds();
    settings_.Commit();
    ScheduleNextLocked();
    return true;
}

std::optional<Alarm> AlarmManager::GetAlarm(int id) const {
    std::lock_guard<std::mutex> lock(alarms_mutex_);
    auto it = alarms_.find(id);
    if (it == alarms_.end()) {
        return std::nullopt;
    }
    return it->second;
}

std::vector<Alarm> AlarmManager::GetAlarms() const {
    std::lock_guard<std::mutex> lock(alarms_mutex_);
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
            auto load_string_field = [&](const char* prefix, const char* legacy_prefix, const std::string& fallback) {
                std::string key = MakeKey(prefix, id);
                auto value = settings_.TryGetString(key);
                std::string legacy_key = MakeKey(legacy_prefix, id);
                if (value.has_value()) {
                    settings_.EraseKey(legacy_key);
                    return *value;
                }
                auto legacy_value = settings_.TryGetString(legacy_key);
                if (legacy_value.has_value()) {
                    settings_.SetString(key, *legacy_value);
                    settings_.EraseKey(legacy_key);
                    return *legacy_value;
                }
                settings_.EraseKey(legacy_key);
                return fallback;
            };
            auto load_int_field = [&](const char* prefix, const char* legacy_prefix, int32_t fallback) {
                std::string key = MakeKey(prefix, id);
                auto value = settings_.TryGetInt(key);
                std::string legacy_key = MakeKey(legacy_prefix, id);
                if (value.has_value()) {
                    settings_.EraseKey(legacy_key);
                    return *value;
                }
                auto legacy_value = settings_.TryGetInt(legacy_key);
                if (legacy_value.has_value()) {
                    settings_.SetInt(key, *legacy_value);
                    settings_.EraseKey(legacy_key);
                    return *legacy_value;
                }
                settings_.EraseKey(legacy_key);
                return fallback;
            };
            auto load_bool_field = [&](const char* prefix, const char* legacy_prefix, bool fallback) {
                std::string key = MakeKey(prefix, id);
                auto value = settings_.TryGetBool(key);
                std::string legacy_key = MakeKey(legacy_prefix, id);
                if (value.has_value()) {
                    settings_.EraseKey(legacy_key);
                    return *value;
                }
                auto legacy_value = settings_.TryGetBool(legacy_key);
                if (legacy_value.has_value()) {
                    settings_.SetBool(key, *legacy_value);
                    settings_.EraseKey(legacy_key);
                    return *legacy_value;
                }
                settings_.EraseKey(legacy_key);
                return fallback;
            };

            alarm.name = load_string_field(kAlarmNameKeyPrefix, kLegacyAlarmNameKeyPrefix, "");

            std::string time_str = load_string_field(kAlarmTimeKeyPrefix, kLegacyAlarmTimeKeyPrefix, "0");
            auto trimmed_time = TrimStringView(time_str);
            auto parsed_time = ParseIntegral<long long>(trimmed_time);
            if (!parsed_time.has_value()) {
                if (!trimmed_time.empty()) {
                    std::string invalid(trimmed_time);
                    ESP_LOGW(TAG, "Invalid time for alarm %d: %s", id, invalid.c_str());
                } else {
                    ESP_LOGW(TAG, "Invalid time for alarm %d", id);
                }
                continue;
            }

            long long time_value = parsed_time.value();
            if (time_value < std::numeric_limits<time_t>::min() ||
                time_value > std::numeric_limits<time_t>::max()) {
                std::string invalid(trimmed_time);
                ESP_LOGW(TAG, "Time for alarm %d out of range: %s", id, invalid.c_str());
                continue;
            }
            alarm.time = static_cast<time_t>(time_value);

            alarm.repeat = load_bool_field(kAlarmRepeatKeyPrefix, kLegacyAlarmRepeatKeyPrefix, false);

            alarm.interval = load_int_field(kAlarmIntervalKeyPrefix, kLegacyAlarmIntervalKeyPrefix, 0);

            std::string stored_sound = load_string_field(kAlarmSoundKeyPrefix, kLegacyAlarmSoundKeyPrefix, "ALARM1");
            if (!IsValidSound(stored_sound)) {
                ESP_LOGW(TAG, "Invalid sound for alarm %d: %s", id, stored_sound.c_str());
                stored_sound = "ALARM1";
            }
            alarm.sound = stored_sound;

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
    settings_.Commit();
}

void AlarmManager::PersistAlarm(const Alarm& alarm) {
    settings_.SetString(MakeKey(kAlarmNameKeyPrefix, alarm.id), alarm.name);
    settings_.SetString(MakeKey(kAlarmTimeKeyPrefix, alarm.id), std::to_string(static_cast<long long>(alarm.time)));
    settings_.SetBool(MakeKey(kAlarmRepeatKeyPrefix, alarm.id), alarm.repeat);
    settings_.SetInt(MakeKey(kAlarmIntervalKeyPrefix, alarm.id), alarm.interval);
    settings_.SetString(MakeKey(kAlarmSoundKeyPrefix, alarm.id), NormalizeSound(alarm.sound));
}

void AlarmManager::PersistAlarmIds() {
    settings_.SetString(kAlarmIdsKey, JoinIds(alarms_));
}

void AlarmManager::RemoveAlarmFromStorage(int id) {
    settings_.EraseKey(MakeKey(kAlarmNameKeyPrefix, id));
    settings_.EraseKey(MakeKey(kAlarmTimeKeyPrefix, id));
    settings_.EraseKey(MakeKey(kAlarmRepeatKeyPrefix, id));
    settings_.EraseKey(MakeKey(kAlarmIntervalKeyPrefix, id));
    settings_.EraseKey(MakeKey(kAlarmSoundKeyPrefix, id));
    settings_.EraseKey(MakeKey(kLegacyAlarmNameKeyPrefix, id));
    settings_.EraseKey(MakeKey(kLegacyAlarmTimeKeyPrefix, id));
    settings_.EraseKey(MakeKey(kLegacyAlarmRepeatKeyPrefix, id));
    settings_.EraseKey(MakeKey(kLegacyAlarmIntervalKeyPrefix, id));
    settings_.EraseKey(MakeKey(kLegacyAlarmSoundKeyPrefix, id));
}

int AlarmManager::GenerateId() {
    return next_alarm_id_++;
}

void AlarmManager::SetCloudNotifier(std::function<void(const Alarm&)> notifier) {
    std::lock_guard<std::mutex> lock(alarms_mutex_);
    cloud_notifier_ = std::move(notifier);
}

void AlarmManager::ScheduleNext() {
    std::lock_guard<std::mutex> lock(alarms_mutex_);
    ScheduleNextLocked();
}

void AlarmManager::ScheduleNextLocked() {
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
    std::lock_guard<std::mutex> lock(alarms_mutex_);
    std::vector<int> due_ids;
    due_ids.reserve(alarms_.size());

    for (const auto& entry : alarms_) {
        if (entry.second.time <= now) {
            due_ids.push_back(entry.first);
        }
    }

    if (due_ids.empty()) {
        ScheduleNextLocked();
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
        std::string_view sound_clip = ResolveAlarmSoundClip(alarm_copy.sound);

        Application::GetInstance().Schedule([alarm_copy, notification, notifier, sound_clip]() {
            auto display = Board::GetInstance().GetDisplay();
            if (display != nullptr) {
                display->ShowNotification(notification.c_str(), 5000);
            }

            auto& app = Application::GetInstance();
            app.PlayAlarmSound(sound_clip, alarm_copy.id);

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

    ScheduleNextLocked();
}
