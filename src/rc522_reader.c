#include "rc522_reader.h"
#ifndef RC522_HOST_TEST
#include "driver/gpio.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#endif

// Register definitions/commands: NXP MFRC522 data sheet, Rev 3.9.
// https://www.nxp.com/docs/en/data-sheet/MFRC522.pdf
enum {
    COMMAND = 0x01, COM_IRQ = 0x04, ERROR_REG = 0x06, STATUS2 = 0x08,
    FIFO_DATA = 0x09, FIFO_LEVEL = 0x0A, CONTROL = 0x0C, BIT_FRAMING = 0x0D,
    COLL = 0x0E, MODE = 0x11, TX_MODE = 0x12, RX_MODE = 0x13,
    TX_CONTROL = 0x14, TX_ASK = 0x15, MOD_WIDTH = 0x24,
    T_MODE = 0x2A, T_PRESCALER = 0x2B, T_RELOAD_H = 0x2C, T_RELOAD_L = 0x2D,
    VERSION = 0x37
};
static const char *TAG = "RC522";
static spi_device_handle_t device;
static rc522_uid_callback_t on_uid;
static void *on_uid_context;
static bool started;
static uint32_t spi_hz = 1000000;
static portMUX_TYPE diagnostics_lock = portMUX_INITIALIZER_UNLOCKED;
static rc522_diagnostics_t diagnostics;
void rc522_reader_diagnostics(rc522_diagnostics_t *out)
{
    if (!out) return;
    portENTER_CRITICAL(&diagnostics_lock);
    *out = diagnostics;
    portEXIT_CRITICAL(&diagnostics_lock);
}

static esp_err_t write_reg(uint8_t reg, uint8_t value)
{
    spi_transaction_t t = {
        .flags = SPI_TRANS_USE_TXDATA,
        .cmd = (reg << 1) & 0x7E,
        .length = 8,
        .tx_data = {value},
    };
    return spi_device_transmit(device, &t);
}

static esp_err_t read_reg_at(uint8_t reg, uint8_t *value, uint32_t frequency)
{
    spi_transaction_t t = {
        // Exactly 8 command clocks, then 8 receive clocks under the same CS.
        // The receive buffer contains data only, never the command-phase byte.
        .flags = SPI_TRANS_USE_RXDATA,
        .cmd = 0x80 | ((reg << 1) & 0x7E),
        .length = 0,
        .rxlength = 8,
        .override_freq_hz = frequency,
    };
    esp_err_t err = spi_device_transmit(device, &t);
    if (err == ESP_OK) {
        *value = t.rx_data[0];
        if (frequency) spi_hz = frequency;
    }
    return err;
}
static esp_err_t read_reg(uint8_t reg, uint8_t *value) { return read_reg_at(reg, value, 0); }

#define TRY(call) do { esp_err_t e = (call); if (e != ESP_OK) return e; } while (0)

