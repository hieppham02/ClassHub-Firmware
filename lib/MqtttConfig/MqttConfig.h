#pragma once

#include <Arduino.h>

struct MqttTopics {
    String action;
    String status;
    String heartbeat;
    String qr;
    String qrResult;
};

struct MqttSettings {
    String host;
    uint16_t port = 8883;
    String username;
    String password;
    String roomId;
    MqttTopics topics;

    void updateTopics() {
        topics.action    = "backend/cabinet/" + roomId + "/action";
        topics.status    = "iot/cabinet/" + roomId + "/status";
        topics.heartbeat = "iot/cabinet/" + roomId + "/heartbeat";
        topics.qr        = "backend/cabinet/" + roomId + "/qr";
        topics.qrResult  = "iot/cabinet/" + roomId + "/qr-result";
    }
};

extern MqttSettings activeMqttConfig;

bool saveMqttConfig(const MqttSettings& settings);
bool loadMqttConfig(MqttSettings& settings);
void initMqttConfig();
