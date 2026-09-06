#include "protocol.h"
#include "service.h"
#include "../jetson/parse_number.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>

static void counted_callback(const proto_request *request, command_response *response,
                              void *context)
{
    unsigned *calls = context;
    assert(request->cmd == 0x42 && request->subcmd == 0x07);
    assert(response->status == COMMAND_EXECUTION_FAILED);
    ++*calls;
    response->data[0] = 0xa5;
    response->status = COMMAND_OK;
}

static void unfinished_callback(const proto_request *request, command_response *response,
                                void *context)
{
    (void)request;
    (void)context;
    response->data[0] = 0x55; /* Deliberately omit status: must not report success. */
}

static void test_dispatch(void)
{
    unsigned calls = 0;
    const command_entry entries[] = {{0x07, counted_callback, &calls}};
    const command_group groups[] = {{0x42, entries, 1}};
    spi_service service;
    uint8_t frame[32], dummy[PROTO_REPLY_SIZE];
    size_t size;
    service_init_commands(&service, groups, 1);
    size = proto_write(frame, sizeof frame, 0x42, 0x07, NULL, 0);
    service_transaction(&service, frame, size, 0);
    assert(calls == 1 && service.tx[1] == COMMAND_OK && service.tx[2] == 0xa5);
    memset(dummy, 0xff, sizeof dummy);
    service_transaction(&service, dummy, sizeof dummy, 0);
    assert(calls == 1 && !service.pending);
    /* Read completion must preserve the custom registry. */
    service_transaction(&service, frame, size, 0);
    assert(calls == 2);
    frame[size - 1] = 0;
    service_transaction(&service, frame, size, 0);
    assert(calls == 2 && service.tx[1] == COMMAND_BAD_FRAME);
    size = proto_write(frame, sizeof frame, 0x42, 0x07, NULL, 0);
    frame[5] ^= 1;
    service_transaction(&service, frame, size, 0);
    assert(calls == 2 && service.tx[1] == COMMAND_BAD_FRAME);
    size = proto_write(frame, sizeof frame, 0x42, 0x08, NULL, 0);
    service_transaction(&service, frame, size, 0);
    assert(calls == 2 && service.tx[1] == COMMAND_NOT_FOUND);
    size = proto_write(frame, sizeof frame, 0x43, 0x07, NULL, 0);
    service_transaction(&service, frame, size, 0);
    assert(calls == 2 && service.tx[1] == COMMAND_NOT_FOUND);
    size = proto_write(frame, sizeof frame, 0x42, 0x07, NULL, 0);
    service_transaction(&service, frame, size, 1);
    assert(calls == 2 && service.tx[1] == COMMAND_BAD_FRAME);
    assert(proto_parse_reply(service.tx, PROTO_REPLY_SIZE, PROTO_REPLY_DATA));
}

static void test_response_status(void)
{
    const command_entry entries[] = {{0, unfinished_callback, NULL}};
    const command_group groups[] = {{1, entries, 1}};
    spi_service service;
    uint8_t frame[8];
    size_t size = proto_write(frame, sizeof frame, 1, 0, NULL, 0);
    service_init_commands(&service, groups, 1);
    service_transaction(&service, frame, size, 0);
    assert(service.tx[0] == 0x60 && service.tx[1] == COMMAND_EXECUTION_FAILED);
    assert(service.tx[2] == 0x55 && service.tx[PROTO_REPLY_SIZE - 1] == 0x0a);
    assert(proto_parse_reply(service.tx, PROTO_REPLY_SIZE, PROTO_REPLY_DATA));
    service.tx[1] = COMMAND_OK;
    assert(!proto_parse_reply(service.tx, PROTO_REPLY_SIZE, PROTO_REPLY_DATA));
    /* Reject the old response header even when its CRC is otherwise valid. */
    service.tx[0] = 0xff;
    {
        uint16_t crc = proto_crc16(service.tx, PROTO_REPLY_SIZE - 3);
        service.tx[PROTO_REPLY_SIZE - 3] = (uint8_t)crc;
        service.tx[PROTO_REPLY_SIZE - 2] = (uint8_t)(crc >> 8);
    }
    assert(!proto_parse_reply(service.tx, PROTO_REPLY_SIZE, PROTO_REPLY_DATA));
}

