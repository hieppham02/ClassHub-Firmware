#include "classhub.h"
#include "qr_rules.hpp"
#include "ota_gate.hpp"
#include "wifi_fallback.hpp"
#if __has_include("classhub_defaults.h")
#include "classhub_defaults.h"
#else
#define CLASSHUB_DEFAULT_HOST ""
#define CLASSHUB_DEFAULT_USERNAME ""
#define CLASSHUB_DEFAULT_PASSWORD ""
#define CLASSHUB_DEFAULT_ROOMID "DTD201"
#endif
#include <algorithm>
#include <atomic>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <memory>
#include <string>
#include <time.h>
#include "cJSON.h"
#ifndef CLASSHUB_HOST_TEST
#include "rc522_reader.h"
#include "esp_ota_ops.h"
#include "esp_app_desc.h"
#include "esp_app_format.h"
#include "driver/gpio.h"
#include "driver/ledc.h"
#include "esp_crt_bundle.h"
#include "esp_event.h"
#include "esp_log.h"
#include "esp_mac.h"
#include "esp_netif.h"
#include "esp_netif_sntp.h"
#include "esp_sntp.h"
#include "esp_timer.h"
#include "esp_system.h"
#include "esp_attr.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "mqtt_client.h"
#include "nvs.h"
#include "nvs_flash.h"
#include "qrcode.h"
#endif

