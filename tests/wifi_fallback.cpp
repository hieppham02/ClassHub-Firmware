#include "../src/wifi_fallback.hpp"
#include <iostream>
#include <cstdlib>
int count=0;
void check(bool ok,const char *label){if(!ok){std::cerr<<"FAIL: "<<label<<'\n';std::exit(1);}++count;}
int main(){
    WifiFallback saved;
    check(!saved.access_point(0,true,false),"saved credentials boot in STA without advertising AP");
    check(!saved.access_point(29999,true,false),"normal connection attempt receives 30 second grace");
    check(saved.access_point(30000,true,false),"failed connection exposes fallback setup AP");
    check(saved.access_point(31000,true,false),"AP remains reachable while retrying");
    check(!saved.access_point(32000,true,true),"getting LAN IP turns AP off");
    check(!saved.access_point(40000,true,true),"connected station never advertises setup AP");
    check(!saved.access_point(50000,true,false),"brief disconnect does not restart AP immediately");
    check(!saved.access_point(79000,true,false),"outage timing starts again after successful connection");
    check(saved.access_point(80000,true,false),"sustained later outage enables configuration recovery");
    check(!saved.access_point(81000,true,true),"reconnection disables fallback AP again");
    WifiFallback fresh;
    check(fresh.access_point(0,false,false),"no saved credentials exposes AP immediately");
    check(fresh.access_point(100,true,false),"saving credentials keeps AP available until connection succeeds");
    check(!fresh.access_point(200,true,true),"newly configured station disables AP on success");
    WifiFallback long_uptime;
    constexpr int64_t uptime=INT64_C(5000000000);
    check(!long_uptime.access_point(uptime,true,false),"outage timer works after 32-bit millisecond range");
    check(long_uptime.access_point(uptime+30000,true,false),"long uptime still enables timely recovery");
    std::cout<<"PASS: "<<count<<" Wi-Fi fallback mode checks\n";
}
