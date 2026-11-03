// Cabinet task owns flash and actuators; HTTP and RFID only exchange snapshots/commands.
SemaphoreHandle_t console_mutex;
QueueHandle_t publish_acks;
nvs_handle_t tag_store = 0;
std::string tags_json = "[]", latest_uid, console_result = "Ready";
bool tags_ok = false, console_pending = false;
uint32_t console_id = 0, test_epoch = 0;
int test_message = -1;
int64_t console_queued_at = 0, test_deadline = 0, uid_at = 0;
struct PublishAck { uint32_t epoch; int id; };

bool valid_uid(const std::string &uid) {
    return (uid.size() == 8 || uid.size() == 14 || uid.size() == 20) &&
        uid.find_first_not_of("0123456789ABCDEF") == std::string::npos;
}
bool valid_tags(const cJSON *j) {
    if (!cJSON_IsArray(j) || cJSON_GetArraySize(j) > 32) return false;
    std::string seen;
    const cJSON *entry;
    cJSON_ArrayForEach(entry, j) {
        auto uid = str(entry, "uid"), name = str(entry, "name");
        if (!valid_uid(uid) || name.size() > 48 || seen.find("/" + uid + "/") != std::string::npos) return false;
        seen += "/" + uid + "/";
    }
    return true;
}
void console_finish(const std::string &result) {
    xSemaphoreTake(console_mutex, portMAX_DELAY);
    console_result = result; console_pending = false;
    xSemaphoreGive(console_mutex);
    test_message = -1;
}
void console_command(const cJSON *j) {
    if (ota_gate.blocked()) { console_finish("Error: OTA maintenance"); return; }
    if (ms() - console_queued_at > 3000) { console_finish("Error: request expired; retry"); return; }
    auto verb = str(j, "action");
    if (verb == "save" || verb == "delete") {
        auto uid = str(j, "uid"), name = str(j, "name");
        if (!tags_ok || !valid_uid(uid) || name.size() > 48) { console_finish("Error: RFID storage or invalid UID/name"); return; }
        auto list = parse(tags_json);
        int index = -1;
        for (int i = 0; i < cJSON_GetArraySize(list.get()); ++i)
            if (str(cJSON_GetArrayItem(list.get(), i), "uid") == uid) index = i;
        if (index >= 0) cJSON_DeleteItemFromArray(list.get(), index);
        if (verb == "save") {
            if (cJSON_GetArraySize(list.get()) >= 32) { console_finish("Error: maximum 32 tags"); return; }
            auto entry = object(); put(entry.get(), "uid", uid); put(entry.get(), "name", name);
            cJSON_AddItemToArray(list.get(), entry.release());
        }
        auto next = dump(list.get());
        if (!write_string(tag_store, "tags", next)) { console_finish("Error: flash write failed"); return; }
        xSemaphoreTake(console_mutex, portMAX_DELAY); tags_json = next; xSemaphoreGive(console_mutex);
        console_finish("OK: RFID saved to flash");
    } else if (verb == "open" || verb == "beep") {
        if (storage_fault || busy() || unlocked || pulse_active || display_expiry > utc_now()) {
            console_finish("Error: cabinet busy; finish current session first"); return;
        }
        if (verb == "open") { current_session.clear(); unlock(""); }
        else beep(300);
        console_finish(verb == "open" ? "OK: unlock pulse started (1500 ms); check physical door" : "OK: buzzer pulse started (300 ms)");
    } else if (verb == "mqtt") {
        if (!connected || !mqtt_ready) { console_finish("Error: MQTT offline or subscriptions not ready"); return; }
        PublishAck old;
        while (xQueueReceive(publish_acks, &old, 0) == pdTRUE) {}
        test_epoch = connection_epoch.load(); test_deadline = ms() + 10000;
        auto payload = object(); put(payload.get(), "event", "web_connection_test");
        cJSON_AddNumberToObject(payload.get(), "requestId", console_id);
        test_message = publish(topic("diagnostics"), dump(payload.get()));
        if (test_message < 0) console_finish("Error: MQTT publish failed");
    } else console_finish("Error: unknown action");
}
void console_tick() {
    PublishAck ack;
    while (xQueueReceive(publish_acks, &ack, 0) == pdTRUE)
        if (test_message >= 0 && connected && connection_epoch.load() == test_epoch &&
            ack.epoch == test_epoch && ack.id == test_message)
            console_finish("OK: MQTT broker confirmed QoS 1 PUBACK (not backend application acknowledgement)");
    if (test_message >= 0 && (!connected || connection_epoch.load() != test_epoch || ms() >= test_deadline))
        console_finish("Error: MQTT disconnected or PUBACK timeout");
}
