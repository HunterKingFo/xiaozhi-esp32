#ifndef ALARM_CLOCK_H
#define ALARM_CLOCK_H

#include <ctime>
#include <functional>
#include <map>
#include <optional>
#include <string>
#include <vector>
#include <mutex>

#include <esp_timer.h>

#include "settings.h"

struct Alarm {
    time_t time = 0;
    bool repeat = false;
    int interval = 0;
    std::string name;
    std::string sound{"ALARM1"};
    int id = 0;
    esp_timer_handle_t timer_handle = nullptr;
};

class AlarmManager {
public:
    AlarmManager();
    ~AlarmManager();

    int AddAlarm(const Alarm& alarm);
    bool RemoveAlarm(int id);
    bool UpdateAlarm(const Alarm& alarm);

    std::optional<Alarm> GetAlarm(int id) const;
    std::vector<Alarm> GetAlarms() const;

    void SetCloudNotifier(std::function<void(const Alarm&)> notifier);

private:
    void LoadAlarms();
    void PersistAlarm(const Alarm& alarm);
    void PersistAlarmIds();
    void RemoveAlarmFromStorage(int id);
    int GenerateId();
    void ScheduleNext();
    void OnSchedulerTimer();
    void ScheduleNextLocked();

    std::map<int, Alarm> alarms_;
    int next_alarm_id_ = 1;
    Settings settings_;
    esp_timer_handle_t scheduler_timer_ = nullptr;
    std::function<void(const Alarm&)> cloud_notifier_;
    mutable std::mutex alarms_mutex_;
};

#endif  // ALARM_CLOCK_H
