#ifndef SPI_PROTOCOL_H
#define SPI_PROTOCOL_H
#include <stddef.h>
#include <stdint.h>

#define PROTO_MAX_FRAME 256u
#define PROTO_REPLY_HEAD 0x60u
#define PROTO_MAX_DATA (PROTO_MAX_FRAME - 8u)
#define PROTO_REPLY_OVERHEAD 5u /* head + status + CRC16 + tail */
#define PROTO_MAX_REPLY_DATA (PROTO_MAX_FRAME - PROTO_REPLY_OVERHEAD)
#define PROTO_REPLY_CLOCKS PROTO_MAX_FRAME

typedef struct {
    uint8_t cmd, subcmd;
    const uint8_t *data;
    size_t size;
} proto_request;
typedef struct {
    uint8_t status;
    const uint8_t *data;
    size_t size;
    size_t frame_size;
} proto_response;

uint16_t proto_crc16(const uint8_t *data, size_t size);
/* Return frame length, or zero on invalid arguments/capacity. */
size_t proto_write(uint8_t *out, size_t capacity, uint8_t cmd, uint8_t subcmd,
                   const uint8_t *data, size_t size);
int proto_parse_write(const uint8_t *frame, size_t size, proto_request *request);
size_t proto_reply(uint8_t *out, size_t capacity, const uint8_t *data, size_t size);
/* Find a variable response in a clocked buffer; bytes after frame_size are padding. */
int proto_parse_reply(const uint8_t *buffer, size_t size, proto_response *response);
#endif