int main(void)
{
    uint8_t frame[PROTO_MAX_FRAME + 1], copy[PROTO_MAX_FRAME + 1];
    uint8_t payload[PROTO_MAX_DATA];
    uint8_t special[] = {0x30, 0xff, 0x0a, 0x00};
    proto_request request;
    spi_service service;
    size_t size, i, j;
    unsigned long value;
    assert(number("08", 255, &value) && value == 8);
    assert(number("010", 255, &value) && value == 10);
    assert(number("0xFF", 255, &value) && value == 255);
    assert(!number("256", 255, &value));
    assert(!number("0x", 255, &value));
    assert(!number("-1", 255, &value));
    assert(!number(" 1", 255, &value));
    assert(!number("1 ", 255, &value));
    assert(!number("", 255, &value));
    test_dispatch();
    test_response_status();
    assert(proto_crc16((const uint8_t *)"123456789", 9) == 0x4b37);
    assert(proto_crc16(NULL, 0) == 0xffff);
    for (i = 0; i < sizeof payload; ++i) payload[i] = (uint8_t)i;
    for (i = 0; i <= sizeof payload; ++i) {
        size = proto_write(frame, sizeof frame, 7, 9, payload, i);
        assert(size == i + 8);
        assert(proto_parse_write(frame, size, &request));
        assert(request.cmd == 7 && request.subcmd == 9 && request.size == i);
        assert(memcmp(request.data, payload, i) == 0);
        assert(!proto_parse_write(frame, size - 1, &request));
    }
    assert(frame[2] == 0 && frame[3] == 1); /* total 256, little endian */
    assert(!proto_write(frame, sizeof frame, 1, 0, payload, PROTO_MAX_DATA + 1));
    assert(!proto_write(frame, 7, 1, 0, NULL, 0));
    assert(!proto_write(frame, sizeof frame, 1, 0, NULL, 1));
    assert(!proto_parse_write(NULL, 8, &request));
    /* Valid CRC with an incorrect declared length must still be rejected. */
    size = proto_write(frame, sizeof frame, 1, 0, NULL, 0);
    frame[2] = 9;
    {
        uint16_t crc = proto_crc16(frame, size - 3);
        frame[size - 3] = (uint8_t)crc;
        frame[size - 2] = (uint8_t)(crc >> 8);
    }
    assert(!proto_parse_write(frame, size, &request));
    assert(!proto_parse_write(frame, 0, &request));
    assert(!proto_parse_write(frame, PROTO_MAX_FRAME + 1, &request));
    size = proto_write(frame, sizeof frame, 1, 0, special, sizeof special);
    for (i = 0; i < size; ++i) {
        for (j = 0; j < 8; ++j) {
            memcpy(copy, frame, size);
            copy[i] ^= (uint8_t)(1u << j);
            assert(!proto_parse_write(copy, size, &request));
        }
    }
    service_init(&service);
    service_transaction(&service, frame, size, 0);
    assert(service.pending);
    assert(proto_parse_reply(service.tx, PROTO_REPLY_SIZE, PROTO_REPLY_DATA));
    assert(service.tx[1] == 0 && memcmp(service.tx + 2, special, sizeof special) == 0);
    assert(!proto_parse_reply(service.tx, PROTO_REPLY_SIZE - 1, PROTO_REPLY_DATA));
    for (i = 0; i < PROTO_REPLY_SIZE; ++i) {
        memcpy(copy, service.tx, PROTO_REPLY_SIZE);
        copy[i] ^= 1;
        assert(!proto_parse_reply(copy, PROTO_REPLY_SIZE, PROTO_REPLY_DATA));
    }
    /* Missing read followed by a new write must resynchronize. */
    size = proto_write(frame, sizeof frame, 99, 0, NULL, 0);
    service_transaction(&service, frame, size, 0);
    assert(service.pending && service.tx[1] == 2);
    memset(copy, 0xff, PROTO_REPLY_SIZE);
    service_transaction(&service, copy, PROTO_REPLY_SIZE, 0);
    assert(!service.pending && service.tx[0] == 0xff);
    size = proto_write(frame, sizeof frame, 1, 0, special, sizeof special);
    frame[size - 2] ^= 1;
    service_transaction(&service, frame, size, 0);
    assert(service.tx[1] == 1); /* bad CRC does not execute echo */
    size = proto_write(frame, sizeof frame, 1, 0, payload, 32);
    service_transaction(&service, frame, size, 0);
    assert(service.tx[1] == 3);
    service_transaction(&service, frame, size, 1);
    assert(service.tx[1] == 1); /* DMA/SPI fault */
    service_transaction(&service, copy, 2, 0);
    assert(!service.pending); /* aborted read resets state */
    service_transaction(&service, copy, sizeof copy, 1);
    assert(service.pending && service.tx[1] == 1);
    puts("protocol/service tests passed");
    return 0;
}
