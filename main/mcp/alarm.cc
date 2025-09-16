// Alarm Manager Implementation
#include "alarm.h"
#include "settings.h"
#include "mcp_server.h"
#include "application.h"
#include "assets/lang_config.h"
#include <esp_log.h>
#include <cJSON.h>
#include "display.h"
#include <algorithm>

static const char* TAG = "AlarmManager";

// =============== 工具函数 ===============
// 兼容性实现：部分工具链没有 timegm，提供一个简单的 UTC 计算版本
static time_t timegm_compat(struct tm* t) {
	int year = t->tm_year + 1900;   // full year
	int month = t->tm_mon + 1;      // 1-12
	int day = t->tm_mday;           // 1-31
	if (month <= 0) { month = 1; }
	if (month > 12) { month = 12; }
	static const int mdays[12] = {31,28,31,30,31,30,31,31,30,31,30,31};

	// 计算从 1970-01-01 到前一年末的天数
	long days = 0;
	for (int y = 1970; y < year; ++y) {
		days += 365 + ((y % 4 == 0 && y % 100 != 0) || (y % 400 == 0));
	}
	// 累加本年已过月份
	for (int m = 1; m < month; ++m) {
		days += mdays[m - 1];
		if (m == 2 && ((year % 4 == 0 && year % 100 != 0) || (year % 400 == 0))) {
			days += 1; // 闰年 2 月
		}
	}
	// 当月天数（从 0 开始计）
	days += (day - 1);

	long seconds = days * 86400L + t->tm_hour * 3600L + t->tm_min * 60L + t->tm_sec;
	return (time_t)seconds;
}
static time_t MakeTimeUTC(int year, int month, int day, int hour, int minute, int second) {
	struct tm tm_time = {};
	tm_time.tm_year = year - 1900;
	tm_time.tm_mon = month - 1;
	tm_time.tm_mday = day;
	tm_time.tm_hour = hour;
	tm_time.tm_min = minute;
	tm_time.tm_sec = second;
	return timegm_compat(&tm_time);
}

static std::string AlarmTypeToString(AlarmType t) {
	switch (t) {
		case AlarmType::OneShot: return "once";
		case AlarmType::Daily: return "daily";
		case AlarmType::Weekly: return "weekly";
		case AlarmType::Monthly: return "monthly";
		case AlarmType::Interval: return "interval";
	}
	return "unknown";
}

static AlarmType ParseAlarmType(const std::string& s) {
	if (s == "once") return AlarmType::OneShot;
	if (s == "daily") return AlarmType::Daily;
	if (s == "weekly") return AlarmType::Weekly;
	if (s == "monthly") return AlarmType::Monthly;
	if (s == "interval") return AlarmType::Interval;
	return AlarmType::OneShot;
}

// =============== 单例 ===============
AlarmManager& AlarmManager::GetInstance() {
	static AlarmManager instance;
	return instance;
}

AlarmManager::~AlarmManager() {
	if (timer_) {
		esp_timer_stop(timer_);
		esp_timer_delete(timer_);
		timer_ = nullptr;
	}
}

void AlarmManager::Initialize() {
	std::lock_guard<std::mutex> lk(mutex_);
	LoadFromSettings();
	RecalculateAllNextTriggers();
	ScheduleTimer();
    AddMcpTools();
}

