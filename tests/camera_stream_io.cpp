#include "camera_stream_io.h"
#include "camera_profile.h"
#include <cstdlib>
#include <iostream>
#include <string>
#include <vector>
#include <algorithm>
struct Writer {
    std::vector<int> writes;
    size_t index = 0;
    int64_t now = 0, write_cost = 10;
    std::string received;
};
int write_fake(void *ctx, const char *p, size_t n) {
    auto &w = *static_cast<Writer *>(ctx);
    w.now += w.write_cost;
    int count = w.index < w.writes.size() ? w.writes[w.index++] : static_cast<int>(n);
    if (count > 0 && static_cast<size_t>(count) <= n) w.received.append(p, count);
    return count;
}
int64_t clock_fake(void *ctx) { return static_cast<Writer *>(ctx)->now; }
void wait_fake(void *ctx) { static_cast<Writer *>(ctx)->now += 100; }
int main() {
    int checks = 0;
    auto check = [&](bool ok, const char *name) {
        if (!ok) { std::cerr << "FAIL: " << name << '\n'; std::exit(1); } ++checks;
    };
    std::string packet(CAMERA_PACKET_BYTES, 'x');
    for (size_t i = 0; i < packet.size(); ++i) packet[i] = static_cast<char>(i % 251);
    auto send = [&](Writer &w, int64_t limit = 1000000) {
        return camera_send_all(&w, packet.data(), packet.size(), write_fake, clock_fake, wait_fake, limit);
    };
    Writer whole;
    check(send(whole) == CAMERA_PACKET_BYTES && whole.received == packet, "complete VGA frame");
    Writer partial; partial.writes = {2, 100, 1460, 5};
    check(send(partial) == CAMERA_PACKET_BYTES && partial.received == packet, "partial writes preserve every byte in order");
    Writer transient; transient.writes = {20, -2, -2, 100, -2};
    check(send(transient) == CAMERA_PACKET_BYTES && transient.received == packet, "temporary socket pressure recovers");
    Writer failure; failure.writes = {100, -1};
    check(send(failure) < 0 && failure.received.size() == 100, "mid-frame failure is never success");
    Writer zero; zero.writes = {0};
    check(send(zero) < 0, "closed socket cannot loop forever");
    Writer blocked; blocked.writes = std::vector<int>(100, -2);
    check(send(blocked, 1000) < 0 && blocked.now < 1200, "repeated transient errors respect deadline");
    Writer slow; slow.writes = std::vector<int>(100, 1); slow.write_cost = 100;
    check(send(slow, 1000) < 0 && slow.received.size() == 10, "partial progress does not reset deadline");
    Writer invalid; invalid.writes = {700000};
    check(send(invalid) < 0, "invalid write size rejected");
    Writer empty;
    check(camera_send_all(&empty, "", 0, write_fake, clock_fake, wait_fake, 1000) == 0, "empty payload");
    std::cout << "PASS: " << checks << " camera transport checks\n";
}
