#include "commands.h"
#include <string.h>

static void echo(const proto_request *request, command_response *response, void *context)
{
    uint8_t status = COMMAND_OK;
    (void)context;
    if (request->size > sizeof response->data) {
        status = COMMAND_BAD_ARGUMENT;
    } else if (request->size) {
        memcpy(response->data, request->data, request->size);
    }
    response->status = status; /* Final callback action: publish execution status. */
}

static const command_entry system_commands[] = {
    {0x00, echo, NULL}
};
const command_group default_command_groups[] = {
    {0x01, system_commands, sizeof system_commands / sizeof system_commands[0]}
};
const size_t default_command_group_count =
    sizeof default_command_groups / sizeof default_command_groups[0];

void command_dispatch(const command_group *groups, size_t count,
                      const proto_request *request, command_response *response)
{
    size_t g, c;
    if (!response) return;
    memset(response->data, 0, sizeof response->data);
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