static esp_err_t probe_version(uint8_t *version)
{
    portENTER_CRITICAL(&diagnostics_lock);
    ++diagnostics.attempts;
    memset(diagnostics.fast, 0, sizeof(diagnostics.fast));
    memset(diagnostics.slow, 0, sizeof(diagnostics.slow));
    diagnostics.slow_tested = false;
    portEXIT_CRITICAL(&diagnostics_lock);
    // A short exclusive probe distinguishes stable replies without LCD traffic.
    TRY(spi_device_acquire_bus(device, portMAX_DELAY));
    const uint32_t rates[] = {1000000, 100000};
    esp_err_t result = ESP_ERR_NOT_FOUND;
    for (unsigned rate = 0; rate < 2; ++rate) {
        uint8_t samples[3] = {0};
        bool stable = true;
        for (unsigned n = 0; n < 3; ++n) {
            result = read_reg_at(VERSION, &samples[n], n == 0 ? rates[rate] : 0);
            if (result != ESP_OK) break;
            if (n && samples[n] != samples[0]) stable = false;
        }
        ESP_LOGI(TAG, "SPI_PROBE hz=%lu version=%02X,%02X,%02X stable=%d transport=%s RST_GPIO=%d CS_GPIO=%d MISO_GPIO=%d",
                 (unsigned long)rates[rate], samples[0], samples[1], samples[2], stable,
                 esp_err_to_name(result), RFID_PIN_RST, RFID_PIN_CS, RFID_PIN_MISO);
        portENTER_CRITICAL(&diagnostics_lock);
        memcpy(rate ? diagnostics.slow : diagnostics.fast, samples, sizeof(samples));
        if (rate) diagnostics.slow_tested = true;
        diagnostics.spi_hz = spi_hz;
        diagnostics.miso_level = gpio_get_level(RFID_PIN_MISO);
        portEXIT_CRITICAL(&diagnostics_lock);
        ESP_LOGI(TAG, "MISO_INPUT gpio=%d level=%d SPI_first=%02X hz=%lu", RFID_PIN_MISO,
                 gpio_get_level(RFID_PIN_MISO), samples[0], (unsigned long)rates[rate]);
        if (result != ESP_OK) break;
        const bool command_echo = stable && samples[0] == (0x80 | (VERSION << 1));
        if (command_echo) ESP_LOGW(TAG, "SPI_COMMAND_ECHO hz=%lu received EE (read command), not a valid version; trying next rate", (unsigned long)rates[rate]);
        if (stable && samples[0] != 0 && samples[0] != 0xFF && !command_echo) {
            *version = samples[0]; result = ESP_OK;
            break; // Keep the working device clock for normal register I/O.
        }
        result = ESP_ERR_NOT_FOUND;
    }
    spi_device_release_bus(device);
    if (result == ESP_ERR_NOT_FOUND)
        ESP_LOGW(TAG, "No stable chip reply at either rate; LED power does not verify SPI. Check voltage at module, RST and MISO; do not change camera/LCD wiring.");
    return result;
}

static esp_err_t initialize(void)
{
    TRY(gpio_set_level(RFID_PIN_RST, 0));
    vTaskDelay(pdMS_TO_TICKS(5));
    TRY(gpio_set_level(RFID_PIN_RST, 1));
    vTaskDelay(pdMS_TO_TICKS(50));
    uint8_t version;
    TRY(probe_version(&version));
    TRY(write_reg(COMMAND, 0x0F)); // SoftReset
    int64_t deadline = esp_timer_get_time() + 100000;
    uint8_t command;
    do {
        vTaskDelay(1);
        TRY(read_reg(COMMAND, &command));
        if (esp_timer_get_time() >= deadline) return ESP_ERR_TIMEOUT;
    } while (command & 0x10);
    TRY(write_reg(TX_MODE, 0)); // 106 kBd, CRC checked in software
    TRY(write_reg(RX_MODE, 0));
    TRY(write_reg(MOD_WIDTH, 0x26));
    TRY(write_reg(T_MODE, 0x80)); // Auto-start timer after transmission
    TRY(write_reg(T_PRESCALER, 0xA9)); // 40 kHz
    TRY(write_reg(T_RELOAD_H, 0x03)); // 1000 ticks = 25 ms
    TRY(write_reg(T_RELOAD_L, 0xE8));
    TRY(write_reg(TX_ASK, 0x40)); // 100% ASK
    TRY(write_reg(MODE, 0x3D));
    TRY(write_reg(STATUS2, 0)); // Crypto1 off: UID only, no sector access
    TRY(write_reg(TX_CONTROL, 0x83)); // Antenna drivers enabled
    uint8_t mode;
    TRY(read_reg(MODE, &mode));
    // Reserved ModeReg bits may read differently on compatible silicon.
    if ((mode & 0xAB) != (0x3D & 0xAB)) {
        ESP_LOGW(TAG, "REGISTER_CHECK MODE wrote=3D read=%02X; SPI read/write not verified", mode);
        return ESP_ERR_INVALID_RESPONSE;
    }
    ESP_LOGI(TAG, "READY version=0x%02X SPI=%lu Hz CS=%d RST=%d; MODE readback passed; present one tag at a time",
             version, (unsigned long)spi_hz, RFID_PIN_CS, RFID_PIN_RST);
    return ESP_OK;
}

static struct {
    const char *stage;
    uint8_t irq, error, count, valid_bits;
} rf_detail;

