#ifndef SERVICE_H
#define SERVICE_H
#include "protocol.h"
#include "commands.h"
typedef struct {
    int pending;
    uint8_t tx[PROTO_MAX_FRAME + 1];
    size_t tx_size;
    const command_group *groups;
    size_t group_count;
} protocol_service;
void service_init(protocol_service *service);
/* Tables and callback contexts must outlive service. */
void service_init_commands(protocol_service *service, const command_group *groups, size_t count);
/* Background only: process exactly one host-to-device command frame. */
void service_process_write(protocol_service *service, const uint8_t *rx, size_t size,
                           int transport_error);
/* Call only after the host has read a complete pending response. */
void service_consume_response(protocol_service *service);
#endif
