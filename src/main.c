#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include "lwip/sockets.h"
#include "camera_stream_io.h"
#include "camera_profile.h"
#include "classhub.h"
#include "rc522_reader.h"
#include "driver/i2c_master.h"
#include "driver/ledc.h"
#include "driver/gpio.h"
#include "driver/pulse_cnt.h"
#include "driver/spi_master.h"
#include "esp_camera.h"
#include "img_converters.h"
#include "esp_timer.h"
#include "esp_err.h"
#include "esp_log.h"
#include "esp_wifi.h"
#include "esp_event.h"
#include "esp_netif.h"
#include "esp_http_server.h"
#include "esp_heap_caps.h"
#include "nvs_flash.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"

// OV7670 -> ESP32-S3 GPIO mapping, matching testxxxx/include/test_config.h.
// PWDN=LOW and RESET=HIGH externally; -1 means no GPIO control.
// D2 currently uses GPIO8, previously GPIO6.
#define CAM_PIN_PWDN -1
#define CAM_PIN_RESET -1
#define CAM_PIN_SIOD 9
#define CAM_PIN_SIOC 10
#define CAM_PIN_XCLK 11
#define CAM_PIN_D0 4
#define CAM_PIN_D1 5
#define CAM_PIN_D2 8
#define CAM_PIN_D3 7
#define CAM_PIN_D4 15
#define CAM_PIN_D5 16
#define CAM_PIN_D6 17
#define CAM_PIN_D7 18
#define CAM_PIN_VSYNC 14
#define CAM_PIN_HREF 12
#define CAM_PIN_PCLK 13

#define TEST_ADDRESS 0x21 // 7-bit address; do not use the wire bytes 0x42/0x43.
#define TEST_XCLK_HZ 10000000
#define TEST_SCCB_HZ 100000
#define CAPTURE_CLKRC_VALUE 0x01 // Faster capture trial; keep XCLK, data pins and pixel format unchanged.
#define CAPTURE_HSTOP_VALUE 14   // Native VGA window from the OV7670 driver.
#define CAPTURE_HREF_WINDOW (((CAPTURE_HSTOP_VALUE & 0x07) << 3) | (158 & 0x07))
#define CAPTURE_REVERSE_PCLK 0 // Restore original phase: reversing PCLK did not improve colors.
#define TEST_ROUNDS 100
#define TEST_TIMEOUT_MS 100
#define TEST_MAX_ERROR_LOGS 8

// User wiring: 38 39 40 41 42 -> CS RST A0 SDA SCK.
#define TFT_PIN_CS 38
#define TFT_PIN_RST 39
#define TFT_PIN_DC 40   // A0 / command-data select
#define TFT_PIN_MOSI 41 // SDA on the SPI display
#define TFT_PIN_SCK 42
#define TFT_PIN_BL -1 // Backlight powered externally; no GPIO specified.
#define TFT_WIDTH 160 // Standard 128x160 ST7735, landscape.
#define TFT_HEIGHT 128
#define TFT_X_OFFSET 0
#define TFT_Y_OFFSET 0
#define TFT_MADCTL 0xA0         // Landscape, RGB (red/blue order corrected for this panel).
#define TFT_SWAP_CAMERA_BYTES 0 // Byte swap did not resolve camera colors; retain native order.
#define CAMERA_WEB_LIVE_VIEW 1
#define TFT_SPI_HZ 20000000
#define TFT_SPI_CHUNK_BYTES 3840 // 12 RGB565 rows; below the SPI hardware transaction limit.
#define CAMERA_COLORBAR_DIAGNOSTIC 0
#define CAMERA_STARTUP_DIAGNOSTICS 0 // Enable extended electrical/register diagnostics when needed.

static const char *TAG = "SCCB_ONLY";

static spi_device_handle_t tft_spi;
static uint8_t tft_line[TFT_WIDTH * 2] __attribute__((aligned(4)));

static esp_err_t tft_transfer(bool data, const void *bytes, size_t length)
{
    if (!tft_spi || !bytes || length == 0)
        return ESP_ERR_INVALID_ARG;
    esp_err_t error = gpio_set_level(TFT_PIN_DC, data);
    if (error != ESP_OK)
        return error;
    const uint8_t *cursor = bytes;
    while (length > 0)
    {
        size_t count = length > TFT_SPI_CHUNK_BYTES ? TFT_SPI_CHUNK_BYTES : length;
        spi_transaction_t transaction = {.length = count * 8, .tx_buffer = cursor};
        error = spi_device_polling_transmit(tft_spi, &transaction);
        if (error != ESP_OK)
            return error;
        cursor += count;
        length -= count;
    }
    return ESP_OK;
}

static esp_err_t tft_command(uint8_t command, const uint8_t *data, size_t length)
{
    esp_err_t error = tft_transfer(false, &command, 1);
    if (error != ESP_OK || length == 0)
        return error;
    return tft_transfer(true, data, length);
}

#define TFT_TRY(call)                 \
    do                                \
    {                                 \
        esp_err_t tft_error = (call); \
        if (tft_error != ESP_OK)      \
            return tft_error;         \
    } while (0)

static esp_err_t tft_window(unsigned x, unsigned y, unsigned width, unsigned height)
{
    if (!width || !height || x + width > TFT_WIDTH || y + height > TFT_HEIGHT)
        return ESP_ERR_INVALID_ARG;
    unsigned x0 = x + TFT_X_OFFSET, x1 = x0 + width - 1;
    unsigned y0 = y + TFT_Y_OFFSET, y1 = y0 + height - 1;
    uint8_t columns[] = {x0 >> 8, x0, x1 >> 8, x1};
    uint8_t rows[] = {y0 >> 8, y0, y1 >> 8, y1};
    TFT_TRY(tft_command(0x2A, columns, sizeof(columns)));
    TFT_TRY(tft_command(0x2B, rows, sizeof(rows)));
    return tft_command(0x2C, NULL, 0);
}

static esp_err_t tft_init(void)
{
    // RC522 shares SCK/MOSI; keep it deselected during LCD initialization.
    TFT_TRY(gpio_set_level(RFID_PIN_CS, 1));
    TFT_TRY(gpio_set_direction(RFID_PIN_CS, GPIO_MODE_OUTPUT));
    gpio_config_t pins = {
        .pin_bit_mask = (1ULL << TFT_PIN_DC) | (1ULL << TFT_PIN_RST),
        .mode = GPIO_MODE_OUTPUT,
    };
    TFT_TRY(gpio_config(&pins));
    if (TFT_PIN_BL >= 0)
    {
        TFT_TRY(gpio_set_direction(TFT_PIN_BL, GPIO_MODE_OUTPUT));
        TFT_TRY(gpio_set_level(TFT_PIN_BL, 1));
    }
    const spi_bus_config_t bus = {
        .mosi_io_num = TFT_PIN_MOSI,
        .miso_io_num = RFID_PIN_MISO,
        .sclk_io_num = TFT_PIN_SCK,
        .quadwp_io_num = -1,
        .quadhd_io_num = -1,
        .max_transfer_sz = TFT_SPI_CHUNK_BYTES,
    };
    TFT_TRY(spi_bus_initialize(SPI2_HOST, &bus, SPI_DMA_CH_AUTO));
    const spi_device_interface_config_t device = {
        .clock_speed_hz = TFT_SPI_HZ,
        .mode = 0,
        .spics_io_num = TFT_PIN_CS,
        .queue_size = 1,
    };
    TFT_TRY(spi_bus_add_device(SPI2_HOST, &device, &tft_spi));
    TFT_TRY(gpio_set_level(TFT_PIN_RST, 0));
    vTaskDelay(pdMS_TO_TICKS(20));
    TFT_TRY(gpio_set_level(TFT_PIN_RST, 1));
    vTaskDelay(pdMS_TO_TICKS(150));
    TFT_TRY(tft_command(0x01, NULL, 0)); // Software reset
    vTaskDelay(pdMS_TO_TICKS(150));
    TFT_TRY(tft_command(0x11, NULL, 0)); // Sleep out
    vTaskDelay(pdMS_TO_TICKS(150));
    // Standard ST7735R initialization (frame rate, power, gamma).
    // Reference: github.com/adafruit/Adafruit-ST7735-Library, Adafruit_ST7735.cpp.
    static const struct
    {
        uint8_t command, length, data[16];
    } init[] = {
        {0xB1, 3, {0x01, 0x2C, 0x2D}},
        {0xB2, 3, {0x01, 0x2C, 0x2D}},
        {0xB3, 6, {0x01, 0x2C, 0x2D, 0x01, 0x2C, 0x2D}},
        {0xB4, 1, {0x07}},
        {0xC0, 3, {0xA2, 0x02, 0x84}},
        {0xC1, 1, {0xC5}},
        {0xC2, 2, {0x0A, 0x00}},
        {0xC3, 2, {0x8A, 0x2A}},
        {0xC4, 2, {0x8A, 0xEE}},
        {0xC5, 1, {0x0E}},
        {0x20, 0, {0}},
        {0x36, 1, {TFT_MADCTL}},
        {0x3A, 1, {0x05}},
        {0xE0, 16, {0x02, 0x1C, 0x07, 0x12, 0x37, 0x32, 0x29, 0x2D, 0x29, 0x25, 0x2B, 0x39, 0x00, 0x01, 0x03, 0x10}},
        {0xE1, 16, {0x03, 0x1D, 0x07, 0x06, 0x2E, 0x2C, 0x29, 0x2D, 0x2E, 0x2E, 0x37, 0x3F, 0x00, 0x00, 0x02, 0x10}},
    };
    for (unsigned i = 0; i < sizeof(init) / sizeof(init[0]); i++)
        TFT_TRY(tft_command(init[i].command, init[i].data, init[i].length));
    TFT_TRY(tft_command(0x13, NULL, 0));
    vTaskDelay(pdMS_TO_TICKS(10));
    TFT_TRY(tft_command(0x29, NULL, 0));
    vTaskDelay(pdMS_TO_TICKS(100));
    ESP_LOGI(TAG, "TFT_INIT SPI2 CS=%d RST=%d DC/A0=%d MOSI=%d SCK=%d landscape=%dx%d",
             TFT_PIN_CS, TFT_PIN_RST, TFT_PIN_DC, TFT_PIN_MOSI, TFT_PIN_SCK, TFT_WIDTH, TFT_HEIGHT);
    ESP_LOGI(TAG, "TFT_CAMERA RGB565 swap_bytes=%d MADCTL=0x%02X SPI=%d Hz chunk_bytes=%d",
             TFT_SWAP_CAMERA_BYTES, TFT_MADCTL, TFT_SPI_HZ, TFT_SPI_CHUNK_BYTES);
    return ESP_OK;
}

#undef TFT_TRY

// Compact 5x7 ASCII glyphs; LCD text is deliberately unaccented for this font.
static const uint8_t cabinet_font[][5] = {
    {0x3e, 0x51, 0x49, 0x45, 0x3e}, {0, 0x42, 0x7f, 0x40, 0}, {0x42, 0x61, 0x51, 0x49, 0x46}, {0x21, 0x41, 0x45, 0x4b, 0x31}, {0x18, 0x14, 0x12, 0x7f, 0x10}, {0x27, 0x45, 0x45, 0x45, 0x39}, {0x3c, 0x4a, 0x49, 0x49, 0x30}, {1, 0x71, 9, 5, 3}, {0x36, 0x49, 0x49, 0x49, 0x36}, {6, 0x49, 0x49, 0x29, 0x1e}, {0x7e, 0x11, 0x11, 0x11, 0x7e}, {0x7f, 0x49, 0x49, 0x49, 0x36}, {0x3e, 0x41, 0x41, 0x41, 0x22}, {0x7f, 0x41, 0x41, 0x22, 0x1c}, {0x7f, 0x49, 0x49, 0x49, 0x41}, {0x7f, 9, 9, 9, 1}, {0x3e, 0x41, 0x49, 0x49, 0x7a}, {0x7f, 8, 8, 8, 0x7f}, {0, 0x41, 0x7f, 0x41, 0}, {0x20, 0x40, 0x41, 0x3f, 1}, {0x7f, 8, 0x14, 0x22, 0x41}, {0x7f, 0x40, 0x40, 0x40, 0x40}, {0x7f, 2, 0x0c, 2, 0x7f}, {0x7f, 4, 8, 0x10, 0x7f}, {0x3e, 0x41, 0x41, 0x41, 0x3e}, {0x7f, 9, 9, 9, 6}, {0x3e, 0x41, 0x51, 0x21, 0x5e}, {0x7f, 9, 0x19, 0x29, 0x46}, {0x46, 0x49, 0x49, 0x49, 0x31}, {1, 1, 0x7f, 1, 1}, {0x3f, 0x40, 0x40, 0x40, 0x3f}, {0x1f, 0x20, 0x40, 0x20, 0x1f}, {0x3f, 0x40, 0x38, 0x40, 0x3f}, {0x63, 0x14, 8, 0x14, 0x63}, {7, 8, 0x70, 8, 7}, {0x61, 0x51, 0x49, 0x45, 0x43}};

