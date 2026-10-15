#include "QrAccess.h"
#include "QrRules.h"
#include "MqttConfig.h"
#include <Preferences.h>
#include <time.h>

namespace
{
    struct Receipt
    {
        String room, session, command, purpose, expected;
        QrRules::Phase phase = QrRules::ACCEPTED;
        uint32_t sequence = 0;
        bool verified = false, rfid = false, camera = false;
    };
    Receipt active;
    Receipt queuedReplay;
    bool replayQueued = false;
    Preferences storage;
    bool storageReady = false, storageFault = false, pendingUnlock = false, resultsDirty = false;
    QrPublish publishResult = nullptr;
    unsigned long lastResultAttempt = 0;
    lv_obj_t *overlay = nullptr, *code = nullptr;
    String displaySession, displayPurpose;
    int64_t displayExpiry = 0;
    bool displayScanNotified = false;
    int64_t commandExpiry = 0;

    String key(const String &command) { return command.substring(0, 15); }
    bool save(const Receipt &r)
    {
        if (!storageReady)
            return false;
        JsonDocument doc;
        doc["room"] = r.room;
        doc["session"] = r.session;
        doc["command"] = r.command;
        doc["purpose"] = r.purpose;
        doc["phase"] = (int)r.phase;
        doc["expected"] = r.expected;
        doc["sequence"] = r.sequence;
        doc["verified"] = r.verified;
        doc["rfid"] = r.rfid;
        doc["camera"] = r.camera;
        String data;
        serializeJson(doc, data);
        return storage.putString(key(r.command).c_str(), data) == data.length();
    }
    bool read(const String &command, Receipt &r)
    {
        String data = storage.getString(key(command).c_str(), "");
        JsonDocument doc;
        if (data.isEmpty() || deserializeJson(doc, data))
            return false;
        r.room = doc["room"].as<String>();
        r.session = doc["session"].as<String>();
        r.command = doc["command"].as<String>();
        r.purpose = doc["purpose"].as<String>();
        int phase = doc["phase"] | -1;
        if (r.command != command || !QrRules::hex(r.command.c_str(), 32) || !QrRules::session(r.session.c_str()) || !QrRules::purpose(r.purpose.c_str()) || phase < 0 || phase > 3)
            return false;
        r.phase = (QrRules::Phase)phase;
        r.expected = doc["expected"].as<String>();
        r.sequence = doc["sequence"] | 0U;
        r.verified = doc["verified"] | false;
        r.rfid = doc["rfid"] | false;
        r.camera = doc["camera"] | false;
        return true;
    }
    bool send(const Receipt &r, const char *event)
    {
        if (!publishResult || r.room != activeMqttConfig.roomId)
            return false;
        JsonDocument doc;
        doc["sessionId"] = r.session.toInt();
        doc["commandId"] = r.command;
        doc["event"] = event;
        if (!strcmp(event, "VERIFIED"))
        {
            doc["rfidPassed"] = r.rfid;
            doc["cameraPassed"] = r.camera;
            doc["sequence"] = r.sequence;
        }
        String payload;
        serializeJson(doc, payload);
        return publishResult(payload);
    }
    bool replay(const Receipt &r)
    {
        if (r.phase == QrRules::FAILED)
            return send(r, "OPEN_FAILED");
        if (r.phase == QrRules::ACCEPTED)
            return false;
        if (!send(r, "OPENED"))
            return false;
        if (r.phase == QrRules::CLOSED && !send(r, "CLOSED"))
            return false;
        return !CLASSHUB_REQUIRE_RETURN_VERIFICATION || !r.verified || send(r, "VERIFIED");
    }
    bool activeBusy()
    {
        return !active.command.isEmpty() && QrRules::busy(active.phase, active.purpose == "RETURN" && CLASSHUB_REQUIRE_RETURN_VERIFICATION, active.verified, active.rfid, active.camera);
    }
    bool verificationComplete() { return active.phase == QrRules::CLOSED && active.verified && active.rfid && active.camera; }
    bool validExpected(JsonVariantConst value)
    {
        if (!value.is<JsonArrayConst>())
            return false;
        JsonArrayConst items = value.as<JsonArrayConst>();
        if (items.size() > 64)
            return false;
        for (JsonObjectConst item : items)
        {
            if (!item["equipmentId"].is<int>() || item["equipmentId"].as<int>() <= 0 || !item["quantity"].is<int>() || item["quantity"].as<int>() <= 0 || !item["name"].is<const char *>())
                return false;
        }
        return true;
    }
    void failStorage()
    {
        storageFault = true;
        Serial.println("[QR] NVS failure; QR actuation blocked. Legacy configuration retained.");
    }
}

