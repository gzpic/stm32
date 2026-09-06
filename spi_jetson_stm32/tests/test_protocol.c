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
    assert(response->status == COMMAND_EXECUTION_FAILED && response->size == 0);
    ++*calls;
    response->data[0] = 0xa5;
    response->size = 1;
    response->status = COMMAND_OK;
}

static void unfinished_callback(const proto_request *request, command_response *response,
                                void *context)
{
    (void)request;
    (void)context;
    response->data[0] = 0x55;
    response->size = 1; /* Deliberately omit status: must not report success. */
}

static void oversized_callback(const proto_request *request, command_response *response,
                               void *context)
{
    (void)request;
    (void)context;
    response->size = sizeof response->data + 1;
    response->status = COMMAND_OK;
}

static proto_response parse_service_reply(const spi_service *service)
{
    uint8_t clocked[PROTO_REPLY_CLOCKS];
    proto_response response;
    assert(service->tx_size >= PROTO_REPLY_OVERHEAD && service->tx_size <= sizeof clocked);
    memset(clocked, 0xff, sizeof clocked);
    memcpy(clocked, service->tx, service->tx_size);
    assert(proto_parse_reply(clocked, sizeof clocked, &response));
    assert(response.frame_size == service->tx_size);
    return response;
}

static void consume_reply(spi_service *service)
{
    uint8_t dummy[PROTO_REPLY_CLOCKS];
    memset(dummy, 0xff, sizeof dummy);
    service_transaction(service, dummy, sizeof dummy, 0);
    assert(!service->pending && service->tx_size == 0 && service->tx[0] == 0xff);
}

static void test_dispatch(void)
{
    unsigned calls = 0;
    const command_entry entries[] = {{0x07, counted_callback, &calls}};
    const command_group groups[] = {{0x42, entries, 1}};
    spi_service service;
    proto_response response;
    uint8_t frame[32];
    size_t size;

    service_init_commands(&service, groups, 1);
    size = proto_write(frame, sizeof frame, 0x42, 0x07, NULL, 0);
    service_transaction(&service, frame, size, 0);
    response = parse_service_reply(&service);
    assert(calls == 1 && response.status == COMMAND_OK);
    assert(response.size == 1 && response.data[0] == 0xa5);
    consume_reply(&service);

    service_transaction(&service, frame, size, 0);
    assert(calls == 2);
    consume_reply(&service);

    frame[size - 1] = 0;
    service_transaction(&service, frame, size, 0);
    response = parse_service_reply(&service);
    assert(calls == 2 && response.status == COMMAND_BAD_FRAME && response.size == 0);
    consume_reply(&service);

    size = proto_write(frame, sizeof frame, 0x42, 0x08, NULL, 0);
    service_transaction(&service, frame, size, 0);
    response = parse_service_reply(&service);
    assert(calls == 2 && response.status == COMMAND_NOT_FOUND && response.size == 0);
}

static void test_callback_response(void)
{
    const command_entry unfinished[] = {{0, unfinished_callback, NULL}};
    const command_entry oversized[] = {{0, oversized_callback, NULL}};
    const command_group unfinished_group[] = {{1, unfinished, 1}};
    const command_group oversized_group[] = {{1, oversized, 1}};
    spi_service service;
    proto_response response;
    uint8_t frame[8];
    size_t size = proto_write(frame, sizeof frame, 1, 0, NULL, 0);

    service_init_commands(&service, unfinished_group, 1);
    service_transaction(&service, frame, size, 0);
    response = parse_service_reply(&service);
    assert(response.status == COMMAND_EXECUTION_FAILED);
    assert(response.size == 1 && response.data[0] == 0x55);

    service_init_commands(&service, oversized_group, 1);
    service_transaction(&service, frame, size, 0);
    response = parse_service_reply(&service);
    assert(response.status == COMMAND_EXECUTION_FAILED && response.size == 0);
}

