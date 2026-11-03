void ota_service() {
    xSemaphoreTake(console_mutex, portMAX_DELAY);
    const bool pending = console_pending;
    xSemaphoreGive(console_mutex);
    ota_gate.decide(!storage_fault && !busy() && !unlocked && !pulse_active &&
        display_expiry <= utc_now() && buzzer_until <= ms() && !pending);
#ifndef CLASSHUB_HOST_TEST
    if (ota_gate.state.load() == OtaGate::REBOOT && ms() >= ota_restart_at.load()) esp_restart();
    // Give startup ten seconds to establish the cabinet task and HTTP console.
    // An unconfirmed OTA image rolls back on reset if startup aborts.
    static bool boot_checked = false;
    if (!boot_checked && ms() >= 10000 && ota_http_ready && !storage_fault) {
        esp_ota_img_states_t state;
        if (esp_ota_get_state_partition(esp_ota_get_running_partition(), &state) == ESP_OK && state == ESP_OTA_IMG_PENDING_VERIFY)
            ESP_ERROR_CHECK(esp_ota_mark_app_valid_cancel_rollback());
        boot_checked = true;
    }
#endif
}