void clearQrDisplay()
{
    if (overlay)
        lv_obj_add_flag(overlay, LV_OBJ_FLAG_HIDDEN);
    displaySession = "";
    displayPurpose = "";
    displayExpiry = 0;
    displayScanNotified = false;
}
void initializeQrAccess(QrPublish publish)
{
    publishResult = publish;
    storageReady = storage.begin("classhub-qr", false);
    if (!storageReady)
        failStorage();
    if (storageReady)
    {
        String command = storage.getString("active", "");
        if (!command.isEmpty())
        {
            if (!read(command, active))
                failStorage();
            else
            {
                // Setup initializes the electrical latch OFF/locked. An
                // interrupted command is never executed again after a reset.
                if (active.phase == QrRules::ACCEPTED)
                {
                    active.phase = QrRules::FAILED;
                    if (!save(active))
                        failStorage();
                }
                resultsDirty = true;
            }
        }
    }
    overlay = lv_obj_create(lv_screen_active());
    lv_obj_remove_style_all(overlay);
    lv_obj_set_size(overlay, 160, 128);
    lv_obj_align(overlay, LV_ALIGN_CENTER, 0, 0);
    lv_obj_set_style_bg_color(overlay, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(overlay, LV_OPA_COVER, 0);
    lv_obj_remove_flag(overlay, LV_OBJ_FLAG_SCROLLABLE);
    code = lv_qrcode_create(overlay);
    lv_qrcode_set_size(code, 120);
    lv_qrcode_set_dark_color(code, lv_color_black());
    lv_qrcode_set_light_color(code, lv_color_white());
    lv_qrcode_set_quiet_zone(code, true);
    lv_obj_align(code, LV_ALIGN_CENTER, 0, 0);
    clearQrDisplay();
    // No blocking NTP wait: unsynchronised clocks reject new QR commands.
    configTime(0, 0, "pool.ntp.org", "time.google.com");
}
void handleQrDisplay(const String &payload, size_t size)
{
    if (size != payload.length() || size > 1024 || storageFault)
        return;
    JsonDocument doc;
    if (deserializeJson(doc, payload))
        return;
    String room = doc["room"] | "", session = doc["sessionId"].as<String>(), purpose = doc["purpose"] | "";
    String content = doc["content"] | "", expires = doc["expiresAt"] | "";
    int64_t expiry = QrRules::utc(expires.c_str());
    if (activeBusy() && !(active.phase == QrRules::CLOSED && active.purpose == "RETURN" && purpose == "RETURN" && active.session == session))
        return;
    if (room != activeMqttConfig.roomId || String(doc["action"] | "") != "display_qr" || !QrRules::session(session.c_str()) || !QrRules::purpose(purpose.c_str()) || !QrRules::content(content.c_str()) || !QrRules::fresh(time(nullptr), expiry, 180))
        return;
    if (lv_qrcode_update(code, content.c_str(), content.length()) != LV_RESULT_OK)
    {
        clearQrDisplay();
        return;
    }
    if (displaySession != session || displayPurpose != purpose || displayExpiry != expiry)
        displayScanNotified = false;
    displaySession = session;
    displayPurpose = purpose;
    displayExpiry = expiry;
    lv_obj_remove_flag(overlay, LV_OBJ_FLAG_HIDDEN);
    Serial.println("[QR] Display updated (token omitted).");
}
bool acceptQrScanFeedback(JsonDocument &doc)
{
    String room = doc["room"] | "", session = doc["sessionId"].as<String>();
    String purpose = doc["purpose"] | "", expires = doc["expiresAt"] | "";
    if (String(doc["action"] | "") != "qr_scanned" || room != activeMqttConfig.roomId ||
        displaySession.isEmpty() || session != displaySession || purpose != displayPurpose ||
        QrRules::utc(expires.c_str()) != displayExpiry || time(nullptr) >= displayExpiry || displayScanNotified)
        return false;
    displayScanNotified = true;
    return true;
}
bool handleQrAction(JsonDocument &doc, bool unlocked, bool actionPending)
{
    if (doc["commandId"].isNull() && doc["purpose"].isNull() && doc["sessionId"].isNull() && doc["expectedEquipment"].isNull())
        return false;
    String room = doc["room"] | "", command = doc["commandId"] | "", session = doc["sessionId"].as<String>(), purpose = doc["purpose"] | "";
    command.toLowerCase();
    if (room != activeMqttConfig.roomId || String(doc["action"] | "") != "open" || !QrRules::hex(command.c_str(), 32) || !QrRules::session(session.c_str()) || !QrRules::purpose(purpose.c_str()) || !validExpected(doc["expectedEquipment"]))
        return true;
    if (!storageReady || storageFault)
        return true;
    // Collision/corrupt entry is rejected rather than overwriting a receipt.
    if (storage.isKey(key(command).c_str()))
    {
        Receipt existing;
        if (!read(command, existing))
        {
            failStorage();
            return true;
        }
        if (existing.session != session || existing.purpose != purpose || existing.room != room)
            return true;
        // MQTT callbacks only enqueue; publishing from inside lwmqtt's receive
        // callback can recurse into the same transport while awaiting PUBACK.
        if (existing.phase != QrRules::ACCEPTED)
        {
            queuedReplay = existing;
            replayQueued = true;
        }
        return true; // Even a fresh retry must never actuate twice.
    }
    String expires = doc["expiresAt"] | "";
    int64_t expiry = QrRules::utc(expires.c_str());
    if (!QrRules::fresh(time(nullptr), expiry, 30) || unlocked || actionPending || pendingUnlock)
        return true;
    // Failed verification permits a replacement RETURN command for the SAME
    // session; pending receive/open commands cannot be superseded.
    if (activeBusy() && !(active.phase == QrRules::CLOSED && active.purpose == "RETURN" && purpose == "RETURN" && active.session == session))
        return true;
    Receipt next;
    next.room = room;
    next.session = session;
    next.command = command;
    next.purpose = purpose;
    serializeJson(doc["expectedEquipment"], next.expected);
    // Persist before touching the actuator. Receipts are never auto-evicted.
    if (!save(next) || storage.putString("active", command) != command.length())
    {
        failStorage();
        return true;
    }
    Receipt previous = active;
    active = next;
    pendingUnlock = true;
    resultsDirty = false;
    // Historical receipts only need identity/events for replay. Keep the
    // equipment snapshot for the active command, not every old command.
    if (!previous.command.isEmpty())
    {
        previous.expected = "";
        if (!save(previous))
            failStorage();
    }
    commandExpiry = expiry;
    clearQrDisplay();
    return true;
}
bool qrUnlockPending(String &sessionId)
{
    if (!pendingUnlock || storageFault)
        return false;
    pendingUnlock = false;
    if (!QrRules::fresh(time(nullptr), commandExpiry, 30))
    {
        active.phase = QrRules::FAILED;
        if (!save(active))
            failStorage();
        resultsDirty = true;
        return false;
    }
    sessionId = active.session;
    return true;
}
void qrUnlockExecuted()
{
    active.phase = QrRules::OPENED;
    if (!save(active))
        failStorage();
    resultsDirty = true;
}
void qrLockExecuted()
{
    if (active.command.isEmpty() || active.phase != QrRules::OPENED || active.room != activeMqttConfig.roomId)
        return;
    active.phase = QrRules::CLOSED;
    if (!save(active))
        failStorage();
    resultsDirty = true;
}
bool qrNeedsDoorClose() { return active.room == activeMqttConfig.roomId && active.phase == QrRules::OPENED && !active.command.isEmpty(); }
bool qrOwnsCabinet() { return pendingUnlock || activeBusy(); }
String qrExpectedEquipment() { return active.expected; }
bool qrVerificationContext(String &sessionId, String &commandId, String &expectedEquipment)
{
    if (!CLASSHUB_REQUIRE_RETURN_VERIFICATION || storageFault || verificationComplete() || active.room != activeMqttConfig.roomId || !QrRules::canVerify(active.phase, active.purpose == "RETURN"))
        return false;
    sessionId = active.session;
    commandId = active.command;
    expectedEquipment = active.expected;
    return true;
}
bool submitQrVerification(const String &sessionId, const String &commandId, bool rfidPassed, bool cameraPassed)
{
    if (!CLASSHUB_REQUIRE_RETURN_VERIFICATION || storageFault || verificationComplete() || active.room != activeMqttConfig.roomId || active.session != sessionId || active.command != commandId || !QrRules::canVerify(active.phase,active.purpose == "RETURN") || active.sequence == UINT32_MAX)
        return false;
    Receipt next = active;
    next.verified = true;
    next.rfid = rfidPassed;
    next.camera = cameraPassed;
    ++next.sequence;
    if (!save(next))
    {
        failStorage();
        return false;
    }
    active = next;
    resultsDirty = true;
    return true;
}
void replayQrResults()
{
    if (!active.command.isEmpty())
        resultsDirty = true;
}
void tickQrAccess()
{
    if (displayExpiry && time(nullptr) >= displayExpiry)
        clearQrDisplay();
    if (resultsDirty && millis() - lastResultAttempt >= 2000)
    {
        lastResultAttempt = millis();
        if (replay(active))
            resultsDirty = false;
    }
    else if (replayQueued && millis() - lastResultAttempt >= 2000)
    {
        lastResultAttempt = millis();
        if (replay(queuedReplay))
            replayQueued = false;
    }
}
