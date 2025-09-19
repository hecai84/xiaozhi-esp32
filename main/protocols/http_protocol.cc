// 简单 HTTP JSON POST 工具实现
#include "http_protocol.h"
#include "system_info.h"
#include <wifi_station.h>

#include <esp_http_client.h>
#include <esp_log.h>
#include <cJSON.h>
#include <string>

static const char* TAG_HTTP = "HTTP_HELPER";
static const char* kPostUrl = "http://ec2-16-162-3-210.ap-east-1.compute.amazonaws.com:5000/api/records";

// =========== 内部同步发送（带角色） ===========
static esp_err_t HttpPostMessageInternal(const char* role, const char* content) {
	if (content == nullptr || role == nullptr) return ESP_ERR_INVALID_ARG;
	cJSON* root = cJSON_CreateObject();
	if (!root) return ESP_ERR_NO_MEM;
	cJSON_AddStringToObject(root, "username", SystemInfo::GetMacAddress().c_str());
	cJSON_AddStringToObject(root, "role", role);
	cJSON_AddStringToObject(root, "content", content);
	char* json_unformatted = cJSON_PrintUnformatted(root);
	if (!json_unformatted) {
		cJSON_Delete(root);
		return ESP_ERR_NO_MEM;
	}
	std::string payload(json_unformatted);
	cJSON_free(json_unformatted);
	cJSON_Delete(root);

	esp_http_client_config_t config = {};
	config.url = kPostUrl;
	config.method = HTTP_METHOD_POST;
	config.timeout_ms = 5000;
	esp_http_client_handle_t client = esp_http_client_init(&config);
	if (!client) {
		ESP_LOGE(TAG_HTTP, "Failed to init http client");
		return ESP_FAIL;
	}
	esp_err_t err = esp_http_client_set_header(client, "Content-Type", "application/json");
	if (err != ESP_OK) { esp_http_client_cleanup(client); return err; }
	err = esp_http_client_open(client, payload.size());
	if (err != ESP_OK) { ESP_LOGE(TAG_HTTP, "open failed: %s", esp_err_to_name(err)); esp_http_client_cleanup(client); return err; }
	int wlen = esp_http_client_write(client, payload.data(), payload.size());
	if (wlen < 0 || (size_t)wlen != payload.size()) { ESP_LOGE(TAG_HTTP, "write failed"); esp_http_client_cleanup(client); return ESP_FAIL; }
	int status_code = esp_http_client_fetch_headers(client);
	if (status_code < 0) { ESP_LOGE(TAG_HTTP, "fetch headers failed: %d", status_code); esp_http_client_cleanup(client); return ESP_FAIL; }
	status_code = esp_http_client_get_status_code(client);
	if (status_code / 100 != 2) { ESP_LOGW(TAG_HTTP, "status %d", status_code); esp_http_client_cleanup(client); return ESP_FAIL; }
	char buffer[64];
	while (true) { int rl = esp_http_client_read(client, buffer, sizeof(buffer)); if (rl <= 0) break; }
	esp_http_client_cleanup(client);
	return ESP_OK;
}


// =========== 异步队列 ==========
#include <freertos/FreeRTOS.h>
#include <freertos/queue.h>
#include <freertos/task.h>

struct HttpChatItem { char* role; char* text; };
static QueueHandle_t s_http_chat_queue = nullptr;
static TaskHandle_t  s_http_chat_task = nullptr;
static constexpr int HTTP_CHAT_QUEUE_LEN = 30;
static constexpr uint32_t HTTP_CHAT_TASK_STACK = 4096;
static constexpr UBaseType_t HTTP_CHAT_TASK_PRIO = 1;

static void HttpChatTask(void*) {
	HttpChatItem item{};
	// 等待网络就绪的辅助函数（返回 true 表示可发送；false 表示超时放弃本次消息）
	auto wait_network_ready = []() -> bool {
		// 最长等待 60 秒；若 60 秒仍未连接则丢弃该条消息，避免永久阻塞队列
		static const TickType_t kTimeout = pdMS_TO_TICKS(60000);
		static const TickType_t kInterval = pdMS_TO_TICKS(500);
		TickType_t waited = 0;
		while (true) {
			// WifiStation 组件提供连接状态
			if (WifiStation::GetInstance().IsConnected()) {
				return true;
			}
			if (waited >= kTimeout) {
				ESP_LOGW(TAG_HTTP, "Network not ready after 60s, drop message");
				return false;
			}
			vTaskDelay(kInterval);
			waited += kInterval;
		}
	};
	for (;;) {
		if (xQueueReceive(s_http_chat_queue, &item, portMAX_DELAY) == pdTRUE) {
			if (item.text && item.role) {
				if (wait_network_ready()) {
					HttpPostMessageInternal(item.role, item.text); // 忽略结果
				} else {
					ESP_LOGW(TAG_HTTP, "Drop chat message due to network not ready: role=%s len=%zu", item.role, strlen(item.text));
				}
			}
			if (item.text) free(item.text);
			if (item.role) free(item.role);
		}
	}
}

static void EnsureHttpChatTask() {
	if (!s_http_chat_queue) {
		s_http_chat_queue = xQueueCreate(HTTP_CHAT_QUEUE_LEN, sizeof(HttpChatItem));
	}
	if (!s_http_chat_task && s_http_chat_queue) {
		xTaskCreate(HttpChatTask, "http_chat_task", HTTP_CHAT_TASK_STACK, nullptr, HTTP_CHAT_TASK_PRIO, &s_http_chat_task);
	}
}

void HttpEnqueueChatMessage(const char* role, const char* content) {
	if (!content || !*content) return;
	if (!role || !*role) role = "user"; // 默认 user
	EnsureHttpChatTask();
	if (!s_http_chat_queue) return;
	size_t len_c = strlen(content);
	size_t len_r = strlen(role);
	char* text_copy = (char*)malloc(len_c + 1);
	char* role_copy = (char*)malloc(len_r + 1);
	if (!text_copy || !role_copy) { if (text_copy) free(text_copy); if (role_copy) free(role_copy); return; }
	memcpy(text_copy, content, len_c + 1);
	memcpy(role_copy, role, len_r + 1);
	HttpChatItem item{ role_copy, text_copy };
	if (xQueueSend(s_http_chat_queue, &item, 0) != pdTRUE) {
		free(text_copy); free(role_copy); // 队列满
	}
}

