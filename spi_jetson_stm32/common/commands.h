#ifndef SPI_COMMANDS_H
#define SPI_COMMANDS_H
#include "protocol.h"

enum {
    COMMAND_OK = 0,
    COMMAND_BAD_FRAME = 1,
    COMMAND_NOT_FOUND = 2,
    COMMAND_BAD_ARGUMENT = 3,
    COMMAND_EXECUTION_FAILED = 4
};
typedef struct {
    uint8_t status;
    uint8_t data[PROTO_REPLY_DATA - 1];
} command_response;
/* Background only. Data starts zeroed; status defaults to execution failed.
   Request data is valid only until this callback returns. Do not retain it.
   Write response->status LAST on every exit path, after filling response->data.
   Callbacks must respect sizeof response->data and the SPI timing budget. */
typedef void (*command_callback)(const proto_request *request,
                                 command_response *response, void *context);
typedef struct {
    uint8_t subcmdid;
    command_callback callback;
    void *context;
} command_entry;
typedef struct {
    uint8_t cmdid;
    const command_entry *commands;
    size_t count;
} command_group;

void command_dispatch(const command_group *groups, size_t count,
                      const proto_request *request, command_response *response);
extern const command_group default_command_groups[];
extern const size_t default_command_group_count;
#endif