static bool cabinet_text_pixel(const char *text, int x, int y)
{
    if (x < 0 || y < 0 || y >= 7 || (size_t)(x / 6) >= strlen(text) || x % 6 >= 5)
        return false;
    char c = text[x / 6];
    if (c >= 'a' && c <= 'z')
        c -= 'a' - 'A';
    int index = c >= '0' && c <= '9' ? c - '0' : (c >= 'A' && c <= 'Z' ? 10 + c - 'A' : -1);
    if (index >= 0)
        return cabinet_font[index][x % 6] & (1 << y);
    if (c == '.')
        return y == 6 && x % 6 == 2;
    if (c == '/')
        return y == 6 - x % 6;
    if (c == '-')
        return y == 3;
    return false;
}

static int cabinet_text_left(const char *text, int scale)
{
    int length = (int)strlen(text);
    int width = length > 0 ? (length * 6 - 1) * scale : 0;
    return (TFT_WIDTH - width) / 2;
}

static void cabinet_lcd_task(void *unused)
{
    (void)unused;
    static classhub_display_t screen;
    uint32_t generation = UINT32_MAX;
    while (true)
    {
        if (classhub_display_snapshot(&screen, generation))
        {
            // Fill the largest square that fits the LCD, preserving aspect ratio.
            // Map pixels directly to modules without interpolation or blur.
            int side = TFT_WIDTH < TFT_HEIGHT ? TFT_WIDTH : TFT_HEIGHT;
            int qr_modules = screen.qr_size + 8;
            int left = (TFT_WIDTH - side) / 2, top = (TFT_HEIGHT - side) / 2;
            const char *network_text = screen.online ? "MQTT ONLINE" : "MQTT OFFLINE";
            int header_left = cabinet_text_left("CLASSHUB", 2);
            int title_left = cabinet_text_left(screen.title, 1);
            int detail_left = cabinet_text_left(screen.detail, 1);
            int network_left = cabinet_text_left(network_text, 1);
            esp_err_t error = tft_window(0, 0, TFT_WIDTH, TFT_HEIGHT);
            for (int y = 0; y < TFT_HEIGHT && error == ESP_OK; ++y)
            {
                for (int x = 0; x < TFT_WIDTH; ++x)
                {
                    uint16_t color = 0;
                    if (screen.qr_size > 0)
                    {
                        // Four white modules on every side form the quiet zone.
                        color = x >= left && x < left + side && y >= top && y < top + side ? 0xffff : 0;
                        int qx = (x - left) * qr_modules / side - 4;
                        int qy = (y - top) * qr_modules / side - 4;
                        if (x >= left && x < left + side && y >= top && y < top + side &&
                            qx >= 0 && qy >= 0 &&
                            qx < screen.qr_size && qy < screen.qr_size &&
                            screen.qr[qy * CLASSHUB_QR_SIDE_MAX + qx])
                            color = 0;
                    }
                    else
                    {
                        bool header = x >= header_left && y >= 12 &&
                                      cabinet_text_pixel("CLASSHUB", (x - header_left) / 2, (y - 12) / 2);
                        bool title = cabinet_text_pixel(screen.title, x - title_left, y - 54);
                        bool detail = cabinet_text_pixel(screen.detail, x - detail_left, y - 76);
                        bool network = cabinet_text_pixel(network_text, x - network_left, y - 108);
                        if (header)
                            color = 0x07ff;
                        else if (title || detail)
                            color = 0xffff;
                        else if (network)
                            color = screen.online ? 0x07e0 : 0xfd20;
                    }
                    tft_line[2 * x] = color >> 8;
                    tft_line[2 * x + 1] = color;
                }
                error = tft_transfer(true, tft_line, sizeof(tft_line));
            }
            if (error == ESP_OK)
                generation = screen.generation;
            else
                ESP_LOGW(TAG, "Cabinet LCD transfer: %s", esp_err_to_name(error));
        }
        vTaskDelay(pdMS_TO_TICKS(100));
    }
}

typedef enum
{
    READ_STOP_START
} read_mode_t;
typedef struct
{
    esp_err_t error;
    const char *phase;
    uint8_t value;
} read_result_t;
typedef struct
{
    uint8_t reg;
    const char *name;
    uint8_t expected;
} id_register_t;
static const id_register_t id_registers[] = {
    {0x0A, "PID", 0x76},
    {0x0B, "VER", 0x73},
    {0x1C, "MIDL", 0x7F},
    {0x1D, "MIDH", 0xA2},
};

static const char *mode_name(read_mode_t mode)
{
    (void)mode;
    return "STOP_START";
}

static read_result_t read_register(i2c_master_dev_handle_t device, uint8_t reg, read_mode_t mode)
{
    (void)mode;
    // Separate transactions: register address + STOP, then START + data read.
    read_result_t result = {.error = ESP_FAIL, .phase = "register-address-write", .value = 0};
    result.error = i2c_master_transmit(device, &reg, 1, TEST_TIMEOUT_MS);
    if (result.error != ESP_OK)
        return result;
    result.phase = "data-read";
    result.error = i2c_master_receive(device, &result.value, 1, TEST_TIMEOUT_MS);
    return result;
}

static bool log_read(const char *label, uint8_t reg, read_mode_t mode, read_result_t result)
{
    if (result.error != ESP_OK)
    {
        ESP_LOGE(TAG, "%s %s reg=0x%02X phase=%s ERROR=%s (0x%X); value INVALID",
                 label, mode_name(mode), reg, result.phase,
                 esp_err_to_name(result.error), (unsigned)result.error);
        return false;
    }
    ESP_LOGI(TAG, "%s %s reg=0x%02X value=0x%02X", label, mode_name(mode), reg, result.value);
    return true;
}

static bool test_ids(i2c_master_dev_handle_t device, read_mode_t mode, uint32_t frequency)
{
    unsigned reads = 0, errors = 0, mismatches = 0, consecutive_errors = 0;
    unsigned logged = 0;
    for (unsigned round = 0; round < TEST_ROUNDS; round++)
    {
        for (unsigned index = 0; index < 4; index++)
        {
            // Vary order to expose stale register selection / repeated values.
            const id_register_t *id = &id_registers[(index + round) % 4];
            read_result_t result = read_register(device, id->reg, mode);
            reads++;
            if (round == 0)
                log_read(id->name, id->reg, mode, result);
            if (result.error != ESP_OK)
            {
                errors++;
                consecutive_errors++;
                if (round != 0 && logged++ < TEST_MAX_ERROR_LOGS)
                    log_read(id->name, id->reg, mode, result);
            }
            else
            {
                consecutive_errors = 0;
                if (result.value != id->expected)
                {
                    mismatches++;
                    if (logged++ < TEST_MAX_ERROR_LOGS)
                    {
                        ESP_LOGW(TAG, "%s %s reg=0x%02X got=0x%02X expected=0x%02X",
                                 mode_name(mode), id->name, id->reg, result.value, id->expected);
                    }
                }
            }
            if (consecutive_errors >= 8)
                goto complete;
        }
        vTaskDelay(pdMS_TO_TICKS(10));
    }
complete:
    bool pass = reads == TEST_ROUNDS * 4 && errors == 0 && mismatches == 0;
    ESP_LOGI(TAG, "ID_RESULT hz=%lu mode=%s reads=%u errors=%u mismatches=%u => %s",
             (unsigned long)frequency, mode_name(mode), reads, errors, mismatches, pass ? "PASS" : "FAIL");
    return pass;
}

static bool write_register(i2c_master_dev_handle_t device, uint8_t reg, uint8_t value, const char *label)
{
    const uint8_t bytes[] = {reg, value};
    esp_err_t error = i2c_master_transmit(device, bytes, sizeof(bytes), TEST_TIMEOUT_MS);
    ESP_LOGI(TAG, "%s reg=0x%02X value=0x%02X result=%s (0x%X)", label, reg, value,
             esp_err_to_name(error), (unsigned)error);
    vTaskDelay(pdMS_TO_TICKS(20));
    return error == ESP_OK;
}

static bool test_write_read(i2c_master_dev_handle_t device, read_mode_t mode)
{
    // COM17[3] is the DSP color-bar enable. Do not touch reset or clock registers.
    const uint8_t reg = 0x42;
    read_result_t before = read_register(device, reg, mode);
    read_result_t confirm = read_register(device, reg, mode);
    bool first_ok = log_read("COM17 original", reg, mode, before);
    bool second_ok = log_read("COM17 confirm", reg, mode, confirm);
    if (!first_ok || !second_ok || before.value != confirm.value)
    {
        ESP_LOGE(TAG, "WRITE_RESULT => SKIP: original COM17 is not trustworthy");
        return false;
    }
    uint8_t target = before.value ^ 0x08;
    bool wrote = write_register(device, reg, target, "TEST_WRITE");
    read_result_t after = read_register(device, reg, mode);
    bool read_ok = log_read("COM17 after toggle", reg, mode, after);
    bool matched = wrote && read_ok && after.value == target;

    // Restore even after a NACK: an unsuccessful transaction may have partly applied.
    bool restored = write_register(device, reg, before.value, "RESTORE");
    read_result_t final = read_register(device, reg, mode);
    bool final_ok = log_read("COM17 after restore", reg, mode, final);
    restored = restored && final_ok && final.value == before.value;
    ESP_LOGI(TAG, "WRITE_RESULT mode=%s toggle=%s restore=%s => %s",
             mode_name(mode), matched ? "PASS" : "FAIL", restored ? "PASS" : "FAIL",
             matched && restored ? "PASS" : "FAIL");
    return matched && restored;
}

static void test_suspect_write(i2c_master_dev_handle_t device, read_mode_t mode)
{
    // ov7670_regs.h defines COM3[6] (0x40) as output MSB/LSB swap.
    // Capture is disabled; preserve all other bits and restore after the test.
    const uint8_t target_reg = 0x0C;
    const uint8_t mask = 0x40;
    const uint8_t regs[] = {0x0B, 0x0C, 0x0D, 0x0A, 0x3E, 0x40, 0x42};
    enum
    {
        COUNT = sizeof(regs) / sizeof(regs[0])
    };
    uint8_t original[COUNT];
    bool baseline_ok = true;
    for (unsigned i = 0; i < COUNT; i++)
    {
        read_result_t first = read_register(device, regs[i], mode);
        read_result_t second = read_register(device, regs[i], mode);
        bool first_ok = log_read("SUSPECT baseline", regs[i], mode, first);
        bool second_ok = log_read("SUSPECT confirm", regs[i], mode, second);
        original[i] = first.value;
        if (!first_ok || !second_ok || first.value != second.value)
            baseline_ok = false;
    }
    if (!baseline_ok || original[0] != 0x73 || original[3] != 0x76)
    {
        ESP_LOGE(TAG, "SUSPECT_WRITE_RESULT => SKIP: baseline is not trustworthy");
        return;
    }

    uint8_t toggled = original[1] ^ mask;
    ESP_LOGI(TAG, "SUSPECT COM3[6] original=0x%02X target=0x%02X mask=0x%02X",
             original[1], toggled, mask);
    bool wrote = write_register(device, target_reg, toggled, "SUSPECT_WRITE");
    unsigned toggle_errors = 0, target_mismatches = 0, other_changes = 0;
    for (unsigned round = 0; round < 3; round++)
    {
        for (unsigned step = 0; step < COUNT; step++)
        {
            unsigned i = (round & 1) ? COUNT - 1 - step : step;
            read_result_t result = read_register(device, regs[i], mode);
            if (!log_read("SUSPECT after toggle", regs[i], mode, result))
                toggle_errors++;
            else if (i == 1)
                target_mismatches += result.value != toggled;
            else
                other_changes += result.value != original[i];
        }
        vTaskDelay(pdMS_TO_TICKS(10));
    }

    // Always restore, including after a failed write or readback.
    bool restore_write = write_register(device, target_reg, original[1], "SUSPECT_RESTORE");
    unsigned restore_errors = 0, restore_mismatches = 0;
    for (unsigned round = 0; round < 3; round++)
    {
        for (unsigned i = 0; i < COUNT; i++)
        {
            read_result_t result = read_register(device, regs[i], mode);
            if (!log_read("SUSPECT after restore", regs[i], mode, result))
                restore_errors++;
            else
                restore_mismatches += result.value != original[i];
        }
        vTaskDelay(pdMS_TO_TICKS(10));
    }
    bool toggle_ok = wrote && toggle_errors == 0 && target_mismatches == 0 && other_changes == 0;
    bool restore_ok = restore_write && restore_errors == 0 && restore_mismatches == 0;
    ESP_LOGI(TAG, "SUSPECT_WRITE_RESULT toggle=%s read_errors=%u target_mismatches=%u other_changes=%u restore=%s restore_errors=%u restore_mismatches=%u => %s",
             toggle_ok ? "PASS" : "FAIL", toggle_errors, target_mismatches, other_changes,
             restore_ok ? "PASS" : "FAIL", restore_errors, restore_mismatches,
             toggle_ok && restore_ok ? "PASS" : "FAIL");
    if (!restore_ok)
        ESP_LOGE(TAG, "COM3 restoration not verified; power-cycle the camera before further tests");
}

