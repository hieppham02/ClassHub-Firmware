#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

// ISO14443-A CRC, initial value 0x6363, transmitted low byte first.
static inline uint16_t rfid_crc_a(const uint8_t *data, size_t length)
{
    uint16_t crc = 0x6363;
    for (size_t i = 0; i < length; ++i) {
        uint8_t b = data[i] ^ (uint8_t)crc;
        b ^= (uint8_t)(b << 4);
        crc = (crc >> 8) ^ ((uint16_t)b << 8) ^ ((uint16_t)b << 3) ^ (b >> 4);
    }
    return crc;
}

typedef struct {
    uint8_t bytes[10];
    uint8_t length;
} rfid_uid_t;

// Validate a complete anticollision response + SELECT SAK before appending.
// One tag at a time: collision resolution between multiple tags is not attempted.
static inline bool rfid_uid_append(rfid_uid_t *uid, unsigned level,
                                   const uint8_t block[5], const uint8_t sak[3])
{
    if (!uid || !block || !sak || level > 2 || uid->length != level * 3)
        return false;
    if ((uint8_t)(block[0] ^ block[1] ^ block[2] ^ block[3]) != block[4])
        return false;
    uint16_t crc = rfid_crc_a(sak, 1);
    if (sak[1] != (uint8_t)crc || sak[2] != (uint8_t)(crc >> 8))
        return false;
    bool more = (sak[0] & 4) != 0;
    if (more && (level == 2 || block[0] != 0x88))
        return false;
    if (!more && level < 2 && block[0] == 0x88)
        return false;
    size_t count = more ? 3 : 4;
    memcpy(uid->bytes + uid->length, block + (more ? 1 : 0), count);
    uid->length += count;
    return true;
}

static inline bool rfid_uid_hex(const rfid_uid_t *uid, char out[21])
{
    if (!uid || !out || (uid->length != 4 && uid->length != 7 && uid->length != 10))
        return false;
    static const char hex[] = "0123456789ABCDEF";
    for (unsigned i = 0; i < uid->length; ++i) {
        out[2 * i] = hex[uid->bytes[i] >> 4];
        out[2 * i + 1] = hex[uid->bytes[i] & 15];
    }
    out[2 * uid->length] = '\0';
    return true;
}
