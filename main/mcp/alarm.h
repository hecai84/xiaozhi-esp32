#ifndef _ALARM_H_
#define _ALARM_H_

#include <string>
#include <vector>
#include <mutex>
#include <ctime>
#include <cstdint>

#include <esp_timer.h>

// 闹钟类型：一次 / 每日 / 每周 / 每月
enum class AlarmType {
    OneShot,   // 指定年月日时间执行一次
    Daily,     // 每天指定时间
    Weekly,    // 每周按星期掩码的指定时间
    Monthly,   // 每月指定日与时间
    Interval   // 按固定间隔执行（interval_seconds）
};

struct AlarmItem {
    int id = 0;                 // 唯一 ID
    bool enabled = true;        // 是否启用
    AlarmType type = AlarmType::OneShot;
    int year = 0;               // 一次性闹钟使用
    int month = 0;              // 一次性闹钟使用 (1-12)
    int day = 0;                // 一次性 & 每月闹钟 (1-31)
    int hour = 0;               // 0-23
    int minute = 0;             // 0-59
    int second = 0;             // 0-59 秒级精度
    uint16_t weekdays_mask = 0; // 每周闹钟使用, bit0=Mon ... bit6=Sun
    int interval_seconds = 0;   // Interval 类型使用，>=1
    std::string label;          // 文字标签
    time_t next_trigger = 0;    // 下次触发时间（UTC）
};

// 负责闹钟存储、调度与 MCP 工具注册
class AlarmManager {
public:
    static AlarmManager& GetInstance();

    void Initialize();                 // 从 NVS 读取并调度
    void AddMcpTools();                // 注册 MCP 工具

    // 业务接口
    int AddAlarm(const AlarmItem& item_template); // 返回生成的 id
    bool RemoveAlarm(int id);
    bool EnableAlarm(int id, bool enable);
    void ClearAlarms();
    std::string ListAlarmsJson();
    std::string NextAlarmJson();

private:
    AlarmManager() = default;
    ~AlarmManager();
    AlarmManager(const AlarmManager&) = delete;
    AlarmManager& operator=(const AlarmManager&) = delete;

    std::mutex mutex_;
    std::vector<AlarmItem> alarms_;
    int next_id_ = 1;
    esp_timer_handle_t timer_ = nullptr;

    void LoadFromSettings();
    void SaveToSettings();
    void RecalculateAllNextTriggers();
    void RecalculateNextTrigger(AlarmItem& item, time_t now);
    void ScheduleTimer();
    void OnTimerFired();
};

#endif // _ALARM_H_
