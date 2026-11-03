#pragma once
#ifdef RC522_HOST_TEST
#include "../tests/rc522_stubs.h"
#else
#include "driver/spi_master.h"
#endif
#include "rfid_uid.h"

// SCK/MOSI share the existing LCD bus. GPIO48's board RGB LED must stay unused.
// GPIO19 replaces GPIO6. Native USB D- is unavailable while used for RFID;
// programming/console remain on the user's external UART GPIO43/44.
#define RFID_PIN_MISO 19
#define RFID_PIN_CS 47
#define RFID_PIN_RST 48

typedef void (*rc522_uid_callback_t)(const rfid_uid_t *uid, void *context);
typedef struct {
    uint32_t attempts, spi_hz;
    uint8_t fast[3], slow[3];
    bool slow_tested;
    int miso_level;
    esp_err_t error;
} rc522_diagnostics_t;

#ifdef __cplusplus
extern "C" {
#endif
bool rc522_reader_ready(void);
void rc522_reader_diagnostics(rc522_diagnostics_t *out);

// Call once after SPI bus initialization. Callback runs in the reader task;
// its UID pointer is valid only until callback return. No actuator writes here.
esp_err_t rc522_reader_start(spi_host_device_t host, rc522_uid_callback_t callback, void *context);
#ifdef __cplusplus
}
#endif