namespace {
const char *TAG = "CLASSHUB";
enum OpenStage { OPEN_IDLE, OPEN_RELAY, OPEN_CLEAR_QR, OPEN_LCD,
                 BUZZER_DUTY, BUZZER_UPDATE, BUZZER_DONE, OPEN_STATUS, OPEN_DONE };
#ifndef CLASSHUB_HOST_TEST
// Numeric breadcrumbs survive a watchdog reset without writing flash.
RTC_NOINIT_ATTR volatile uint32_t open_trace_magic;
RTC_NOINIT_ATTR volatile uint32_t open_trace_stage;
#else
volatile uint32_t open_trace_magic = 0, open_trace_stage = 0;
#endif
const uint32_t OPEN_TRACE_MAGIC = 0x43484F50;
const char *stage_name(uint32_t stage) {
    switch (stage) {
    case OPEN_IDLE: return "IDLE";
    case OPEN_RELAY: return "RELAY_GPIO1";
    case OPEN_CLEAR_QR: return "CLEAR_QR";
    case OPEN_LCD: return "LCD_STATE";
    case BUZZER_DUTY: return "BUZZER_SET_DUTY";
    case BUZZER_UPDATE: return "BUZZER_UPDATE";
    case BUZZER_DONE: return "BUZZER_DONE";
    case OPEN_STATUS: return "MQTT_STATUS";
    case OPEN_DONE: return "OPEN_DONE";
    default: return "UNKNOWN";
    }
}
void trace_open(OpenStage stage) {
    open_trace_stage = stage;
    open_trace_magic = OPEN_TRACE_MAGIC;
    ESP_LOGI(TAG, "ACTUATOR_STAGE %s", stage_name(stage));
}
using Json = std::unique_ptr<cJSON, decltype(&cJSON_Delete)>;
Json parse(const std::string &s) {
    // Match the old ArduinoJson nesting bound before entering cJSON's recursive
    // parser, so malformed network JSON cannot exhaust the cabinet task stack.
    bool quoted = false, escaped = false;
    int depth = 0;
    for (unsigned char c : s) {
        if (quoted) {
            if (escaped) escaped = false;
            else if (c == '\\') escaped = true;
            else if (c == '"') quoted = false;
        } else if (c == '"') quoted = true;
        else if (c == '{' || c == '[') { if (++depth > 10) return Json(nullptr, cJSON_Delete); }
        else if (c == '}' || c == ']') { if (--depth < 0) return Json(nullptr, cJSON_Delete); }
    }
    return Json(cJSON_ParseWithLengthOpts(s.c_str(), s.size() + 1, nullptr, true), cJSON_Delete);
}
Json object() { return Json(cJSON_CreateObject(), cJSON_Delete); }
const cJSON *item(const cJSON *j, const char *k) { return cJSON_GetObjectItemCaseSensitive(j, k); }
std::string str(const cJSON *j, const char *k) {
    const cJSON *v = item(j, k);
    return cJSON_IsString(v) && v->valuestring ? v->valuestring : "";
}
std::string id(const cJSON *j, const char *k) {
    const cJSON *v = item(j, k);
    if (cJSON_IsString(v)) return v->valuestring;
    if (cJSON_IsNumber(v) && v->valuedouble > 0 && v->valuedouble <= INT32_MAX &&
        std::floor(v->valuedouble) == v->valuedouble) return std::to_string(v->valueint);
    return "";
}
int num(const cJSON *j, const char *k, int fallback = 0) {
    const cJSON *v = item(j, k);
    return cJSON_IsNumber(v) ? v->valueint : fallback;
}
bool flag(const cJSON *j, const char *k) { return cJSON_IsTrue(item(j, k)); }
std::string dump(const cJSON *j) {
    char *raw = cJSON_PrintUnformatted(j);
    std::string s = raw ? raw : "";
    cJSON_free(raw);
    return s;
}
void put(cJSON *j, const char *k, const std::string &v) { cJSON_AddStringToObject(j, k, v.c_str()); }
int64_t ms() { return esp_timer_get_time() / 1000; }
int64_t utc_now() {
#ifdef CLASSHUB_HOST_TEST
    return fake_utc();
#else
    return time(nullptr);
#endif
}
constexpr int CLOCK_SKEW_SECONDS = 5;
bool fresh_message(int64_t expiry, int lifetime) {
    return QrRules::fresh(utc_now(), expiry, lifetime, CLOCK_SKEW_SECONDS);
}

struct SubscriptionState {
    int action_id = -1, qr_id = -1;
    unsigned acknowledged = 0;
    bool failed = false;
    void begin(int action, int qr) { action_id = action; qr_id = qr; acknowledged = 0; failed = false; }
    void accept(int id, bool success) {
        unsigned bit = id == action_id && action_id >= 0 ? 1 : (id == qr_id && qr_id >= 0 ? 2 : 0);
        if (!bit) return;
        if (!success) failed = true;
        else acknowledged |= bit;
    }
    bool ready() const { return action_id >= 0 && qr_id >= 0 && acknowledged == 3 && !failed; }
};
struct SubscriptionAck { uint32_t epoch; int id; bool success; };

struct Settings {
    std::string ssid, wifi_password;
    std::string host = CLASSHUB_DEFAULT_HOST, username = CLASSHUB_DEFAULT_USERNAME;
    std::string password = CLASSHUB_DEFAULT_PASSWORD, room = CLASSHUB_DEFAULT_ROOMID;
    int port = 8883;
};
Settings config;
bool ap_credentials(const std::string &cabinet_id, std::string &ssid, std::string &password) {
    // The suffix is 5 characters; WPA2 needs at least 8 in the full password.
    if (cabinet_id.size() < 3 || cabinet_id.size() > 20 ||
        cabinet_id.find_first_not_of("abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789-_") != std::string::npos) return false;
    ssid = cabinet_id;
    password = cabinet_id + "123@@";
    return true;
}
struct Receipt {
    std::string room, session, command, purpose, expected;
    QrRules::Phase phase = QrRules::ACCEPTED;
    uint32_t sequence = 0;
    bool verified = false, rfid = false, camera = false;
};
Receipt active, queued_replay;
bool replay_queued = false, results_dirty = false, storage_fault = false;
nvs_handle_t ledger = 0;
bool unlocked = false, pulse_active = false;
bool door_open_seen = false;
std::string current_session;
int64_t pulse_begin = 0, buzzer_until = 0;
std::string display_session, display_purpose;
int64_t display_expiry = 0;
bool scan_notified = false;
std::atomic<bool> connected{false}, ip_ready{false}, setup_allowed{false};
std::atomic<int64_t> station_retry_at{0};
WifiFallback wifi_fallback;
std::atomic<bool> ap_active{false};
std::atomic<bool> mqtt_ready{false};
OtaGate ota_gate;
std::atomic<bool> ota_http_ready{false};
std::atomic<int64_t> ota_restart_at{0};
std::atomic<uint32_t> connection_epoch{0};
uint32_t replay_epoch = 0;
esp_mqtt_client_handle_t mqtt = nullptr;
SemaphoreHandle_t display_mutex;
SemaphoreHandle_t config_mutex;
classhub_display_t display = {};

enum MessageKind { ACTION, QR, CONFIGURE, VERIFICATION, CONSOLE };
struct Message { MessageKind kind; char *body; uint32_t epoch; };
QueueHandle_t messages;
QueueHandle_t subscription_acks;
bool enqueue(MessageKind kind, const std::string &body) {
    if (ota_gate.blocked()) return false;
    char *copy = static_cast<char *>(malloc(body.size() + 1));
    if (!copy) return false;
    memcpy(copy, body.c_str(), body.size() + 1);
    Message m{kind, copy, connection_epoch.load()};
    if (xQueueSend(messages, &m, 0) == pdTRUE) return true;
    free(copy);
    return false;
}

// All cabinet state, NVS, MQTT requests and UI changes belong to cabinet_task.
void ui(const char *title, const char *detail = "") {
    xSemaphoreTake(display_mutex, portMAX_DELAY);
    snprintf(display.title, sizeof(display.title), "%s", title);
    snprintf(display.detail, sizeof(display.detail), "%s", detail);
    display.online = mqtt_ready.load();
    ++display.generation;
    xSemaphoreGive(display_mutex);
}
void clear_qr() {
    display_session.clear(); display_purpose.clear(); display_expiry = 0; scan_notified = false;
    xSemaphoreTake(display_mutex, portMAX_DELAY);
    display.qr_size = 0; ++display.generation;
    xSemaphoreGive(display_mutex);
}
void qr_pixels(esp_qrcode_handle_t code) {
    int size = esp_qrcode_get_size(code);
    xSemaphoreTake(display_mutex, portMAX_DELAY);
    display.qr_size = size <= CLASSHUB_QR_SIDE_MAX ? size : 0;
    for (int y = 0; y < display.qr_size; ++y)
        for (int x = 0; x < display.qr_size; ++x)
            display.qr[y * CLASSHUB_QR_SIDE_MAX + x] = esp_qrcode_get_module(code, x, y);
    ++display.generation;
    xSemaphoreGive(display_mutex);
}
bool busy() {
    return !active.command.empty() && QrRules::busy(active.phase,
        active.purpose == "RETURN" && CLASSHUB_REQUIRE_RETURN_VERIFICATION,
        active.verified, active.rfid, active.camera);
}
void fault() { storage_fault = true; ui("LOI LUU NVS", "QR BI KHOA"); ESP_LOGE(TAG, "NVS fault: QR actuation blocked; storage preserved"); }

enum class Read { missing, ok, error };
Read read_string(nvs_handle_t handle, const std::string &key, std::string &out) {
    size_t length = 0;
    esp_err_t e = nvs_get_str(handle, key.c_str(), nullptr, &length);
    if (e == ESP_ERR_NVS_NOT_FOUND) return Read::missing;
    if (e != ESP_OK || !length || length > 12288) return Read::error;
    std::string data(length, '\0');
    if (nvs_get_str(handle, key.c_str(), &data[0], &length) != ESP_OK) return Read::error;
    data.resize(length - 1); out = data;
    return Read::ok;
}
bool write_string(nvs_handle_t handle, const std::string &key, const std::string &value) {
    return !value.empty() && nvs_set_str(handle, key.c_str(), value.c_str()) == ESP_OK && nvs_commit(handle) == ESP_OK;
}
bool save(const Receipt &r) {
    auto j = object(); if (!j) return false;
    put(j.get(), "room", r.room); put(j.get(), "session", r.session); put(j.get(), "command", r.command);
    put(j.get(), "purpose", r.purpose); put(j.get(), "expected", r.expected);
    cJSON_AddNumberToObject(j.get(), "phase", r.phase); cJSON_AddNumberToObject(j.get(), "sequence", r.sequence);
    cJSON_AddBoolToObject(j.get(), "verified", r.verified); cJSON_AddBoolToObject(j.get(), "rfid", r.rfid);
    cJSON_AddBoolToObject(j.get(), "camera", r.camera);
    return ledger && write_string(ledger, r.command.substr(0, 15), dump(j.get()));
}
Read read_receipt(const std::string &command, Receipt &r) {
    std::string data;
    Read result = read_string(ledger, command.substr(0, 15), data);
    if (result != Read::ok) return result;
    auto j = parse(data); if (!j) return Read::error;
    r.room = str(j.get(), "room"); r.session = str(j.get(), "session"); r.command = str(j.get(), "command");
    r.purpose = str(j.get(), "purpose"); r.expected = str(j.get(), "expected");
    int phase = num(j.get(), "phase", -1);
    if (r.command != command || !QrRules::hex(command.c_str(), 32) ||
        !QrRules::session(r.session.c_str()) || !QrRules::purpose(r.purpose.c_str()) ||
        r.room.empty() || phase < 0 || phase > 3) return Read::error;
    r.phase = static_cast<QrRules::Phase>(phase);
    const cJSON *sequence = item(j.get(), "sequence");
    if (sequence && (!cJSON_IsNumber(sequence) || sequence->valuedouble < 0 ||
        sequence->valuedouble > UINT32_MAX || std::floor(sequence->valuedouble) != sequence->valuedouble)) return Read::error;
    r.sequence = sequence ? static_cast<uint32_t>(sequence->valuedouble) : 0;
    r.verified = flag(j.get(), "verified"); r.rfid = flag(j.get(), "rfid"); r.camera = flag(j.get(), "camera");
    return Read::ok;
}
std::string topic(const char *suffix, bool backend = false) {
    return std::string(backend ? "backend/cabinet/" : "iot/cabinet/") + config.room + "/" + suffix;
}
int publish(const std::string &channel, const std::string &body, bool retain = false) {
    if (!mqtt || !connected || body.empty()) return -1;
    return esp_mqtt_client_enqueue(mqtt, channel.c_str(), body.c_str(), body.size(), 1, retain, true);
}
void heartbeat(bool online = true) {
    auto j = object(); put(j.get(), "room", config.room); cJSON_AddBoolToObject(j.get(), "isOnline", online);
    publish(topic("heartbeat"), dump(j.get()), true);
}
void status() {
    auto j = object(); put(j.get(), "id", current_session); put(j.get(), "room", config.room);
    put(j.get(), "lockState", unlocked ? "UNLOCKED" : "LOCKED");
    cJSON_AddBoolToObject(j.get(), "isOpen", unlocked); cJSON_AddBoolToObject(j.get(), "isOnline", mqtt_ready.load());
    publish(topic("status"), dump(j.get()));
}
bool send_result(const Receipt &r, const char *event) {
    if (r.room != config.room) return false;
    auto j = object(); cJSON_AddNumberToObject(j.get(), "sessionId", atoi(r.session.c_str()));
    put(j.get(), "commandId", r.command); put(j.get(), "event", event);
    if (!strcmp(event, "VERIFIED")) {
        cJSON_AddBoolToObject(j.get(), "rfidPassed", r.rfid); cJSON_AddBoolToObject(j.get(), "cameraPassed", r.camera);
        cJSON_AddNumberToObject(j.get(), "sequence", r.sequence);
    }
    return publish(topic("qr-result"), dump(j.get())) >= 0;
}
bool replay(const Receipt &r) {
    if (r.phase == QrRules::FAILED) return send_result(r, "OPEN_FAILED");
    if (r.phase == QrRules::ACCEPTED || !send_result(r, "OPENED")) return false;
    if (r.phase == QrRules::CLOSED && !send_result(r, "CLOSED")) return false;
    return !CLASSHUB_REQUIRE_RETURN_VERIFICATION || !r.verified || send_result(r, "VERIFIED");
}
void beep(int duration) {
    // LEDC timer/channel 1 are separate from camera XCLK on timer/channel 0.
    trace_open(BUZZER_DUTY);
    ledc_set_duty(LEDC_LOW_SPEED_MODE, LEDC_CHANNEL_1, 512);
    trace_open(BUZZER_UPDATE);
    ledc_update_duty(LEDC_LOW_SPEED_MODE, LEDC_CHANNEL_1);
    buzzer_until = ms() + duration;
    trace_open(BUZZER_DONE);
}
void unlock(const std::string &session) {
    if (!session.empty()) current_session = session;
    unlocked = true; pulse_active = true; pulse_begin = ms();
    door_open_seen = false; // A held closed switch must first be released.
    trace_open(OPEN_RELAY);
    gpio_set_level(static_cast<gpio_num_t>(CLASSHUB_LATCH_PIN), 1);
    trace_open(OPEN_CLEAR_QR); clear_qr();
    trace_open(OPEN_LCD); ui("CUA DANG MO", config.room.c_str());
    beep(1000);
    trace_open(OPEN_STATUS); status();
    trace_open(OPEN_DONE);
}
void lock() {
    unlocked = false; pulse_active = false; door_open_seen = false;
    gpio_set_level(static_cast<gpio_num_t>(CLASSHUB_LATCH_PIN), 0);
    ui("DA KHOA", config.room.c_str()); status();
    if (active.room == config.room && active.phase == QrRules::OPENED && !active.command.empty()) {
        active.phase = QrRules::CLOSED;
        if (!save(active)) fault();
        results_dirty = true;
    }
}
void restore_ledger() {
    std::string command;
    Read r = read_string(ledger, "active", command);
    if (r == Read::error || (r == Read::ok && read_receipt(command, active) != Read::ok)) { fault(); return; }
    if (active.command.empty()) return;
    if (active.phase == QrRules::ACCEPTED) {
        active.phase = QrRules::FAILED;
        if (!save(active)) fault();
    }
    current_session = active.session;
    results_dirty = true;
}
int door_raw = 1, door_stable = 1;
int64_t door_changed_at = 0;
void tick_hardware(int64_t now) {
    if (pulse_active && now - pulse_begin >= 1500) {
        gpio_set_level(static_cast<gpio_num_t>(CLASSHUB_LATCH_PIN), 0); pulse_active = false;
    }
    if (buzzer_until && now >= buzzer_until) {
        ledc_stop(LEDC_LOW_SPEED_MODE, LEDC_CHANNEL_1, 0); buzzer_until = 0;
    }
    int value = gpio_get_level(static_cast<gpio_num_t>(CLASSHUB_CLOSE_PIN));
    if (value != door_raw) { door_raw = value; door_changed_at = now; }
    if (now - door_changed_at > 50) {
        door_stable = door_raw;
        bool waiting_for_close = unlocked || (active.room == config.room && active.phase == QrRules::OPENED);
        if (waiting_for_close) {
            // Pull-up switch: HIGH/released = open; LOW/held to GND = closed.
            // Confirm a complete open -> close cycle, not the initial held LOW.
            if (door_stable) door_open_seen = true;
            else if (door_open_seen) lock();
        }
    }
}
bool valid_expected(const cJSON *array) {
    if (!cJSON_IsArray(array) || cJSON_GetArraySize(array) > 64) return false;
    const cJSON *v;
    cJSON_ArrayForEach(v, array) {
        if (!cJSON_IsObject(v) || !cJSON_IsString(item(v, "name"))) return false;
        for (const char *field : {"equipmentId", "quantity"}) {
            auto n = item(v, field);
            if (!cJSON_IsNumber(n) || n->valuedouble <= 0 || n->valuedouble > INT32_MAX ||
                std::floor(n->valuedouble) != n->valuedouble) return false;
        }
    }
    return true;
}
bool replacement(const std::string &session, const std::string &purpose) {
    return active.phase == QrRules::CLOSED && active.purpose == "RETURN" && purpose == "RETURN" && active.session == session;
}
void show_qr(const cJSON *j, size_t length) {
    if (ota_gate.blocked()) { ESP_LOGW(TAG, "QR_REJECT OTA maintenance"); return; }
    if (length > 1024 || storage_fault) { ESP_LOGW(TAG, "QR_REJECT size_or_storage"); return; }
    auto session = id(j, "sessionId"), purpose = str(j, "purpose"), content = str(j, "content");
    int64_t expiry = QrRules::utc(str(j, "expiresAt").c_str());
    if (str(j, "room") != config.room || str(j, "action") != "display_qr" ||
        !QrRules::session(session.c_str()) || !QrRules::purpose(purpose.c_str()) ||
        !QrRules::content(content.c_str())) { ESP_LOGW(TAG, "QR_REJECT invalid_payload_or_room"); return; }
    if (!fresh_message(expiry, 180)) {
        ESP_LOGW(TAG, "QR_REJECT deadline now=%lld expiry=%lld remaining=%lld", (long long)utc_now(), (long long)expiry, (long long)(expiry-utc_now())); return;
    }
    if (busy() && !replacement(session, purpose)) { ESP_LOGW(TAG, "QR_REJECT cabinet_busy phase=%d", active.phase); return; }
    esp_qrcode_config_t cfg = ESP_QRCODE_CONFIG_DEFAULT();
    cfg.display_func = qr_pixels; cfg.max_qrcode_version = 7;
    if (esp_qrcode_generate(&cfg, content.c_str()) != ESP_OK) { ESP_LOGE(TAG, "QR_REJECT encoding_failed"); clear_qr(); return; }
    if (display_session != session || display_purpose != purpose || display_expiry != expiry) scan_notified = false;
    display_session = session; display_purpose = purpose; display_expiry = expiry;
    ESP_LOGI(TAG, "QR_DISPLAY session=%s purpose=%s remaining=%lld", session.c_str(), purpose.c_str(), (long long)(expiry-utc_now()));
}
void action(const cJSON *j) {
    if (ota_gate.blocked()) { ESP_LOGW(TAG, "ACTION_REJECT OTA maintenance"); return; }
    if (str(j, "room") != config.room) { ESP_LOGW(TAG, "ACTION_REJECT wrong_room"); return; }
    std::string verb = str(j, "action"), session = id(j, "sessionId"), purpose = str(j, "purpose");
    if (verb == "qr_scanned") {
        if (!display_session.empty() && session == display_session && purpose == display_purpose &&
            QrRules::utc(str(j, "expiresAt").c_str()) == display_expiry && utc_now() < display_expiry && !scan_notified) {
            scan_notified = true; beep(500);
        }
        return;
    }
    // Any QR-shaped message stays in this branch, including malformed messages.
    if (item(j, "commandId") || item(j, "purpose") || item(j, "sessionId") || item(j, "expectedEquipment")) {
        std::string command = str(j, "commandId");
        std::transform(command.begin(), command.end(), command.begin(), [](unsigned char c) { return std::tolower(c); });
        if (verb != "open" || !QrRules::hex(command.c_str(), 32) || !QrRules::session(session.c_str()) ||
            !QrRules::purpose(purpose.c_str()) || !valid_expected(item(j, "expectedEquipment")) || storage_fault) {
            ESP_LOGW(TAG, "OPEN_REJECT invalid_payload_or_storage"); return;
        }
        Receipt existing;
        Read result = read_receipt(command, existing);
        if (result == Read::error) { fault(); return; }
        if (result == Read::ok) {
            if (existing.room == config.room && existing.session == session && existing.purpose == purpose && existing.phase != QrRules::ACCEPTED) {
                queued_replay = existing; replay_queued = true;
            }
            ESP_LOGI(TAG, "OPEN_DUPLICATE session=%s phase=%d; no repeat actuation", session.c_str(), existing.phase);
            return;
        }
        auto expiry = QrRules::utc(str(j, "expiresAt").c_str());
        if (!fresh_message(expiry, 30)) {
            ESP_LOGW(TAG, "OPEN_REJECT deadline session=%s now=%lld expiry=%lld remaining=%lld", session.c_str(), (long long)utc_now(), (long long)expiry, (long long)(expiry-utc_now())); return;
        }
        if (unlocked || (busy() && !replacement(session, purpose))) { ESP_LOGW(TAG, "OPEN_REJECT cabinet_busy session=%s phase=%d", session.c_str(), active.phase); return; }
        Receipt next; next.room = config.room; next.command = command; next.session = session; next.purpose = purpose;
        next.expected = dump(item(j, "expectedEquipment"));
        if (!save(next) || !write_string(ledger, "active", command)) { fault(); return; }
        Receipt previous = active; active = next;
        if (!previous.command.empty()) { previous.expected.clear(); if (!save(previous)) { fault(); return; } }
        // Recheck expiry after flash writes before actuating.
        if (!fresh_message(expiry, 30)) {
            active.phase = QrRules::FAILED; if (!save(active)) fault(); results_dirty = true; return;
        }
        unlock(session); active.phase = QrRules::OPENED;
        ESP_LOGI(TAG, "OPEN_EXECUTED session=%s", session.c_str());
        if (!save(active)) fault();
        results_dirty = true;
        return;
    }
    if (busy()) return;
    if (verb == "open") unlock(id(j, "id"));
    else if (verb == "close" || verb == "lock") lock();
}

#include "console_runtime.inc"
#include "ota_runtime.inc"

// ESP-MQTT may split an 8 KiB payload into multiple DATA events. Queue complete
// bounded messages only; no actuator, flash or display work in event callbacks.
void mqtt_event(void *, esp_event_base_t, int32_t event_id, void *data) {
    auto e = static_cast<esp_mqtt_event_handle_t>(data);
    static std::string incoming, channel;
    static int total = 0;
    if (event_id == MQTT_EVENT_CONNECTED) {
        mqtt_ready = false; ++connection_epoch; connected = true;
        ESP_LOGI(TAG, "MQTT_LINK_CONNECTED boot_ms=%lld session_present=%d; awaiting SUBACK", (long long)ms(), e->session_present);
    }
    else if (event_id == MQTT_EVENT_DISCONNECTED) {
        connected = false; mqtt_ready = false; incoming.clear(); total = 0;
        ESP_LOGW(TAG, "MQTT_DISCONNECTED boot_ms=%lld", (long long)ms());
    }
    else if (event_id == MQTT_EVENT_SUBSCRIBED) {
        SubscriptionAck ack{connection_epoch.load(), e->msg_id,
            !e->error_handle || e->error_handle->error_type != MQTT_ERROR_TYPE_SUBSCRIBE_FAILED};
        if (xQueueSend(subscription_acks, &ack, 0) != pdTRUE) ESP_LOGW(TAG, "MQTT_SUBACK queue_full id=%d", e->msg_id);
    }
    else if (event_id == MQTT_EVENT_PUBLISHED) {
        PublishAck ack{connection_epoch.load(), e->msg_id};
        if (publish_acks) xQueueSend(publish_acks, &ack, 0);
    }
    else if (event_id == MQTT_EVENT_ERROR && e->error_handle) {
        auto error = e->error_handle;
        ESP_LOGE(TAG, "MQTT_ERROR type=%d tls=0x%x stack=0x%x verify=0x%x errno=%d broker=%d", error->error_type,
            error->esp_tls_last_esp_err, error->esp_tls_stack_err, error->esp_tls_cert_verify_flags,
            error->esp_transport_sock_errno, error->connect_return_code);
    }
    else if (event_id == MQTT_EVENT_DATA) {
        if (e->current_data_offset == 0) {
            if (!e->topic || e->topic_len <= 0) { total = 0; incoming.clear(); return; }
            incoming.clear(); channel.assign(e->topic, e->topic_len); total = e->total_data_len;
            if (total <= 0 || total > 8192) { total = 0; return; }
            incoming.reserve(total);
        }
        if (!total || e->total_data_len != total || e->current_data_offset != static_cast<int>(incoming.size()) ||
            e->data_len < 0 || incoming.size() + e->data_len > static_cast<size_t>(total)) { total = 0; incoming.clear(); return; }
        incoming.append(e->data, e->data_len);
        if (static_cast<int>(incoming.size()) == total) {
            // Reject embedded NULs: cJSON must see exactly the received document.
            if (incoming.find('\0') == std::string::npos) {
                bool accepted = true;
                if (channel == topic("action", true)) accepted = enqueue(ACTION, incoming);
                else if (channel == topic("qr", true)) accepted = enqueue(QR, incoming);
                ESP_LOGI(TAG, "MQTT_RX topic=%s bytes=%d queued=%d", channel.c_str(), total, accepted);
                if (!accepted) ESP_LOGE(TAG, "MQTT_DROP receive_queue_full_or_memory");
            }
            total = 0; incoming.clear();
        }
    }
}
#ifndef CLASSHUB_HOST_TEST
void time_synchronized(struct timeval *tv) {
    ESP_LOGI(TAG, "TIME_SYNC boot_ms=%lld utc=%lld", (long long)ms(), (long long)tv->tv_sec);
}
void wifi_event(void *, esp_event_base_t base, int32_t event, void *data) {
    if (base == WIFI_EVENT && event == WIFI_EVENT_STA_DISCONNECTED) {
        ip_ready = false;
        auto event = static_cast<wifi_event_sta_disconnected_t *>(data);
        ESP_LOGW(TAG, "WIFI_DISCONNECTED reason=%d boot_ms=%lld", event->reason, (long long)ms());
        // Keep retries spaced so the fallback AP remains usable.
        station_retry_at = ms() + 15000;
    } else if (base == IP_EVENT && event == IP_EVENT_STA_LOST_IP) {
        ip_ready = false; station_retry_at = ms() + 15000;
        ESP_LOGW(TAG, "WIFI_LOST_IP; fallback AP after sustained outage");
    } else if (base == IP_EVENT && event == IP_EVENT_STA_GOT_IP) {
        station_retry_at = 0;
        ip_ready = true;
        ESP_LOGI(TAG, "WIFI_GOT_IP boot_ms=%lld; start NTP", (long long)ms());
        esp_err_t sntp_error = esp_netif_sntp_start();
        if (sntp_error != ESP_OK) ESP_LOGE(TAG, "NTP_START failed: %s", esp_err_to_name(sntp_error));
        auto e = static_cast<ip_event_got_ip_t *>(data);
        ESP_LOGI(TAG, "Camera on LAN: http://" IPSTR "/", IP2STR(&e->ip_info.ip));
    } else if (base == WIFI_EVENT && event == WIFI_EVENT_AP_START) {
        ESP_LOGI(TAG, "AP_READY http://192.168.4.1/setup boot_ms=%lld", (long long)ms());
    }
}
bool valid_config(const Settings &s) {
    std::string ap_ssid, ap_password;
    return !s.ssid.empty() && s.ssid.size() <= 32 && s.wifi_password.size() <= 64 &&
        !s.host.empty() && s.host.size() <= 100 &&
        s.host.find_first_not_of("abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789.-") == std::string::npos &&
        s.port > 0 && s.port <= 65535 &&
        ap_credentials(s.room, ap_ssid, ap_password) &&
        !s.username.empty() && s.username.size() <= 64 && !s.password.empty() && s.password.size() <= 128;
}
std::string config_json(const Settings &s, bool network) {
    auto j = object();
    if (network) { put(j.get(), "ssid", s.ssid); put(j.get(), "wifiPassword", s.wifi_password); }
    else { put(j.get(), "host", s.host); put(j.get(), "username", s.username); put(j.get(), "password", s.password);
        put(j.get(), "roomId", s.room); cJSON_AddNumberToObject(j.get(), "port", s.port); }
    return dump(j.get());
}
bool store_config(const Settings &s) {
    // A single atomic NVS value is authoritative for new configurations.
    nvs_handle_t h;
    if (nvs_open("classhub-idf", NVS_READWRITE, &h) != ESP_OK) return false;
    auto j = parse(config_json(s, false)); put(j.get(), "ssid", s.ssid); put(j.get(), "wifiPassword", s.wifi_password);
    bool ok = write_string(h, "config", dump(j.get())); nvs_close(h); return ok;
}
void load_config() {
    // Retain the legacy classhub-mqtt/config format; never erase old namespaces.
    nvs_handle_t h; std::string data;
    if (nvs_open("classhub-idf", NVS_READONLY, &h) == ESP_OK) {
        read_string(h, "config", data); nvs_close(h);
    }
    if (data.empty() && nvs_open("classhub-mqtt", NVS_READONLY, &h) == ESP_OK) {
        read_string(h, "config", data); nvs_close(h);
    }
    auto j = parse(data);
    if (j) {
        config.host = str(j.get(), "host"); config.port = num(j.get(), "port", 8883);
        config.username = str(j.get(), "username"); config.password = str(j.get(), "password"); config.room = str(j.get(), "roomId");
        config.ssid = str(j.get(), "ssid"); config.wifi_password = str(j.get(), "wifiPassword");
    }
    std::string ap_ssid, ap_password;
    if (!ap_credentials(config.room, ap_ssid, ap_password)) {
        config.room = CLASSHUB_DEFAULT_ROOMID;
        if (!ap_credentials(config.room, ap_ssid, ap_password)) config.room = "DTD201";
        ESP_LOGW(TAG, "Invalid stored cabinet ID; using default ID %s", config.room.c_str());
    }
}
esp_err_t configure_ap() {
    std::string ssid, password;
    if (!ap_credentials(config.room, ssid, password)) return ESP_ERR_INVALID_ARG;
    wifi_config_t ap = {};
    memcpy(ap.ap.ssid, ssid.data(), ssid.size());
    memcpy(ap.ap.password, password.data(), password.size());
    ap.ap.ssid_len = ssid.size(); ap.ap.channel = 6;
    ap.ap.ssid_hidden = 0; ap.ap.beacon_interval = 100;
    ap.ap.max_connection = 3; ap.ap.authmode = WIFI_AUTH_WPA2_PSK;
    return esp_wifi_set_config(WIFI_IF_AP, &ap);
}
bool wifi_credentials() {
    return !config.ssid.empty() && config.ssid.size() <= 32 && config.wifi_password.size() <= 64;
}
void service_wifi_mode(int64_t now) {
    // Do not disconnect an AP client midway through an OTA upload.
    if (ota_gate.blocked()) return;
    const bool desired = wifi_fallback.access_point(now, wifi_credentials(), ip_ready.load());
    if (desired == ap_active.load()) return;
    esp_err_t error = esp_wifi_set_mode(desired ? WIFI_MODE_APSTA : WIFI_MODE_STA);
    if (error == ESP_OK && desired) error = configure_ap();
    if (error != ESP_OK) {
        ESP_LOGE(TAG,"WIFI_MODE change failed: %s",esp_err_to_name(error)); return;
    }
    ap_active = desired;
    setup_allowed = desired || !valid_config(config);
    ESP_LOGI(TAG, "%s", desired ? "WIFI_SETUP_AP enabled: http://192.168.4.1/setup" : "WIFI_LAN_ONLY: AP disabled; use station IP");
    if (desired && !busy() && !unlocked) ui("CAU HINH WIFI MQTT", "192.168.4.1/setup");
    else if (!desired && !busy() && !unlocked && display_expiry<=utc_now()) ui("WIFI DA KET NOI", "TRUY CAP IP LAN");
}
void connect_station() {
    // Validate stored lengths before copying into the driver's fixed buffers.
    if (!wifi_credentials()) return;
    wifi_config_t sta = {};
    memcpy(sta.sta.ssid, config.ssid.data(), config.ssid.size());
    memcpy(sta.sta.password, config.wifi_password.data(), config.wifi_password.size());
    sta.sta.pmf_cfg.capable = true;
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &sta));
    // Delay also lets the HTTP setup acknowledgement reach an AP client.
    station_retry_at = ms() + (ap_active ? 5000 : 100);
}
void service_station(int64_t now) {
    int64_t due = station_retry_at.load();
    if (!due || now < due || !station_retry_at.compare_exchange_strong(due, 0)) return;
    if (ip_ready || config.ssid.empty()) return;
    esp_err_t error = esp_wifi_connect();
    if (error != ESP_OK) {
        ESP_LOGW(TAG, "WIFI_CONNECT failed: %s; retry in 15s", esp_err_to_name(error));
        station_retry_at = now + 15000;
    }
}
void start_mqtt() {
    if (mqtt || !valid_config(config)) return;
    std::string uri = "mqtts://" + config.host + ":" + std::to_string(config.port);
    uint8_t mac[6]; esp_read_mac(mac, ESP_MAC_WIFI_STA);
    char client_id[64]; snprintf(client_id, sizeof(client_id), "classhub-%s-%02x%02x%02x%02x%02x%02x", config.room.c_str(), mac[0],mac[1],mac[2],mac[3],mac[4],mac[5]);
    auto will = object(); put(will.get(), "room", config.room); cJSON_AddBoolToObject(will.get(), "isOnline", false);
    auto will_payload = dump(will.get()); auto will_topic = topic("heartbeat");
    esp_mqtt_client_config_t cfg = {};
    cfg.broker.address.uri = uri.c_str(); cfg.broker.verification.crt_bundle_attach = esp_crt_bundle_attach;
    cfg.credentials.client_id = client_id; cfg.credentials.username = config.username.c_str(); cfg.credentials.authentication.password = config.password.c_str();
    cfg.session.last_will.topic = will_topic.c_str(); cfg.session.last_will.msg = will_payload.c_str(); cfg.session.last_will.qos = 1; cfg.session.last_will.retain = 1;
    cfg.session.disable_clean_session = true; // Broker retains QoS1 deliveries across reconnects.
    cfg.session.keepalive = 20;
    cfg.buffer.size = 2048; cfg.buffer.out_size = 2048;
    cfg.network.reconnect_timeout_ms = 5000; cfg.task.stack_size = 6144;
    cfg.outbox.limit = 32768;
    mqtt = esp_mqtt_client_init(&cfg);
    if (!mqtt) { ui("LOI MQTT"); return; }
    esp_mqtt_client_register_event(mqtt, MQTT_EVENT_ANY, mqtt_event, nullptr);
    ESP_LOGI(TAG, "MQTT_START boot_ms=%lld utc=%lld", (long long)ms(), (long long)utc_now());
    if (esp_mqtt_client_start(mqtt) != ESP_OK) { esp_mqtt_client_destroy(mqtt); mqtt = nullptr; }
}
void apply_configuration(const cJSON *j) {
    if (ota_gate.blocked()) return;
    if (unlocked || busy()) { ui("TU DANG BAN", "DONG CUA TRUOC"); return; }
    Settings next = config;
    next.ssid = str(j, "ssid");
    if (!str(j, "wifiPassword").empty()) next.wifi_password = str(j, "wifiPassword");
    if (flag(j, "openNetwork")) next.wifi_password.clear();
    next.host = str(j, "host"); next.port = num(j, "port", 8883); next.username = str(j, "username"); next.room = str(j, "roomId");
    if (!str(j, "password").empty()) next.password = str(j, "password");
    if (!valid_config(next)) { ui("CAU HINH SAI", "MO LAI /setup"); return; }
    if (!store_config(next)) { ui("LOI LUU CAU HINH"); return; }
    if (mqtt) { esp_mqtt_client_stop(mqtt); esp_mqtt_client_destroy(mqtt); mqtt = nullptr; }
    connected = false; mqtt_ready = false; ip_ready = false;
    // Only the cabinet task initiates connections, after applying the new config.
    station_retry_at = 0;
    esp_wifi_disconnect();
    xSemaphoreTake(config_mutex, portMAX_DELAY); config = next; xSemaphoreGive(config_mutex);
    ++connection_epoch; clear_qr();
    esp_err_t ap_error = ap_active ? configure_ap() : ESP_OK;
    setup_allowed = ap_active.load();
    connect_station();
    if (ap_error == ESP_OK) ui("DANG NOI WIFI", config.room.c_str());
    else {
        ESP_LOGE(TAG, "AP configuration failed: %s", esp_err_to_name(ap_error));
        ui("LOI CAU HINH AP", "KHOI DONG LAI");
    }
}
void verification(const cJSON *j) {
    if (ota_gate.blocked()) return;
    if (!CLASSHUB_REQUIRE_RETURN_VERIFICATION || storage_fault ||
        (active.phase == QrRules::CLOSED && active.verified && active.rfid && active.camera) ||
        !QrRules::canVerify(active.phase, active.purpose == "RETURN") || active.room != config.room ||
        id(j, "sessionId") != active.session || str(j, "commandId") != active.command || active.sequence == UINT32_MAX) return;
    Receipt next = active; next.verified = true; next.rfid = flag(j, "rfid"); next.camera = flag(j, "camera"); ++next.sequence;
    if (!save(next)) { fault(); return; } active = next; results_dirty = true;
}
void cabinet_task(void *) {
    std::string stored_tags;
    bool loaded = nvs_open("classhub-rfid", NVS_READWRITE, &tag_store) == ESP_OK;
    if (loaded) {
        Read result = read_string(tag_store, "tags", stored_tags);
        loaded = result == Read::missing || (result == Read::ok && valid_tags(parse(stored_tags).get()));
    }
    xSemaphoreTake(console_mutex, portMAX_DELAY);
    tags_ok = loaded;
    if (loaded && !stored_tags.empty()) tags_json = stored_tags;
    xSemaphoreGive(console_mutex);
    if (nvs_open("classhub-qr", NVS_READWRITE, &ledger) != ESP_OK) fault();
    else restore_ledger();
    if (ap_active) ui("CAU HINH WIFI MQTT", "192.168.4.1/setup");
    else if (setup_allowed) ui("CAU HINH MQTT", "TRUY CAP IP LAN");
    else if (!storage_fault) ui("DA KHOA", config.room.c_str());
    door_raw = door_stable = gpio_get_level(static_cast<gpio_num_t>(CLASSHUB_CLOSE_PIN));
    door_changed_at = ms();
    int64_t setup_since = 0, heartbeat_at = 0, replay_at = 0, subscribe_at = 0;
    bool setup_consumed = false, was_ready = false;
    uint32_t subscribing_epoch = UINT32_MAX;
    SubscriptionState subscriptions;
    for (;;) {
        const int64_t now = ms();
        ota_service();
        service_wifi_mode(now);
        service_station(now);
        if (ip_ready && utc_now() >= 1704067200) start_mqtt();
        bool transport_online = connected.load(); uint32_t epoch = connection_epoch.load();
        bool new_connection = transport_online && subscribing_epoch != epoch;
        if (transport_online && (new_connection || (!subscriptions.ready() && now - subscribe_at >= 3000))) {
            subscribing_epoch = epoch; subscribe_at = now;
            // Set offline until the broker acknowledges BOTH incoming topics.
            // Inbound ACKs are queued, so a fast ACK cannot race ID assignment.
            if (new_connection) heartbeat(false);
            int action_id = esp_mqtt_client_subscribe(mqtt, topic("action", true).c_str(), 1);
            int qr_id = esp_mqtt_client_subscribe(mqtt, topic("qr", true).c_str(), 1);
            subscriptions.begin(action_id, qr_id);
            ESP_LOGI(TAG, "MQTT_SUBSCRIBE epoch=%u action=%d qr=%d", epoch, action_id, qr_id);
        }
        SubscriptionAck ack;
        while (xQueueReceive(subscription_acks, &ack, 0) == pdTRUE) {
            if (transport_online && ack.epoch == epoch) {
                subscriptions.accept(ack.id, ack.success);
                ESP_LOGI(TAG, "MQTT_SUBACK id=%d success=%d", ack.id, ack.success);
            }
        }
        bool online = transport_online && epoch == connection_epoch.load() && connected.load() && subscriptions.ready();
        mqtt_ready = online;
        if (online && (!was_ready || replay_epoch != epoch)) {
            replay_epoch = epoch;
            heartbeat(); heartbeat_at = now; status(); results_dirty = !active.command.empty();
            ui(unlocked ? "CUA DANG MO" : "DA KHOA", config.room.c_str());
            ESP_LOGI(TAG, "MQTT_READY boot_ms=%lld epoch=%u; both subscriptions acknowledged", (long long)now, epoch);
        } else if (!online && was_ready) ui("MQTT OFFLINE", config.room.c_str());
        was_ready = online;
        Message m;
        // Bound processing to keep door debounce and electrical pulse responsive.
        for (int n = 0; n < 2 && xQueueReceive(messages, &m, 0) == pdTRUE; ++n) {
            std::unique_ptr<char, decltype(&free)> body(m.body, free);
            auto j = parse(body.get());
            if (!j || !cJSON_IsObject(j.get())) { ESP_LOGW(TAG, "MESSAGE_REJECT invalid_json"); continue; }
            if (m.kind == CONFIGURE) apply_configuration(j.get());
            else if (m.kind == VERIFICATION) verification(j.get());
            else if (m.kind == CONSOLE) console_command(j.get());
            else if (m.epoch == connection_epoch.load()) {
                if (m.kind == ACTION) action(j.get());
                else show_qr(j.get(), strlen(body.get()));
            } else ESP_LOGW(TAG, "MESSAGE_REJECT stale_connection_epoch");
        }
        tick_hardware(ms());
        console_tick();
        bool setup_pressed = !gpio_get_level(static_cast<gpio_num_t>(CLASSHUB_SETUP_PIN));
        if (!setup_pressed) { setup_since = 0; setup_consumed = false; }
        else if (!setup_consumed) {
            if (!setup_since) setup_since = now;
            if (now - setup_since >= 3000 && !unlocked && !busy() && !ota_gate.blocked()) {
                setup_consumed = true; setup_allowed = true; clear_qr();
                ui("CAU HINH WIFI MQTT", ap_active ? "192.168.4.1/setup" : "TRUY CAP IP LAN");
            }
        }
        if (display_expiry && utc_now() >= display_expiry) { clear_qr(); ui("QR HET HAN", config.room.c_str()); }
        if (online && now - replay_at >= 2000) {
            replay_at = now;
            if (results_dirty && replay(active)) results_dirty = false;
            else if (!results_dirty && replay_queued && replay(queued_replay)) replay_queued = false;
        }
        if (online && now - heartbeat_at >= 15000) { heartbeat_at = now; heartbeat(); }
        vTaskDelay(pdMS_TO_TICKS(10));
    }
}