// 读取 JSON 存储: key="list"  (使用 Settings namespace "alarm")
void AlarmManager::LoadFromSettings() {
	Settings settings("alarm");
	std::string json = settings.GetString("list", "");
	if (json.empty()) return;
	cJSON* root = cJSON_Parse(json.c_str());
	if (!root || !cJSON_IsArray(root)) {
		cJSON_Delete(root);
		return;
	}
	ESP_LOGI(TAG, "Loaded alarms:%s", json.c_str());
	alarms_.clear();
	int count = cJSON_GetArraySize(root);
	for (int i = 0; i < count; ++i) {
		cJSON* item = cJSON_GetArrayItem(root, i);
		if (!cJSON_IsObject(item)) continue;
		AlarmItem alarm;
		alarm.id = cJSON_GetObjectItem(item, "id")->valueint;
		alarm.enabled = cJSON_IsTrue(cJSON_GetObjectItem(item, "enabled"));
		alarm.type = ParseAlarmType(cJSON_GetObjectItem(item, "type")->valuestring);
		alarm.year = cJSON_GetObjectItem(item, "year")->valueint;
		alarm.month = cJSON_GetObjectItem(item, "month")->valueint;
		alarm.day = cJSON_GetObjectItem(item, "day")->valueint;
		alarm.hour = cJSON_GetObjectItem(item, "hour")->valueint;
		alarm.minute = cJSON_GetObjectItem(item, "minute")->valueint;
		cJSON* sec = cJSON_GetObjectItem(item, "second");
		if (cJSON_IsNumber(sec)) alarm.second = sec->valueint; else alarm.second = 0;
		cJSON* wd = cJSON_GetObjectItem(item, "weekdays");
		if (cJSON_IsNumber(wd)) alarm.weekdays_mask = (uint16_t)wd->valueint;
		cJSON* interval = cJSON_GetObjectItem(item, "interval");
		if (cJSON_IsNumber(interval)) alarm.interval_seconds = interval->valueint;
		cJSON* label = cJSON_GetObjectItem(item, "label");
		if (cJSON_IsString(label)) alarm.label = label->valuestring;
		alarms_.push_back(alarm);
		next_id_ = std::max(next_id_, alarm.id + 1);
	}
	cJSON_Delete(root);
}

void AlarmManager::SaveToSettings() {
	cJSON* root = cJSON_CreateArray();
	for (auto& alarm : alarms_) {
        if(alarm.enabled==false)
            continue;
        cJSON* item = cJSON_CreateObject();
		cJSON_AddNumberToObject(item, "id", alarm.id);
		cJSON_AddBoolToObject(item, "enabled", alarm.enabled);
		cJSON_AddStringToObject(item, "type", AlarmTypeToString(alarm.type).c_str());
		cJSON_AddNumberToObject(item, "year", alarm.year);
		cJSON_AddNumberToObject(item, "month", alarm.month);
		cJSON_AddNumberToObject(item, "day", alarm.day);
		cJSON_AddNumberToObject(item, "hour", alarm.hour);
		cJSON_AddNumberToObject(item, "minute", alarm.minute);
		cJSON_AddNumberToObject(item, "second", alarm.second);
		cJSON_AddNumberToObject(item, "weekdays", alarm.weekdays_mask);
		if (alarm.type == AlarmType::Interval) {
			cJSON_AddNumberToObject(item, "interval", alarm.interval_seconds);
		}
		cJSON_AddStringToObject(item, "label", alarm.label.c_str());
		cJSON_AddItemToArray(root, item);
	}
	char* str = cJSON_PrintUnformatted(root);
	Settings settings("alarm", true);
	settings.SetString("list", str ? str : "");
	if (str) cJSON_free(str);
	cJSON_Delete(root);
}

