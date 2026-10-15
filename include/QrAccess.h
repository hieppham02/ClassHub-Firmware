#pragma once
#include <Arduino.h>
#include <ArduinoJson.h>
#include <lvgl.h>

// TEMP QR TEST: set to 1 together with the backend verification switch.
#ifndef CLASSHUB_REQUIRE_RETURN_VERIFICATION
#define CLASSHUB_REQUIRE_RETURN_VERIFICATION 0
#endif

// All calls run on the foreground task. Sensor/ISR tasks must enqueue results.
using QrPublish = bool (*)(const String &payload);
void initializeQrAccess(QrPublish publish);
void handleQrDisplay(const String &payload, size_t size);
// true means QR-shaped message was handled/rejected; never treat malformed QR as an admin command.
bool handleQrAction(JsonDocument &document, bool unlocked, bool actionPending);
bool acceptQrScanFeedback(JsonDocument &document);
bool qrUnlockPending(String &sessionId);
void qrUnlockExecuted();
void qrLockExecuted();
bool qrNeedsDoorClose();
void tickQrAccess();
void replayQrResults();
void clearQrDisplay();
bool qrOwnsCabinet();
// Real RFID/camera pipeline must compare against the saved expectedEquipment.
String qrExpectedEquipment();
bool qrVerificationContext(String &sessionId, String &commandId, String &expectedEquipment);
bool submitQrVerification(const String &sessionId, const String &commandId, bool rfidPassed, bool cameraPassed);