static void test_variable_reply(void)
{
    uint8_t payload[PROTO_MAX_FRAME - 4];
    uint8_t frame[PROTO_MAX_FRAME], clocked[PROTO_REPLY_CLOCKS];
    proto_response response;
    size_t result_size, frame_size, i;

    payload[0] = COMMAND_OK;
    for (i = 1; i < sizeof payload; ++i) payload[i] = (uint8_t)i;
    payload[3] = 0x0a; /* A delimiter byte inside result data is legal. */
    for (result_size = 0; result_size <= PROTO_MAX_REPLY_DATA; ++result_size) {
        frame_size = proto_reply(frame, sizeof frame, payload, result_size + 1);
        assert(frame_size == result_size + PROTO_REPLY_OVERHEAD);
        memset(clocked, 0xff, sizeof clocked);
        memcpy(clocked, frame, frame_size);
        assert(proto_parse_reply(clocked, sizeof clocked, &response));
        assert(response.status == COMMAND_OK && response.size == result_size);
        assert(response.frame_size == frame_size);
        assert(memcmp(response.data, payload + 1, result_size) == 0);
        if (frame_size < sizeof clocked) {
            clocked[frame_size] = 0;
            assert(!proto_parse_reply(clocked, sizeof clocked, &response));
        }
    }
    assert(frame_size == PROTO_MAX_FRAME);
    assert(!proto_reply(frame, sizeof frame, NULL, 1));
    assert(!proto_reply(frame, sizeof frame, payload, 0));
    assert(!proto_reply(frame, sizeof frame, payload, sizeof payload + 1));
    assert(!proto_parse_reply(NULL, sizeof clocked, &response));
    assert(!proto_parse_reply(clocked, sizeof clocked, NULL));

    frame_size = proto_reply(frame, sizeof frame, payload, 1);
    frame[0] = 0xff; /* Old head, with recomputed valid CRC, must be rejected. */
    {
        uint16_t crc = proto_crc16(frame, frame_size - 3);
        frame[frame_size - 3] = (uint8_t)crc;
        frame[frame_size - 2] = (uint8_t)(crc >> 8);
    }
    assert(!proto_parse_reply(frame, frame_size, &response));
}

static void test_pending_response(void)
{
    spi_service service;
    uint8_t frame[PROTO_REPLY_CLOCKS];
    size_t size;

    service_init(&service);
    size = proto_write(frame, sizeof frame, 1, 0, NULL, 0);
    service_transaction(&service, frame, size, 0);
    assert(service.pending);
    memset(frame, 0xff, sizeof frame);
    frame[0] = 0x30;
    service_transaction(&service, frame, sizeof frame, 0);
    assert(!service.pending);

    size = proto_write(frame, sizeof frame, 99, 0, NULL, 0);
    service_transaction(&service, frame, size, 0);
    assert(parse_service_reply(&service).status == COMMAND_NOT_FOUND);
    size = proto_write(frame, sizeof frame, 1, 0, NULL, 0);
    service_transaction(&service, frame, size, 0);
    assert(parse_service_reply(&service).status == COMMAND_OK);
}

int main(void)
{
    uint8_t frame[PROTO_MAX_FRAME + 1], copy[PROTO_MAX_FRAME + 1];
    uint8_t payload[PROTO_MAX_DATA];
    proto_request request;
    spi_service service;
    proto_response response;
    size_t size, i, j;
    unsigned long value;

    assert(number("08", 255, &value) && value == 8);
    assert(number("010", 255, &value) && value == 10);
    assert(number("0xFF", 255, &value) && value == 255);
    assert(!number("256", 255, &value) && !number("0x", 255, &value));
    assert(!number("-1", 255, &value) && !number(" 1", 255, &value));
    test_dispatch();
    test_callback_response();
    test_variable_reply();
    test_pending_response();

    assert(proto_crc16((const uint8_t *)"123456789", 9) == 0x4b37);
    for (i = 0; i < sizeof payload; ++i) payload[i] = (uint8_t)i;
    for (i = 0; i <= sizeof payload; ++i) {
        size = proto_write(frame, sizeof frame, 7, 9, payload, i);
        assert(size == i + 8 && proto_parse_write(frame, size, &request));
        assert(request.cmd == 7 && request.subcmd == 9 && request.size == i);
        assert(memcmp(request.data, payload, i) == 0);
    }
    assert(frame[2] == 0 && frame[3] == 1);
    assert(!proto_write(frame, sizeof frame, 1, 0, payload, PROTO_MAX_DATA + 1));

    size = proto_write(frame, sizeof frame, 1, 0, NULL, 0);
    frame[2] = 9;
    {
        uint16_t crc = proto_crc16(frame, size - 3);
        frame[size - 3] = (uint8_t)crc;
        frame[size - 2] = (uint8_t)(crc >> 8);
    }
    assert(!proto_parse_write(frame, size, &request));

    size = proto_write(frame, sizeof frame, 1, 0, payload, sizeof payload);
    service_init(&service);
    service_transaction(&service, frame, size, 0);
    response = parse_service_reply(&service);
    assert(response.status == COMMAND_OK && response.size == sizeof payload);
    assert(memcmp(response.data, payload, sizeof payload) == 0);

    for (i = 0; i < service.tx_size; ++i) {
        for (j = 0; j < 8; ++j) {
            memcpy(copy, service.tx, service.tx_size);
            copy[i] ^= (uint8_t)(1u << j);
            assert(!proto_parse_reply(copy, service.tx_size, &response));
        }
    }
    puts("protocol/service tests passed");
    return 0;
}