// 计算下一次触发时间
void AlarmManager::RecalculateNextTrigger(AlarmItem& item, time_t now) {
	if (!item.enabled) { item.next_trigger = 0; return; }
	struct tm* tm_now = gmtime(&now);
	if (!tm_now) { item.next_trigger = 0; return; }
	if (item.type == AlarmType::OneShot) {
		time_t t = MakeTimeUTC(item.year, item.month, item.day, item.hour, item.minute, item.second);
		if (t <= now) {
			// 已经过期，禁用
			item.enabled = false;
			item.next_trigger = 0;
		} else {
			item.next_trigger = t;
		}
	} else if (item.type == AlarmType::Daily) {
		struct tm tm_target = *tm_now;
		tm_target.tm_hour = item.hour;
		tm_target.tm_min = item.minute;
		tm_target.tm_sec = item.second;
		time_t candidate = timegm_compat(&tm_target);
		if (candidate <= now) candidate += 24 * 3600;
		item.next_trigger = candidate;
	} else if (item.type == AlarmType::Weekly) {
		// tm_wday: 0=Sun ... 6=Sat  我们规定 weekdays_mask bit0=Mon ... bit6=Sun
		// 先将当前时间移动到今天目标 hour:minute, 然后循环找下一天匹配
		for (int offset = 0; offset < 14; ++offset) { // 最多找两周
			time_t candidate = now + offset * 24 * 3600;
			struct tm* tm_cand = gmtime(&candidate);
			int weekday = tm_cand->tm_wday; // 0=Sun
			int maskIndex = (weekday == 0) ? 6 : (weekday - 1); // 转成 Mon=0 ... Sun=6
			if (item.weekdays_mask & (1 << maskIndex)) {
				struct tm tm_target = *tm_cand;
				tm_target.tm_hour = item.hour;
				tm_target.tm_min = item.minute;
				tm_target.tm_sec = item.second;
				time_t t = timegm_compat(&tm_target);
				if (t > now) { item.next_trigger = t; return; }
			}
		}
		// 没找到（mask 为 0）
		item.next_trigger = 0;
	} else if (item.type == AlarmType::Monthly) {
		// 本月指定日
		int year = tm_now->tm_year + 1900;
		int month = tm_now->tm_mon + 1;
		for (int i = 0; i < 24; ++i) { // 最多找两年
			int y = year + (month - 1) / 12;
			int m = (month - 1) % 12 + 1;
			// 简单合法性：day 1-31，若该月没有该日（28/29/30问题），则跳到下个月
			int day = item.day;
			if (day < 1) day = 1;
			// 粗略判定每月天数
			int mdays = 31;
			if (m==4||m==6||m==9||m==11) mdays = 30;
			else if (m==2) {
				bool leap = ( (y%4==0 && y%100!=0) || (y%400==0) );
				mdays = leap ? 29 : 28;
			}
			if (day > mdays) { month++; continue; }
			time_t t = MakeTimeUTC(y, m, day, item.hour, item.minute, item.second);
			if (t > now) { item.next_trigger = t; return; }
			month++;
		}
		item.next_trigger = 0;
	} else if (item.type == AlarmType::Interval) {
		// 如果首次（next_trigger==0）或已过期，则从当前时间开始加 interval
		int interval = item.interval_seconds;
		if (interval <= 0) { interval = 60; } // 默认 60 秒
		if (item.next_trigger == 0 || item.next_trigger <= now) {
			item.next_trigger = now + interval;
		}
	}
}

void AlarmManager::RecalculateAllNextTriggers() {
	time_t now = time(nullptr);
	for (auto& a : alarms_) {
		RecalculateNextTrigger(a, now);
	}
}

void AlarmManager::ScheduleTimer() {
	if (timer_) {
		esp_timer_stop(timer_);
	}
	time_t soonest = 0;
	AlarmItem* target = nullptr;
	for (auto& a : alarms_) {
		if (!a.enabled || a.next_trigger == 0) continue;
		if (soonest == 0 || a.next_trigger < soonest) {
			soonest = a.next_trigger;
			target = &a;
		}
	}
	if (!target) {
		ESP_LOGI(TAG, "No active alarms to schedule");
		return;
	}
	int64_t now_us = (int64_t)time(nullptr) * 1000000LL;
	int64_t alarm_us = (int64_t)soonest * 1000000LL;
	int64_t delay = alarm_us - now_us;
	if (delay < 1000) delay = 1000; // >=1ms
	if (!timer_) {
		esp_timer_create_args_t args = {};
		args.callback = [](void* arg){
			AlarmManager::GetInstance().OnTimerFired();
		};
		args.dispatch_method = ESP_TIMER_TASK;
		args.name = "alarm";
		ESP_ERROR_CHECK(esp_timer_create(&args, &timer_));
	}
	ESP_ERROR_CHECK(esp_timer_start_once(timer_, delay+1000000));//额外加多一秒,防止时间误差
    delay = delay / 1000000;
    ESP_LOGI(TAG, "Scheduled alarm id=%d label=%s after %dh %dm %ds", target->id,  target->label.c_str(),(long)(delay/3600), (long)(delay/60%60), (long)(delay%60));

	time_t now = time(nullptr);
	struct tm* tm_now = gmtime(&now);
	ESP_LOGI(TAG, "Now:%02d:%02d:%02d", tm_now->tm_hour, tm_now->tm_min, tm_now->tm_sec);
}

