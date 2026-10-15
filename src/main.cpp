#include <Arduino.h>
#include <ArduinoJson.h>
#include <MQTTPubSubClient.h>
#include <TFT_eSPI.h>
#include <WiFiClientSecure.h>
#include <WiFi.h>
#include <lvgl.h>
#include "WifiConfig.h"
#include "MqttConfig.h"
#include "QrAccess.h"

#define SCREEN_WIDTH 160
#define SCREEN_HEIGHT 128
#define DRAW_BUFFER_SIZE (SCREEN_WIDTH * SCREEN_HEIGHT / 10 * 2)

#define LATCH_PIN 4
#define BUZZER_PIN 6
#define CLOSE_BUTTON_PIN 5
#define LED_PIN 35
#define WIFI_CONFIG_BUTTON_PIN 15

// Electrical latch driver: HIGH energizes/unlocks, LOW de-energizes/locks.
#define LATCH_ON HIGH
#define LATCH_OFF LOW

const unsigned long DOOR_DEBOUNCE_DELAY_MS = 50;
const unsigned long LATCH_PULSE_MS = 1500;
const unsigned long HEARTBEAT_INTERVAL_MS = 15000;
const unsigned long MQTT_RECONNECT_INTERVAL_MS = 5000;

unsigned long wifiButtonPressedAt = 0;
bool wifiButtonHeld = false;
unsigned long latchStartedAt = 0;
bool latchPulseActive = false;
bool qrScanBeepPending = false;

TFT_eSPI tft;
WiFiClientSecure mqttTransport;
// QR actions carry the equipment snapshot; 256 bytes truncates these packets.
MQTTPubSub::PubSubClient<8192> mqttClient;

uint8_t drawBuffer[DRAW_BUFFER_SIZE];
lv_obj_t *lb_header;
lv_obj_t *lb_status;
lv_obj_t *lb_detail;
lv_obj_t *lb_network;

struct CabinetState
{
    String currentSessionId = "";
    bool isUnlocked = false;
    bool isOnline = false;
};
CabinetState cabinetState;

enum PendingAction
{
    ACTION_NONE,
    ACTION_OPEN,
    ACTION_LOCK
};

PendingAction pendingAction = ACTION_NONE;
String pendingSessionId = "";

uint8_t stableDoorState = LOW;
uint8_t lastRawDoorState = LOW;
unsigned long lastDoorDebounceTime = 0;
unsigned long lastHeartbeatTime = 0;
unsigned long lastMqttReconnectAttempt = 0;

void displayFlush(lv_display_t *display, const lv_area_t *area, uint8_t *pixelMap)
{
    uint32_t width = lv_area_get_width(area);
    uint32_t height = lv_area_get_height(area);

    tft.startWrite();
    tft.setAddrWindow(area->x1, area->y1, width, height);
    tft.pushColors(reinterpret_cast<uint16_t *>(pixelMap), width * height, false);
    tft.endWrite();

    lv_display_flush_ready(display);
}