static esp_err_t exchange(const char *stage, const uint8_t *tx, size_t length, uint8_t tx_bits,
                          uint8_t *rx, size_t expected)
{
    rf_detail.stage=stage; rf_detail.irq=rf_detail.error=rf_detail.count=rf_detail.valid_bits=0;
    if (length > 64 || expected > 64) return ESP_ERR_INVALID_SIZE;
    TRY(write_reg(COMMAND, 0));
    TRY(write_reg(COM_IRQ, 0x7F));
    TRY(write_reg(FIFO_LEVEL, 0x80));
    for (size_t i = 0; i < length; ++i) TRY(write_reg(FIFO_DATA, tx[i]));
    TRY(write_reg(BIT_FRAMING, tx_bits));
    TRY(write_reg(COMMAND, 0x0C)); // Transceive
    TRY(write_reg(BIT_FRAMING, tx_bits | 0x80)); // StartSend
    int64_t deadline = esp_timer_get_time() + 60000;
    uint8_t irq;
    for (;;) {
        TRY(read_reg(COM_IRQ, &irq));
        rf_detail.irq=irq;
        if (irq & 0x30) break; // Rx/Idle
        if ((irq & 1) || esp_timer_get_time() >= deadline) {
            TRY(write_reg(COMMAND, 0));
            TRY(write_reg(BIT_FRAMING, 0));
            return ESP_ERR_TIMEOUT;
        }
        vTaskDelay(1);
    }
    TRY(write_reg(BIT_FRAMING, 0));
    uint8_t error, count, control;
    TRY(read_reg(ERROR_REG, &error));
    TRY(read_reg(FIFO_LEVEL, &count));
    TRY(read_reg(CONTROL, &control));
    rf_detail.error=error; rf_detail.count=count; rf_detail.valid_bits=control&7;
    // CRC checking is disabled in RxMode. WUPA/anticollision have no CRC;
    // SELECT SAK CRC is validated explicitly by rfid_uid_append().
    // Still reject write, thermal, overflow, collision, parity and protocol errors.
    if (error & 0xDB) return ESP_ERR_INVALID_RESPONSE;
    if (count != expected || (control & 7)) return ESP_ERR_INVALID_RESPONSE;
    for (size_t i = 0; i < count; ++i) TRY(read_reg(FIFO_DATA, rx + i));
    return ESP_OK;
}

static esp_err_t scan(rfid_uid_t *uid)
{
    uid->length = 0;
    // WUPA includes halted tags, allowing held-tag detection without repeated events.
    const uint8_t wake = 0x52;
    uint8_t atqa[2];
    TRY(write_reg(COLL, 0x80));
    TRY(exchange("WUPA", &wake, 1, 7, atqa, sizeof(atqa)));
    for (unsigned level = 0; level < 3; ++level) {
        uint8_t select[9] = {(uint8_t)(0x93 + level * 2), 0x20};
        uint8_t block[5], sak[3];
        TRY(exchange("ANTICOLL", select, 2, 0, block, sizeof(block)));
        // Do not SELECT a corrupt anticollision block.
        if ((uint8_t)(block[0] ^ block[1] ^ block[2] ^ block[3]) != block[4])
            return ESP_ERR_INVALID_CRC;
        select[1] = 0x70;
        memcpy(select + 2, block, 5);
        uint16_t crc = rfid_crc_a(select, 7);
        select[7] = crc;
        select[8] = crc >> 8;
        TRY(exchange("SELECT", select, sizeof(select), 0, sak, sizeof(sak)));
        if (!rfid_uid_append(uid, level, block, sak)) { rf_detail.stage="SAK_CHECK"; return ESP_ERR_INVALID_RESPONSE; }
        if (!(sak[0] & 4)) {
            uint8_t halt[] = {0x50, 0, 0x57, 0xCD};
            uint8_t ignored;
            // A selected tag must remain silent for HALT. Next scan uses WUPA.
            esp_err_t err = exchange("HALT", halt, sizeof(halt), 0, &ignored, 0);
            return err == ESP_ERR_TIMEOUT ? ESP_OK : ESP_ERR_INVALID_RESPONSE;
        }
    }
    return ESP_ERR_INVALID_RESPONSE;
}