void AlarmManager::OnTimerFired() {
	std::lock_guard<std::mutex> lk(mutex_);
	time_t now = time(nullptr);
	struct tm* tm_now = gmtime(&now);
	ESP_LOGI(TAG, "Now:%02d:%02d:%02d", tm_now->tm_hour, tm_now->tm_min, tm_now->tm_sec);
	ESP_LOGI(TAG, "OnTimerFired at %ld", (long)now);
	for (auto& a : alarms_) {
		ESP_LOGI(TAG, "Alarm id=%d next_trigger=%ld", a.id,(long)a.next_trigger);
		if (a.enabled && a.next_trigger && a.next_trigger <= now) {
			ESP_LOGI(TAG, "Alarm fired id=%d label=%s", a.id, a.label.c_str());
			// 1. 播放提示音
			Application::GetInstance().PlaySound(Lang::Sounds::OGG_POPUP);
			// 2. 生成文本（含时间 + 标签）
			char buffer[160];
			tm_now = gmtime(&now);
			if (tm_now) {
				snprintf(buffer, sizeof(buffer), "%02d:%02d:%02d %s", tm_now->tm_hour, tm_now->tm_min, tm_now->tm_sec, a.label.c_str());
			} else {
				snprintf(buffer, sizeof(buffer), "%s", a.label.c_str());
			}
			std::string text(buffer);
			// 3. 显示到对话区（assistant）
			Application::GetInstance().Schedule([text]() {
				auto display = Board::GetInstance().GetDisplay();
				if (display) display->SetChatMessage("assistant", text.c_str());
			});
			// 4. 伪装最终识别文本触发服务器回答与 TTS
			Application::GetInstance().RequestTts("到"+a.label+"的时间了,再大声提醒我一次,并询问我的完成情况");

			// 计算下一次
			if (a.type == AlarmType::OneShot) {
				a.enabled = false;
				a.next_trigger = 0;
			} else if (a.type == AlarmType::Interval) {
				int interval = a.interval_seconds > 0 ? a.interval_seconds : 60;
				a.next_trigger = now + interval;
			} else {
				RecalculateNextTrigger(a, now + 1); // 避免重复
			}
		}
	}
	SaveToSettings();
	ScheduleTimer();
}

int AlarmManager::AddAlarm(const AlarmItem& tpl) {
	std::lock_guard<std::mutex> lk(mutex_);
	AlarmItem item = tpl;
	item.id = next_id_++;
	time_t now = time(nullptr);
	RecalculateNextTrigger(item, now);
	alarms_.push_back(item);
	SaveToSettings();
	ScheduleTimer();
	return item.id;
}

bool AlarmManager::RemoveAlarm(int id) {
	std::lock_guard<std::mutex> lk(mutex_);
	auto it = std::remove_if(alarms_.begin(), alarms_.end(), [id](const AlarmItem& a){ return a.id == id; });
	if (it == alarms_.end()) return false;
	alarms_.erase(it, alarms_.end());
	SaveToSettings();
	ScheduleTimer();
	return true;
}

bool AlarmManager::EnableAlarm(int id, bool enable) {
	std::lock_guard<std::mutex> lk(mutex_);
	for (auto& a : alarms_) {
		if (a.id == id) {
			a.enabled = enable;
			if (enable) RecalculateNextTrigger(a, time(nullptr));
			else a.next_trigger = 0;
			SaveToSettings();
			ScheduleTimer();
			return true;
		}
	}
	return false;
}

void AlarmManager::ClearAlarms() {
	std::lock_guard<std::mutex> lk(mutex_);
	alarms_.clear();
	SaveToSettings();
	ScheduleTimer();
}

std::string AlarmManager::ListAlarmsJson() {
	std::lock_guard<std::mutex> lk(mutex_);
	cJSON* root = cJSON_CreateArray();
	for (auto& a : alarms_) {
		cJSON* item = cJSON_CreateObject();
		cJSON_AddNumberToObject(item, "id", a.id);
		cJSON_AddBoolToObject(item, "enabled", a.enabled);
		cJSON_AddStringToObject(item, "type", AlarmTypeToString(a.type).c_str());
		cJSON_AddNumberToObject(item, "hour", a.hour);
		cJSON_AddNumberToObject(item, "minute", a.minute);
		cJSON_AddNumberToObject(item, "second", a.second);
		cJSON_AddNumberToObject(item, "day", a.day);
		cJSON_AddNumberToObject(item, "month", a.month);
		cJSON_AddNumberToObject(item, "year", a.year);
		cJSON_AddNumberToObject(item, "weekdays", a.weekdays_mask);
		cJSON_AddNumberToObject(item, "next", (double)a.next_trigger);
		if (a.type == AlarmType::Interval) {
			cJSON_AddNumberToObject(item, "interval", a.interval_seconds);
		}
		cJSON_AddStringToObject(item, "label", a.label.c_str());
		cJSON_AddItemToArray(root, item);
	}
	char* str = cJSON_PrintUnformatted(root);
	std::string out = str ? str : "[]";
	if (str) cJSON_free(str);
	cJSON_Delete(root);
	return out;
}