void initializeScreen()
{
    lv_display_t *display = lv_display_create(SCREEN_WIDTH, SCREEN_HEIGHT);
    lv_display_set_flush_cb(display, displayFlush);
    lv_display_set_buffers(display, drawBuffer, nullptr, sizeof(drawBuffer), LV_DISPLAY_RENDER_MODE_PARTIAL);
    lv_obj_set_style_bg_color(lv_screen_active(), lv_color_hex(0x0000), LV_PART_MAIN);

    lb_header = lv_label_create(lv_screen_active());
    lv_obj_set_width(lb_header, SCREEN_WIDTH);
    lv_obj_set_style_text_color(lb_header, lv_color_hex(0xFFFF), LV_PART_MAIN);
    lv_obj_set_style_text_align(lb_header, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_align(lb_header, LV_ALIGN_TOP_MID, 0, 8);
    String room_name = activeMqttConfig.roomId;
    String display_name = room_name;

    for (unsigned int i = 0; i < room_name.length(); i++)
    {
        if (isDigit(room_name[i]))
        {
            display_name = room_name.substring(0, i) + " - " + room_name.substring(i);
            break;
        }
    }
    lv_label_set_text_fmt(lb_header, "CLASSHUB\n%s", display_name.c_str());

    lb_status = lv_label_create(lv_screen_active());
    lv_obj_set_width(lb_status, SCREEN_WIDTH);
    lv_obj_set_style_text_color(lb_status, lv_color_hex(0xFFFF), LV_PART_MAIN);
    lv_obj_set_style_text_align(lb_status, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_align(lb_status, LV_ALIGN_CENTER, 0, -8);
    lv_label_set_text(lb_status, "Dang khoi dong...");

    lb_detail = lv_label_create(lv_screen_active());
    lv_obj_set_width(lb_detail, SCREEN_WIDTH);
    lv_obj_set_style_text_color(lb_detail, lv_color_hex(0xFFFF), LV_PART_MAIN);
    lv_obj_set_style_text_align(lb_detail, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_align(lb_detail, LV_ALIGN_CENTER, 0, 20);
    lv_label_set_text(lb_detail, "");

    lb_network = lv_label_create(lv_screen_active());
    lv_obj_set_width(lb_network, SCREEN_WIDTH);
    lv_obj_set_style_text_color(lb_network, lv_color_hex(0xFFFF), LV_PART_MAIN);
    lv_obj_set_style_text_align(lb_network, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_align(lb_network, LV_ALIGN_BOTTOM_MID, 0, -6);
    lv_label_set_text(lb_network, "STATUS: OFFLINE");
}

String readMessageId(JsonVariantConst value)
{
    if (value.is<const char *>())
        return String(value.as<const char *>());
    if (value.is<int>())
        return String(value.as<int>());
    if (value.is<long>())
        return String(value.as<long>());
    return "";
}

String createHeartbeatPayload(bool isOnline)
{
    JsonDocument document;
    document["room"] = activeMqttConfig.roomId;
    document["isOnline"] = isOnline;

    String payload;
    serializeJson(document, payload);
    return payload;
}

void publishHeartbeat()
{
    if (!mqttClient.isConnected())
        return;

    String payload = createHeartbeatPayload(true);
    bool published = mqttClient.publish(activeMqttConfig.topics.heartbeat.c_str(), payload, true, 1);

    if (published)
    {
        Serial.println("[MQTT Sent -> Heartbeat] " + payload);
    }
    else
    {
        Serial.println("[MQTT] Gui heartbeat that bai.");
    }
}

void publishStatus(const String &sessionId = "")
{
    if (!mqttClient.isConnected())
        return;

    String currentSessionId = sessionId.length() > 0 ? sessionId : cabinetState.currentSessionId;

    JsonDocument document;
    document["id"] = currentSessionId;
    document["room"] = activeMqttConfig.roomId;
    document["isOpen"] = cabinetState.isUnlocked;
    document["isOnline"] = true;
    document["lockState"] = cabinetState.isUnlocked ? "UNLOCKED" : "LOCKED";

    String payload;
    serializeJson(document, payload);

    bool published = mqttClient.publish(activeMqttConfig.topics.status.c_str(), payload, false, 1);

    if (published)
    {
        Serial.println("[MQTT Sent -> Status] " + payload);
    }
    else
    {
        Serial.println("[MQTT] Gui status that bai.");
    }
}

void executeUnlock(const String &sessionId = "")
{
    if (sessionId.length() > 0)
        cabinetState.currentSessionId = sessionId;

    cabinetState.isUnlocked = true;

    digitalWrite(LED_PIN, HIGH);
    digitalWrite(LATCH_PIN, LATCH_ON);
    latchStartedAt = millis();
    latchPulseActive = true;
    tone(BUZZER_PIN, 1000, 1500);

   

    lv_label_set_text(lb_status, "CUA DANG MO");
    lv_label_set_text(lb_detail, "");

    publishStatus();
    Serial.println(">> [LATCH] DA BAT KHOA DIEN DE MO!");
}

void executeLock()
{
    cabinetState.isUnlocked = false;

    digitalWrite(LED_PIN, LOW);
    digitalWrite(LATCH_PIN, LATCH_OFF);
    latchPulseActive = false;

    lv_label_set_text(lb_status, "DA KHOA");
    lv_label_set_text(lb_detail, "");

    publishStatus();
    Serial.println(">> [LATCH] DA TAT KHOA DIEN!");
}

void handleDoorSensor()
{
    uint8_t currentRawState = digitalRead(CLOSE_BUTTON_PIN);

    if (currentRawState != lastRawDoorState)
        lastDoorDebounceTime = millis();

    if (millis() - lastDoorDebounceTime > DOOR_DEBOUNCE_DELAY_MS && currentRawState != stableDoorState)
    {
        stableDoorState = currentRawState;

        if (stableDoorState == HIGH && (cabinetState.isUnlocked || qrNeedsDoorClose()))
        {
            Serial.println(">> [SENSOR] Da dong cua tu.");
            pendingAction = ACTION_LOCK;
        }
    }

    lastRawDoorState = currentRawState;
}

void handleActionMessage(const String &payload, const size_t size)
{
    (void)size;
    Serial.println("[MQTT Recv -> Action] " + payload);

    JsonDocument document;
    DeserializationError error = deserializeJson(document, payload);

    if (error)
    {
        Serial.println("[MQTT] Payload action khong phai JSON hop le.");
        return;
    }

    String targetRoom = document["room"] | "";
    if (targetRoom != activeMqttConfig.roomId)
    {
        Serial.println("[MQTT] Bo qua action khong thuoc tu nay.");
        return;
    }

    if (String(document["action"] | "") == "qr_scanned")
    {
        if (acceptQrScanFeedback(document))
        {
            qrScanBeepPending = true;
            Serial.println("[BUZZER] Valid QR scan queued.");
        }
        else Serial.println("[BUZZER] QR scan ignored: stale, mismatched or duplicate.");
        return;
    }
    if (handleQrAction(document, cabinetState.isUnlocked, pendingAction != ACTION_NONE)) return;
    if (qrOwnsCabinet()) return;

    String action = document["action"] | "";
    String sessionId = readMessageId(document["id"]);
    long timestamp = document["ts"] | 0;

    Serial.printf(">> [CMD] Room: %s | Action: %s | TS: %ld\n", targetRoom.c_str(), action.c_str(), timestamp);

    if (action == "open")
    {
        pendingSessionId = sessionId;
        pendingAction = ACTION_OPEN;
    }
    else if (action == "lock" || action == "close")
    {
        pendingAction = ACTION_LOCK;
    }
    else
    {
        Serial.println("[MQTT] Action khong duoc ho tro.");
    }
}

void subscribeToMqttTopics()
{
    bool actionSubscribed = mqttClient.subscribe(activeMqttConfig.topics.action.c_str(), 1, handleActionMessage);
    bool qrSubscribed = mqttClient.subscribe(activeMqttConfig.topics.qr.c_str(), 1, handleQrDisplay);

    if (actionSubscribed && qrSubscribed)
    {
        Serial.println("[MQTT] Da subscribe QR va action.");
    }
    else
    {
        Serial.println("[MQTT] Subscribe topic that bai.");
    }
}

void connectMqtt()
{
    if (WiFi.status() != WL_CONNECTED || mqttClient.isConnected())
        return;

    if (!mqttTransport.connected())
    {
        Serial.print("Dang ket noi TLS toi HiveMQ... ");
        mqttTransport.setHandshakeTimeout(10);

        if (!mqttTransport.connect(activeMqttConfig.host.c_str(), activeMqttConfig.port))
        {
            Serial.println("KET NOI TLS THAT BAI!");
            cabinetState.isOnline = false;
            lv_label_set_text(lb_network, "MANG: OFFLINE");
            return;
        }
        Serial.println("TLS OK!");
    }

    String clientId = "ESP32S3_" + activeMqttConfig.roomId;
    Serial.print("Dang ket noi MQTT (" + clientId + ")... ");

    if (!mqttClient.connect(clientId.c_str(), activeMqttConfig.username.c_str(), activeMqttConfig.password.c_str()))
    {
        cabinetState.isOnline = false;
        lv_label_set_text(lb_network, "MANG: OFFLINE");
        Serial.println("THAT BAI!");
        mqttTransport.stop();
        return;
    }

    cabinetState.isOnline = true;
    lastHeartbeatTime = millis();

    lv_label_set_text(lb_status, "SAN SANG");
    lv_label_set_text(lb_network, "MANG: ONLINE");

    subscribeToMqttTopics();
    publishHeartbeat();
    publishStatus();
    replayQrResults();

    Serial.println("THANH CONG!");
}

static uint32_t get_lv_tick()
{
    return millis();
}

void setup()
{
    Serial.begin(115200);
    while (!Serial && millis() < 3000)
    {
    }

    initMqttConfig();

    pinMode(WIFI_CONFIG_BUTTON_PIN, INPUT_PULLDOWN);
    pinMode(CLOSE_BUTTON_PIN, INPUT_PULLDOWN);
    pinMode(LED_PIN, OUTPUT);
    digitalWrite(LED_PIN, LOW);
    pinMode(BUZZER_PIN, OUTPUT);
    digitalWrite(BUZZER_PIN, LOW);

    // Set the output latch LOW before enabling the pin as an output.
    digitalWrite(LATCH_PIN, LATCH_OFF);
    pinMode(LATCH_PIN, OUTPUT);

    tft.init();
    tft.setRotation(1);
    tft.fillScreen(TFT_BLACK);

    lv_init();
    lv_tick_set_cb(get_lv_tick);
    initializeScreen();
    initializeQrAccess([](const String &payload) {
        return mqttClient.isConnected() && mqttClient.publish(activeMqttConfig.topics.qrResult.c_str(), payload, false, 1);
    });

    lv_label_set_text(lb_status, "CAU HINH WIFI");
    lv_label_set_text(lb_detail, "ClassHub-Setup\nclasshub123");
    lv_label_set_text(lb_network, "192.168.4.1");
    lv_timer_handler();

    if (!connectConfiguredWifi())
    {
        Serial.println("Chua ket noi duoc WiFi. Khoi dong lai sau 3 giay.");
        delay(3000);
        ESP.restart();
        return;
    }

    lv_label_set_text(lb_status, "WIFI DA KET NOI");
    lv_label_set_text(lb_detail, "");
    lv_label_set_text(lb_network, "DANG NOI MQTT");
    lv_obj_invalidate(lv_screen_active());
    lv_refr_now(nullptr);

    Serial.println("WiFi OK!");

    mqttTransport.setInsecure();
    mqttClient.begin(mqttTransport);
    mqttClient.setWill(activeMqttConfig.topics.heartbeat.c_str(), createHeartbeatPayload(false).c_str(), true, 1);

    connectMqtt();
}

void loop()
{
    if (digitalRead(WIFI_CONFIG_BUTTON_PIN) == HIGH)
    {
        if (!wifiButtonHeld)
        {
            wifiButtonHeld = true;
            wifiButtonPressedAt = millis();
        }
        if (millis() - wifiButtonPressedAt >= 3000)
        {
            mqttTransport.stop();

            lv_label_set_text(lb_status, "CAU HINH WIFI");
            lv_label_set_text(lb_detail, "ClassHub-Setup\nclasshub123");
            lv_label_set_text(lb_network, "192.168.4.1");
            lv_timer_handler();

            openWifiSetup();

            ESP.restart();
            return;
        }
    }
    else
    {
        wifiButtonHeld = false;
    }

    mqttClient.update();
    lv_timer_handler();

    if (!mqttClient.isConnected())
    {
        cabinetState.isOnline = false;
        lv_label_set_text(lb_network, "MANG: OFFLINE");

        if (millis() - lastMqttReconnectAttempt >= MQTT_RECONNECT_INTERVAL_MS)
        {
            lastMqttReconnectAttempt = millis();
            Serial.println("[MQTT] Thu ket noi lai...");
            connectMqtt();
        }
    }

    if (qrScanBeepPending)
    {
        qrScanBeepPending = false;
        tone(BUZZER_PIN, 1000, 500);
        Serial.println("[BUZZER] Scan tone: GPIO 7, 1000 Hz, 500 ms.");
    }

    if (pendingAction == ACTION_OPEN)
    {
        String sessionId = pendingSessionId;
        pendingSessionId = "";
        pendingAction = ACTION_NONE;
        executeUnlock(sessionId);
    }
    else if (pendingAction == ACTION_LOCK)
    {
        pendingAction = ACTION_NONE;
        executeLock();
        qrLockExecuted();
    }

    String qrSessionId;
    if (pendingAction == ACTION_NONE && qrUnlockPending(qrSessionId))
    {
        executeUnlock(qrSessionId);
        qrUnlockExecuted();
    }

    if (latchPulseActive && millis() - latchStartedAt >= LATCH_PULSE_MS)
    {
        digitalWrite(LATCH_PIN, LATCH_OFF);
        latchPulseActive = false;
    }

    handleDoorSensor();
    tickQrAccess();

    if (mqttClient.isConnected() && millis() - lastHeartbeatTime >= HEARTBEAT_INTERVAL_MS)
    {
        lastHeartbeatTime = millis();
        publishHeartbeat();
    }

    delay(5);
}
