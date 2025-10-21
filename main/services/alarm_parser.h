#ifndef ALARM_PARSER_H
#define ALARM_PARSER_H

#include <optional>
#include <string>

#include <cJSON.h>

// 描述闹钟触发的方式
enum class AlarmTriggerType {
    kDelay,
    kTimeOfDay,
};

struct AlarmScheduleRequest {
    std::string id;
    std::optional<int> delay_seconds;
    std::optional<int> hour;
    std::optional<int> minute;
    bool repeat = false;
    std::optional<int> interval_minutes;
    AlarmTriggerType trigger_type = AlarmTriggerType::kDelay;
};

struct AlarmParseResult {
    bool success = false;
    AlarmScheduleRequest request;
    std::string error_message;
};

// 解析并校验闹钟参数，返回组合是否合法及错误信息
AlarmParseResult ParseAlarmRequest(const cJSON* arguments);

// 根据触发方式生成统一的闹钟 ID
std::string GenerateAlarmId(AlarmTriggerType trigger_type);

#endif // ALARM_PARSER_H