static void test_register_selection(i2c_master_dev_handle_t device, read_mode_t mode)
{
    const uint8_t regs[] = {0x12, 0x11, 0x0C, 0x3E, 0x40, 0x72, 0x73, 0x17, 0x18, 0x32, 0x19, 0x1A, 0x03};
    enum
    {
        COUNT = sizeof(regs) / sizeof(regs[0]),
        ROUNDS = 8
    };
    uint8_t baseline[COUNT] = {0};
    bool baseline_valid[COUNT] = {false};
    unsigned errors = 0, id_mismatches = 0, com17_mismatches = 0;
    unsigned target_changes = 0, pair_mismatches = 0, a1_reads = 0, target_reads = 0;
    read_result_t com17 = read_register(device, 0x42, mode);
    if (!log_read("SELECT COM17 baseline", 0x42, mode, com17))
    {
        ESP_LOGE(TAG, "SELECT_RESULT => SKIP: COM17 baseline read failed");
        return;
    }

    ESP_LOGI(TAG, "Selection test: 8 rounds, forward/reverse with rotating start; read-only");
    for (unsigned round = 0; round < ROUNDS; round++)
    {
        for (unsigned step = 0; step < COUNT; step++)
        {
            unsigned index = (round + ((round & 1) ? COUNT - 1 - step : step)) % COUNT;
            const id_register_t *id = &id_registers[(round + step) % 4];
            // Read the same target after two different known registers.
            read_result_t anchor = read_register(device, id->reg, mode);
            read_result_t first = read_register(device, regs[index], mode);
            read_result_t control = read_register(device, 0x42, mode);
            read_result_t second = read_register(device, regs[index], mode);
            read_result_t results[] = {anchor, first, control, second};
            const uint8_t addresses[] = {id->reg, regs[index], 0x42, regs[index]};
            for (unsigned n = 0; n < 4; n++)
            {
                if (results[n].error != ESP_OK)
                {
                    if (errors < TEST_MAX_ERROR_LOGS)
                        log_read("SELECT", addresses[n], mode, results[n]);
                    errors++;
                }
            }
            if (anchor.error == ESP_OK && anchor.value != id->expected)
                id_mismatches++;
            if (control.error == ESP_OK && control.value != com17.value)
                com17_mismatches++;
            if (first.error == ESP_OK && second.error == ESP_OK && first.value != second.value)
                pair_mismatches++;
            for (unsigned n = 1; n <= 3; n += 2)
            {
                if (results[n].error != ESP_OK)
                    continue;
                target_reads++;
                a1_reads += results[n].value == 0xA1;
                if (!baseline_valid[index])
                {
                    baseline[index] = results[n].value;
                    baseline_valid[index] = true;
                }
                else if (results[n].value != baseline[index])
                    target_changes++;
            }
            // Only print values when all four reads succeeded; never show invalid data as a value.
            if (anchor.error == ESP_OK && first.error == ESP_OK &&
                control.error == ESP_OK && second.error == ESP_OK)
                ESP_LOGI(TAG, "SELECT round=%u order=%s ID[0x%02X]=0x%02X target[0x%02X]=0x%02X COM17=0x%02X target_again=0x%02X",
                         round + 1, (round & 1) ? "REV" : "FWD", id->reg, anchor.value,
                         regs[index], first.value, control.value, second.value);
        }
        vTaskDelay(pdMS_TO_TICKS(10));
    }
    ESP_LOGI(TAG, "SELECT_RESULT errors=%u id_mismatches=%u com17_changes=%u target_changes=%u pair_mismatches=%u target_A1=%u/%u",
             errors, id_mismatches, com17_mismatches, target_changes, pair_mismatches, a1_reads, target_reads);
    ESP_LOGI(TAG, "SELECT_RESULT checks read consistency only; stable target values do not prove correct register contents");
}

static void dump_registers(i2c_master_dev_handle_t device, read_mode_t mode)
{
    const uint8_t regs[] = {0x12, 0x11, 0x0C, 0x3E, 0x40, 0x72, 0x73, 0x17, 0x18, 0x32, 0x19, 0x1A, 0x03};
    ESP_LOGI(TAG, "Raw registers WITHOUT camera initialization; no RGB565/QQVGA comparison");
    for (unsigned index = 0; index < sizeof(regs); index++)
    {
        log_read("RAW", regs[index], mode, read_register(device, regs[index], mode));
    }
}

static void test_soft_reset(i2c_master_dev_handle_t device, read_mode_t mode)
{
    // COM7[7] is SCCB register reset (see ov7670_regs.h).
    // Do not load the library's default table: inspect reset-only state.
    const uint8_t regs[] = {0x12, 0x11, 0x0C, 0x3E, 0x40, 0x72, 0x73,
                            0x17, 0x18, 0x32, 0x19, 0x1A, 0x03, 0x42};
    enum
    {
        COUNT = sizeof(regs) / sizeof(regs[0]),
        ROUNDS = 3
    };
    read_result_t before[COUNT];
    uint8_t first_after[COUNT] = {0};
    bool after_valid[COUNT] = {false};
    unsigned before_errors = 0, after_errors = 0, changed = 0;
    unsigned unstable = 0, a1_reads = 0, valid_reads = 0, reset_bit_set = 0;
    ESP_LOGI(TAG, "RESET_TEST begin: reset only, no RGB565/QQVGA initialization");
    for (unsigned i = 0; i < COUNT; i++)
    {
        before[i] = read_register(device, regs[i], mode);
        if (!log_read("RESET before", regs[i], mode, before[i]))
            before_errors++;
    }

    bool wrote = write_register(device, 0x12, 0x80, "SOFT_RESET");
    // write_register already waits 20 ms; add a conservative settling interval.
    vTaskDelay(pdMS_TO_TICKS(200));
    bool ids_ok = test_ids(device, mode, TEST_SCCB_HZ);
    for (unsigned round = 0; round < ROUNDS; round++)
    {
        ESP_LOGI(TAG, "RESET after round=%u", round + 1);
        for (unsigned step = 0; step < COUNT; step++)
        {
            unsigned i = (round & 1) ? COUNT - 1 - step : step;
            read_result_t result = read_register(device, regs[i], mode);
            if (!log_read("RESET after", regs[i], mode, result))
            {
                after_errors++;
                continue;
            }
            valid_reads++;
            a1_reads += result.value == 0xA1;
            if (regs[i] == 0x12 && (result.value & 0x80))
                reset_bit_set++;
            if (!after_valid[i])
            {
                after_valid[i] = true;
                first_after[i] = result.value;
                if (before[i].error == ESP_OK && before[i].value != result.value)
                {
                    changed++;
                    ESP_LOGI(TAG, "RESET change reg=0x%02X before=0x%02X after=0x%02X",
                             regs[i], before[i].value, result.value);
                }
            }
            else if (result.value != first_after[i])
                unstable++;
        }
        vTaskDelay(pdMS_TO_TICKS(100));
    }
    bool checks_ok = wrote && ids_ok && before_errors == 0 && after_errors == 0 &&
                     unstable == 0 && reset_bit_set == 0;
    ESP_LOGI(TAG, "RESET_RESULT write=%s ids=%s before_errors=%u after_errors=%u changed_regs=%u unstable_reads=%u reset_bit_set=%u A1=%u/%u checks=%s",
             wrote ? "PASS" : "FAIL", ids_ok ? "PASS" : "FAIL", before_errors,
             after_errors, changed, unstable, reset_bit_set, a1_reads, valid_reads,
             checks_ok ? "PASS" : "FAIL");
    ESP_LOGI(TAG, "RESET_RESULT checks communication/stability; does not verify all factory defaults or prove reset took effect");
}

static bool configure_rgb565_qqvga(i2c_master_dev_handle_t device, read_mode_t mode)
{
    typedef struct
    {
        uint8_t reg;
        uint8_t value;
    } setting_t;
    // Application-local settings derived from this project's ov7670.c:
    // default table, RGB565 table, QQVGA table and frame_control(158,14,12,490).
    // Repeated entries are merged to their final values; CLKRC is fixed at 0.
    // These are requested register settings, not proof of image dimensions.
    static const setting_t settings[] = {
        {0x3A, 0x04}, // TSLB
        {0x11, 0x00}, // CLKRC
        {0x6B, 0x4A}, // DBLV
        {0x15, 0x02}, // COM10: negative VSYNC, free-running PCLK
        {0x0D, 0x40}, // COM4
        {0xB0, 0x84},
        {0x3B, 0x12}, // COM11
        {0x30, 0x00},
        {0x31, 0x00},
        {0x43, 0x0A},
        {0x44, 0xF0},
        {0x45, 0x34},
        {0x46, 0x58},
        {0x47, 0x28},
        {0x48, 0x3A},
        {0x6C, 0x0A},
        {0x6D, 0x55},
        {0x6E, 0x11},
        {0x6F, 0x9E},
        {0x13, 0xC7}, // COM8: auto exposure/gain/white balance
        {0x12, 0x04}, // COM7: RGB
        {0x8C, 0x00}, // RGB444 disabled
        {0x04, 0x00}, // COM1
        {0x40, 0xD0}, // COM15: RGB565, full range
        {0x1E, 0x02},
        {0x14, 0x6A},
        {0x4F, 0xB3},
        {0x50, 0xB3},
        {0x51, 0x00},
        {0x52, 0x3D},
        {0x53, 0xA7},
        {0x54, 0xE4},
        {0x3D, 0x40},
        {0x0C, 0x04}, // COM3: DCW enabled
        {0x3E, 0x1A}, // COM14
        {0x70, 0x3A},
        {0x71, 0x35},
        {0x72, 0x22},
        {0x73, 0xF2},
        {0xA2, 0x02},
        {0x17, 0x13},
        {0x18, 0x01},
        {0x32, 0x36}, // horizontal window
        {0x19, 0x03},
        {0x1A, 0x7A},
        {0x03, 0x08}, // vertical window
    };
    enum
    {
        COUNT = sizeof(settings) / sizeof(settings[0])
    };
    ESP_LOGI(TAG, "CONFIG_TEST begin: reset -> RGB565/QQVGA settings -> readback; capture disabled");
    bool reset_write = write_register(device, 0x12, 0x80, "CONFIG_RESET");
    vTaskDelay(pdMS_TO_TICKS(200));
    read_result_t com7 = read_register(device, 0x12, mode);
    bool reset_read = log_read("CONFIG reset COM7", 0x12, mode, com7);
    bool ids_ok = test_ids(device, mode, TEST_SCCB_HZ);
    if (!reset_write || !reset_read || (com7.value & 0x80) || !ids_ok)
    {
        ESP_LOGE(TAG, "CONFIG_RESULT => SKIP: reset/ID checks failed");
        return false;
    }

    unsigned writes = 0;
    for (unsigned i = 0; i < COUNT; i++)
    {
        if (!write_register(device, settings[i].reg, settings[i].value, "CONFIG_WRITE"))
        {
            ESP_LOGE(TAG, "CONFIG_RESULT => FAIL: write reg=0x%02X; partial configuration, capture disabled",
                     settings[i].reg);
            return false;
        }
        writes++;
    }
    // Match the RGB565 driver's final clock rewrite after format/window setup.
    bool clock_ok = write_register(device, 0x11, 0x00, "CONFIG_CLKRC");
    vTaskDelay(pdMS_TO_TICKS(100));
    unsigned read_errors = 0, mismatches = 0, matches = 0;
    for (unsigned round = 0; round < 3; round++)
    {
        ESP_LOGI(TAG, "CONFIG_VERIFY round=%u", round + 1);
        for (unsigned step = 0; step < COUNT; step++)
        {
            unsigned i = (round & 1) ? COUNT - 1 - step : step;
            uint8_t mask = 0xFF;
            // COM1[1:0] contains automatic exposure bits; only bit 6 selects CCIR656.
            if (settings[i].reg == 0x04)
                mask = 0x40;
            // VREF[7:6] contains automatic gain bits; [3:0] controls the vertical window.
            else if (settings[i].reg == 0x03)
                mask = 0x0F;
            read_result_t result = read_register(device, settings[i].reg, mode);
            if (!log_read("CONFIG_READ", settings[i].reg, mode, result))
                read_errors++;
            else if ((result.value & mask) != (settings[i].value & mask))
            {
                mismatches++;
                ESP_LOGW(TAG, "CONFIG_MISMATCH reg=0x%02X expected=0x%02X actual=0x%02X mask=0x%02X",
                         settings[i].reg, settings[i].value, result.value, mask);
            }
            else
            {
                matches++;
                if (mask != 0xFF)
                    ESP_LOGI(TAG, "CONFIG_MASK_MATCH reg=0x%02X expected=0x%02X actual=0x%02X mask=0x%02X",
                             settings[i].reg, settings[i].value, result.value, mask);
            }
        }
        vTaskDelay(pdMS_TO_TICKS(100));
    }
    ids_ok = test_ids(device, mode, TEST_SCCB_HZ);
    bool pass = clock_ok && ids_ok && read_errors == 0 && mismatches == 0;
    ESP_LOGI(TAG, "CONFIG_RESULT writes=%u/%u clock=%s ids=%s matches=%u read_errors=%u mismatches=%u => %s",
             writes, (unsigned)COUNT, clock_ok ? "PASS" : "FAIL", ids_ok ? "PASS" : "FAIL",
             matches, read_errors, mismatches, pass ? "PASS" : "FAIL");
    ESP_LOGI(TAG, "CONFIG_RESULT covers register readback only; 160x120 RGB565 output remains unverified");
    return pass;
}

