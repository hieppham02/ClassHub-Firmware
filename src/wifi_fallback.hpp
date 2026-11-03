#pragma once
#include <stdint.h>

// Retry saved Wi-Fi first; expose setup after a sustained outage.
class WifiFallback {
    bool offline = false;
    bool advertising = false;
    int64_t since = 0;
public:
    static constexpr int64_t grace_ms = 30000;
    bool access_point(int64_t now, bool credentials, bool online) {
        if (online) { offline=false; advertising=false; return false; }
        if (!offline) { offline=true; since=now; }
        if (!credentials || now-since >= grace_ms) advertising=true;
        return advertising;
    }
};
