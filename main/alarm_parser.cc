#include "alarm_parser.h"

#include <atomic>
#include <cstdio>

#include <esp_timer.h>

namespace {
constexpr int kMinDelaySeconds = 1;
constexpr int kMaxDelaySeconds = 24 * 60 * 60; // 24 小时
constexpr int kMinHour = 0;
constexpr int kMaxHour = 23;
constexpr int kMinMinute = 0;
constexpr int kMaxMinute = 59;
constexpr int kMinIntervalMinutes = 1;
constexpr int kMaxIntervalMinutes = 7 * 24 * 60; // 最长重复周期 7 天

std::atomic<uint32_t> g_alarm_counter{0};

std::optional<int> GetOptionalInt(const cJSON* object, const char* name, std::string& error_message) {
    const cJSON* item = cJSON_GetObjectItemCaseSensitive(object, name);
    if (!item) {
        return std::nullopt;
    }
    if (!cJSON_IsNumber(item)) {
        error_message = std::string("参数 \"") + name + "\" 需要是整数";
        return std::nullopt;
    }
    return item->valueint;
}

std::optional<bool> GetOptionalBool(const cJSON* object, const char* name, std::string& error_message) {
    const cJSON* item = cJSON_GetObjectItemCaseSensitive(object, name);
    if (!item) {
        return std::nullopt;
    }
    if (!cJSON_IsBool(item)) {
        error_message = std::string("参数 \"") + name + "\" 需要是布尔值";
        return std::nullopt;
    }
    return item->valueint == 1;
}

bool ValidateRange(const std::optional<int>& value, int min_value, int max_value, const char* name, std::string& error_message) {
    if (!value.has_value()) {
        return true;
    }
    if (value.value() < min_value || value.value() > max_value) {
        error_message = std::string("参数 \"") + name + "\" 需要在 " + std::to_string(min_value) + "-" + std::to_string(max_value) + " 之间";
        return false;
    }
    return true;
}

} // namespace

AlarmParseResult ParseAlarmRequest(const cJSON* arguments) {
    AlarmParseResult result;
    if (!cJSON_IsObject(arguments)) {
        result.error_message = "参数必须是 JSON 对象";
        return result;
    }

    std::string error_message;

    auto delay = GetOptionalInt(arguments, "delay", error_message);
    if (!error_message.empty()) {
        result.error_message = error_message;
        return result;
    }

    auto hour = GetOptionalInt(arguments, "hour", error_message);
    if (!error_message.empty()) {
        result.error_message = error_message;
        return result;
    }

    auto minute = GetOptionalInt(arguments, "minute", error_message);
    if (!error_message.empty()) {
        result.error_message = error_message;
        return result;
    }

    auto repeat = GetOptionalBool(arguments, "repeat", error_message);
    if (!error_message.empty()) {
        result.error_message = error_message;
        return result;
    }

    auto interval = GetOptionalInt(arguments, "interval", error_message);
    if (!error_message.empty()) {
        result.error_message = error_message;
        return result;
    }

    const bool has_delay = delay.has_value();
    const bool has_time = hour.has_value() || minute.has_value();

    if (has_delay && has_time) {
        result.error_message = "不支持同时指定 delay 与 hour/minute";
        return result;
    }

    if (!has_delay && !has_time) {
        result.error_message = "必须提供 delay 或 hour/minute";
        return result;
    }

    if (minute.has_value() && !hour.has_value()) {
        result.error_message = "提供 minute 时必须同时提供 hour";
        return result;
    }

    if (hour.has_value() && !minute.has_value()) {
        result.error_message = "提供 hour 时必须同时提供 minute";
        return result;
    }

    if (!ValidateRange(delay, kMinDelaySeconds, kMaxDelaySeconds, "delay", error_message)) {
        result.error_message = error_message;
        return result;
    }

    if (!ValidateRange(hour, kMinHour, kMaxHour, "hour", error_message)) {
        result.error_message = error_message;
        return result;
    }

    if (!ValidateRange(minute, kMinMinute, kMaxMinute, "minute", error_message)) {
        result.error_message = error_message;
        return result;
    }

    if (!ValidateRange(interval, kMinIntervalMinutes, kMaxIntervalMinutes, "interval", error_message)) {
        result.error_message = error_message;
        return result;
    }

    const bool repeat_value = repeat.value_or(false);

    if (repeat_value && !interval.has_value()) {
        result.error_message = "repeat 为 true 时必须提供 interval";
        return result;
    }

    if (!repeat_value && interval.has_value()) {
        result.error_message = "repeat 为 false 时不需要提供 interval";
        return result;
    }

    result.request.delay_seconds = delay;
    result.request.hour = hour;
    result.request.minute = minute;
    result.request.repeat = repeat_value;
    result.request.interval_minutes = interval;
    result.request.trigger_type = has_delay ? AlarmTriggerType::kDelay : AlarmTriggerType::kTimeOfDay;
    result.request.id = GenerateAlarmId(result.request.trigger_type);
    result.success = true;
    return result;
}

std::string GenerateAlarmId(AlarmTriggerType trigger_type) {
    uint64_t timestamp_us = static_cast<uint64_t>(esp_timer_get_time());
    uint32_t counter = g_alarm_counter.fetch_add(1, std::memory_order_relaxed);
    char buffer[64];
    snprintf(buffer, sizeof(buffer), "alarm-%s-%llu-%u",
             trigger_type == AlarmTriggerType::kDelay ? "delay" : "time",
             static_cast<unsigned long long>(timestamp_us),
             static_cast<unsigned>(counter));
    return std::string(buffer);
}