static bool measure_signal(const char *name, int pin)
{
    pcnt_unit_handle_t unit = NULL;
    pcnt_channel_handle_t channel = NULL;
    bool enabled = false, started = false;
    int count = 0;
    const pcnt_unit_config_t unit_config = {
        .low_limit = -30000,
        .high_limit = 30000,
        .flags.accum_count = true,
    };
    const pcnt_chan_config_t channel_config = {.edge_gpio_num = pin, .level_gpio_num = -1};
    esp_err_t error = pcnt_new_unit(&unit_config, &unit);
    if (error != ESP_OK)
        goto cleanup;
    error = pcnt_new_channel(unit, &channel_config, &channel);
    if (error != ESP_OK)
        goto cleanup;
    error = pcnt_channel_set_edge_action(channel, PCNT_CHANNEL_EDGE_ACTION_INCREASE,
                                         PCNT_CHANNEL_EDGE_ACTION_HOLD);
    if (error != ESP_OK)
        goto cleanup;
    error = pcnt_channel_set_level_action(channel, PCNT_CHANNEL_LEVEL_ACTION_KEEP,
                                          PCNT_CHANNEL_LEVEL_ACTION_KEEP);
    if (error != ESP_OK)
        goto cleanup;
    // Watch points let the driver accumulate counter overflows for MHz PCLK.
    error = pcnt_unit_add_watch_point(unit, 30000);
    if (error != ESP_OK)
        goto cleanup;
    error = pcnt_unit_add_watch_point(unit, -30000);
    if (error != ESP_OK)
        goto cleanup;
    error = pcnt_unit_enable(unit);
    if (error != ESP_OK)
        goto cleanup;
    enabled = true;
    error = pcnt_unit_clear_count(unit);
    if (error != ESP_OK)
        goto cleanup;
    int64_t begin = esp_timer_get_time();
    error = pcnt_unit_start(unit);
    if (error != ESP_OK)
        goto cleanup;
    started = true;
    vTaskDelay(pdMS_TO_TICKS(500));
    error = pcnt_unit_stop(unit);
    if (error != ESP_OK)
        goto cleanup;
    started = false;
    int64_t elapsed = esp_timer_get_time() - begin;
    error = pcnt_unit_get_count(unit, &count);
    if (error != ESP_OK)
        goto cleanup;
    ESP_LOGI(TAG, "SIGNAL name=%s gpio=%d rising_edges=%d window_us=%lld approx_hz=%lld level=%d => %s",
             name, pin, count, (long long)elapsed,
             (long long)((int64_t)count * 1000000 / elapsed), gpio_get_level(pin),
             count > 0 ? "ACTIVE" : "NO_EDGES");
cleanup:
    if (error != ESP_OK)
        ESP_LOGE(TAG, "SIGNAL name=%s gpio=%d measurement failed: %s", name, pin, esp_err_to_name(error));
    if (started)
        pcnt_unit_stop(unit);
    if (enabled)
        pcnt_unit_disable(unit);
    if (channel)
        pcnt_del_channel(channel);
    if (unit)
        pcnt_del_unit(unit);
    return error == ESP_OK && count > 0;
}

enum
{
    SYNC_EDGE_LIMIT = 20
};
typedef struct
{
    int64_t time_us;
    int href_count;
    int level;
    esp_err_t error;
} sync_edge_t;
static struct
{
    pcnt_unit_handle_t unit;
    int edge_pin;
    bool accepting;
    unsigned used;
    sync_edge_t edges[SYNC_EDGE_LIMIT];
} sync_trace;
static portMUX_TYPE sync_trace_lock = portMUX_INITIALIZER_UNLOCKED;

static void sync_edge_isr(void *arg)
{
    (void)arg;
    // Bounded snapshot only; no logging or allocation in the ISR.
    portENTER_CRITICAL_ISR(&sync_trace_lock);
    if (sync_trace.accepting && sync_trace.used < SYNC_EDGE_LIMIT)
    {
        sync_edge_t *edge = &sync_trace.edges[sync_trace.used];
        edge->time_us = esp_timer_get_time();
        edge->level = gpio_get_level(sync_trace.edge_pin);
        edge->error = pcnt_unit_get_count(sync_trace.unit, &edge->href_count);
        sync_trace.used++;
    }
    portEXIT_CRITICAL_ISR(&sync_trace_lock);
}

typedef struct
{
    unsigned lines, errors, minimum, maximum;
} pclk_measurement_t;

static void test_frame_sync(bool pclk_per_line, bool falling_pclk, pclk_measurement_t *measurement)
{
    if (measurement)
        *measurement = (pclk_measurement_t){.errors = 1};
    pcnt_unit_handle_t unit = NULL;
    pcnt_channel_handle_t channel = NULL;
    bool enabled = false, started = false, handler_added = false, own_service = false;
    unsigned used = 0, valid_periods = 0, errors = 0;
    unsigned min_lines = UINT32_MAX, max_lines = 0;
    unsigned mismatches = 0;
    int interrupt_pin = pclk_per_line ? CAM_PIN_HREF : CAM_PIN_VSYNC;
    const pcnt_unit_config_t unit_config = {
        .low_limit = -30000,
        .high_limit = 30000,
        .flags.accum_count = true,
    };
    const pcnt_chan_config_t channel_config = {
        .edge_gpio_num = pclk_per_line ? CAM_PIN_PCLK : CAM_PIN_HREF,
        .level_gpio_num = pclk_per_line ? CAM_PIN_HREF : -1,
    };
    const char *pclk_edge = falling_pclk ? "FALLING" : "RISING";
    ESP_LOGI(TAG, "%s begin: %s, PCLK_edge=%s CLKRC=0x%02X, up to 5 seconds",
             pclk_per_line ? "PCLK_TEST" : "SYNC_TEST",
             pclk_per_line ? "PCLK gated by HREF HIGH, snapshots at HREF falling edges" : "HREF rising edges per VSYNC interval",
             pclk_edge, CAPTURE_CLKRC_VALUE);
    esp_err_t error = pcnt_new_unit(&unit_config, &unit);
    if (error != ESP_OK)
        goto cleanup;
    error = pcnt_new_channel(unit, &channel_config, &channel);
    if (error != ESP_OK)
        goto cleanup;
    bool count_falling = pclk_per_line && falling_pclk;
    error = pcnt_channel_set_edge_action(channel,
                                         count_falling ? PCNT_CHANNEL_EDGE_ACTION_HOLD : PCNT_CHANNEL_EDGE_ACTION_INCREASE,
                                         count_falling ? PCNT_CHANNEL_EDGE_ACTION_INCREASE : PCNT_CHANNEL_EDGE_ACTION_HOLD);
    if (error != ESP_OK)
        goto cleanup;
    error = pcnt_channel_set_level_action(channel, PCNT_CHANNEL_LEVEL_ACTION_KEEP,
                                          pclk_per_line ? PCNT_CHANNEL_LEVEL_ACTION_HOLD : PCNT_CHANNEL_LEVEL_ACTION_KEEP);
    if (error != ESP_OK)
        goto cleanup;
    error = pcnt_unit_add_watch_point(unit, 30000);
    if (error != ESP_OK)
        goto cleanup;
    error = pcnt_unit_add_watch_point(unit, -30000);
    if (error != ESP_OK)
        goto cleanup;
    error = pcnt_unit_enable(unit);
    if (error != ESP_OK)
        goto cleanup;
    enabled = true;
    error = pcnt_unit_clear_count(unit);
    if (error != ESP_OK)
        goto cleanup;
    error = pcnt_unit_start(unit);
    if (error != ESP_OK)
        goto cleanup;
    started = true;
    error = gpio_install_isr_service(0);
    if (error == ESP_OK)
        own_service = true;
    else if (error != ESP_ERR_INVALID_STATE)
        goto cleanup;
    error = gpio_set_intr_type(interrupt_pin, pclk_per_line ? GPIO_INTR_NEGEDGE : GPIO_INTR_ANYEDGE);
    if (error != ESP_OK)
        goto cleanup;
    portENTER_CRITICAL(&sync_trace_lock);
    sync_trace.unit = unit;
    sync_trace.edge_pin = interrupt_pin;
    sync_trace.used = 0;
    sync_trace.accepting = true;
    portEXIT_CRITICAL(&sync_trace_lock);
    error = gpio_isr_handler_add(interrupt_pin, sync_edge_isr, NULL);
    if (error != ESP_OK)
        goto cleanup;
    handler_added = true;
    int64_t deadline = esp_timer_get_time() + 5000000;
    do
    {
        vTaskDelay(pdMS_TO_TICKS(20));
        portENTER_CRITICAL(&sync_trace_lock);
        used = sync_trace.used;
        portEXIT_CRITICAL(&sync_trace_lock);
    } while (used < SYNC_EDGE_LIMIT && esp_timer_get_time() < deadline);

cleanup:
    // Synchronize with the ISR before freeing its PCNT handle or reading samples.
    portENTER_CRITICAL(&sync_trace_lock);
    sync_trace.accepting = false;
    if (handler_added)
        used = sync_trace.used;
    portEXIT_CRITICAL(&sync_trace_lock);
    if (handler_added)
    {
        gpio_intr_disable(interrupt_pin);
        gpio_isr_handler_remove(interrupt_pin);
    }
    if (own_service)
        gpio_uninstall_isr_service();
    if (started)
        pcnt_unit_stop(unit);
    if (enabled)
        pcnt_unit_disable(unit);
    if (channel)
        pcnt_del_channel(channel);
    if (unit)
        pcnt_del_unit(unit);
    if (error != ESP_OK)
    {
        ESP_LOGE(TAG, "%s edge=%s measurement failed: %s", pclk_per_line ? "PCLK_RESULT" : "SYNC_RESULT", pclk_edge, esp_err_to_name(error));
        return;
    }
    for (unsigned i = 0; i < used; i++)
    {
        const sync_edge_t *edge = &sync_trace.edges[i];
        if (edge->error != ESP_OK)
            errors++;
        if (i == 0)
            continue;
        const sync_edge_t *previous = &sync_trace.edges[i - 1];
        if (pclk_per_line)
        {
            // PCNT holds during HREF LOW. Falling-to-falling differences include
            // a complete active line and avoid losing clocks to rising-edge ISR latency.
            int clocks = edge->href_count - previous->href_count;
            if (edge->error != ESP_OK || previous->error != ESP_OK ||
                edge->level != 0 || previous->level != 0 || clocks < 0)
            {
                errors++;
                continue;
            }
            valid_periods++;
            if ((unsigned)clocks < min_lines)
                min_lines = (unsigned)clocks;
            if ((unsigned)clocks > max_lines)
                max_lines = (unsigned)clocks;
            mismatches += clocks != CAMERA_WIDTH * 2;
            ESP_LOGI(TAG, "PCLK_LINE edge=%s sample=%u clocks=%d expected=1280 interval_us=%lld => %s",
                     pclk_edge, i, clocks, (long long)(edge->time_us - previous->time_us),
                     clocks == CAMERA_WIDTH * 2 ? "PASS" : "MISMATCH");
            continue;
        }
        if (edge->error != ESP_OK || previous->error != ESP_OK || edge->level == previous->level)
        {
            errors++;
            continue;
        }
        ESP_LOGI(TAG, "SYNC_INTERVAL VSYNC=%s duration_us=%lld HREF_rising=%d",
                 previous->level ? "HIGH" : "LOW",
                 (long long)(edge->time_us - previous->time_us),
                 edge->href_count - previous->href_count);
        if (i >= 2)
        {
            const sync_edge_t *start = &sync_trace.edges[i - 2];
            if (start->error != ESP_OK || start->level != edge->level)
                continue;
            int lines = edge->href_count - start->href_count;
            if (lines < 0)
            {
                errors++;
                continue;
            }
            valid_periods++;
            if ((unsigned)lines < min_lines)
                min_lines = (unsigned)lines;
            if ((unsigned)lines > max_lines)
                max_lines = (unsigned)lines;
            ESP_LOGI(TAG, "SYNC_FRAME edge=%s period_us=%lld HREF_lines=%d expected=480",
                     edge->level ? "RISING" : "FALLING",
                     (long long)(edge->time_us - start->time_us), lines);
        }
    }
    if (pclk_per_line)
    {
        if (measurement)
            *measurement = (pclk_measurement_t){.lines = valid_periods, .errors = errors, .minimum = valid_periods ? min_lines : 0, .maximum = max_lines};
        ESP_LOGI(TAG, "PCLK_RESULT edge=%s edges=%u sampled_lines=%u errors=%u clocks_min=%u clocks_max=%u mismatches=%u expected=1280 => %s",
                 pclk_edge, used, valid_periods, errors, valid_periods ? min_lines : 0, max_lines, mismatches,
                 valid_periods > 0 && errors == 0 && mismatches == 0 ? "PASS" : "INCONCLUSIVE_OR_MISMATCH");
        ESP_LOGI(TAG, "PCLK_RESULT samples adjacent lines; ISR delays or missed HREF interrupts can affect counts");
        return;
    }
    ESP_LOGI(TAG, "SYNC_RESULT edges=%u periods=%u errors=%u lines_min=%u lines_max=%u expected=480; overlapping periods",
             used, valid_periods, errors, valid_periods ? min_lines : 0, max_lines);
    ESP_LOGI(TAG, "SYNC_RESULT observes line count and physical VSYNC levels; does not prove DMA timing or PCLK count per line");
}

