#include <iostream>
#include <vector>
#include <ctime>
// Exercise the actual firmware adapter with host hardware/NVS/transport stubs.
#include "../src/QrAccess.cpp"
int checks = 0, motions = 0;
bool networkUp = true;
std::vector<std::string> sent;
void check(bool ok,const char *name) { if(!ok){std::cerr << "FAIL: " << name << '\n'; std::exit(1);} ++checks; }
bool publish(const String &payload) { if(!networkUp) return false; sent.push_back(payload.c_str()); return true; }
std::string deadline(int seconds) { time_t t=std::time(nullptr)+seconds; char out[30]; std::strftime(out,sizeof(out),"%Y-%m-%dT%H:%M:%SZ",std::gmtime(&t)); return out; }
const char *c1="11111111111111111111111111111111", *c2="22222222222222222222222222222222", *c3="33333333333333333333333333333333";
bool action(const char *command,const char *purpose="RECEIVE",int seconds=30,const char *session="123",const char *room="DTD201",bool unlocked=false) {
    JsonDocument doc;
    doc["room"]=room; doc["commandId"]=command; doc["sessionId"]=session; doc["purpose"]=purpose; doc["action"]="open"; doc["expiresAt"]=deadline(seconds);
    auto item=doc["expectedEquipment"].to<JsonArray>().add<JsonObject>(); item["equipmentId"]=12; item["name"]="Micro"; item["quantity"]=1;
    return handleQrAction(doc,unlocked,false);
}
void tick() { fakeMillis+=2100; tickQrAccess(); }
void reboot() {
    active=Receipt(); queuedReplay=Receipt(); replayQueued=false; storageReady=false; storageFault=false; pendingUnlock=false; resultsDirty=false; displayExpiry=0;
    initializeQrAccess(publish);
}
bool open() { String id; if(!qrUnlockPending(id)) return false; ++motions; qrUnlockExecuted(); return true; }
int main() {
    initializeQrAccess(publish);
    JsonDocument display;
    display["room"]="DTD201"; display["sessionId"]=123; display["purpose"]="RECEIVE"; display["action"]="display_qr";
    display["content"]=std::string("classhub:qr:v1:")+std::string(64,'a'); display["expiresAt"]=deadline(180);
    String displayPayload; serializeJson(display,displayPayload);
    handleQrDisplay(displayPayload,displayPayload.length());
    check(qrRenders==1 && !overlay->hidden,"backend QR displays on HMI");
    JsonDocument scan;
    scan["action"]="qr_scanned"; scan["room"]="DTD201"; scan["sessionId"]=123;
    scan["purpose"]="RECEIVE"; scan["expiresAt"]=display["expiresAt"].as<String>();
    scan["room"]="OTHER";
    check(!acceptQrScanFeedback(scan),"wrong cabinet scan feedback ignored");
    scan["room"]="DTD201"; scan["sessionId"]=999;
    check(!acceptQrScanFeedback(scan),"wrong session scan feedback ignored");
    scan["sessionId"]=123; scan["purpose"]="RETURN";
    check(!acceptQrScanFeedback(scan),"wrong purpose scan feedback ignored");
    scan["purpose"]="RECEIVE"; scan["expiresAt"]=deadline(-1);
    check(!acceptQrScanFeedback(scan),"stale scan feedback ignored");
    scan["expiresAt"]=display["expiresAt"].as<String>();
    check(acceptQrScanFeedback(scan),"matching valid scan feedback accepted");
    check(!acceptQrScanFeedback(scan),"duplicate scan feedback ignored");
    handleQrDisplay(displayPayload,displayPayload.length());
    check(!acceptQrScanFeedback(scan),"duplicate QR display does not repeat scan feedback");
    displayExpiry=std::time(nullptr)-1; tick();
    check(overlay->hidden,"expired HMI QR cleared");
    check(!acceptQrScanFeedback(scan),"cleared QR cannot beep");
#if !CLASSHUB_REQUIRE_RETURN_VERIFICATION
    {
    JsonDocument admin; admin["action"]="open"; admin["id"]="123";
    check(!handleQrAction(admin,false,false),"admin actions preserved");
    check(action("invalid") && !open(),"malformed QR does not open");
    check(action(c1,"RECEIVE",-1) && !open(),"expired QR action blocked");
    check(action(c1,"RECEIVE",30,"123","TT201") && !open(),"wrong cabinet blocked");
    check(action(c1) && open() && qrOwnsCabinet(),"receive opens and holds cabinet");
    action(c1); check(!open() && motions==1,"duplicate receive does not reopen");
    qrLockExecuted(); check(!qrOwnsCabinet(),"receive close releases cabinet");
    action(c2,"RETURN"); check(open() && qrOwnsCabinet(),"return QR opens and waits closing");
    String session,command,expected;
    check(!qrVerificationContext(session,command,expected) && !submitQrVerification("123",c2,true,true),"sensor pipeline disabled in door test mode");
    reboot(); check(qrNeedsDoorClose() && !open(),"reboot after opening waits close without reopening");
    networkUp=false; qrLockExecuted(); tick();
    check(!qrOwnsCabinet() && resultsDirty,"door closure releases cabinet and queues offline results");
    networkUp=true; sent.clear(); tick();
    check(!resultsDirty && sent.size()==2 && sent[0].find("OPENED")!=std::string::npos && sent[1].find("CLOSED")!=std::string::npos,"reconnect publishes only opened and closed");
    check(!active.verified && active.sequence==0,"door-only flow does not create fake sensor pass");
    action(c2,"RETURN"); check(!open() && motions==2,"duplicate return cannot reopen closed cabinet");
    reboot(); check(!qrOwnsCabinet(),"closed return stays released after reboot");
    action(c3); check(open() && motions==3,"next receive available after closed return");
    qrLockExecuted(); failNvsWrites=true;
    action("44444444444444444444444444444444"); check(storageFault && !open(),"storage failure still blocks actuation");
    std::cout << "PASS: " << checks << " QR door-only firmware checks\n";
    return 0;
    }
#endif
    JsonDocument legacy; legacy["action"]="open"; legacy["id"]="123";
    check(!handleQrAction(legacy,false,false),"legacy action preserved");
    check(action("invalid") && !open(),"malformed QR cannot fall back/open");
    check(action(c1,"RECEIVE",-1) && !open(),"expired action blocked");
    check(action(c1,"RECEIVE",30,"123","TT201") && !open(),"wrong cabinet blocked");
    check(action(c1) && open() && motions==1,"one accepted QR opens once");
    check(qrOwnsCabinet() && qrNeedsDoorClose(),"open holds legacy operations");
    check(!submitQrVerification("123",c1,true,true),"receive cannot verify return");
    check(action(c1) && !open() && motions==1,"duplicate never reopens");
    check(sent.empty(),"MQTT receive callback does not publish recursively");
    tick(); check(!sent.empty() && sent.front().find("OPENED")!=std::string::npos,"opened reported on foreground");
    action(c2,"RETURN"); check(!open(),"new command while opened rejected");
    qrLockExecuted(); tick(); check(!qrOwnsCabinet() && active.phase==QrRules::CLOSED,"receive closes and releases");
    action(c1); check(!open() && motions==1,"closed command cannot replay actuator");
    reboot(); action(c1); check(!open() && motions==1,"reboot preserves dedup receipt");
    action(c2,"RETURN"); check(open() && motions==2,"new return opens");
    reboot(); check(active.phase==QrRules::OPENED && qrNeedsDoorClose() && !open(),"opened reboot waits real door sensor without reopening");
    String session,command,expected;
    check(qrVerificationContext(session,command,expected) && session=="123" && command==c2 && std::string(expected.c_str()).find("equipmentId")!=std::string::npos,"RFID snapshot available with correlation");
    check(!submitQrVerification("999",c2,true,true) && !submitQrVerification("123",c1,true,true),"stale sensor context rejected");
    check(submitQrVerification("123",c2,false,true) && active.sequence==1,"real failed verification recorded");
    qrLockExecuted(); check(qrOwnsCabinet(),"failed return stays pending");
    action(c3,"RETURN",30,"999"); check(!open(),"another session cannot replace failed return");
    action(c3,"RETURN"); check(open() && motions==3,"same session may correct failed return");
    check(!submitQrVerification("123",c2,true,true),"superseded sensor context rejected");
    check(submitQrVerification("123",c3,true,true) && qrOwnsCabinet(),"both sensors still wait close");
    networkUp=false; qrLockExecuted(); tick(); check(resultsDirty,"offline results preserved");
    networkUp=true; tick(); check(!resultsDirty && !qrOwnsCabinet(),"online result replay completes local phase");
    check(!submitQrVerification("123",c3,false,false) && !qrOwnsCabinet(),"late failed sensor result cannot regress completed local return");
    check(sent.back().find("VERIFIED")!=std::string::npos && sent.back().find("sequence")!=std::string::npos,"verification carries sequence");
    reboot(); check(active.verified && active.rfid && active.camera && active.sequence==1,"verification persists reboot");
    // Reset before actuator execution must fail rather than auto-open on boot.
    const char *c4="44444444444444444444444444444444";
    action(c4); reboot(); check(active.phase==QrRules::FAILED && !open(),"interrupted command never reopens");
    sent.clear(); tick(); check(sent.size()==1 && sent[0].find("OPEN_FAILED")!=std::string::npos,"interrupted command reports explicit failure");
    failNvsWrites=true; action("55555555555555555555555555555555"); check(storageFault && !open(),"storage failure prevents motion");
    std::cout << "PASS: " << checks << " QR firmware runtime checks\n";
}

