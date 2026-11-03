#define CLASSHUB_HOST_TEST
#include "../src/classhub.cpp"
#include <iostream>
#include <cstdlib>

int checks = 0;
void check(bool ok, const char *name) {
    if (!ok) { std::cerr << "FAIL: " << name << '\n'; std::exit(1); }
    ++checks;
}
std::string timestamp(int seconds) {
    time_t value = utc_now() + seconds;
    char out[32]; strftime(out, sizeof(out), "%Y-%m-%dT%H:%M:%SZ", gmtime(&value)); return out;
}
std::string command(char c = 'a', const char *purpose = "RECEIVE", int seconds = 25) {
    auto j = object(); put(j.get(), "room", "DTD201"); put(j.get(), "action", "open");
    put(j.get(), "commandId", std::string(32,c)); cJSON_AddNumberToObject(j.get(), "sessionId", 123);
    put(j.get(), "purpose", purpose); put(j.get(), "expiresAt", timestamp(seconds));
    cJSON *array = cJSON_AddArrayToObject(j.get(), "expectedEquipment");
    cJSON *equipment = cJSON_CreateObject(); cJSON_AddItemToArray(array,equipment);
    cJSON_AddNumberToObject(equipment,"equipmentId",1); cJSON_AddNumberToObject(equipment,"quantity",2); put(equipment,"name","Sensor");
    return dump(j.get());
}
void invoke(const std::string &body) { auto j = parse(body); action(j.get()); }
void reset(bool flash = true) {
    ota_gate.release(); buzzer_until=0;
    Message queued;
    while (xQueueReceive(messages,&queued,0)==pdTRUE) free(queued.body);
    if (messages) vQueueDelete(messages);
    if (subscription_acks) vQueueDelete(subscription_acks);
    if (publish_acks) vQueueDelete(publish_acks);
    publish_acks=xQueueCreate(16,sizeof(PublishAck));
    tags_json="[]"; tags_ok=true; tag_store=2; console_pending=false; console_id=0;
    console_queued_at=0; test_message=-1; test_deadline=0; console_result="Ready";
    messages=xQueueCreate(8,sizeof(Message)); subscription_acks=xQueueCreate(8,sizeof(SubscriptionAck));
    active = Receipt{}; queued_replay = Receipt{}; replay_queued = results_dirty = storage_fault = false;
    unlocked = pulse_active = door_open_seen = false; current_session.clear(); display_session.clear(); display_purpose.clear(); display_expiry = 0; scan_notified = false;
    display = {}; config.room="DTD201"; connected=true; mqtt_ready=true; connection_epoch=0; mqtt=reinterpret_cast<void *>(1); ledger=1;
    pins().clear(); actuations()=0; beeps()=0; sent().clear(); fail_write()=false; offline()=false;
    if (flash) { persisted().clear(); staged().clear(); }
    fake_ms()=0; fake_utc()=std::time(nullptr); door_raw=door_stable=1; pins()[CLASSHUB_CLOSE_PIN]=1; door_changed_at=0;
}
void receive(const std::string &channel, const std::string &body, size_t chunk=8192) {
    for (size_t offset=0; offset<body.size(); offset+=chunk) {
        esp_mqtt_event_t e;
        e.total_data_len=body.size(); e.current_data_offset=offset;
        e.data=const_cast<char *>(body.data()+offset); e.data_len=std::min(chunk,body.size()-offset);
        if (!offset) { e.topic=const_cast<char *>(channel.data()); e.topic_len=channel.size(); }
        mqtt_event(nullptr,nullptr,MQTT_EVENT_DATA,&e);
    }
}
bool process_received() {
    Message m;
    if (xQueueReceive(messages,&m,0)!=pdTRUE) return false;
    auto j=parse(m.body);
    if (j && m.epoch==connection_epoch.load()) {
        if (m.kind==ACTION) action(j.get());
        else if (m.kind==QR) show_qr(j.get(),strlen(m.body));
    }
    free(m.body); return true;
}
int main() {
    reset();
    invoke("{\"room\":\"DTD201\",\"action\":\"open\",\"id\":\"0\",\"ts\":1792557581}");
    check(unlocked && pins()[CLASSHUB_LATCH_PIN]==1 && actuations()==1, "legacy admin payload opens latch GPIO");
    check(!pins().count(35) && !pins().count(47) && beeps()==1 && open_trace_stage==OPEN_DONE, "admin open completes all stages without accessing unused LED or PSRAM GPIOs");
    tick_hardware(1500);
    check(pins()[CLASSHUB_LATCH_PIN]==0 && unlocked, "admin pulse ends while waiting for physical door close");
    std::string ap_ssid, ap_password;
    check(ap_credentials("DTD201",ap_ssid,ap_password) && ap_ssid=="DTD201" && ap_password=="DTD201123@@","AP uses cabinet ID and requested password suffix");
    check(ap_credentials("CAB-02",ap_ssid,ap_password) && ap_ssid=="CAB-02" && ap_password=="CAB-02123@@","AP credentials follow changed cabinet ID");
    check(ap_credentials("ABC",ap_ssid,ap_password) && ap_password.size()==8,"shortest cabinet ID produces valid WPA2 password");
    check(!ap_credentials("AB",ap_ssid,ap_password),"cabinet ID cannot produce too-short WPA2 password");
    check(ap_credentials(std::string(20,'A'),ap_ssid,ap_password) && ap_ssid.size()<=32 && ap_password.size()<=63,"maximum cabinet ID fits WiFi credential buffers");
    check(!ap_credentials(std::string(21,'A'),ap_ssid,ap_password) && !ap_credentials("CAB/01",ap_ssid,ap_password),"oversized or topic-invalid cabinet ID rejected");
    reset(); invoke(command());
    tick_hardware(1499); check(pins()[CLASSHUB_LATCH_PIN]==1,"latch remains on before 1500ms");
    tick_hardware(1500); check(pins()[CLASSHUB_LATCH_PIN]==0 && unlocked && active.phase==QrRules::OPENED,"pulse ends without fabricating door close");
    check(actuations()==1 && unlocked && active.phase==QrRules::OPENED,"valid QR actuates once");
    check(persisted().count("active") && persisted().count(std::string(15,'a')),"receipt and active pointer persist");
    invoke(command()); check(actuations()==1 && replay_queued,"duplicate replays without actuation");
    invoke(command('b')); check(actuations()==1,"different QR blocked while door open");
    invoke("{\"room\":\"DTD201\",\"action\":\"lock\"}"); check(unlocked,"admin cannot interrupt QR");
    lock(); check(!unlocked && pins()[CLASSHUB_LATCH_PIN]==0 && active.phase==QrRules::CLOSED,"door close locks and persists CLOSED");
    check(replay(active),"closed receipt replays");
    check(sent()[sent().size()-2].find("OPENED")!=std::string::npos && sent().back().find("CLOSED")!=std::string::npos,"replay preserves OPENED CLOSED order");
    check(!busy(),"test mode releases cabinet after close");
    reset(false); invoke(command()); check(actuations()==0 && replay_queued,"duplicate after reboot never opens again");
    restore_ledger(); check(active.phase==QrRules::CLOSED && results_dirty,"boot restores CLOSED receipt for replay");
    reset(); Receipt interrupted; interrupted.room="DTD201"; interrupted.command=std::string(32,'a'); interrupted.session="123"; interrupted.purpose="RECEIVE";
    save(interrupted); write_string(ledger,"active",interrupted.command); restore_ledger();
    check(active.phase==QrRules::FAILED && actuations()==0,"boot converts ACCEPTED to OPEN_FAILED without actuation");
    reset(); invoke(command()); reset(false); restore_ledger();
    check(active.phase==QrRules::OPENED && actuations()==0 && !unlocked,"boot restores OPENED without re-actuating");
    tick_hardware(51); // Recovered session observes the physically open door first.
    pins()[CLASSHUB_CLOSE_PIN]=0; tick_hardware(60); pins()[CLASSHUB_CLOSE_PIN]=1; tick_hardware(90); pins()[CLASSHUB_CLOSE_PIN]=0; tick_hardware(120); tick_hardware(170);
    check(active.phase==QrRules::OPENED,"bouncing door input does not close session");
    tick_hardware(171); check(active.phase==QrRules::CLOSED,"stable LOW over 50ms confirms pull-up door close after reboot");
    reset(); pins()[CLASSHUB_CLOSE_PIN]=0; door_raw=door_stable=0; invoke(command());
    tick_hardware(500); tick_hardware(2000);
    check(unlocked && active.phase==QrRules::OPENED && !door_open_seen,"held closed switch cannot complete a new unlock");
    pins()[CLASSHUB_CLOSE_PIN]=1; tick_hardware(2010); pins()[CLASSHUB_CLOSE_PIN]=0; tick_hardware(2030); tick_hardware(2100);
    check(unlocked && !door_open_seen,"brief release bounce does not count as opening");
    pins()[CLASSHUB_CLOSE_PIN]=1; tick_hardware(2110); tick_hardware(2160);
    check(!door_open_seen,"release must remain stable longer than 50ms");
    tick_hardware(2161); check(door_open_seen && unlocked,"stable release arms the close confirmation");
    pins()[CLASSHUB_CLOSE_PIN]=0; tick_hardware(2200); tick_hardware(2251);
    check(!unlocked && active.phase==QrRules::CLOSED,"press and hold after opening confirms CLOSED");
    auto held_status_count=sent().size(); tick_hardware(3000); tick_hardware(5000);
    check(sent().size()==held_status_count && actuations()==1,"held switch does not repeat lock status or actuate again");
    invoke(command('b')); tick_hardware(6000);
    check(unlocked && active.phase==QrRules::OPENED && !door_open_seen,"next unlock requires a new release despite held switch");
    reset(); invoke(command('c',"RETURN")); lock();
    check(active.purpose=="RETURN" && !busy() && !active.verified,"RETURN close does not fabricate verification");
    reset(); invoke(command('a',"RECEIVE",-1)); check(actuations()==0,"expired command rejected");
    check(!parse("{}trailing"),"JSON trailing garbage rejected");
    check(!parse(std::string(11,'[')+"0"+std::string(11,']')),"deep JSON rejected before recursive parsing");
    invoke(command('a',"RECEIVE",60)); check(actuations()==0,"excessive future expiry rejected");
    invoke("{\"room\":\"DTD201\",\"action\":\"open\",\"commandId\":null}");
    check(actuations()==0,"malformed QR cannot fall through to admin");
    auto j=parse(command()); put(j.get(),"unused",""); cJSON_ReplaceItemInObject(j.get(),"room",cJSON_CreateString("OTHER")); invoke(dump(j.get()));
    check(actuations()==0,"wrong room rejected");
    j=parse(command()); cJSON_ReplaceItemInObject(j.get(),"expectedEquipment",cJSON_CreateString("invalid")); invoke(dump(j.get()));
    check(actuations()==0,"invalid equipment rejected");
    reset(); fail_write()=true; invoke(command()); check(actuations()==0 && storage_fault,"NVS write failure blocks opening");
    reset(); persisted()[std::string(15,'a')]="corrupt"; invoke(command()); check(actuations()==0 && storage_fault,"corrupt ledger fails closed");
    reset(); Receipt collision; collision.room="DTD201"; collision.command=std::string(15,'a')+std::string(17,'b'); collision.session="123"; collision.purpose="RECEIVE"; collision.phase=QrRules::CLOSED; save(collision);
    invoke(command()); check(actuations()==0 && storage_fault,"NVS key collision cannot overwrite old receipt");
    reset(); invoke(command()); offline()=true; check(!replay(active),"transport failure keeps result pending");
    offline()=false; check(replay(active),"result replays after transport recovery");
    reset(); invoke("{\"room\":\"DTD201\",\"action\":\"open\",\"id\":123}");
    check(actuations()==1 && current_session=="123","legacy admin open supported");
    invoke("{\"room\":\"DTD201\",\"action\":\"close\"}"); check(!unlocked,"legacy admin close supported");
    reset(); j=object(); put(j.get(),"room","DTD201"); put(j.get(),"action","display_qr"); put(j.get(),"sessionId","123"); put(j.get(),"purpose","RECEIVE"); put(j.get(),"content","classhub:qr:v1:"+std::string(64,'a')); auto expiry=timestamp(100); put(j.get(),"expiresAt",expiry);
    show_qr(j.get(),dump(j.get()).size()); check(display.qr_size==37 && display_session=="123","valid display message reaches QR encoder");
    cJSON_ReplaceItemInObject(j.get(),"action",cJSON_CreateString("qr_scanned")); action(j.get()); action(j.get()); check(beeps()==1,"duplicate QR scan beeps only once");
    cJSON_ReplaceItemInObject(j.get(),"sessionId",cJSON_CreateString("124")); action(j.get()); check(beeps()==1,"mismatched scan ignored");
    reset(); j=parse(command()); cJSON *array=const_cast<cJSON *>(item(j.get(),"expectedEquipment")); cJSON_ReplaceItemInObject(cJSON_GetArrayItem(array,0),"quantity",cJSON_CreateNumber(1.5)); invoke(dump(j.get())); check(actuations()==0,"fractional equipment count rejected");
    reset();
    auto qr=object(); put(qr.get(),"room","DTD201"); put(qr.get(),"action","display_qr"); put(qr.get(),"sessionId","123");
    put(qr.get(),"purpose","RECEIVE"); put(qr.get(),"content","classhub:qr:v1:"+std::string(64,'a')); put(qr.get(),"expiresAt",timestamp(180));
    auto qr_payload=dump(qr.get()), open_payload=command('d',"RECEIVE",30);
    fake_utc()-=4;
    check(!QrRules::fresh(utc_now(),QrRules::utc(str(qr.get(),"expiresAt").c_str()),180),"reproduce old first-QR rejection with receiver clock 4s behind");
    auto open_json=parse(open_payload);
    check(!QrRules::fresh(utc_now(),QrRules::utc(str(open_json.get(),"expiresAt").c_str()),30),"reproduce old first-open rejection with receiver clock 4s behind");
    receive(topic("qr",true),qr_payload,71); process_received();
    check(display_session=="123" && display.qr_size>0,"first MQTT QR displays despite small clock skew");
    receive(topic("action",true),open_payload,73); process_received();
    check(actuations()==1 && active.phase==QrRules::OPENED,"first MQTT open executes despite small clock skew");
    receive(topic("action",true),open_payload); process_received();
    check(actuations()==1,"MQTT retry still cannot actuate duplicate command");
    reset(); open_payload=command('d',"RECEIVE",30); fake_utc()-=6; invoke(open_payload);
    check(actuations()==0,"clock tolerance remains bounded to 5s");
    reset(); invoke(command('e',"RECEIVE",-1)); check(actuations()==0,"clock tolerance never accepts already-expired open");
    reset(); esp_mqtt_event_t event; mqtt_event(nullptr,nullptr,MQTT_EVENT_CONNECTED,&event);
    check(connected && !mqtt_ready,"transport connect alone does not mean MQTT is ready");
    event.msg_id=41; mqtt_event(nullptr,nullptr,MQTT_EVENT_SUBSCRIBED,&event);
    event.msg_id=42; mqtt_event(nullptr,nullptr,MQTT_EVENT_SUBSCRIBED,&event);
    SubscriptionState subscriptions; subscriptions.begin(41,42);
    SubscriptionAck ack; xQueueReceive(subscription_acks,&ack,0); subscriptions.accept(ack.id,ack.success);
    check(!subscriptions.ready(),"one SUBACK is insufficient to announce online");
    xQueueReceive(subscription_acks,&ack,0); subscriptions.accept(ack.id,ack.success);
    check(subscriptions.ready(),"ACKs arriving before ID assignment are safely queued");
    subscriptions.begin(43,44); subscriptions.accept(43,true); subscriptions.accept(44,false);
    check(!subscriptions.ready(),"broker subscription rejection never announces ready");
    subscriptions.begin(45,46); subscriptions.accept(41,true); subscriptions.accept(42,true);
    check(!subscriptions.ready(),"old subscription IDs cannot satisfy retry readiness");
    event.msg_id=45; mqtt_event(nullptr,nullptr,MQTT_EVENT_SUBSCRIBED,&event);
    mqtt_event(nullptr,nullptr,MQTT_EVENT_DISCONNECTED,&event); mqtt_event(nullptr,nullptr,MQTT_EVENT_CONNECTED,&event);
    xQueueReceive(subscription_acks,&ack,0);
    check(ack.epoch!=connection_epoch.load() && !mqtt_ready,"reconnect invalidates stale queued SUBACKs");
    reset(); auto large=parse(command()); auto expected=const_cast<cJSON *>(item(large.get(),"expectedEquipment"));
    std::string long_name(2200,'X'); cJSON_ReplaceItemInObject(cJSON_GetArrayItem(expected,0),"name",cJSON_CreateString(long_name.c_str()));
    auto large_payload=dump(large.get()); receive(topic("action",true),large_payload,512);
    Message queued; bool received=xQueueReceive(messages,&queued,0)==pdTRUE;
    check(received && std::string(queued.body)==large_payload,"fragmented MQTT message over 2048 bytes is reconstructed exactly");
    if (received) free(queued.body);
    reset(); event={};
    std::string channel=topic("qr",true); event.topic=const_cast<char *>(channel.c_str()); event.topic_len=channel.size();
    event.total_data_len=qr_payload.size(); event.data=const_cast<char *>(qr_payload.data()); event.data_len=50;
    mqtt_event(nullptr,nullptr,MQTT_EVENT_DATA,&event); mqtt_event(nullptr,nullptr,MQTT_EVENT_DISCONNECTED,&event);
    event.current_data_offset=50; event.data=const_cast<char *>(qr_payload.data()+50); event.data_len=qr_payload.size()-50;
    mqtt_event(nullptr,nullptr,MQTT_EVENT_DATA,&event);
    check(xQueueReceive(messages,&queued,0)!=pdTRUE,"disconnect discards incomplete MQTT payload");
    reset();
    auto local = [&](const char *body) { auto j=parse(body); console_pending=true; console_queued_at=ms(); console_command(j.get()); };
    check(valid_uid("AABBCCDD") && valid_uid("00112233445566") && valid_uid("00112233445566778899"),"RFID accepts standard 4/7/10 byte UIDs");
    check(!valid_uid("aabbccdd") && !valid_uid("AABBCC") && !valid_uid("AABBCCDG"),"RFID rejects noncanonical and malformed UIDs");
    local(R"({"action":"save","uid":"AABBCCDD","name":"TV"})");
    check(!console_pending && persisted()["tags"]==tags_json && cJSON_GetArraySize(parse(tags_json).get())==1,"RFID save commits to flash before success");
    local(R"({"action":"save","uid":"AABBCCDD","name":"Projector"})");
    auto catalog=parse(tags_json);
    check(cJSON_GetArraySize(catalog.get())==1 && str(cJSON_GetArrayItem(catalog.get(),0),"name")=="Projector","saving same UID updates label without duplicate");
    std::string before=tags_json; fail_write()=true;
    local(R"({"action":"save","uid":"11223344","name":"Remote"})");
    check(tags_json==before && console_result.find("Error") == 0,"flash failure preserves visible saved catalog");
    fail_write()=false;
    local(R"({"action":"delete","uid":"AABBCCDD"})");
    check(tags_json=="[]" && persisted()["tags"]=="[]","RFID deletion persists");
    check(!valid_tags(parse(R"([{"uid":"AABBCCDD","name":"x"},{"uid":"AABBCCDD","name":"y"}])").get()) && !valid_tags(parse("{}").get()),"corrupt or duplicate persisted tags rejected");
    reset(); tags_ok=false; local(R"({"action":"save","uid":"AABBCCDD"})");
    check(persisted().empty(),"unreadable RFID store cannot be overwritten by web save");
    reset(); pins()[CLASSHUB_CLOSE_PIN]=0; door_raw=door_stable=0;
    current_session="old-session"; local(R"({"action":"open"})");
    check(actuations()==1 && pulse_active && current_session.empty(),"local test starts one pulse without reusing borrower session");
    local(R"({"action":"open"})"); check(actuations()==1,"repeated local test blocked while door cycle active");
    tick_hardware(1500); check(pins()[CLASSHUB_LATCH_PIN]==0,"web unlock pulse ends after 1500ms");
    check(unlocked && !door_open_seen,"held LOW closed switch does not finish local test before door opens");
    pins()[CLASSHUB_CLOSE_PIN]=1; tick_hardware(1600); tick_hardware(1651);
    pins()[CLASSHUB_CLOSE_PIN]=0; tick_hardware(1700); tick_hardware(1751);
    check(!unlocked && !busy(),"pull-up open HIGH then closed LOW releases local cabinet busy state");
    auto prior_local_beeps=beeps(); local(R"({"action":"beep"})");
    check(beeps()==prior_local_beeps+1 && console_result.find("OK:")==0,"web tests work again after pull-up door cycle");
    reset(); invoke(command()); auto prior=beeps(); local(R"({"action":"beep"})");
    check(beeps()==prior && console_result.find("Error")==0,"web buzzer cannot interrupt active borrow cycle");
    reset(); console_pending=true; console_queued_at=0; fake_ms()=3001;
    auto expired=parse(R"({"action":"open"})"); console_command(expired.get());
    check(actuations()==0 && !console_pending,"expired queued request never actuates");
    reset(); local(R"({"action":"beep"})"); check(beeps()==1 && buzzer_until==300,"web buzzer uses nonblocking 300ms pulse");
    reset(); local(R"({"action":"mqtt"})");
    check(console_pending && test_message>=0,"MQTT test waits for broker acknowledgement");
    int expected_ack=test_message; PublishAck unrelated{0,expected_ack+1}; xQueueSend(publish_acks,&unrelated,0); console_tick();
    check(console_pending,"unrelated PUBACK cannot complete MQTT test");
    PublishAck stale_ack{99,expected_ack}; xQueueSend(publish_acks,&stale_ack,0); console_tick();
    check(console_pending,"old connection PUBACK cannot complete MQTT test");
    PublishAck matched{0,expected_ack}; xQueueSend(publish_acks,&matched,0); console_tick();
    check(!console_pending && console_result.find("OK:")==0,"matching QoS1 PUBACK completes MQTT test");
    local(R"({"action":"mqtt"})"); fake_ms()=10000; console_tick();
    check(!console_pending && console_result.find("Error:")==0,"missing PUBACK times out");
    reset(); local(R"({"action":"mqtt"})"); ++connection_epoch; console_tick();
    check(!console_pending && console_result.find("Error:")==0,"reconnection invalidates pending broker test");
    reset(); connected=false; local(R"({"action":"mqtt"})"); check(!console_pending && test_message<0,"offline MQTT test fails immediately");
    std::cout << "PASS: " << checks << " native cabinet runtime checks\n";
}