std::string AlarmManager::NextAlarmJson() {
	std::lock_guard<std::mutex> lk(mutex_);
	time_t soonest = 0; AlarmItem* target = nullptr;
	for (auto& a : alarms_) {
		if (!a.enabled || a.next_trigger == 0) continue;
		if (soonest == 0 || a.next_trigger < soonest) { soonest = a.next_trigger; target = &a; }
	}
	if (!target) return "{}";
	cJSON* item = cJSON_CreateObject();
	cJSON_AddNumberToObject(item, "id", target->id);
	cJSON_AddStringToObject(item, "type", AlarmTypeToString(target->type).c_str());
	cJSON_AddNumberToObject(item, "hour", target->hour);
	cJSON_AddNumberToObject(item, "minute", target->minute);
	cJSON_AddNumberToObject(item, "second", target->second);
	cJSON_AddStringToObject(item, "label", target->label.c_str());
	cJSON_AddNumberToObject(item, "time", (double)target->next_trigger);
	if (target->type == AlarmType::Interval) {
		cJSON_AddNumberToObject(item, "interval", target->interval_seconds);
	}
	char* str = cJSON_PrintUnformatted(item);
	std::string out = str ? str : "{}";
	if (str) cJSON_free(str);
	cJSON_Delete(item);
	return out;
}

void AlarmManager::AddMcpTools() {
	auto& mcp = McpServer::GetInstance();
	// 添加闹钟：type once/daily/weekly/monthly; hour minute; (once需要 year,month,day) (monthly需要day) (weekly需要 weekdays_mask)
	mcp.AddTool("self.alarm.add", "Add an alarm.", PropertyList({
		Property("type", kPropertyTypeString),
		Property("hour", kPropertyTypeInteger, 0, 23),
		Property("minute", kPropertyTypeInteger, 0, 59),
		Property("second", kPropertyTypeInteger, 0, 59),
		Property("day", kPropertyTypeInteger, 1, 31),
		Property("month", kPropertyTypeInteger, 1, 12),
		Property("year", kPropertyTypeInteger, 2024, 2100),
		Property("weekdays", kPropertyTypeInteger, 0, 0x7F), // bit mask
		Property("interval", kPropertyTypeInteger, 1, 86400), // 1s - 24h
		Property("label", kPropertyTypeString)
	}), [this](const PropertyList& props)->ReturnValue {
		AlarmItem tpl;
		tpl.type = ParseAlarmType(props["type"].value<std::string>());
		tpl.hour = props["hour"].value<int>();
		tpl.minute = props["minute"].value<int>();
		tpl.second = props["second"].value<int>();
		tpl.day = props["day"].value<int>();
		tpl.month = props["month"].value<int>();
		tpl.year = props["year"].value<int>();
		tpl.weekdays_mask = (uint16_t)props["weekdays"].value<int>();
		tpl.interval_seconds = props["interval"].value<int>();
		tpl.label = props["label"].value<std::string>();
		int id = AddAlarm(tpl);
		ESP_LOGI(TAG, "Add alarm id=%d", id);
		return id;
	});

	mcp.AddTool("self.alarm.list", "List all alarms.", PropertyList(), [this](const PropertyList&)->ReturnValue {
		return ListAlarmsJson();
	});

	mcp.AddTool("self.alarm.remove", "Remove an alarm by id.", PropertyList({
		Property("id", kPropertyTypeInteger, 0, 10000)
	}), [this](const PropertyList& props)->ReturnValue {
		int id = props["id"].value<int>();
		return RemoveAlarm(id);
	});

	mcp.AddTool("self.alarm.enable", "Enable or disable an alarm.", PropertyList({
		Property("id", kPropertyTypeInteger, 0, 10000),
		Property("enable", kPropertyTypeBoolean)
	}), [this](const PropertyList& props)->ReturnValue {
		int id = props["id"].value<int>();
		bool enable = props["enable"].value<bool>();
		return EnableAlarm(id, enable);
	});

	mcp.AddTool("self.alarm.next", "Get next alarm info.", PropertyList(), [this](const PropertyList&)->ReturnValue {
		return NextAlarmJson();
	});

	mcp.AddTool("self.alarm.clear", "Clear all alarms.", PropertyList(), [this](const PropertyList&)->ReturnValue {
		ClearAlarms();
		return true;
	});
}



