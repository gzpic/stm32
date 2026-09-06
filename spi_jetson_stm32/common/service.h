#ifndef SPI_SERVICE_H
#define SPI_SERVICE_H
#include "protocol.h"
#include "commands.h"
typedef struct {
    int pending;
    uint8_t tx[PROTO_MAX_FRAME + 1];
    const command_group *groups;
    size_t group_count;
} spi_service;
void service_init(spi_service *service);
/* Tables and callback contexts must outlive service; initialize before enabling SPI. */
void service_init_commands(spi_service *service, const command_group *groups, size_t count);
/* Background only: process a snapshot captured by the NSS rising-edge ISR. */
void service_transaction(spi_service *service, const uint8_t *rx, size_t size,
                         int transport_error);
#endif
