#include "service.h"
#include <string.h>

void service_init(spi_service *service)
{
    service_init_commands(service, default_command_groups, default_command_group_count);
}

void service_init_commands(spi_service *service, const command_group *groups, size_t count)
{
    service->groups = groups;
    service->group_count = count;
    service->pending = 0;
    service->tx_size = 0;
    memset(service->tx, 0xff, sizeof service->tx);
}

void service_transaction(spi_service *service, const uint8_t *rx, size_t size,
                         int transport_error)
{
    proto_request request;
    command_response response;
    uint8_t data[PROTO_MAX_FRAME - 4];
    size_t data_size = 1; /* STATUS is always present. */
    int valid_write = !transport_error && proto_parse_write(rx, size, &request);
    /* Any valid new write supersedes an unread response, including after host restart. */
    if (service->pending && !valid_write) {
        /* Good or aborted read: response is consumed; never replay business. */
        service->pending = 0;
        service->tx_size = 0;
        memset(service->tx, 0xff, sizeof service->tx);
        return;
    }
    if (!valid_write) {
        data[0] = COMMAND_BAD_FRAME;
    } else {
        command_dispatch(service->groups, service->group_count, &request, &response);
        /* Explicit serialization: no dependence on C structure layout. */
        data[0] = response.status;
        if (response.size > sizeof response.data) {
            data[0] = COMMAND_EXECUTION_FAILED;
        } else {
            if (response.size) memcpy(data + 1, response.data, response.size);
            data_size += response.size;
        }
    }
    memset(service->tx, 0xff, sizeof service->tx);
    service->tx_size = proto_reply(service->tx, sizeof service->tx, data, data_size);
    service->pending = service->tx_size != 0;
}
