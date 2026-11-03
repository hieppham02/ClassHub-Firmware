// Raw application .bin only. Never accepts a bootloader, partition table or merged image.
esp_err_t ota_post(httpd_req_t *r) {
    char header[8] = {};
    if (httpd_req_get_hdr_value_str(r, "X-ClassHub-Console", header, sizeof(header)) != ESP_OK || strcmp(header, "1"))
        return httpd_resp_send_err(r, HTTPD_400_BAD_REQUEST, "Console header required");
    const esp_partition_t *partition = esp_ota_get_next_update_partition(nullptr);
    if (!partition || r->content_len < 288 || static_cast<size_t>(r->content_len) > partition->size)
        return httpd_resp_send_err(r, HTTPD_400_BAD_REQUEST, "Invalid image size or OTA partitions unavailable; install OTA firmware via USB first");
    if (!ota_gate.request()) {
        httpd_resp_set_status(r, "409 Conflict"); return httpd_resp_sendstr(r, "OTA already running");
    }
    for (int n=0; n<200 && ota_gate.state.load()==OtaGate::REQUESTED; ++n) vTaskDelay(pdMS_TO_TICKS(10));
    if (ota_gate.state.load()!=OtaGate::GRANTED) {
        ota_gate.release(); httpd_resp_set_status(r, "409 Conflict");
        return httpd_resp_sendstr(r, "Cabinet busy or unavailable; close door and finish current session first");
    }
    esp_ota_handle_t handle = 0;
    bool begun = false;
    auto fail = [&](const char *message) {
        if (begun) esp_ota_abort(handle);
        ota_gate.release();
        // Unread upload bytes cannot be interpreted as another request.
        httpd_resp_set_hdr(r, "Connection", "close");
        httpd_resp_send_err(r, HTTPD_400_BAD_REQUEST, message);
        return ESP_FAIL;
    };
    uint8_t buffer[2048];
    int received=0;
    const int64_t deadline=ms()+120000;
    // Accumulate the full application descriptor even with short TCP reads.
    constexpr size_t prefix_size=sizeof(esp_image_header_t)+sizeof(esp_image_segment_header_t)+sizeof(esp_app_desc_t);
    static_assert(prefix_size<=sizeof(buffer), "OTA prefix buffer");
    while (received < static_cast<int>(prefix_size)) {
        if (ms()>=deadline) return fail("OTA upload timed out");
        int n=httpd_req_recv(r,reinterpret_cast<char *>(buffer)+received,prefix_size-received);
        if (n<=0) return fail("OTA image header incomplete");
        received+=n;
    }
    esp_app_desc_t descriptor;
    memcpy(&descriptor,buffer+sizeof(esp_image_header_t)+sizeof(esp_image_segment_header_t),sizeof(descriptor));
    if (buffer[0]!=0xE9 || descriptor.magic_word!=ESP_APP_DESC_MAGIC_WORD ||
        memcmp(descriptor.project_name,esp_app_get_description()->project_name,sizeof(descriptor.project_name)))
        return fail("Select this project's application firmware.bin, not a bootloader or merged image");
    if (esp_ota_begin(partition,r->content_len,&handle)!=ESP_OK) return fail("Cannot initialize OTA flash writer");
    begun=true;
    if (esp_ota_write(handle,buffer,received)!=ESP_OK) return fail("OTA header write failed");
    while (received < r->content_len) {
        if (ms() >= deadline) return fail("OTA upload timed out; current firmware retained");
        int n=httpd_req_recv(r, reinterpret_cast<char *>(buffer), std::min<int>(sizeof(buffer),r->content_len-received));
        if (n<=0) return fail("OTA upload interrupted; current firmware retained");
        if (esp_ota_write(handle,buffer,n)!=ESP_OK) return fail("OTA flash write failed; current firmware retained");
        received+=n;
    }
    const esp_err_t verified=esp_ota_end(handle); begun=false;
    if (verified!=ESP_OK) return fail("Image verification failed; select firmware.bin built for this ESP32-S3");
    if (esp_ota_set_boot_partition(partition)!=ESP_OK) return fail("Cannot select updated firmware for boot");
    httpd_resp_set_type(r,"application/json"); httpd_resp_set_hdr(r,"Connection","close");
    esp_err_t result=httpd_resp_sendstr(r,"{\"ok\":true,\"rebooting\":true}");
    ota_restart_at=ms()+1500; ota_gate.state=OtaGate::REBOOT;
    return result;
}
