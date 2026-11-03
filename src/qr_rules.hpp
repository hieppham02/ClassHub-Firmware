#pragma once
#include <stdint.h>
#include <stddef.h>
#include <string.h>

namespace QrRules {
enum Phase { ACCEPTED, OPENED, CLOSED, FAILED };
inline bool hex(const char *s, size_t length) {
    if (!s || strlen(s) != length) return false;
    for (size_t i = 0; i < length; ++i)
        if (!((s[i] >= '0' && s[i] <= '9') || (s[i] >= 'a' && s[i] <= 'f') || (s[i] >= 'A' && s[i] <= 'F'))) return false;
    return true;
}
inline bool content(const char *s) {
    return s && strncmp(s, "classhub:qr:v1:", 15) == 0 && hex(s + 15, 64);
}
inline bool session(const char *s) {
    if (!s || !*s || *s == '0') return false;
    int64_t n = 0;
    for (; *s; ++s) {
        if (*s < '0' || *s > '9') return false;
        n = n * 10 + *s - '0';
        if (n > 2147483647) return false;
    }
    return true;
}
inline bool purpose(const char *s) { return s && (!strcmp(s, "RECEIVE") || !strcmp(s, "RETURN")); }
inline bool leap(int y) { return y % 4 == 0 && (y % 100 != 0 || y % 400 == 0); }
inline int number(const char *s, int n) {
    int value = 0;
    for (int i = 0; i < n; ++i) { if (s[i] < '0' || s[i] > '9') return -1; value = value * 10 + s[i] - '0'; }
    return value;
}
// Backend sends UTC ISO-8601 with optional fractional seconds, always Z.
inline int64_t utc(const char *s) {
    if (!s || strlen(s) < 20 || s[4] != '-' || s[7] != '-' || s[10] != 'T' || s[13] != ':' || s[16] != ':') return 0;
    const int y = number(s,4), m = number(s+5,2), d = number(s+8,2), h = number(s+11,2), min = number(s+14,2), sec = number(s+17,2);
    const int days[] = {31,28,31,30,31,30,31,31,30,31,30,31};
    if (y < 2024 || y > 2099 || m < 1 || m > 12 || d < 1 || d > days[m-1] + (m == 2 && leap(y)) || h < 0 || h > 23 || min < 0 || min > 59 || sec < 0 || sec > 59) return 0;
    size_t end = 19;
    if (s[end] == '.') { ++end; size_t start = end; while (s[end] >= '0' && s[end] <= '9') ++end; if (end == start || end - start > 7) return 0; }
    if (s[end] != 'Z' || s[end+1] != '\0') return 0;
    int64_t total = 0;
    for (int year = 1970; year < y; ++year) total += 365 + leap(year);
    for (int month = 1; month < m; ++month) total += days[month-1] + (month == 2 && leap(y));
    return ((total + d - 1) * 24 + h) * 3600 + min * 60 + sec;
}
inline bool fresh(int64_t now, int64_t expiry, int maxSeconds, int futureClockSkew = 0) {
    // Only relax the upper lifetime bound for a slightly slower receiver clock.
    // Never accept an already-expired message, even inside this tolerance.
    return now >= 1704067200 && expiry > now &&
        expiry - now <= (int64_t)maxSeconds + futureClockSkew;
}
inline bool busy(Phase phase, bool returning, bool verified, bool rfid, bool camera) {
    return phase == ACCEPTED || phase == OPENED || (phase == CLOSED && returning && !(verified && rfid && camera));
}
inline bool canVerify(Phase phase, bool returning) { return returning && (phase == OPENED || phase == CLOSED); }
}
