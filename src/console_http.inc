esp_err_t console_get(httpd_req_t *r) {
    auto j = object();
    xSemaphoreTake(console_mutex, portMAX_DELAY);
    put(j.get(), "uid", latest_uid); put(j.get(), "result", console_result);
    cJSON_AddNumberToObject(j.get(), "uidAgeMs", latest_uid.empty() ? -1 : ms() - uid_at);
    cJSON_AddNumberToObject(j.get(), "requestId", console_id);
    cJSON_AddBoolToObject(j.get(), "pending", console_pending);
    cJSON_AddBoolToObject(j.get(), "storageReady", tags_ok);
    auto tags = parse(tags_json); cJSON_AddItemToObject(j.get(), "tags", tags.release());
    xSemaphoreGive(console_mutex);
    cJSON_AddBoolToObject(j.get(), "mqttReady", mqtt_ready.load());
    cJSON_AddBoolToObject(j.get(), "wifiReady", ip_ready.load());
    cJSON_AddBoolToObject(j.get(), "apActive", ap_active.load());
    esp_netif_ip_info_t ip = {};
    esp_netif_t *sta = esp_netif_get_handle_from_ifkey("WIFI_STA_DEF");
    char address[16] = {};
    if (ip_ready && sta && esp_netif_get_ip_info(sta, &ip)==ESP_OK)
        snprintf(address,sizeof(address),IPSTR,IP2STR(&ip.ip));
    put(j.get(),"lanIp",address);
    cJSON_AddBoolToObject(j.get(), "readerReady", rc522_reader_ready());
    const esp_app_desc_t *firmware = esp_app_get_description();
    put(j.get(),"firmwareId","camera-jpeg-v7-miso19");
    put(j.get(),"firmwareBuild",std::string(firmware->date)+" "+firmware->time);
    rc522_diagnostics_t probe = {}; rc522_reader_diagnostics(&probe);
    cJSON *rfid = cJSON_AddObjectToObject(j.get(),"rfidProbe");
    cJSON_AddNumberToObject(rfid,"attempts",probe.attempts);
    cJSON_AddNumberToObject(rfid,"hz",probe.spi_hz);
    cJSON_AddBoolToObject(rfid,"slowTested",probe.slow_tested);
    cJSON_AddNumberToObject(rfid,"misoLevel",probe.miso_level);
    cJSON_AddNumberToObject(rfid,"misoPin",RFID_PIN_MISO);
    char sample[16];
    snprintf(sample,sizeof(sample),"%02X,%02X,%02X",probe.fast[0],probe.fast[1],probe.fast[2]); put(rfid,"fast",sample);
    snprintf(sample,sizeof(sample),"%02X,%02X,%02X",probe.slow[0],probe.slow[1],probe.slow[2]); put(rfid,"slow",sample);
    put(rfid,"error",probe.attempts ? esp_err_to_name(probe.error) : "NOT_STARTED");
    const esp_partition_t *ota = esp_ota_get_next_update_partition(nullptr);
    cJSON_AddNumberToObject(j.get(), "otaMaxBytes", ota ? ota->size : 0);
    cJSON_AddBoolToObject(j.get(), "otaActive", ota_gate.blocked());
    const esp_partition_t *running = esp_ota_get_running_partition();
    put(j.get(), "firmwareSlot", running ? running->label : "unknown");
    xSemaphoreTake(config_mutex, portMAX_DELAY);
    put(j.get(), "host", config.host); put(j.get(), "room", config.room);
    cJSON_AddNumberToObject(j.get(), "port", config.port);
    xSemaphoreGive(config_mutex);
    httpd_resp_set_type(r, "application/json"); httpd_resp_set_hdr(r, "Cache-Control", "no-store");
    auto body = dump(j.get()); return httpd_resp_send(r, body.c_str(), body.size());
}
esp_err_t console_post(httpd_req_t *r) {
    if (ota_gate.blocked()) { httpd_resp_set_status(r,"409 Conflict"); return httpd_resp_sendstr(r,"OTA maintenance"); }
    char header[8] = {};
    if (httpd_req_get_hdr_value_str(r, "X-ClassHub-Console", header, sizeof(header)) != ESP_OK || strcmp(header, "1"))
        return httpd_resp_send_err(r, HTTPD_400_BAD_REQUEST, "Console header required");
    if (r->content_len <= 0 || r->content_len > 512) return httpd_resp_send_err(r, HTTPD_400_BAD_REQUEST, "Invalid length");
    std::string body(r->content_len, '\0'); int received = 0;
    while (received < r->content_len) {
        int n = httpd_req_recv(r, &body[received], r->content_len - received);
        if (n <= 0) return ESP_FAIL;
        received += n;
    }
    auto j = parse(body);
    if (body.find('\0') != std::string::npos || !j || !cJSON_IsObject(j.get()))
        return httpd_resp_send_err(r, HTTPD_400_BAD_REQUEST, "Invalid JSON");
    auto verb = str(j.get(), "action");
    if (verb != "save" && verb != "delete" && verb != "open" && verb != "beep" && verb != "mqtt")
        return httpd_resp_send_err(r, HTTPD_400_BAD_REQUEST, "Unknown action");
    xSemaphoreTake(console_mutex, portMAX_DELAY);
    if (console_pending) {
        xSemaphoreGive(console_mutex); httpd_resp_set_status(r, "409 Conflict");
        return httpd_resp_sendstr(r, "Previous request still running");
    }
    console_pending = true; console_result = "Processing"; console_queued_at = ms(); ++console_id;
    if (!enqueue(CONSOLE, body)) {
        console_pending = false; console_result = "Error: command queue full";
        xSemaphoreGive(console_mutex); httpd_resp_set_status(r, "503 Service Unavailable");
        return httpd_resp_sendstr(r, "Command queue full");
    }
    auto response = object(); cJSON_AddNumberToObject(response.get(), "requestId", console_id);
    xSemaphoreGive(console_mutex);
    httpd_resp_set_status(r, "202 Accepted"); httpd_resp_set_type(r, "application/json");
    auto result = dump(response.get()); return httpd_resp_send(r, result.c_str(), result.size());
}