static void inspect_colorbar_bits(const camera_fb_t *frame)
{
    if (!frame || !frame->buf || frame->width != CAMERA_WIDTH || frame->height != CAMERA_HEIGHT ||
        frame->len != CAMERA_FRAME_BYTES || frame->format != PIXFORMAT_RGB565)
    {
        ESP_LOGE(TAG, "COLORBAR_BITS invalid RGB565 frame");
        return;
    }
    unsigned ones[2][8] = {{0}};
    uint8_t observed_or[2] = {0, 0}, observed_and[2] = {0xFF, 0xFF};
    for (size_t i = 0; i < frame->len; i++)
    {
        unsigned parity = i & 1;
        uint8_t value = frame->buf[i];
        observed_or[parity] |= value;
        observed_and[parity] &= value;
        for (unsigned bit = 0; bit < 8; bit++)
            ones[parity][bit] += (value >> bit) & 1;
    }
    for (unsigned parity = 0; parity < 2; parity++)
    {
        ESP_LOGI(TAG, "COLORBAR_BITS byte=%s samples=%u OR=0x%02X AND=0x%02X observed_always_low=0x%02X observed_always_high=0x%02X",
                 parity ? "ODD" : "EVEN", CAMERA_FRAME_BYTES / 2, observed_or[parity], observed_and[parity],
                 (unsigned)(observed_or[parity] ^ 0xFF), observed_and[parity]);
        for (unsigned bit = 0; bit < 8; bit++)
            ESP_LOGI(TAG, "COLORBAR_BIT bit=D%u byte=%s ones=%u zeros=%u",
                     bit, parity ? "ODD" : "EVEN", ones[parity][bit], CAMERA_FRAME_BYTES / 2 - ones[parity][bit]);
    }
    unsigned row_differences = 0;
    const unsigned rows[] = {CAMERA_HEIGHT / 4, CAMERA_HEIGHT / 2, CAMERA_HEIGHT * 3 / 4};
    for (unsigned row = 0; row < 3; row++)
        for (unsigned pixel = 10; pixel < CAMERA_WIDTH; pixel += CAMERA_WIDTH / 8)
        {
            const uint8_t *pair = frame->buf + (rows[row] * CAMERA_WIDTH + pixel) * 2;
            const uint8_t *reference = frame->buf + ((CAMERA_HEIGHT / 2) * CAMERA_WIDTH + pixel) * 2;
            row_differences += pair[0] != reference[0] || pair[1] != reference[1];
            // Include neighboring pixels to expose pair alignment or alternating data.
            ESP_LOGI(TAG, "COLORBAR_SAMPLE x=%u y=%u bytes=%02X %02X next=%02X %02X",
                     pixel, rows[row], pair[0], pair[1], pair[2], pair[3]);
        }
    ESP_LOGI(TAG, "COLORBAR_BITS_RESULT cross_row_pair_changes=%u; constant bits in this pattern alone do not prove a wiring fault",
             row_differences);
}

static bool test_camera_colorbars(void)
{
    i2c_master_bus_handle_t bus;
    i2c_master_dev_handle_t device;
    esp_err_t error = i2c_master_get_bus_handle(I2C_NUM_1, &bus);
    if (error != ESP_OK)
        return false;
    const i2c_device_config_t config = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7,
        .device_address = TEST_ADDRESS,
        .scl_speed_hz = TEST_SCCB_HZ,
    };
    error = i2c_master_bus_add_device(bus, &config, &device);
    if (error != ESP_OK)
        return false;
    // Same test-pattern selection as ov7670.c, but all reads use STOP/START.
    read_result_t x = read_register(device, 0x70, READ_STOP_START);
    read_result_t y = read_register(device, 0x71, READ_STOP_START);
    bool x_ok = log_read("COLORBAR original XSC", 0x70, READ_STOP_START, x);
    bool y_ok = log_read("COLORBAR original YSC", 0x71, READ_STOP_START, y);
    if (!x_ok || !y_ok)
    {
        i2c_master_bus_rm_device(device);
        return false;
    }
    uint8_t target_x = x.value & 0x7F, target_y = y.value | 0x80;
    bool wrote_x = write_register(device, 0x70, target_x, "COLORBAR_ENABLE_XSC");
    bool wrote_y = write_register(device, 0x71, target_y, "COLORBAR_ENABLE_YSC");
    read_result_t check_x = read_register(device, 0x70, READ_STOP_START);
    read_result_t check_y = read_register(device, 0x71, READ_STOP_START);
    bool enabled = wrote_x && wrote_y && check_x.error == ESP_OK && check_y.error == ESP_OK &&
                   check_x.value == target_x && check_y.value == target_y;
    bool displayed = false;
    if (enabled)
    {
        vTaskDelay(pdMS_TO_TICKS(200));
        // Discard queued/transitional frames after changing the sensor pattern.
        for (unsigned i = 0; i < 3; i++)
        {
            camera_fb_t *stale = esp_camera_fb_get();
            if (stale)
                esp_camera_fb_return(stale);
            vTaskDelay(pdMS_TO_TICKS(100));
        }
        camera_fb_t *frame = esp_camera_fb_get();
        if (frame)
        {
            inspect_colorbar_bits(frame);
            if (frame->buf && frame->len == CAMERA_FRAME_BYTES)
                for (unsigned pixel = 10; pixel < CAMERA_WIDTH; pixel += CAMERA_WIDTH / 8)
                {
                    const uint8_t *pair = frame->buf + ((CAMERA_HEIGHT / 2) * CAMERA_WIDTH + pixel) * 2;
                    ESP_LOGI(TAG, "COLORBAR_PIXEL x=%u y=60 bytes=%02X %02X", pixel, pair[0], pair[1]);
                }
            displayed = true; // Frame inspected in logs; LCD remains dedicated to cabinet UI.
            esp_camera_fb_return(frame);
        }
    }
    // Restore even when enabling or capturing failed.
    bool restored_x = write_register(device, 0x70, x.value, "COLORBAR_RESTORE_XSC");
    bool restored_y = write_register(device, 0x71, y.value, "COLORBAR_RESTORE_YSC");
    check_x = read_register(device, 0x70, READ_STOP_START);
    check_y = read_register(device, 0x71, READ_STOP_START);
    bool restored = restored_x && restored_y && check_x.error == ESP_OK && check_y.error == ESP_OK &&
                    check_x.value == x.value && check_y.value == y.value;
    error = i2c_master_bus_rm_device(device);
    ESP_LOGI(TAG, "COLORBAR_RESULT enabled=%d frame_inspected=%d restored=%d cleanup=%s; visual inspection required",
             enabled, displayed, restored, esp_err_to_name(error));
    return restored && error == ESP_OK;
}

#include "web_console.h"

typedef struct
{
    char name[12];
    bool value;
    bool reset;
} web_command_t;
typedef struct
{
    int fd;
    uint32_t generation;
    bool requested_frame;
} web_session_t;
static httpd_handle_t web_server;
static i2c_master_dev_handle_t web_sensor;
static QueueHandle_t web_commands;
static uint8_t *web_packet;
static uint8_t *web_raw;
static size_t web_packet_length;
static TaskHandle_t web_encoder;
static bool web_job_jpeg, web_job_snapshot;
static bool web_jpeg_overflow;
static portMUX_TYPE web_lock = portMUX_INITIALIZER_UNLOCKED;
static struct
{
    int fd;
    uint32_t generation, packet_generation, sequence;
    bool credit, busy, paused, control_error, snapshot, jpeg;
    bool aec, agc, awb, mirror, flip;
    unsigned camera_fps;
} web_state = {.fd = -1, .aec = true, .agc = true, .awb = true, .jpeg = true};

typedef struct { int fd, flags; } web_writer_t;
static int web_write(void *ctx, const char *data, size_t length)
{
    web_writer_t *writer = ctx;
    int n = send(writer->fd, data, length, writer->flags | MSG_DONTWAIT);
    if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR)) return -2;
    return n;
}
static int64_t web_clock(void *ctx) { (void)ctx; return esp_timer_get_time(); }
static void web_wait(void *ctx) { (void)ctx; vTaskDelay(1); }
static int web_send_complete(httpd_handle_t server, int fd, const char *data, size_t length, int flags)
{
    (void)server;
    web_writer_t writer = {.fd = fd, .flags = flags};
    int n = camera_send_all(&writer, data, length, web_write, web_clock, web_wait, 5000000);
    return n < 0 ? HTTPD_SOCK_ERR_FAIL : n;
}

static void web_session_free(void *context)
{
    web_session_t *session = context;
    portENTER_CRITICAL(&web_lock);
    if (web_state.fd == session->fd && web_state.generation == session->generation)
    {
        web_state.fd = -1;
        web_state.credit = false;
    }
    portEXIT_CRITICAL(&web_lock);
    ESP_LOGI(TAG, "CAMERA_WS closed fd=%d generation=%lu", session->fd, (unsigned long)session->generation);
    free(session);
}

// Runs only on the HTTP server task. Sensor reads/writes run on the camera task.
static void web_send_state(void *unused)
{
    (void)unused;
    char payload[256];
    portENTER_CRITICAL(&web_lock);
    int fd = web_state.fd;
    bool aec = web_state.aec, agc = web_state.agc, awb = web_state.awb;
    bool mirror = web_state.mirror, flip = web_state.flip;
    bool control_error = web_state.control_error;
    portEXIT_CRITICAL(&web_lock);
    if (fd < 0 || httpd_ws_get_fd_info(web_server, fd) != HTTPD_WS_CLIENT_WEBSOCKET)
        return;
    int length = snprintf(payload, sizeof(payload),
                          "{\"aec\":%s,\"agc\":%s,\"awb\":%s,\"mirror\":%s,\"flip\":%s,\"error\":\"%s\"}",
                          aec ? "true" : "false", agc ? "true" : "false", awb ? "true" : "false",
                          mirror ? "true" : "false", flip ? "true" : "false",
                          control_error ? "Sensor command failed; settings show actual readback" : "");
    httpd_ws_frame_t frame = {.type = HTTPD_WS_TYPE_TEXT, .payload = (uint8_t *)payload, .len = length};
    httpd_ws_send_frame_async(web_server, fd, &frame);
}