const char setup_html[] = R"HTML(<!doctype html><html lang="vi"><meta charset="utf-8"><meta name="viewport" content="width=device-width"><title>ClassHub</title><style>body{font:16px system-ui;max-width:520px;margin:32px auto;padding:16px;background:#101820;color:#eef}input,button{box-sizing:border-box;width:100%;padding:12px;margin:6px 0 16px;border-radius:8px}a{color:#8df}</style><h1>Cấu hình ClassHub</h1><p>Nhập WiFi có Internet và MQTT. Để trống mật khẩu để giữ giá trị đã lưu. MQTT dùng TLS và kiểm tra chứng chỉ.</p><form id="f"><label>WiFi SSID<input name="ssid" maxlength="32" required></label><label>Mật khẩu WiFi<input name="wifiPassword" type="password" maxlength="64"></label><label><input name="openNetwork" type="checkbox" style="width:auto"> WiFi không có mật khẩu</label><label>MQTT host<input name="host" maxlength="100" required></label><label>MQTT port<input name="port" type="number" value="8883" min="1" max="65535" required></label><label>MQTT username<input name="username" maxlength="64" required></label><label>MQTT password<input name="password" type="password" maxlength="128"></label><label>Mã phòng<input name="roomId" minlength="3" maxlength="20" value="DTD201" pattern="[A-Za-z0-9_-]{3,20}" required></label><button>Lưu và kết nối</button></form><p id="msg"></p><a href="/">Xem camera</a><script>const f=document.getElementById("f"),msg=document.getElementById("msg");fetch("/setup/info").then(r=>r.json()).then(v=>{for(const k in v){if(f.elements[k])f.elements[k].value=v[k]}}).catch(()=>{});f.onsubmit=async e=>{e.preventDefault();const v=Object.fromEntries(new FormData(f));v.port=Number(v.port);v.openNetwork=f.elements.openNetwork.checked;try{const r=await fetch('/setup',{method:'POST',headers:{'Content-Type':'application/json'},body:JSON.stringify(v)});msg.textContent=await r.text()}catch(e){msg.textContent='Mất kết nối. Kết nối lại WiFi và kiểm tra LCD.'}};</script></html>)HTML";
#include "console_http.inc"
#include "ota_http.inc"
esp_err_t setup_get(httpd_req_t *r) {
    httpd_resp_set_type(r, "text/html; charset=utf-8"); httpd_resp_set_hdr(r, "Cache-Control", "no-store");
    return httpd_resp_send(r, setup_html, HTTPD_RESP_USE_STRLEN);
}
esp_err_t setup_info(httpd_req_t *r) {
    xSemaphoreTake(config_mutex, portMAX_DELAY);
    auto j = object(); put(j.get(), "ssid", config.ssid); put(j.get(), "host", config.host);
    put(j.get(), "username", config.username); put(j.get(), "roomId", config.room);
    cJSON_AddNumberToObject(j.get(), "port", config.port);
    xSemaphoreGive(config_mutex);
    httpd_resp_set_hdr(r, "Cache-Control", "no-store"); httpd_resp_set_type(r, "application/json");
    auto body = dump(j.get()); return httpd_resp_send(r, body.c_str(), body.size());
}
esp_err_t setup_post(httpd_req_t *r) {
    if (r->content_len <= 0 || r->content_len > 2048) return httpd_resp_send_err(r, HTTPD_400_BAD_REQUEST, "Invalid length");
    std::string body(r->content_len, '\0'); int received = 0;
    while (received < r->content_len) {
        int n = httpd_req_recv(r, &body[received], r->content_len - received);
        if (n <= 0) return ESP_FAIL;
        received += n;
    }
    if (body.find('\0') != std::string::npos || !enqueue(CONFIGURE, body)) {
        httpd_resp_set_status(r, "503 Service Unavailable"); return httpd_resp_sendstr(r, "Try again");
    }
    httpd_resp_set_status(r, "202 Accepted");
    httpd_resp_set_type(r, "text/plain; charset=utf-8");
    return httpd_resp_sendstr(r, "Đã nhận yêu cầu. Khi Wi-Fi kết nối thành công, AP sẽ tắt. Kết nối cùng mạng LAN rồi mở IP ESP32 được in trong Serial hoặc xem trong danh sách thiết bị của router.");
}
#endif
} // namespace

