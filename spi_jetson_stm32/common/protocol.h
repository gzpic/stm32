#ifndef SPI_PROTOCOL_H
#define SPI_PROTOCOL_H
#include <stddef.h>
#include <stdint.h>

#define PROTO_MAX_FRAME 256u
#define PROTO_MAX_DATA (PROTO_MAX_FRAME - 8u)
#define PROTO_REPLY_DATA 32u
#define PROTO_REPLY_SIZE (PROTO_REPLY_DATA + 4u)

typedef struct {
    uint8_t cmd, subcmd;
    const uint8_t *data;
    size_t size;
} proto_request;

uint16_t proto_crc16(const uint8_t *data, size_t size);
/* Return frame length, or zero on invalid arguments/capacity. */
size_t proto_write(uint8_t *out, size_t capacity, uint8_t cmd, uint8_t subcmd,
                   const uint8_t *data, size_t size);
int proto_parse_write(const uint8_t *frame, size_t size, proto_request *request);
size_t proto_reply(uint8_t *out, size_t capacity, const uint8_t *data, size_t size);
/* expected_data is known out of band, never inferred from a delimiter. */
int proto_parse_reply(const uint8_t *frame, size_t size, size_t expected_data);
#endif
