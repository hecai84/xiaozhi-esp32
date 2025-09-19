/**
 * @file http_protocol.h
 * @brief Simple helper for posting a JSON message to a fixed HTTP endpoint.
 */
#ifndef HTTP_PROTOCOL_H_
#define HTTP_PROTOCOL_H_

#include <esp_err.h>


/**
 * @brief 异步入队聊天消息，后台任务逐条发送 (失败忽略)。
 * 若任务/队列尚未创建则自动创建。队列满时丢弃。
 * @param role  角色字符串，可为 user/assistant/system 等，将写入 JSON 的 role 字段。
 * @param content 内容文本。
 */
void HttpEnqueueChatMessage(const char* role, const char* content);

#endif // HTTP_PROTOCOL_H_
