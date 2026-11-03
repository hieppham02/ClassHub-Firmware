#include "qr_rules.hpp"
#include <iostream>
#include <cstdlib>
#include <string>
int count = 0;
void check(bool ok, const char *name) {
    if (!ok) { std::cerr << "FAIL: " << name << '\n'; std::exit(1); }
    ++count;
}
int main() {
    using namespace QrRules;
    check(content((std::string("classhub:qr:v1:") + std::string(64,'a')).c_str()), "QR content");
    check(!content("classhub:qr:v1:abc"), "truncated QR");
    check(!content((std::string("classhub:qr:v1:") + std::string(64,'g')).c_str()), "non-hex QR");
    check(!content((std::string("https://qr/") + std::string(64,'a')).c_str()), "wrong QR prefix");
    check(session("2147483647") && session("123"), "valid database IDs");
    check(!session("2147483648") && !session("0") && !session("-1") && !session("12x") && !session("01"), "invalid database IDs");
    check(purpose("RECEIVE") && purpose("RETURN") && !purpose("open") && !purpose(nullptr), "purposes");
    check(hex("0123456789ABCDEF0123456789abcdef",32) && !hex("0123",32), "command format");
    const int64_t now = utc("2026-10-14T07:03:00Z");
    check(now == 1791961380LL, "known UTC epoch");
    check(utc("2026-10-14T07:03:00.1234567Z") == now, "backend fractional precision");
    check(utc("2024-02-29T00:00:00Z") > 0 && !utc("2026-02-29T00:00:00Z"), "leap day");
    check(!utc("2026-13-14T07:03:00Z") && !utc("2026-04-31T00:00:00Z"), "invalid date");
    check(!utc("2026-10-14T24:00:00Z") && !utc("2026-10-14T00:60:00Z"), "invalid time");
    check(!utc("2026-10-14T07:03:00+07:00") && !utc("2026-10-14T07:03:00Zjunk"), "UTC format only");
    check(!utc("2026-10-14T07:03:00.Z") && !utc("2026-10-14T07:03:00.12345678Z"), "malformed fractions");
    check(fresh(now,now+30,30) && !fresh(now,now+31,30), "open deadline bound");
    check(fresh(now,now+180,180) && !fresh(now,now+181,180), "display deadline bound");
    check(!fresh(now,now,30) && !fresh(now,now-1,30), "expired command");
    check(!fresh(0,now,30), "unsynchronised clock");
    check(fresh(now,now+35,30,5) && !fresh(now,now+36,30,5), "bounded clock skew for open commands");
    check(fresh(now,now+185,180,5) && !fresh(now,now+186,180,5), "bounded clock skew for display commands");
    check(!fresh(now,now,30,5) && !fresh(now,now-1,30,5), "clock skew does not extend expired commands");
    check(busy(ACCEPTED,false,false,false,false), "pending command holds cabinet");
    check(busy(OPENED,false,false,false,false), "open receive holds cabinet");
    check(!busy(CLOSED,false,false,false,false), "closed receive releases cabinet");
    check(busy(CLOSED,true,false,false,false), "return waits verification");
    check(busy(CLOSED,true,true,false,true) && busy(CLOSED,true,true,true,false), "both sensors required");
    check(!busy(CLOSED,true,true,true,true), "verified closed return releases cabinet");
    check(busy(OPENED,true,true,true,true), "verified return still waits closing");
    check(!busy(FAILED,true,false,false,false), "explicit failure permits reissue");
    check(canVerify(OPENED,true) && canVerify(CLOSED,true), "verification before or after close");
    check(!canVerify(ACCEPTED,true) && !canVerify(FAILED,true) && !canVerify(CLOSED,false), "verification only opened return");
    std::cout << "PASS: " << count << " QR protocol checks\n";
}
