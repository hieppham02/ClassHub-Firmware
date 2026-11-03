#pragma once
#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include <string.h>
#include <stdarg.h>
typedef int esp_err_t;
typedef int portMUX_TYPE;
#define portMUX_INITIALIZER_UNLOCKED 0
#define portENTER_CRITICAL(lock) ((void)(lock))
#define portEXIT_CRITICAL(lock) ((void)(lock))
typedef int spi_host_device_t;
typedef void *spi_device_handle_t;
enum {ESP_OK, ESP_ERR_NOT_FOUND, ESP_ERR_TIMEOUT, ESP_ERR_INVALID_RESPONSE, ESP_ERR_INVALID_SIZE, ESP_ERR_INVALID_CRC, ESP_ERR_INVALID_STATE, ESP_ERR_NO_MEM};
enum {SPI_TRANS_USE_TXDATA=1,SPI_TRANS_USE_RXDATA=2,SPI_DEVICE_HALFDUPLEX=4,SPI_DEVICE_NO_DUMMY=8,GPIO_MODE_OUTPUT=1,portMAX_DELAY=0,pdPASS=1};
typedef struct {int flags;uint16_t cmd;size_t length,rxlength;uint32_t override_freq_hz;uint8_t tx_data[4],rx_data[4];} spi_transaction_t;
typedef struct {int clock_speed_hz,mode,spics_io_num,queue_size,command_bits,cs_ena_pretrans,cs_ena_posttrans,flags;} spi_device_interface_config_t;
static int64_t clock_us;
static uint8_t registers[64],fast_versions[3],slow_versions[3];
static unsigned active_hz,fast_index,slow_index,acquires,releases,transactions;
static bool locked,fail_acquire,fail_transfer,bad_mode,reserved_zero;
static int input_level=1;
static bool input_enabled;
static bool simulate_rf;
static uint8_t rf_error,rf_count,rf_bits,rf_irq,rf_bytes[64];
static unsigned rf_index;
static int gpio_input_enable(int pin){(void)pin;input_enabled=true;return ESP_OK;}
static int gpio_get_level(int pin){(void)pin;return input_level;}
static unsigned invalid_shapes;
static spi_device_interface_config_t configured;
static void fake_log(const char *tag,const char *format,...){(void)tag;(void)format;}
#define ESP_LOGI(...) fake_log(__VA_ARGS__)
#define ESP_LOGW(...) fake_log(__VA_ARGS__)
static const char *esp_err_to_name(int err){return err==ESP_OK?"ESP_OK":"ERROR";}
static int64_t esp_timer_get_time(void){return clock_us;}
#define pdMS_TO_TICKS(ms) (ms)
static void vTaskDelay(int ticks){clock_us+=(int64_t)ticks*1000;}
static int gpio_set_level(int pin,int level){(void)pin;(void)level;return ESP_OK;}
static int gpio_set_direction(int pin,int mode){(void)pin;(void)mode;return ESP_OK;}
static int spi_device_acquire_bus(void *d,int wait){(void)d;(void)wait;if(fail_acquire)return ESP_ERR_TIMEOUT;locked=true;++acquires;return ESP_OK;}
static void spi_device_release_bus(void *d){(void)d;locked=false;++releases;}
static int spi_device_transmit(void *d,spi_transaction_t *t){
    (void)d;++transactions;
    if(fail_transfer)return ESP_ERR_TIMEOUT;
    if(t->override_freq_hz)active_hz=t->override_freq_hz;
    uint8_t reg=(t->cmd&0x7E)>>1;
    if(t->flags&SPI_TRANS_USE_RXDATA){
        if(t->length!=0 || t->rxlength!=8 || !(t->cmd&0x80) || (t->flags&SPI_TRANS_USE_TXDATA))++invalid_shapes;
        if(reg==0x37){
            if(active_hz==1000000)t->rx_data[0]=fast_versions[fast_index++%3];
            else t->rx_data[0]=slow_versions[slow_index++%3];
        }else if(reg==0x09 && simulate_rf)t->rx_data[0]=rf_bytes[rf_index++%64];
        else if(reg==0x11 && bad_mode)t->rx_data[0]=0;
        else if(reg==0x11 && reserved_zero)t->rx_data[0]=registers[reg]&0xAB;
        else t->rx_data[0]=registers[reg];
    }else {
        if(t->length!=8 || t->rxlength!=0 || (t->cmd&0x80) || !(t->flags&SPI_TRANS_USE_TXDATA))++invalid_shapes;
        registers[reg]=t->tx_data[0];
        if(simulate_rf && reg==0x01 && t->tx_data[0]==0x0C){
            registers[0x04]=rf_irq; registers[0x06]=rf_error;
            registers[0x0A]=rf_count;registers[0x0C]=rf_bits;rf_index=0;
        }
    }
    return ESP_OK;
}
static int spi_bus_add_device(int host,const spi_device_interface_config_t *cfg,void **out){(void)host;configured=*cfg;active_hz=cfg->clock_speed_hz;*out=(void *)1;return ESP_OK;}
static int spi_bus_remove_device(void *d){(void)d;return ESP_OK;}
static int xTaskCreate(void (*task)(void *),const char *name,int stack,void *arg,int priority,void *out){(void)task;(void)name;(void)stack;(void)arg;(void)priority;(void)out;return pdPASS;}