static esp_err_t web_root(httpd_req_t *request)
{
    httpd_resp_set_type(request, "text/html; charset=utf-8");
    httpd_resp_set_hdr(request, "Cache-Control", "no-store");
    return httpd_resp_send(request, CAMERA_WEB_PAGE, sizeof(CAMERA_WEB_PAGE) - 1);
}

static esp_err_t web_error(httpd_req_t *request, const char *message)
{
    char payload[160];
    snprintf(payload, sizeof(payload), "{\"error\":\"%s\"}", message);
    httpd_ws_frame_t frame = {.type = HTTPD_WS_TYPE_TEXT, .payload = (uint8_t *)payload, .len = strlen(payload)};
    return httpd_ws_send_frame(request, &frame);
}

static esp_err_t web_ws(httpd_req_t *request)
{
    int fd = httpd_req_to_sockfd(request);
    // IDF 6.1 completes the upgrade without invoking this handler for HTTP GET.
    // Initialize the session on the first actual WS message as well as older-IDF GET callbacks.
    if (httpd_ws_get_fd_info(web_server, fd) != HTTPD_WS_CLIENT_WEBSOCKET)
        return ESP_FAIL;
    if (!request->sess_ctx)
    {
        portENTER_CRITICAL(&web_lock);
        bool occupied = web_state.fd >= 0;
        portEXIT_CRITICAL(&web_lock);
        if (occupied)
        {
            ESP_LOGW(TAG, "CAMERA_WS rejected second viewer fd=%d", fd);
            return ESP_FAIL;
        }
        web_session_t *session = calloc(1, sizeof(*session));
        if (!session)
            return ESP_ERR_NO_MEM;
        portENTER_CRITICAL(&web_lock);
        web_state.fd = fd;
        web_state.generation++;
        web_state.credit = false;
        web_state.paused = false;
        web_state.snapshot = false;
        web_state.jpeg = true;
        session->fd = fd;
        session->generation = web_state.generation;
        portEXIT_CRITICAL(&web_lock);
        request->sess_ctx = session;
        request->free_ctx = web_session_free;
        // IDF's WS sender does not loop on a short socket write.
        if (httpd_sess_set_send_override(web_server, fd, web_send_complete) != ESP_OK)
            return ESP_FAIL;
        ESP_LOGI(TAG, "CAMERA_WS accepted fd=%d generation=%lu", fd, (unsigned long)session->generation);
        httpd_queue_work(web_server, web_send_state, NULL);
    }
    if (request->method == HTTP_GET)
        return ESP_OK;
    web_session_t *session = request->sess_ctx;
    portENTER_CRITICAL(&web_lock);
    bool owner = session && web_state.fd == fd && web_state.generation == session->generation;
    portEXIT_CRITICAL(&web_lock);
    if (!owner)
        return ESP_FAIL;
    httpd_ws_frame_t frame = {0};
    esp_err_t error = httpd_ws_recv_frame(request, &frame, 0);
    if (error != ESP_OK)
        return error;
    if (frame.type != HTTPD_WS_TYPE_TEXT || frame.len > 48)
        return ESP_FAIL;
    char text[49] = {0};
    frame.payload = (uint8_t *)text;
    error = httpd_ws_recv_frame(request, &frame, frame.len);
    if (error != ESP_OK)
        return error;
    if (!strcmp(text, "NEXT") || !strcmp(text, "RESUME") || !strcmp(text, "PAUSE"))
    {
        if (!session->requested_frame && strcmp(text, "PAUSE"))
        {
            session->requested_frame = true;
            ESP_LOGI(TAG, "CAMERA_WS first frame requested fd=%d", fd);
        }
        portENTER_CRITICAL(&web_lock);
        if (!strcmp(text, "PAUSE"))
        {
            web_state.paused = true;
            web_state.credit = false;
        }
        else if (!strcmp(text, "RESUME"))
        {
            web_state.paused = false;
            web_state.credit = true;
        }
        else if (!web_state.paused)
            web_state.credit = true;
        portEXIT_CRITICAL(&web_lock);
        return ESP_OK;
    }
    if (!strcmp(text,"CAPTURE") || !strcmp(text,"FORMAT JPEG") || !strcmp(text,"FORMAT RAW")) {
        portENTER_CRITICAL(&web_lock);
        if (!strcmp(text,"CAPTURE")) { web_state.snapshot=true; web_state.credit=true; }
        else web_state.jpeg=!strcmp(text,"FORMAT JPEG");
        portEXIT_CRITICAL(&web_lock);
        return ESP_OK;
    }
    web_command_t command = {0};
    if (!strcmp(text, "RESET"))
        command.reset = true;
    else
    {
        int value = -1, consumed = 0;
        if (sscanf(text, "SET %11s %1d%n", command.name, &value, &consumed) != 2 ||
            text[consumed] != '\0' || (value != 0 && value != 1))
            return web_error(request, "Invalid control command");
        if (strcmp(command.name, "aec") && strcmp(command.name, "agc") && strcmp(command.name, "awb") &&
            strcmp(command.name, "mirror") && strcmp(command.name, "flip"))
            return web_error(request, "Unknown control");
        command.value = value != 0;
    }
    if (xQueueSend(web_commands, &command, 0) != pdTRUE)
        return web_error(request, "Controls busy; please retry");
    return ESP_OK;
}

static bool web_update_register(uint8_t reg, uint8_t mask, uint8_t value)
{
    read_result_t before = read_register(web_sensor, reg, READ_STOP_START);
    if (before.error != ESP_OK)
        return false;
    uint8_t target = (before.value & (uint8_t)~mask) | (value & mask);
    if (!write_register(web_sensor, reg, target, "WEB_CONTROL"))
        return false;
    read_result_t after = read_register(web_sensor, reg, READ_STOP_START);
    return after.error == ESP_OK && (after.value & mask) == (target & mask);
}

static void web_apply_controls(void)
{
    if (!web_sensor || !web_commands)
        return;
    web_command_t command;
    while (xQueueReceive(web_commands, &command, 0) == pdTRUE)
    {
        bool ok;
        if (command.reset)
        {
            bool auto_ok = web_update_register(0x13, 0x07, 0x07);
            bool orientation_ok = web_update_register(0x1E, 0x30, 0);
            ok = auto_ok && orientation_ok;
        }
        else if (!strcmp(command.name, "mirror") || !strcmp(command.name, "flip"))
        {
            uint8_t mask = !strcmp(command.name, "mirror") ? 0x20 : 0x10;
            ok = web_update_register(0x1E, mask, command.value ? mask : 0);
        }
        else
        {
            uint8_t mask = !strcmp(command.name, "aec") ? 0x01 : !strcmp(command.name, "agc") ? 0x04
                                                                                              : 0x02;
            ok = web_update_register(0x13, mask, command.value ? mask : 0);
        }
        // Report actual readback even if an operation only partly succeeded.
        read_result_t auto_reg = read_register(web_sensor, 0x13, READ_STOP_START);
        read_result_t direction = read_register(web_sensor, 0x1E, READ_STOP_START);
        portENTER_CRITICAL(&web_lock);
        if (auto_reg.error == ESP_OK)
        {
            web_state.aec = auto_reg.value & 1;
            web_state.agc = auto_reg.value & 4;
            web_state.awb = auto_reg.value & 2;
        }
        if (direction.error == ESP_OK)
        {
            web_state.mirror = direction.value & 0x20;
            web_state.flip = direction.value & 0x10;
        }
        web_state.control_error = !ok || auto_reg.error != ESP_OK || direction.error != ESP_OK;
        portEXIT_CRITICAL(&web_lock);
        if (!ok)
            ESP_LOGE(TAG, "WEB_CONTROL failed: %s", command.reset ? "RESET" : command.name);
        httpd_queue_work(web_server, web_send_state, NULL);
    }
}

static void web_send_packet(void *unused)
{
    (void)unused;
    portENTER_CRITICAL(&web_lock);
    int fd = web_state.fd;
    bool current = web_state.packet_generation == web_state.generation && fd >= 0;
    portEXIT_CRITICAL(&web_lock);
    esp_err_t error = ESP_FAIL;
    if (current && httpd_ws_get_fd_info(web_server, fd) == HTTPD_WS_CLIENT_WEBSOCKET)
    {
        httpd_ws_frame_t frame = {.type = HTTPD_WS_TYPE_BINARY, .payload = web_packet, .len = web_packet_length};
        error = httpd_ws_send_frame_async(web_server, fd, &frame);
        if (error == ESP_OK) httpd_sess_update_lru_counter(web_server, fd);
    }
    portENTER_CRITICAL(&web_lock);
    web_state.busy = false;
    if (current && error != ESP_OK)
        web_state.credit = false;
    portEXIT_CRITICAL(&web_lock);
    if (current && error != ESP_OK)
    {
        ESP_LOGW(TAG, "CAMERA_WS send failed fd=%d error=%s", fd, esp_err_to_name(error));
        httpd_sess_trigger_close(web_server, fd);
    }
    else if (current && (web_state.sequence == 1 || web_state.sequence % 120 == 0))
        ESP_LOGI(TAG, "CAMERA_WS sent frame=%lu bytes=%u format=%u fd=%d", (unsigned long)web_state.sequence, (unsigned)web_packet_length, web_packet[14], fd);
}

static void web_put16(uint8_t *p, unsigned value)
{
    p[0] = value;
    p[1] = value >> 8;
}

static size_t web_jpeg_write(void *unused, size_t index, const void *data, size_t length)
{
    (void)unused;
    if (!data && !length) return 0; // Encoder end-of-image notification.
    if (!data || web_jpeg_overflow || index > CAMERA_FRAME_BYTES || length > CAMERA_FRAME_BYTES-index) {
        web_jpeg_overflow=true; return 0;
    }
    memcpy(web_packet+16+index,data,length);
    web_packet_length=16+index+length;
    return length;
}

static void web_encode_task(void *unused)
{
    (void)unused;
    jpgSetRgb565BE(!TFT_SWAP_CAMERA_BYTES);
    for (;;) {
        ulTaskNotifyTake(pdTRUE,portMAX_DELAY);
        web_packet_length=16;
        web_jpeg_overflow=false;
        bool jpeg=false;
        if (web_job_jpeg && !web_job_snapshot)
            jpeg=fmt2jpg_cb(web_raw,CAMERA_FRAME_BYTES,CAMERA_WIDTH,CAMERA_HEIGHT,
                            PIXFORMAT_RGB565,70,web_jpeg_write,NULL);
        jpeg=jpeg && !web_jpeg_overflow && web_packet_length>16;
        if (!jpeg) {
            memcpy(web_packet+16,web_raw,CAMERA_FRAME_BYTES);
            web_packet_length=CAMERA_PACKET_BYTES;
        }
        web_packet[14]=web_job_snapshot ? 2 : (jpeg ? 1 : 0);
        if (classhub_ota_active() || httpd_queue_work(web_server,web_send_packet,NULL)!=ESP_OK) {
            portENTER_CRITICAL(&web_lock);
            web_state.busy=false; web_state.credit=true;
            portEXIT_CRITICAL(&web_lock);
        }
    }
}

static void web_publish(const camera_fb_t *frame)
{
    if (classhub_ota_active()) return;
    if (!web_server || !web_packet || !web_raw || !web_encoder || !frame || !frame->buf || frame->len != CAMERA_FRAME_BYTES ||
        frame->width != CAMERA_WIDTH || frame->height != CAMERA_HEIGHT || frame->format != PIXFORMAT_RGB565)
        return;
    portENTER_CRITICAL(&web_lock);
    bool ready = web_state.fd >= 0 && web_state.credit && !web_state.busy && (!web_state.paused || web_state.snapshot);
    uint32_t sequence = 0;
    unsigned fps = web_state.camera_fps;
    if (ready)
    {
        web_state.busy = true;
        web_state.credit = false;
        web_state.packet_generation = web_state.generation;
        sequence = ++web_state.sequence;
        web_job_snapshot=web_state.snapshot;
        web_job_jpeg=web_state.jpeg;
        web_state.snapshot=false;
    }
    portEXIT_CRITICAL(&web_lock);
    if (!ready)
        return;
    memcpy(web_packet, "OVF1", 4);
    web_put16(web_packet + 4, CAMERA_WIDTH);
    web_put16(web_packet + 6, CAMERA_HEIGHT);
    for (unsigned i = 0; i < 4; i++)
        web_packet[8 + i] = sequence >> (8 * i);
    web_put16(web_packet + 12, fps);
    web_packet[14] = 0;
    web_packet[15] = TFT_SWAP_CAMERA_BYTES;
    // Release the sensor framebuffer promptly; JPEG encoding has its own core/task.
    memcpy(web_raw,frame->buf,CAMERA_FRAME_BYTES);
    xTaskNotifyGive(web_encoder);
}

