#include "protocol.h"
#include <string.h>

uint16_t proto_crc16(const uint8_t *data, size_t size)
{
    uint16_t crc = 0xffff;
    size_t i;
    unsigned bit;
    for (i = 0; i < size; ++i) {
        crc ^= data[i];
        for (bit = 0; bit < 8; ++bit)
            crc = (uint16_t)((crc >> 1) ^ ((crc & 1) ? 0xa001 : 0));
    }
    return crc;
}

static void finish(uint8_t *out, size_t size)
{
    uint16_t crc = proto_crc16(out, size - 3);
    out[size - 3] = (uint8_t)crc;
    out[size - 2] = (uint8_t)(crc >> 8);
    out[size - 1] = 0x0a;
}

static int valid(const uint8_t *frame, size_t size, uint8_t head)
{
    uint16_t crc;
    if (!frame || size < 4 || size > PROTO_MAX_FRAME ||
        frame[0] != head || frame[size - 1] != 0x0a) return 0;
    crc = (uint16_t)(frame[size - 3] | ((uint16_t)frame[size - 2] << 8));
    return crc == proto_crc16(frame, size - 3);
}

size_t proto_write(uint8_t *out, size_t capacity, uint8_t cmd, uint8_t subcmd,
                   const uint8_t *data, size_t size)
{
    size_t total;
    if (!out || (size && !data) || size > PROTO_MAX_DATA) return 0;
    total = size + 8;
    if (capacity < total) return 0;
    out[0] = 0x30;
    out[1] = cmd;
    out[2] = (uint8_t)total;
    out[3] = (uint8_t)(total >> 8);
    out[4] = subcmd;
    if (size) memcpy(out + 5, data, size);
    finish(out, total);
    return total;
}

int proto_parse_write(const uint8_t *frame, size_t size, proto_request *request)
{
    if (!request || size < 8 || !valid(frame, size, 0x30)) return 0;
    if ((size_t)(frame[2] | ((uint16_t)frame[3] << 8)) != size) return 0;
    request->cmd = frame[1];
    request->subcmd = frame[4];
    request->data = frame + 5;
    request->size = size - 8;
    return 1;
}

size_t proto_reply(uint8_t *out, size_t capacity, const uint8_t *data, size_t size)
{
    size_t total;
    /* data[0] is mandatory STATUS; following bytes are callback result data. */
    if (!out || !data || size < 1 || size > PROTO_MAX_FRAME - 4) return 0;
    total = size + 4;
    if (capacity < total) return 0;
    out[0] = PROTO_REPLY_HEAD;
    if (size) memcpy(out + 1, data, size);
    finish(out, total);
    return total;
}

int proto_parse_reply(const uint8_t *buffer, size_t size, proto_response *response)
{
    size_t candidate;
    size_t found = 0;
    if (!buffer || !response || size < PROTO_REPLY_OVERHEAD ||
        size > PROTO_REPLY_CLOCKS || buffer[0] != PROTO_REPLY_HEAD) return 0;
    /* Select the longest valid prefix. Padding is 0xFF, so it cannot introduce
       another tail; this also ignores an accidental valid-looking prefix in DATA. */
    for (candidate = PROTO_REPLY_OVERHEAD; candidate <= size; ++candidate) {
        if (buffer[candidate - 1] == 0x0a &&
            valid(buffer, candidate, PROTO_REPLY_HEAD)) found = candidate;
    }
    if (!found) return 0;
    for (candidate = found; candidate < size; ++candidate) {
        if (buffer[candidate] != 0xff) return 0;
    }
    response->status = buffer[1];
    response->data = buffer + 2;
    response->size = found - PROTO_REPLY_OVERHEAD;
    response->frame_size = found;
    return 1;
}