static bool reader_ready;
bool rc522_reader_ready(void) { return __atomic_load_n(&reader_ready, __ATOMIC_RELAXED); }

static void reader_task(void *arg)
{
    (void)arg;
    rfid_uid_t last = {0};
    int64_t last_seen = 0, last_warning = 0;
    bool ready = false;
    for (;;) {
        if (!ready) {
            esp_err_t err = initialize();
            portENTER_CRITICAL(&diagnostics_lock);
            diagnostics.error = err;
            portEXIT_CRITICAL(&diagnostics_lock);
            if (err != ESP_OK) {
                ESP_LOGW(TAG, "NOT_READY %s; check 3.3V and wiring; retry in 5s", esp_err_to_name(err));
                vTaskDelay(pdMS_TO_TICKS(5000));
                continue;
            }
            ready = true;
            __atomic_store_n(&reader_ready, true, __ATOMIC_RELAXED);
        }
        rfid_uid_t uid = {0};
        esp_err_t err = scan(&uid);
        int64_t now = esp_timer_get_time();
        if (err == ESP_OK) {
            bool changed = uid.length != last.length || memcmp(uid.bytes, last.bytes, uid.length);
            if (changed || now - last_seen >= 800000) {
                char hex[21];
                if (rfid_uid_hex(&uid, hex)) {
                    ESP_LOGI(TAG, "RFID_UID=%s bytes=%u (read only; not a return verification)", hex, uid.length);
                    if (on_uid) on_uid(&uid, on_uid_context);
                }
            }
            last = uid;
            last_seen = now;
        } else if (err != ESP_ERR_TIMEOUT && now - last_warning >= 5000000) {
            ESP_LOGW(TAG, "READ_FAILED %s stage=%s IRQ=%02X ERROR=%02X FIFO=%u valid_bits=%u; present a single tag",
                     esp_err_to_name(err),rf_detail.stage?rf_detail.stage:"SPI",rf_detail.irq,
                     rf_detail.error,rf_detail.count,rf_detail.valid_bits);
            last_warning = now;
        }
        // Reprobe a disconnected module even when its floating MISO looks like silence.
        uint8_t version;
        if (read_reg(VERSION, &version) != ESP_OK || version == 0 || version == 0xFF) {
            ready = false;
            __atomic_store_n(&reader_ready, false, __ATOMIC_RELAXED);
        }
        vTaskDelay(pdMS_TO_TICKS(150));
    }
}

esp_err_t rc522_reader_start(spi_host_device_t host, rc522_uid_callback_t callback, void *context)
{
    if (started) return ESP_ERR_INVALID_STATE;
    // Deselect before any LCD transaction, even while the module is disconnected.
    TRY(gpio_set_level(RFID_PIN_CS, 1));
    TRY(gpio_set_direction(RFID_PIN_CS, GPIO_MODE_OUTPUT));
    TRY(gpio_set_level(RFID_PIN_RST, 1));
    TRY(gpio_set_direction(RFID_PIN_RST, GPIO_MODE_OUTPUT));
    spi_device_interface_config_t cfg = {
        .command_bits = 8,
        .clock_speed_hz = 1000000, .mode = 0,
        .cs_ena_pretrans = 1, .cs_ena_posttrans = 1,
        .spics_io_num = RFID_PIN_CS, .queue_size = 1,
        .flags = SPI_DEVICE_HALFDUPLEX | SPI_DEVICE_NO_DUMMY,
    };
    TRY(spi_bus_add_device(host, &cfg, &device));
    // Keep the digital input buffer enabled explicitly on the shared bus.
    // Do not reset the pin or change its matrix routing/output function.
    TRY(gpio_input_enable(RFID_PIN_MISO));
    on_uid = callback;
    on_uid_context = context;
    if (xTaskCreate(reader_task, "rc522", 4096, NULL, 2, NULL) != pdPASS) {
        spi_bus_remove_device(device);
        device = NULL;
        return ESP_ERR_NO_MEM;
    }
    started = true;
    return ESP_OK;
}
