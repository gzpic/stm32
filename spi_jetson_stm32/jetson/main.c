#define _POSIX_C_SOURCE 200809L
#include "protocol.h"
#include "commands.h"
#include "parse_number.h"
#include <errno.h>
#include <fcntl.h>
#include <linux/i2c-dev.h>
#include <stdio.h>
#include <string.h>
#include <sys/file.h>
#include <sys/ioctl.h>
#include <time.h>
#include <unistd.h>

#define I2C_ADDRESS 0x42
#define BUSY_RETRY_MS 1u
#define RESPONSE_TIMEOUT_MS 1000u

static void pause_ms(unsigned ms)
{
    struct timespec delay = {(time_t)(ms / 1000), (long)(ms % 1000) * 1000000L};
    while (nanosleep(&delay, &delay) < 0 && errno == EINTR) {}
}

static unsigned elapsed_ms(const struct timespec *start)
{
    struct timespec now;
    clock_gettime(CLOCK_MONOTONIC, &now);
    return (unsigned)((now.tv_sec - start->tv_sec) * 1000L +
                      (now.tv_nsec - start->tv_nsec) / 1000000L);
}

static int write_command(int fd, const uint8_t *data, size_t size)
{
    ssize_t result = write(fd, data, size);
    if (result < 0) {
        perror("I2C write command");
        return 0;
    }
    if ((size_t)result != size) {
        fprintf(stderr, "Short I2C write: expected %zu, got %zd\n", size, result);
        return 0;
    }
    return 1;
}

static int read_response(int fd, uint8_t *data)
{
    ssize_t result = read(fd, data, PROTO_REPLY_CLOCKS);
    if (result < 0) {
        perror("I2C read response");
        return 0;
    }
    if (result != PROTO_REPLY_CLOCKS) {
        fprintf(stderr, "Short I2C read: expected %u, got %zd\n",
                (unsigned)PROTO_REPLY_CLOCKS, result);
        return 0;
    }
    return 1;
}

static void dump_runs(const uint8_t *data, size_t size)
{
    size_t begin = 0, end;
    fprintf(stderr, "RX:");
    while (begin < size) {
        for (end = begin + 1; end < size && data[end] == data[begin]; ++end) {}
        if (end - begin == 1)
            fprintf(stderr, " %02X", (unsigned)data[begin]);
        else
            fprintf(stderr, " %02Xx%zu", (unsigned)data[begin], end - begin);
        begin = end;
    }
    fputc('\n', stderr);
}

int main(int argc, char **argv)
{
    uint8_t payload[PROTO_MAX_DATA], tx[PROTO_MAX_FRAME], rx[PROTO_REPLY_CLOCKS];
    proto_response response;
    unsigned long cmd, sub, value;
    struct timespec started;
    size_t size, i;
    int fd, result = 1;

    if (argc < 4 || argc > (int)PROTO_MAX_DATA + 4 ||
        !number(argv[2], 255, &cmd) || !number(argv[3], 255, &sub)) {
        fprintf(stderr, "Usage: %s /dev/i2c-7 CMD SUBCMD [BYTE ...]\n"
                        "Numbers: decimal or 0x-prefixed hex. Example: DEVICE 1 0 0x30 0xff 0x0a\n",
                        argv[0]);
        return 2;
    }
    for (i = 0; i < (size_t)argc - 4; ++i) {
        if (!number(argv[i + 4], 255, &value)) {
            fprintf(stderr, "Invalid payload byte: %s\n", argv[i + 4]);
            return 2;
        }
        payload[i] = (uint8_t)value;
    }
    size = proto_write(tx, sizeof tx, (uint8_t)cmd, (uint8_t)sub, payload, i);
    if (!size) return 2;

    fd = open(argv[1], O_RDWR | O_CLOEXEC);
    if (fd < 0) {
        perror("open I2C device");
        return 1;
    }
    if (flock(fd, LOCK_EX | LOCK_NB) < 0) {
        perror("lock I2C device");
        goto done;
    }
    if (ioctl(fd, I2C_SLAVE, I2C_ADDRESS) < 0) {
        perror("select I2C slave 0x42");
        goto done;
    }
    if (!write_command(fd, tx, size)) goto done;

    clock_gettime(CLOCK_MONOTONIC, &started);
    for (;;) {
        if (!read_response(fd, rx)) goto done;
        if (!proto_parse_reply(rx, sizeof rx, &response)) {
            fprintf(stderr, "Invalid response: header/length/CRC/tail. Write may have executed; no automatic retry.\n");
            dump_runs(rx, sizeof rx);
            goto done;
        }
        if (response.status != COMMAND_BUSY) break;
        if (elapsed_ms(&started) >= RESPONSE_TIMEOUT_MS) {
            fprintf(stderr, "Timed out waiting for STM32 response; command was not resent.\n");
            goto done;
        }
        pause_ms(BUSY_RETRY_MS);
    }
    printf("status=%u data[%zu]:", (unsigned)response.status, response.size);
    for (i = 0; i < response.size; ++i) printf(" %02X", (unsigned)response.data[i]);
    putchar('\n');
    result = response.status == COMMAND_OK ? 0 : 3;
done:
    close(fd);
    return result;
}
