#include "commands.h"
#include <string.h>

#if defined(__GNUC__) || defined(__clang__)
__attribute__((weak))
#endif
int platform_temperature_read(uint8_t *temperature)
{
    (void)temperature;
    return 0;
}

#if defined(__GNUC__) || defined(__clang__)
__attribute__((weak))
#endif
int platform_internal_temperature_read(int16_t *temperature_centi_c)
{
    (void)temperature_centi_c;
    return 0;
}

#if defined(__GNUC__) || defined(__clang__)
__attribute__((weak))
#endif
int platform_light_read(uint8_t *light_percent)
{
    (void)light_percent;
    return 0;
}

static void echo(const proto_request *request, command_response *response, void *context)
{
    uint8_t status = COMMAND_OK;
    (void)context;
    if (request->size > sizeof response->data) {
        status = COMMAND_BAD_ARGUMENT;
    } else if (request->size) {
        memcpy(response->data, request->data, request->size);
        response->size = request->size;
    }
    response->status = status; /* Final callback action: publish execution status. */
}

static void identity(const proto_request *request, command_response *response, void *context)
{
    static const uint8_t value[] = "JETSON-STM32-COLLA";
    (void)request;
    (void)context;
    memcpy(response->data, value, sizeof value - 1);
    response->size = sizeof value - 1;
    response->status = COMMAND_OK;
}

static void temperature(const proto_request *request, command_response *response, void *context)
{
    uint8_t value;
    (void)request;
    (void)context;
    if (!platform_temperature_read(&value)) {
        response->status = COMMAND_EXECUTION_FAILED;
        return;
    }
    response->data[0] = value;
    response->size = 1;
    response->status = COMMAND_OK;
}

static void internal_temperature(const proto_request *request, command_response *response,
                                 void *context)
{
    int16_t value;
    (void)request;
    (void)context;
    if (!platform_internal_temperature_read(&value)) {
        response->status = COMMAND_EXECUTION_FAILED;
        return;
    }
    response->data[0] = (uint8_t)value;
    response->data[1] = (uint8_t)((uint16_t)value >> 8);
    response->size = 2;
    response->status = COMMAND_OK;
}

static void light(const proto_request *request, command_response *response, void *context)
{
    uint8_t value;
    (void)request;
    (void)context;
    if (!platform_light_read(&value)) {
        response->status = COMMAND_EXECUTION_FAILED;
        return;
    }
    response->data[0] = value;
    response->size = 1;
    response->status = COMMAND_OK;
}

static const command_entry system_commands[] = {
    {0x00, echo, NULL}
};
static const command_entry identity_commands[] = {
    {0x00, identity, NULL},
    {0x01, temperature, NULL},
    {0x02, internal_temperature, NULL},
    {0x03, light, NULL}
};
const command_group default_command_groups[] = {
    {0x01, system_commands, sizeof system_commands / sizeof system_commands[0]},
    {0xf0, identity_commands, sizeof identity_commands / sizeof identity_commands[0]}
};
const size_t default_command_group_count =
    sizeof default_command_groups / sizeof default_command_groups[0];

void command_dispatch(const command_group *groups, size_t count,
                      const proto_request *request, command_response *response)
{
    size_t g, c;
    if (!response) return;
    memset(response->data, 0, sizeof response->data);
    response->size = 0;
    response->status = COMMAND_EXECUTION_FAILED;
    if (!groups || !request) {
        response->status = COMMAND_BAD_ARGUMENT;
        return;
    }
    for (g = 0; g < count; ++g) {
        if (groups[g].cmdid != request->cmd || !groups[g].commands) continue;
        for (c = 0; c < groups[g].count; ++c) {
            const command_entry *entry = &groups[g].commands[c];
            if (entry->subcmdid == request->subcmd && entry->callback) {
                entry->callback(request, response, entry->context);
                return;
            }
        }
    }
    response->status = COMMAND_NOT_FOUND;
}