static esp_err_t web_start(void)
{
    // Allocate one bounded network slot; never hold a camera framebuffer for a slow client.
    web_packet = heap_caps_malloc(CAMERA_PACKET_BYTES, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    web_raw = heap_caps_malloc(CAMERA_FRAME_BYTES, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    web_commands = xQueueCreate(8, sizeof(web_command_t));
    if (web_packet && web_raw && web_commands &&
        xTaskCreatePinnedToCore(web_encode_task,"web_jpeg",6144,NULL,1,&web_encoder,1)!=pdPASS)
        web_encoder=NULL;
    if (!web_packet || !web_raw || !web_commands || !web_encoder)
    {
        ESP_LOGE(TAG, "VGA web buffer allocation failed: requires %u bytes in PSRAM; verify N16R8 and PSRAM boot log", CAMERA_PACKET_BYTES);
        free(web_packet);
        web_packet = NULL;
        free(web_raw); web_raw=NULL;
        if (web_commands)
            vQueueDelete(web_commands);
        web_commands = NULL;
        ESP_LOGW(TAG, "Camera streaming disabled; /setup remains available");
    }
    esp_err_t error;
    httpd_config_t server = HTTPD_DEFAULT_CONFIG();
    server.stack_size = 6144;
    server.max_uri_handlers = 12;
    server.max_open_sockets = 5;
    server.recv_wait_timeout = 2;
    server.send_wait_timeout = 1;
    server.lru_purge_enable = true;
    error = httpd_start(&web_server, &server);
    if (error != ESP_OK)
        return error;
    const httpd_uri_t root = {.uri = "/", .method = HTTP_GET, .handler = web_root};
    const httpd_uri_t ws = {.uri = "/ws", .method = HTTP_GET, .handler = web_ws, .is_websocket = true};
    error = httpd_register_uri_handler(web_server, &root);
    if (error == ESP_OK && web_packet && web_commands)
        error = httpd_register_uri_handler(web_server, &ws);
    if (error == ESP_OK)
        error = classhub_register_http(web_server);
    if (error != ESP_OK)
        return error;
    ESP_LOGI(TAG, "CAMERA_WEB ready: use LAN IP printed by Wi-Fi; 192.168.4.1 only when fallback AP is enabled; native VGA stream");
    ESP_LOGI(TAG, "CAMERA_WEB heap_free=%u largest_block=%u network_frame_slot=%u bytes",
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_8BIT),
             (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_8BIT), CAMERA_PACKET_BYTES);
    return ESP_OK;
}

static esp_err_t web_attach_sensor(void)
{
    i2c_master_bus_handle_t bus;
    esp_err_t error = i2c_master_get_bus_handle(I2C_NUM_1, &bus);
    if (error != ESP_OK)
        return error;
    const i2c_device_config_t sensor = {.dev_addr_length = I2C_ADDR_BIT_LEN_7,
                                        .device_address = TEST_ADDRESS,
                                        .scl_speed_hz = TEST_SCCB_HZ};
    error = i2c_master_bus_add_device(bus, &sensor, &web_sensor);
    if (error != ESP_OK)
        return error;
    read_result_t auto_reg = read_register(web_sensor, 0x13, READ_STOP_START);
    read_result_t direction = read_register(web_sensor, 0x1E, READ_STOP_START);
    if (auto_reg.error != ESP_OK || direction.error != ESP_OK)
        return ESP_FAIL;
    portENTER_CRITICAL(&web_lock);
    web_state.aec = auto_reg.value & 1;
    web_state.agc = auto_reg.value & 4;
    web_state.awb = auto_reg.value & 2;
    web_state.mirror = direction.value & 0x20;
    web_state.flip = direction.value & 0x10;
    portEXIT_CRITICAL(&web_lock);
    return ESP_OK;
}

static void test_capture(void)
{
    // The library now owns XCLK/SCCB and initializes its own sensor settings.
    // Its SCCB read still tries repeated START before the existing fallback.
    const camera_config_t config = {
        .pin_pwdn = CAM_PIN_PWDN,
        .pin_reset = CAM_PIN_RESET,
        .pin_xclk = CAM_PIN_XCLK,
        .pin_sccb_sda = CAM_PIN_SIOD,
        .pin_sccb_scl = CAM_PIN_SIOC,
        .pin_d0 = CAM_PIN_D0,
        .pin_d1 = CAM_PIN_D1,
        .pin_d2 = CAM_PIN_D2,
        .pin_d3 = CAM_PIN_D3,
        .pin_d4 = CAM_PIN_D4,
        .pin_d5 = CAM_PIN_D5,
        .pin_d6 = CAM_PIN_D6,
        .pin_d7 = CAM_PIN_D7,
        .pin_vsync = CAM_PIN_VSYNC,
        .pin_href = CAM_PIN_HREF,
        .pin_pclk = CAM_PIN_PCLK,
        .xclk_freq_hz = TEST_XCLK_HZ,
        .ledc_timer = LEDC_TIMER_0,
        .ledc_channel = LEDC_CHANNEL_0,
        .pixel_format = PIXFORMAT_RGB565,
        .frame_size = FRAMESIZE_VGA,
        .jpeg_quality = 12,
        .fb_count = 2,
        .fb_location = CAMERA_FB_IN_PSRAM,
        .grab_mode = CAMERA_GRAB_LATEST,
    };
    ESP_LOGI(TAG, "CAPTURE_TEST begin: library init, RGB565 640x480 PSRAM, CLKRC=0x%02X, 5 attempts",
             CAPTURE_CLKRC_VALUE);
    ESP_LOGI(TAG, "D2_REROUTE_TEST OV7670 D2 must be wired to GPIO%d (previously GPIO6); other data pins unchanged",
             CAM_PIN_D2);
    esp_err_t error = esp_camera_init(&config);
    if (error != ESP_OK)
    {
        ESP_LOGE(TAG, "CAPTURE_RESULT init=%s => FAIL", esp_err_to_name(error));
        return;
    }

    // Verify the library's actual setup through a separate STOP/START handle.
    // Do not reset the sensor here. Apply capture clock/window trials before taking frames.
    i2c_master_bus_handle_t bus;
    i2c_master_dev_handle_t device = NULL;
    error = i2c_master_get_bus_handle(I2C_NUM_1, &bus);
    if (error == ESP_OK)
    {
        const i2c_device_config_t dev_config = {
            .dev_addr_length = I2C_ADDR_BIT_LEN_7,
            .device_address = TEST_ADDRESS,
            .scl_speed_hz = TEST_SCCB_HZ,
        };
        error = i2c_master_bus_add_device(bus, &dev_config, &device);
    }
    unsigned setup_errors = 0;
    if (error == ESP_OK)
    {
        read_result_t clock_before = read_register(device, 0x11, READ_STOP_START);
        log_read("CAPTURE CLKRC before fix", 0x11, READ_STOP_START, clock_before);
        // The library caches CLKRC through its own SCCB read path and rewrites it.
        // Apply the selected trial clock after library initialization.
        bool clock_written = write_register(device, 0x11, CAPTURE_CLKRC_VALUE, "CAPTURE_CLKRC_WRITE");
        read_result_t clock_after = read_register(device, 0x11, READ_STOP_START);
        bool clock_read = log_read("CAPTURE CLKRC after fix", 0x11, READ_STOP_START, clock_after);
        bool clock_ok = clock_written && clock_read && clock_after.value == CAPTURE_CLKRC_VALUE;
        if (!clock_ok)
            setup_errors++;
        ESP_LOGI(TAG, "CAPTURE_CLKRC_RESULT expected=0x%02X => %s",
                 CAPTURE_CLKRC_VALUE, clock_ok ? "PASS" : "FAIL");
        // Trial: native VGA vertical start=10, stop=490.
        // Keep VREF[7:4] (including automatic gain) and replace only window bits.
        read_result_t vref_before = read_register(device, 0x03, READ_STOP_START);
        bool vref_read = log_read("CAPTURE VREF before window fix", 0x03,
                                  READ_STOP_START, vref_before);
        bool window_written = false;
        if (vref_read)
        {
            uint8_t vref_target = (vref_before.value & 0xF0) | 0x0A;
            bool stop_written = write_register(device, 0x1A, 0x7A, "CAPTURE_VSTOP_WRITE");
            bool vref_written = write_register(device, 0x03, vref_target, "CAPTURE_VREF_WRITE");
            window_written = stop_written && vref_written;
        }
        if (!window_written)
            setup_errors++;
        ESP_LOGI(TAG, "CAPTURE_WINDOW_WRITE start=10 stop=490 VSTOP=0x7A VREF_mask=0x0F expected=0x0A => %s",
                 window_written ? "PASS" : "FAIL");
        // HSTOP carries bits [10:3]; HREF[5:3] carries stop[2:0].
        // Preserve HREF[7:6] and the existing start bits [2:0].
        read_result_t href_before = read_register(device, 0x32, READ_STOP_START);
        bool href_read = log_read("CAPTURE HREF before horizontal trial", 0x32,
                                  READ_STOP_START, href_before);
        bool horizontal_written = false;
        if (href_read)
        {
            uint8_t href_target = (href_before.value & 0xC7) |
                                  ((CAPTURE_HSTOP_VALUE & 0x07) << 3);
            bool hstop_written = write_register(device, 0x18, CAPTURE_HSTOP_VALUE >> 3,
                                                "CAPTURE_HSTOP_WRITE");
            bool href_written = write_register(device, 0x32, href_target, "CAPTURE_HREF_WRITE");
            horizontal_written = hstop_written && href_written;
        }
        if (!horizontal_written)
            setup_errors++;
        ESP_LOGI(TAG, "CAPTURE_HORIZONTAL_TRIAL start=158 baseline_stop=14 trial_stop=%u expected_HREF_window=0x%02X => %s",
                 (unsigned)CAPTURE_HSTOP_VALUE, (unsigned)CAPTURE_HREF_WINDOW,
                 horizontal_written ? "PASS" : "FAIL");
        // COM10[4] reverses the sensor PCLK output; preserve all other bits.
        read_result_t com10_before = read_register(device, 0x15, READ_STOP_START);
        bool com10_read = log_read("PCLK_PHASE COM10 before", 0x15, READ_STOP_START, com10_before);
        bool phase_ok = false;
        if (com10_read)
        {
            uint8_t target = (com10_before.value & 0xEF) | (CAPTURE_REVERSE_PCLK ? 0x10 : 0);
            bool wrote = write_register(device, 0x15, target, "PCLK_PHASE_WRITE");
            read_result_t after = read_register(device, 0x15, READ_STOP_START);
            bool read_ok = log_read("PCLK_PHASE COM10 after", 0x15, READ_STOP_START, after);
            phase_ok = wrote && read_ok && after.value == target;
        }
        if (!phase_ok)
            setup_errors++;
        ESP_LOGI(TAG, "PCLK_PHASE_RESULT reverse=%d => %s", CAPTURE_REVERSE_PCLK,
                 phase_ok ? "PASS" : "FAIL");
        vTaskDelay(pdMS_TO_TICKS(100));
        static const struct
        {
            uint8_t reg, expected, mask;
        } checks[] = {
            {0x0A, 0x76, 0xFF},
            {0x0B, 0x73, 0xFF},
            {0x12, 0x04, 0xFF},
            {0x11, CAPTURE_CLKRC_VALUE, 0xFF},
            {0x15, CAPTURE_REVERSE_PCLK ? 0x10 : 0x00, 0x10},
            {0x40, 0xD0, 0xFF},
            {0x0C, 0x00, 0xFF},
            {0x3E, 0x00, 0xFF},
            {0x72, 0x11, 0xFF},
            {0x73, 0xF0, 0xFF},
            {0x17, 0x13, 0xFF},
            {0x18, CAPTURE_HSTOP_VALUE >> 3, 0xFF},
            {0x32, CAPTURE_HREF_WINDOW, 0x3F},
            {0x19, 0x02, 0xFF},
            {0x1A, 0x7A, 0xFF},
            {0x03, 0x0A, 0x0F},
            {0x04, 0x00, 0x40},
        };
        for (unsigned i = 0; i < sizeof(checks) / sizeof(checks[0]); i++)
        {
            read_result_t result = read_register(device, checks[i].reg, READ_STOP_START);
            if (!log_read("CAPTURE_SETUP", checks[i].reg, READ_STOP_START, result))
                setup_errors++;
            else if ((result.value & checks[i].mask) != (checks[i].expected & checks[i].mask))
            {
                setup_errors++;
                ESP_LOGE(TAG, "CAPTURE_SETUP_MISMATCH reg=0x%02X expected=0x%02X actual=0x%02X mask=0x%02X",
                         checks[i].reg, checks[i].expected, result.value, checks[i].mask);
            }
        }
        error = i2c_master_bus_rm_device(device);
        if (error != ESP_OK)
            setup_errors++;
    }
    else
    {
        setup_errors++;
        ESP_LOGE(TAG, "CAPTURE_SETUP readback unavailable: %s", esp_err_to_name(error));
    }
    unsigned received = 0, valid = 0;
    if (setup_errors == 0)
    {
        vTaskDelay(pdMS_TO_TICKS(200));
        if (CAMERA_STARTUP_DIAGNOSTICS)
        {
            test_frame_sync(false, false, NULL);
            pclk_measurement_t rising, falling;
            test_frame_sync(true, false, &rising);
            test_frame_sync(true, true, &falling);
            bool comparison_valid = rising.lines > 0 && falling.lines > 0 &&
                                    rising.errors == 0 && falling.errors == 0;
            ESP_LOGI(TAG, "PCLK_COMPARE hstop=%u rising=%u..%u samples=%u errors=%u falling=%u..%u samples=%u errors=%u expected=1280 measurements=%s",
                     (unsigned)CAPTURE_HSTOP_VALUE, rising.minimum, rising.maximum, rising.lines, rising.errors,
                     falling.minimum, falling.maximum, falling.lines, falling.errors,
                     comparison_valid ? "VALID" : "INCONCLUSIVE");
            if (comparison_valid && rising.minimum == rising.maximum && falling.minimum == falling.maximum)
                ESP_LOGI(TAG, "PCLK_COMPARE stable falling_minus_rising=%d; sequential line samples, not the same lines",
                         (int)falling.minimum - (int)rising.minimum);
        }
        for (unsigned attempt = 0; attempt < 5; attempt++)
        {
            int64_t begin = esp_timer_get_time();
            camera_fb_t *frame = esp_camera_fb_get();
            long long wait_ms = (esp_timer_get_time() - begin) / 1000;
            if (!frame)
                ESP_LOGE(TAG, "CAPTURE_FRAME attempt=%u wait_ms=%lld => NULL", attempt + 1, wait_ms);
            else
            {
                received++;
                bool shape_ok = frame->buf && frame->width == CAMERA_WIDTH && frame->height == CAMERA_HEIGHT &&
                                frame->format == PIXFORMAT_RGB565 && frame->len == CAMERA_FRAME_BYTES;
                valid += shape_ok;
                unsigned minimum = 255, maximum = 0;
                uint32_t checksum = 2166136261u;
                if (frame->buf)
                    for (size_t i = 0; i < frame->len; i++)
                    {
                        uint8_t value = frame->buf[i];
                        if (value < minimum)
                            minimum = value;
                        if (value > maximum)
                            maximum = value;
                        checksum = (checksum ^ value) * 16777619u;
                    }
                ESP_LOGI(TAG, "CAPTURE_FRAME attempt=%u wait_ms=%lld size=%ux%u bytes=%u format=%d min=%u max=%u hash=0x%08lX shape=%s",
                         attempt + 1, wait_ms, (unsigned)frame->width, (unsigned)frame->height,
                         (unsigned)frame->len, frame->format, minimum, maximum,
                         (unsigned long)checksum, shape_ok ? "PASS" : "FAIL");
                esp_camera_fb_return(frame);
            }
            vTaskDelay(pdMS_TO_TICKS(200));
        }
    }
    else
        ESP_LOGE(TAG, "CAPTURE_FRAME => SKIP: library setup readback failed");
    ESP_LOGI(TAG, "CAPTURE_RESULT setup_errors=%u received=%u/5 valid_shape=%u/5 => %s",
             setup_errors, received, valid,
             setup_errors == 0 && valid == 5 ? "PASS" : "FAIL");
    ESP_LOGI(TAG, "Frame shape/hash do not verify image colors, orientation or D0-D7 wiring");
    bool pattern_restored = true;
    if (CAMERA_COLORBAR_DIAGNOSTIC && setup_errors == 0 && valid == 5)
        pattern_restored = test_camera_colorbars();
    if (CAMERA_WEB_LIVE_VIEW && setup_errors == 0 && valid == 5 && pattern_restored)
    {
        esp_err_t network_error = web_attach_sensor();
        if (network_error != ESP_OK)
        {
            ESP_LOGE(TAG, "Camera controls unavailable: %s; streaming continues", esp_err_to_name(network_error));
            // Cabinet network and setup remain running without camera controls.
        }
        ESP_LOGI(TAG, "CAMERA_WEB capture started: 640x480, two PSRAM framebuffers, GRAB_LATEST; reset board to restart diagnostics");
        unsigned total = 0, failures = 0;
        int64_t begin = esp_timer_get_time();
        int64_t fps_window_begin = begin;
        while (failures < 10)
        {
            web_apply_controls();
            camera_fb_t *frame = esp_camera_fb_get();
            if (!frame)
            {
                failures++;
                ESP_LOGW(TAG, "CAMERA_WEB no frame, consecutive failures=%u", failures);
            }
            else
            {
                web_publish(frame);
                esp_camera_fb_return(frame);
                {
                    failures = 0;
                    total++;
                    if (total % 30 == 0)
                    {
                        int64_t now = esp_timer_get_time();
                        int64_t window_us = now - fps_window_begin;
                        unsigned fps_hundredths = window_us > 0 ? (unsigned)(3000000000LL / window_us) : 0;
                        portENTER_CRITICAL(&web_lock);
                        web_state.camera_fps = fps_hundredths;
                        portEXIT_CRITICAL(&web_lock);
                        ESP_LOGI(TAG, "CAMERA_WEB frames=%u elapsed_ms=%lld capture_fps=%u.%02u CLKRC=0x%02X",
                                 total, (long long)((now - begin) / 1000),
                                 fps_hundredths / 100, fps_hundredths % 100, CAPTURE_CLKRC_VALUE);
                        fps_window_begin = now;
                    }
                }
            }
            vTaskDelay(pdMS_TO_TICKS(10));
        }
        ESP_LOGE(TAG, "CAMERA_WEB stopped after 10 consecutive failures");
    }
    // Keep HTTP/WiFi and cabinet control alive if the camera stops.
    error = esp_camera_deinit();
    ESP_LOGI(TAG, "CAMERA_SHUTDOWN deinit=%s", esp_err_to_name(error));
}

static void web_rfid_uid(const rfid_uid_t *uid, void *context)
{
    (void)context;
    char hex[21];
    rfid_uid_hex(uid, hex);
    classhub_observe_rfid(hex);
}

void app_main(void)
{
    ESP_ERROR_CHECK(classhub_start());
    // Serve configuration before LCD/camera initialization or diagnostics.
    esp_err_t http_error = web_start();
    if (http_error != ESP_OK)
        ESP_LOGE(TAG, "HTTP start failed: %s", esp_err_to_name(http_error));
    esp_err_t lcd_error = tft_init();
    if (lcd_error == ESP_OK)
    {
        esp_err_t rfid_error = rc522_reader_start(SPI2_HOST, web_rfid_uid, NULL);
        if (rfid_error != ESP_OK)
            ESP_LOGE(TAG, "RFID reader unavailable: %s", esp_err_to_name(rfid_error));
        if (xTaskCreate(cabinet_lcd_task, "cabinet_lcd", 4096, NULL, 3, NULL) != pdPASS)
            ESP_LOGE(TAG, "Cannot start cabinet LCD task");
    }
    else
        ESP_LOGE(TAG, "Cabinet LCD unavailable: %s", esp_err_to_name(lcd_error));
    ESP_LOGI(TAG, "SCCB diagnostics then signals/capture: XCLK=%d Hz GPIO%d, SDA=%d SCL=%d",
             TEST_XCLK_HZ, CAM_PIN_XCLK, CAM_PIN_SIOD, CAM_PIN_SIOC);
    ESP_LOGI(TAG, "PWDN/RESET are not controlled; verify module PWDN=LOW, RESET=HIGH");
    const ledc_timer_config_t timer = {
        .speed_mode = LEDC_LOW_SPEED_MODE,
        .duty_resolution = LEDC_TIMER_1_BIT,
        .timer_num = LEDC_TIMER_0,
        .freq_hz = TEST_XCLK_HZ,
        .clk_cfg = LEDC_AUTO_CLK,
    };
    ESP_ERROR_CHECK(ledc_timer_config(&timer));
    const ledc_channel_config_t channel = {
        .gpio_num = CAM_PIN_XCLK,
        .speed_mode = LEDC_LOW_SPEED_MODE,
        .channel = LEDC_CHANNEL_0,
        .timer_sel = LEDC_TIMER_0,
        .duty = 1,
        .hpoint = 0,
    };
    ESP_ERROR_CHECK(ledc_channel_config(&channel));
    vTaskDelay(pdMS_TO_TICKS(300));

    i2c_master_bus_handle_t bus;
    const i2c_master_bus_config_t bus_config = {
        .i2c_port = I2C_NUM_1,
        .sda_io_num = CAM_PIN_SIOD,
        .scl_io_num = CAM_PIN_SIOC,
        .clk_source = I2C_CLK_SRC_DEFAULT,
        .glitch_ignore_cnt = 7,
        .flags.enable_internal_pullup = true,
    };
    ESP_ERROR_CHECK(i2c_new_master_bus(&bus_config, &bus));
    const uint32_t frequencies[] = {TEST_SCCB_HZ};
    unsigned stable_modes = 0, write_passes = 0;
    bool configuration_ok = false;
    for (unsigned index = 0; index < sizeof(frequencies) / sizeof(frequencies[0]); index++)
    {
        i2c_master_dev_handle_t device;
        const i2c_device_config_t config = {
            .dev_addr_length = I2C_ADDR_BIT_LEN_7,
            .device_address = TEST_ADDRESS,
            .scl_speed_hz = frequencies[index],
        };
        ESP_ERROR_CHECK(i2c_master_bus_add_device(bus, &config, &device));
        ESP_LOGI(TAG, "========== SCCB %lu Hz ==========", (unsigned long)frequencies[index]);
        bool stop_ok = test_ids(device, READ_STOP_START, frequencies[index]);
        stable_modes += stop_ok;
        if (stop_ok)
        {
            read_mode_t selected = READ_STOP_START;
            if (CAMERA_STARTUP_DIAGNOSTICS)
            {
                if (test_write_read(device, selected))
                    write_passes++;
                dump_registers(device, selected);
                test_register_selection(device, selected);
                test_suspect_write(device, selected);
                test_soft_reset(device, selected);
            }
            configuration_ok = configure_rgb565_qqvga(device, selected);
        }
        else
        {
            ESP_LOGW(TAG, "WRITE_RESULT => SKIP: STOP_START does not read IDs reliably");
        }
        ESP_ERROR_CHECK(i2c_master_bus_rm_device(device));
    }
    ESP_ERROR_CHECK(i2c_del_master_bus(bus));
    ESP_LOGI(TAG, "SCCB_ONLY_RESULT mode=STOP_START hz=%d stable_id_modes=%u/1 write_passes=%u/1",
             TEST_SCCB_HZ, stable_modes, write_passes);
    if (!configuration_ok)
    {
        ESP_LOGE(TAG, "SIGNAL/CAPTURE => SKIP: configuration verification failed; XCLK remains active");
        return;
    }
    if (CAMERA_STARTUP_DIAGNOSTICS)
    {
        bool pclk_active = measure_signal("PCLK", CAM_PIN_PCLK);
        bool vsync_active = measure_signal("VSYNC", CAM_PIN_VSYNC);
        bool href_active = measure_signal("HREF", CAM_PIN_HREF);
        ESP_LOGI(TAG, "SIGNAL_RESULT PCLK=%s VSYNC=%s HREF=%s; edge activity only, timing/polarity unverified",
                 pclk_active ? "ACTIVE" : "NO_EDGES_OR_ERROR",
                 vsync_active ? "ACTIVE" : "NO_EDGES_OR_ERROR",
                 href_active ? "ACTIVE" : "NO_EDGES_OR_ERROR");
    }
    ESP_ERROR_CHECK(ledc_stop(LEDC_LOW_SPEED_MODE, LEDC_CHANNEL_0, 0));
    test_capture();
    ESP_LOGI(TAG, "Diagnostics finished");
}
