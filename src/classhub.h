#pragma once
#include <stdbool.h>
#include <stdint.h>
#ifdef CLASSHUB_HOST_TEST
#include "../tests/idf_stubs.hpp"
#else
#include "esp_err.h"
#include "esp_http_server.h"
#endif

// Camera and TFT wiring remain in main.c. These GPIOs do not overlap them.
#define CLASSHUB_LATCH_PIN 1
#define CLASSHUB_CLOSE_PIN 2 // Pull-up: switch to GND; LOW = closed, HIGH = open.
#define CLASSHUB_BUZZER_PIN 21
#define CLASSHUB_SETUP_PIN 0 // BOOT button, active LOW; hold 3 seconds after boot.
#define CLASSHUB_REQUIRE_RETURN_VERIFICATION 0
#define CLASSHUB_QR_SIDE_MAX 57

typedef struct
{
    uint32_t generation;
    int qr_size;
    uint8_t qr[CLASSHUB_QR_SIDE_MAX * CLASSHUB_QR_SIDE_MAX];
    char title[32];
    char detail[32];
    bool online;
} classhub_display_t;

#ifdef __cplusplus
extern "C"
{
#endif
    // Initialize safe outputs/network once, independently of camera diagnostics.
    esp_err_t classhub_start(void);
    esp_err_t classhub_register_http(httpd_handle_t server);
    void classhub_observe_rfid(const char *uid);
    bool classhub_ota_active(void);
    bool classhub_display_snapshot(classhub_display_t *out, uint32_t previous_generation);
    // Enqueued and validated by the cabinet task; no ISR calls.
    bool classhub_submit_verification(const char *session, const char *command,
                                      bool rfid_passed, bool camera_passed);
#ifdef __cplusplus
}
#endif
