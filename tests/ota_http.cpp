#define CLASSHUB_HOST_TEST
#include "../src/classhub.cpp"
#include <iostream>
#include <cstdlib>

struct esp_image_header_t { uint8_t bytes[24]; };
struct esp_image_segment_header_t { uint8_t bytes[8]; };
struct esp_app_desc_t { uint32_t magic_word; uint8_t before_project[44]; char project_name[32]; uint8_t rest[176]; };
static_assert(sizeof(esp_app_desc_t)==256,"descriptor layout");
constexpr uint32_t ESP_APP_DESC_MAGIC_WORD=0xABCD5432;
struct esp_partition_t { size_t size=4096; } slot;
using esp_ota_handle_t=int;
struct httpd_req_t {
    int content_len=1024; std::vector<uint8_t> body; size_t offset=0;
    bool header=true; int status=200; std::string response;
};
constexpr int HTTPD_400_BAD_REQUEST=400;
int chunk=97,interrupt_at=-1,read_delay=1,fail_at=0,begins=0,aborts=0,ends=0,boots=0;
bool owner_running=true,has_slot=true;
std::vector<uint8_t> written;
esp_app_desc_t current={};
const esp_app_desc_t *esp_app_get_description(){return &current;}
const esp_partition_t *esp_ota_get_next_update_partition(const void *){return has_slot?&slot:nullptr;}
int httpd_req_get_hdr_value_str(httpd_req_t *r,const char *,char *out,size_t){if(!r->header)return ESP_FAIL;strcpy(out,"1");return ESP_OK;}
int httpd_resp_set_status(httpd_req_t *r,const char *s){r->status=atoi(s);return 0;}
int httpd_resp_sendstr(httpd_req_t *r,const char *s){r->response=s;return 0;}
int httpd_resp_send_err(httpd_req_t *r,int status,const char *s){r->status=status;r->response=s;return 0;}
int httpd_resp_set_hdr(httpd_req_t *,const char *,const char *){return 0;}
int httpd_resp_set_type(httpd_req_t *,const char *){return 0;}
int httpd_req_recv(httpd_req_t *r,char *out,size_t count){
    fake_ms()+=read_delay;
    if(interrupt_at>=0 && r->offset>=static_cast<size_t>(interrupt_at))return -1;
    size_t n=std::min({count,static_cast<size_t>(chunk),r->body.size()-r->offset});
    if(!n)return -1;
    memcpy(out,r->body.data()+r->offset,n);r->offset+=n;return n;
}
int pdMS_TO_TICKS(int ms){return ms;}
void vTaskDelay(int delay){fake_ms()+=delay;if(owner_running)ota_service();}
int esp_ota_begin(const esp_partition_t *,int,esp_ota_handle_t *handle){++begins;*handle=1;return fail_at==1?ESP_FAIL:ESP_OK;}
int esp_ota_write(int,const void *data,size_t count){if(fail_at==2)return ESP_FAIL;auto p=static_cast<const uint8_t *>(data);written.insert(written.end(),p,p+count);return ESP_OK;}
int esp_ota_abort(int){++aborts;return 0;}
int esp_ota_end(int){++ends;return fail_at==3?ESP_FAIL:ESP_OK;}
int esp_ota_set_boot_partition(const esp_partition_t *){++boots;return fail_at==4?ESP_FAIL:ESP_OK;}
#include "../src/ota_http.inc"
int checks=0;
void check(bool ok,const char *label){if(!ok){std::cerr<<"FAIL: "<<label<<'\n';std::exit(1);}++checks;}
httpd_req_t reset(){
    ota_gate.release();fake_ms()=0;active=Receipt{};storage_fault=unlocked=pulse_active=console_pending=false;
    display_expiry=buzzer_until=0;test_message=-1;begins=aborts=ends=boots=0;written.clear();
    chunk=97;interrupt_at=-1;read_delay=1;fail_at=0;owner_running=has_slot=true;
    current={};strcpy(current.project_name,"OV7670");current.magic_word=ESP_APP_DESC_MAGIC_WORD;
    httpd_req_t r;r.body.resize(1024,0x55);r.body[0]=0xE9;memcpy(r.body.data()+32,&current,sizeof(current));return r;
}
int main(){
    auto r=reset();chunk=1;check(ota_post(&r)==ESP_OK && r.status==200,"short reads assemble valid application");
    check(written==r.body && begins==1 && ends==1 && boots==1,"exact image bytes verified before boot selection");
    check(ota_gate.state==OtaGate::REBOOT && ota_restart_at>ms(),"successful update keeps maintenance until deferred reboot");
    r=reset();r.header=false;ota_post(&r);check(r.status==400 && begins==0,"cross-origin form without console header rejected");
    r=reset();r.content_len=5000;ota_post(&r);check(begins==0 && r.status==400,"oversized image rejected before erase");
    r=reset();has_slot=false;ota_post(&r);check(begins==0,"old partition table cannot begin OTA");
    r=reset();unlocked=true;ota_post(&r);check(r.status==409 && begins==0 && !ota_gate.blocked(),"open door refuses maintenance");
    r=reset();display_expiry=utc_now()+30;ota_post(&r);check(r.status==409 && begins==0,"active QR blocks OTA");
    r=reset();console_pending=true;ota_post(&r);check(r.status==409,"pending console command blocks OTA");
    r=reset();owner_running=false;ota_post(&r);check(r.status==409 && !ota_gate.blocked(),"owner timeout cancels request without flashing");
    r=reset();r.body[0]=0;ota_post(&r);check(begins==0 && !ota_gate.blocked(),"non-image rejected before flash erase");
    r=reset();r.body[32]=0;ota_post(&r);check(begins==0,"bootloader without app descriptor rejected");
    r=reset();r.body[32+48]='X';ota_post(&r);check(begins==0,"other project's image rejected");
    r=reset();interrupt_at=100;ota_post(&r);check(begins==0 && !ota_gate.blocked(),"incomplete descriptor never starts OTA");
    r=reset();interrupt_at=400;ota_post(&r);check(aborts==1 && boots==0 && !ota_gate.blocked(),"interrupted body aborts and retains current boot selection");
    r=reset();read_delay=60000;ota_post(&r);check(boots==0 && !ota_gate.blocked(),"slow upload reaches bounded deadline");
    for(int stage=1;stage<=4;++stage){r=reset();fail_at=stage;ota_post(&r);check(!ota_gate.blocked() && r.status==400,"flash API failure releases maintenance");check(ota_gate.state!=OtaGate::REBOOT,"failed update never schedules reboot");if(stage==3)check(aborts==0 && boots==0,"invalid image does not reuse ended handle or change boot");}
    r=reset();ota_gate.request();ota_service();check(ota_gate.state==OtaGate::GRANTED,"idle owner grants maintenance");
    auto open=parse(R"({"room":"DTD201","action":"open"})");int count=actuations();action(open.get());
    check(actuations()==count,"MQTT action cannot actuate during OTA");
    check(!enqueue(ACTION,"{}"),"commands arriving during maintenance are not queued for later actuation");
    ota_gate.release();ota_service();check(ota_gate.state==OtaGate::IDLE,"canceled request cannot be granted after cancellation");
    std::cout<<"PASS: "<<checks<<" OTA upload and arbitration checks\n";
}
