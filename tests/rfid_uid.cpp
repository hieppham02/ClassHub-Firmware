#include "rfid_uid.h"
#include <cstdlib>
#include <iostream>

static unsigned checks;
static void check(bool ok, const char *message) {
    ++checks;
    if (!ok) { std::cerr << "FAIL: " << message << '\n'; std::exit(1); }
}
static void sak_frame(uint8_t value, uint8_t out[3]) {
    out[0] = value;
    uint16_t crc = rfid_crc_a(out, 1);
    out[1] = crc; out[2] = crc >> 8;
}
int main() {
    const uint8_t halt[] = {0x50, 0x00};
    check(rfid_crc_a(halt, 2) == 0xCD57, "HALT CRC wire vector");
    const uint8_t select[] = {0x93, 0x70, 0xDE, 0xAD, 0xBE, 0xEF, 0x22};
    check(rfid_crc_a(select, 7) == 0x9CB9, "SELECT CRC wire vector");
    rfid_uid_t uid = {};
    uint8_t final_sak[3], more_sak[3];
    sak_frame(0x08, final_sak); sak_frame(0x04, more_sak);
    uint8_t block4[] = {0xDE, 0xAD, 0xBE, 0xEF, 0x22};
    char hex[21];
    check(rfid_uid_append(&uid, 0, block4, final_sak), "4-byte UID accepted");
    check(rfid_uid_hex(&uid, hex) && !strcmp(hex, "DEADBEEF"), "canonical UID hex");
    check(!rfid_uid_append(&uid, 1, block4, final_sak), "cannot append after completed UID");
    uid = {};
    block4[4] ^= 1;
    check(!rfid_uid_append(&uid, 0, block4, final_sak) && uid.length == 0, "bad BCC leaves UID empty");
    block4[4] ^= 1;
    final_sak[2] ^= 1;
    check(!rfid_uid_append(&uid, 0, block4, final_sak), "bad SAK CRC rejected");
    final_sak[2] ^= 1;
    check(!rfid_uid_append(&uid, 0, block4, more_sak), "cascade requires CT marker");
    uint8_t first[] = {0x88, 1, 2, 3, 0x88};
    uint8_t second[] = {4, 5, 6, 7, 0};
    check(rfid_uid_append(&uid, 0, first, more_sak), "first cascade strips CT");
    check(!rfid_uid_hex(&uid, hex), "partial UID cannot be published");
    check(rfid_uid_append(&uid, 1, second, final_sak), "7-byte UID accepted");
    check(rfid_uid_hex(&uid, hex) && !strcmp(hex, "01020304050607"), "7-byte order preserved");
    uid = {};
    check(!rfid_uid_append(&uid, 1, second, final_sak), "out-of-order cascade rejected");
    check(rfid_uid_append(&uid, 0, first, more_sak), "10-byte first cascade");
    check(rfid_uid_append(&uid, 1, first, more_sak), "10-byte second cascade");
    check(!rfid_uid_append(&uid, 2, first, more_sak) && uid.length == 6, "fourth cascade forbidden");
    check(rfid_uid_append(&uid, 2, second, final_sak), "10-byte final cascade");
    check(rfid_uid_hex(&uid, hex) && !strcmp(hex, "01020301020304050607"), "10-byte output fits including NUL");
    uid = {};
    check(!rfid_uid_append(&uid, 0, first, final_sak), "CT without cascade flag rejected");
    std::cout << "PASS: " << checks << " RFID UID checks\n";
}
