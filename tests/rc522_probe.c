#define RC522_HOST_TEST
#include "../src/rc522_reader.c"
#include <stdio.h>
#include <stdlib.h>
static unsigned checks;
static void check(bool ok,const char *label){if(!ok){fprintf(stderr,"FAIL: %s\n",label);exit(1);}++checks;}
static void reset(void){
    memset(registers,0,sizeof(registers));memset(fast_versions,0x92,3);memset(slow_versions,0x92,3);
    fast_index=slow_index=acquires=releases=transactions=0;
    locked=fail_acquire=fail_transfer=bad_mode=reserved_zero=false;
    clock_us=0;active_hz=spi_hz=1000000;device=(void *)1;
    invalid_shapes=0;
    simulate_rf=false;rf_error=rf_bits=0;rf_count=2;rf_irq=0x30;
    rf_bytes[0]=0x04;rf_bytes[1]=0x00;
}
int main(void){
    uint8_t version=0;
    reset();check(probe_version(&version)==ESP_OK && version==0x92,"stable normal-speed chip reply accepted");
    check(fast_index==3 && slow_index==0 && spi_hz==1000000,"working reader stays at 1MHz");
    check(acquires==1 && releases==1 && !locked,"probe releases shared SPI bus");
    reset();memset(fast_versions,0,3);
    check(probe_version(&version)==ESP_OK && spi_hz==100000,"slow-only chip retains 100kHz fallback");
    check(fast_index==3 && slow_index==3,"both rates sampled when fast rate fails");
    rc522_diagnostics_t snapshot; rc522_reader_diagnostics(&snapshot);
    check(snapshot.slow_tested && snapshot.fast[0]==0 && snapshot.slow[0]==0x92 && snapshot.spi_hz==100000,"web diagnostic snapshot exposes both rates without serial monitor");
    reset();memset(fast_versions,0,3);memset(slow_versions,0,3);
    check(probe_version(&version)==ESP_ERR_NOT_FOUND && !locked,"all-zero MISO is not a chip detection");
    rc522_reader_diagnostics(&snapshot);
    check(snapshot.miso_level==1 && snapshot.fast[0]==0,"diagnostics distinguishes digital HIGH from SPI zero");
    reset();memset(fast_versions,0xFF,3);memset(slow_versions,0xFF,3);
    check(probe_version(&version)==ESP_ERR_NOT_FOUND && releases==1,"all-one MISO is not a chip detection");
    reset();fast_versions[1]=0x91;slow_versions[1]=0x91;
    check(probe_version(&version)==ESP_ERR_NOT_FOUND,"unstable versions rejected at both clocks");
    reset();fail_transfer=true;
    check(probe_version(&version)==ESP_ERR_TIMEOUT && releases==1 && !locked,"transport failure always releases SPI bus");
    reset();fail_acquire=true;
    check(probe_version(&version)==ESP_ERR_TIMEOUT && transactions==0 && releases==0,"bus acquire failure does not transact or release unowned bus");
    reset();check(initialize()==ESP_OK && registers[MODE]==0x3D,"initialization validates writable register readback");
    reset();bad_mode=true;
    check(initialize()==ESP_ERR_INVALID_RESPONSE,"false version alone cannot pass initialization");
    reset();reserved_zero=true;
    check(initialize()==ESP_OK,"reserved ModeReg bits do not reject compatible chip");
    reset();memset(fast_versions,0xFF,3);
    check(initialize()==ESP_OK && spi_hz==100000 && active_hz==100000,"full initialization continues at working fallback speed");
    check(invalid_shapes==0,"register reads and writes use command phase plus exactly one data byte");
    reset();memset(fast_versions,0xEE,3);
    check(initialize()==ESP_OK && spi_hz==100000,"echoed address at fast rate cannot suppress slow-rate retry");
    reset();memset(fast_versions,0xEE,3);memset(slow_versions,0xEE,3);
    check(probe_version(&version)==ESP_ERR_NOT_FOUND,"echoed read command never counts as chip version");
    reset();started=false;
    check(rc522_reader_start(2,NULL,NULL)==ESP_OK && configured.command_bits==8 &&
          configured.flags==(SPI_DEVICE_HALFDUPLEX|SPI_DEVICE_NO_DUMMY) &&
          configured.cs_ena_pretrans==1 && configured.cs_ena_posttrans==1,"reader uses half-duplex command/data phases with explicit CS timing and no extra dummy clocks");
    check(input_enabled,"reader explicitly enables MISO digital input buffer");
    uint8_t wake=0x52,answer[2];
    reset();simulate_rf=true;rf_error=0x04;
    check(exchange("WUPA",&wake,1,7,answer,2)==ESP_OK && answer[0]==4 && answer[1]==0,"hardware CRC flag does not reject a short non-CRC ATQA response");
    check(rf_detail.error==4 && rf_detail.count==2 && !strcmp(rf_detail.stage,"WUPA"),"RF diagnostics preserve stage and register values");
    const uint8_t fatal[]={0x80,0x40,0x10,0x08,0x02,0x01};
    for(unsigned i=0;i<sizeof(fatal);++i){
        reset();simulate_rf=true;rf_error=fatal[i];
        check(exchange("WUPA",&wake,1,7,answer,2)==ESP_ERR_INVALID_RESPONSE,"fatal RF errors still reject response");
    }
    reset();simulate_rf=true;rf_count=3;
    check(exchange("WUPA",&wake,1,7,answer,2)==ESP_ERR_INVALID_RESPONSE,"unexpected response length cannot overflow destination");
    reset();simulate_rf=true;rf_bits=7;
    check(exchange("WUPA",&wake,1,7,answer,2)==ESP_ERR_INVALID_RESPONSE,"partial byte ATQA rejected");
    reset();simulate_rf=true;rf_irq=1;
    check(exchange("WUPA",&wake,1,7,answer,2)==ESP_ERR_TIMEOUT,"no-card timer expiry remains normal timeout");
    printf("PASS: %u RC522 probe and register checks\n",checks);
    return 0;
}
