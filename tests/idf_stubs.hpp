#pragma once
// Host fakes are restricted to transport, GPIO, clock and NVS boundaries.
// Tests include the production cabinet protocol/receipt/actuation functions.
#include <stdint.h>
#include <stddef.h>
#include <cstring>
#include <map>
#include <string>
#include <vector>
#include <deque>
#include <ctime>
using esp_err_t = int;
using httpd_handle_t = void *;
using esp_mqtt_client_handle_t = void *;
using nvs_handle_t = int;
using SemaphoreHandle_t = void *;
using QueueHandle_t = void *;
using gpio_num_t = int;
constexpr int ESP_OK = 0, ESP_FAIL = -1, ESP_ERR_NVS_NOT_FOUND = 1;
constexpr int pdTRUE = 1, portMAX_DELAY = 0, LEDC_LOW_SPEED_MODE = 0, LEDC_CHANNEL_1 = 1;
inline void xSemaphoreTake(void *, int) {}
inline void xSemaphoreGive(void *) {}
struct FakeQueue { size_t capacity, item_size; std::deque<std::vector<unsigned char>> data; };
inline void *xQueueCreate(size_t capacity, size_t item_size) { return new FakeQueue{capacity,item_size,{}}; }
inline int xQueueSend(void *handle, const void *item, int) {
    auto q = static_cast<FakeQueue *>(handle);
    if (!q || q->data.size() >= q->capacity) return 0;
    auto data = static_cast<const unsigned char *>(item);
    q->data.emplace_back(data, data + q->item_size); return pdTRUE;
}
inline int xQueueReceive(void *handle, void *item, int) {
    auto q = static_cast<FakeQueue *>(handle); if (!q || q->data.empty()) return 0;
    memcpy(item, q->data.front().data(), q->item_size); q->data.pop_front(); return pdTRUE;
}
inline void vQueueDelete(void *handle) { delete static_cast<FakeQueue *>(handle); }
inline int64_t &fake_utc() { static int64_t v = std::time(nullptr); return v; }
inline int64_t &fake_ms() { static int64_t v = 0; return v; }
inline int64_t esp_timer_get_time() { return fake_ms() * 1000; }
inline std::map<int,int> &pins() { static std::map<int,int> v; return v; }
inline int &actuations() { static int v = 0; return v; }
inline int gpio_set_level(int pin, int value) { pins()[pin] = value; if (pin == 1 && value) ++actuations(); return 0; }
inline int gpio_get_level(int pin) { return pins()[pin]; }
inline int &beeps() { static int v = 0; return v; }
inline int ledc_set_duty(int, int, int) { ++beeps(); return 0; }
inline int ledc_update_duty(int, int) { return 0; }
inline int ledc_stop(int, int, int) { return 0; }
inline void fake_log(const char *, const char *, ...) {}
#define ESP_LOGE(...) fake_log(__VA_ARGS__)
#define ESP_LOGW(...) fake_log(__VA_ARGS__)
#define ESP_LOGI(...) fake_log(__VA_ARGS__)
using esp_event_base_t = const char *;
enum { MQTT_EVENT_CONNECTED, MQTT_EVENT_DISCONNECTED, MQTT_EVENT_SUBSCRIBED, MQTT_EVENT_ERROR, MQTT_EVENT_DATA, MQTT_EVENT_PUBLISHED, MQTT_ERROR_TYPE_SUBSCRIBE_FAILED };
struct esp_mqtt_error_codes_t {
    int error_type=0, esp_tls_last_esp_err=0, esp_tls_stack_err=0, esp_tls_cert_verify_flags=0;
    int esp_transport_sock_errno=0, connect_return_code=0;
};
struct esp_mqtt_event_t {
    int session_present=0, msg_id=0, current_data_offset=0, total_data_len=0, topic_len=0, data_len=0;
    char *topic=nullptr, *data=nullptr;
    esp_mqtt_error_codes_t *error_handle=nullptr;
};
using esp_mqtt_event_handle_t = esp_mqtt_event_t *;
inline std::map<std::string,std::string> &persisted() { static std::map<std::string,std::string> v; return v; }
inline std::map<std::string,std::string> &staged() { static std::map<std::string,std::string> v; return v; }
inline bool &fail_write() { static bool v = false; return v; }
inline int nvs_get_str(int, const char *key, char *out, size_t *size) {
    auto v = persisted().find(key); if (v == persisted().end()) return ESP_ERR_NVS_NOT_FOUND;
    if (!out) { *size = v->second.size() + 1; return 0; }
    if (*size < v->second.size() + 1) return ESP_FAIL;
    std::memcpy(out, v->second.c_str(), v->second.size() + 1); *size = v->second.size() + 1; return 0;
}
inline int nvs_set_str(int, const char *key, const char *value) {
    if (fail_write()) return ESP_FAIL;
    staged()[key] = value; return 0;
}
inline int nvs_commit(int) {
    if (fail_write()) return ESP_FAIL;
    for (auto &v : staged()) persisted()[v.first] = v.second;
    staged().clear(); return 0;
}
inline std::vector<std::string> &sent() { static std::vector<std::string> v; return v; }
inline bool &offline() { static bool v = false; return v; }
inline int esp_mqtt_client_enqueue(void *, const char *, const char *body, int, int, bool, bool) {
    if (offline()) return -1;
    sent().push_back(body); return sent().size();
}
using esp_qrcode_handle_t = const uint8_t *;
struct esp_qrcode_config_t { void (*display_func)(esp_qrcode_handle_t); int max_qrcode_version; };
#define ESP_QRCODE_CONFIG_DEFAULT() esp_qrcode_config_t{nullptr,7}
inline int esp_qrcode_get_size(const uint8_t *) { return 37; }
inline bool esp_qrcode_get_module(const uint8_t *, int x, int y) { return (x+y)%2; }
inline int esp_qrcode_generate(esp_qrcode_config_t *cfg, const char *) { cfg->display_func(nullptr); return ESP_OK; }