#ifndef CLASSHUB_HOST_TEST
extern "C" esp_err_t classhub_start() {
    const esp_app_desc_t *firmware = esp_app_get_description();
    ESP_LOGW(TAG,"FIRMWARE_ID=camera-jpeg-v7-miso19 build=%s %s; RC522 diagnostics available on web",firmware->date,firmware->time);
    esp_reset_reason_t reason = esp_reset_reason();
    bool retained = reason != ESP_RST_POWERON && reason != ESP_RST_BROWNOUT &&
                    open_trace_magic == OPEN_TRACE_MAGIC;
    ESP_LOGW(TAG, "RESET_DIAG reason=%d previous_stage=%s retained=%d int_wdt_ms=%d",
             (int)reason, retained ? stage_name(open_trace_stage) : "UNAVAILABLE",
             retained, CONFIG_ESP_INT_WDT_TIMEOUT_MS);
    open_trace_stage = OPEN_IDLE; open_trace_magic = OPEN_TRACE_MAGIC;
    // The caller starts this before any camera diagnostics. Safe OFF at boot.
    gpio_set_level(static_cast<gpio_num_t>(CLASSHUB_LATCH_PIN), 0);
    gpio_config_t output = {};
    output.pin_bit_mask = 1ULL << CLASSHUB_LATCH_PIN;
    output.mode = GPIO_MODE_OUTPUT; ESP_ERROR_CHECK(gpio_config(&output));
    gpio_config_t close = {}; close.pin_bit_mask = 1ULL << CLASSHUB_CLOSE_PIN;
    close.mode = GPIO_MODE_INPUT;
    close.pull_up_en = GPIO_PULLUP_ENABLE; close.pull_down_en = GPIO_PULLDOWN_DISABLE;
    ESP_ERROR_CHECK(gpio_config(&close));
    gpio_config_t setup = {}; setup.pin_bit_mask = 1ULL << CLASSHUB_SETUP_PIN;
    setup.mode = GPIO_MODE_INPUT; setup.pull_up_en = GPIO_PULLUP_ENABLE; ESP_ERROR_CHECK(gpio_config(&setup));
    ledc_timer_config_t timer = {}; timer.speed_mode = LEDC_LOW_SPEED_MODE; timer.timer_num = LEDC_TIMER_1;
    timer.duty_resolution = LEDC_TIMER_10_BIT; timer.freq_hz = 1000; timer.clk_cfg = LEDC_AUTO_CLK;
    ESP_ERROR_CHECK(ledc_timer_config(&timer));
    ledc_channel_config_t channel = {}; channel.gpio_num = CLASSHUB_BUZZER_PIN; channel.speed_mode = LEDC_LOW_SPEED_MODE;
    channel.channel = LEDC_CHANNEL_1; channel.timer_sel = LEDC_TIMER_1;
    ESP_ERROR_CHECK(ledc_channel_config(&channel));
    esp_err_t error = nvs_flash_init(); if (error != ESP_OK) return error; // Never erase ledger/config automatically.
    display_mutex = xSemaphoreCreateMutex(); config_mutex = xSemaphoreCreateMutex(); messages = xQueueCreate(8, sizeof(Message));
    subscription_acks = xQueueCreate(8, sizeof(SubscriptionAck));
    console_mutex = xSemaphoreCreateMutex(); publish_acks = xQueueCreate(16, sizeof(PublishAck));
    if (!display_mutex || !config_mutex || !messages || !subscription_acks || !console_mutex || !publish_acks) return ESP_ERR_NO_MEM;
    load_config();
    ESP_ERROR_CHECK(esp_netif_init());
    error = esp_event_loop_create_default(); if (error != ESP_OK && error != ESP_ERR_INVALID_STATE) return error;
    if (!esp_netif_create_default_wifi_ap() || !esp_netif_create_default_wifi_sta()) return ESP_ERR_NO_MEM;
    wifi_init_config_t init = WIFI_INIT_CONFIG_DEFAULT(); ESP_ERROR_CHECK(esp_wifi_init(&init));
    wifi_config_t previous = {};
    if (config.ssid.empty() && esp_wifi_get_config(WIFI_IF_STA, &previous) == ESP_OK && previous.sta.ssid[0]) {
        config.ssid.assign(reinterpret_cast<char *>(previous.sta.ssid), strnlen(reinterpret_cast<char *>(previous.sta.ssid), 32));
        config.wifi_password.assign(reinterpret_cast<char *>(previous.sta.password), strnlen(reinterpret_cast<char *>(previous.sta.password), 64));
    }
    setup_allowed = !valid_config(config);
    ESP_ERROR_CHECK(esp_event_handler_register(WIFI_EVENT, WIFI_EVENT_STA_DISCONNECTED, wifi_event, nullptr));
    ESP_ERROR_CHECK(esp_event_handler_register(WIFI_EVENT, WIFI_EVENT_AP_START, wifi_event, nullptr));
    ESP_ERROR_CHECK(esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP, wifi_event, nullptr));
    ESP_ERROR_CHECK(esp_event_handler_register(IP_EVENT, IP_EVENT_STA_LOST_IP, wifi_event, nullptr));
    ESP_ERROR_CHECK(esp_wifi_set_storage(WIFI_STORAGE_RAM)); ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_APSTA));
    esp_sntp_config_t ntp = ESP_NETIF_SNTP_DEFAULT_CONFIG_MULTIPLE(2, ESP_SNTP_SERVER_LIST("time.google.com", "pool.ntp.org"));
    ntp.start = false; ntp.wait_for_sync = false; ntp.sync_cb = time_synchronized;
    ESP_ERROR_CHECK(esp_netif_sntp_init(&ntp));
    // Prepare fallback credentials without broadcasting when saved Wi-Fi exists.
    ESP_ERROR_CHECK(configure_ap());
    ap_active = wifi_fallback.access_point(ms(), wifi_credentials(), false);
    if (!ap_active) ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_start());
    esp_wifi_set_ps(WIFI_PS_NONE); connect_station();
    if (xTaskCreate(cabinet_task, "classhub", 12288, nullptr, 5, nullptr) != pdPASS) return ESP_ERR_NO_MEM;
    ESP_LOGI(TAG, "IDF cabinet: latch=%d close=%d buzzer=%d setup=%d; AP=%d; saved Wi-Fi first, fallback after 30s offline",
        CLASSHUB_LATCH_PIN, CLASSHUB_CLOSE_PIN, CLASSHUB_BUZZER_PIN, CLASSHUB_SETUP_PIN, (int)ap_active.load());
    return ESP_OK;
}
extern "C" esp_err_t classhub_register_http(httpd_handle_t server) {
    httpd_uri_t ota = {}; ota.uri="/api/ota"; ota.method=HTTP_POST; ota.handler=ota_post;
    esp_err_t ota_error=httpd_register_uri_handler(server,&ota);
    if (ota_error!=ESP_OK) return ota_error;
    httpd_uri_t device_get = {}; device_get.uri = "/api/device"; device_get.method = HTTP_GET; device_get.handler = console_get;
    httpd_uri_t device_post = {}; device_post.uri = "/api/device"; device_post.method = HTTP_POST; device_post.handler = console_post;
    esp_err_t device_error = httpd_register_uri_handler(server, &device_get);
    if (device_error == ESP_OK) device_error = httpd_register_uri_handler(server, &device_post);
    if (device_error != ESP_OK) return device_error;
    httpd_uri_t get = {}; get.uri = "/setup"; get.method = HTTP_GET; get.handler = setup_get;
    httpd_uri_t post = {}; post.uri = "/setup"; post.method = HTTP_POST; post.handler = setup_post;
    httpd_uri_t info = {}; info.uri = "/setup/info"; info.method = HTTP_GET; info.handler = setup_info;
    esp_err_t e = httpd_register_uri_handler(server, &get);
    if (e == ESP_OK) e = httpd_register_uri_handler(server, &post);
    if (e == ESP_OK) e = httpd_register_uri_handler(server, &info);
    ota_http_ready = e == ESP_OK;
    return e;
}
extern "C" void classhub_observe_rfid(const char *uid) {
    if (!console_mutex || !uid || !valid_uid(uid)) return;
    xSemaphoreTake(console_mutex, portMAX_DELAY);
    latest_uid = uid; uid_at = ms();
    xSemaphoreGive(console_mutex);
}
extern "C" bool classhub_ota_active() { return ota_gate.blocked(); }
extern "C" bool classhub_display_snapshot(classhub_display_t *out, uint32_t generation) {
    if (!display_mutex || !out) return false;
    xSemaphoreTake(display_mutex, portMAX_DELAY);
    bool changed = display.generation != generation; if (changed) *out = display;
    xSemaphoreGive(display_mutex); return changed;
}
extern "C" bool classhub_submit_verification(const char *session, const char *command, bool rfid, bool camera) {
    if (!CLASSHUB_REQUIRE_RETURN_VERIFICATION || !messages || !session || !command) return false;
    auto j = object(); put(j.get(), "sessionId", session); put(j.get(), "commandId", command);
    cJSON_AddBoolToObject(j.get(), "rfid", rfid); cJSON_AddBoolToObject(j.get(), "camera", camera);
    return enqueue(VERIFICATION, dump(j.get()));
}
#endif
