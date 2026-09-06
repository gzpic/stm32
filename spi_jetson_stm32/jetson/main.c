#define _POSIX_C_SOURCE 200809L
#include "protocol.h"
#include "parse_number.h"
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/file.h>
#include <sys/ioctl.h>
#include <time.h>
#include <unistd.h>
#include <linux/spi/spidev.h>

static void pause_ms(unsigned ms)
{
    struct timespec delay = {(time_t)(ms / 1000), (long)(ms % 1000) * 1000000L};
    while (nanosleep(&delay, &delay) < 0 && errno == EINTR) {}
}

static int transfer(int fd, const uint8_t *tx, uint8_t *rx, size_t size, uint32_t hz)
{
    struct spi_ioc_transfer t;
    int result;
    memset(&t, 0, sizeof t);
    t.tx_buf = (uintptr_t)tx;
    t.rx_buf = (uintptr_t)rx;
    t.len = (uint32_t)size;
    t.speed_hz = hz;
    t.bits_per_word = 8;
    /* One ioctl per CS window. Do not retry an uncertain write. */
    result = ioctl(fd, SPI_IOC_MESSAGE(1), &t);
    if (result < 0) { perror("SPI transfer"); return 0; }
    if ((size_t)result != size) { fprintf(stderr, "Short SPI transfer\n"); return 0; }
    return 1;
}

int main(int argc, char **argv)
{
    uint8_t payload[PROTO_MAX_DATA], tx[PROTO_MAX_FRAME], rx[PROTO_MAX_FRAME];
    proto_response response;
    uint8_t mode = SPI_MODE_0, bits = 8, lsb = 0;
    uint32_t hz = 100000;
    unsigned long cmd, sub, value;
    size_t size, i;
    int fd, result = 1;
    if (argc < 4 || argc > (int)PROTO_MAX_DATA + 4 ||
        !number(argv[2], 255, &cmd) || !number(argv[3], 255, &sub)) {
        fprintf(stderr, "Usage: %s /dev/spidevB.C CMD SUBCMD [BYTE ...]\n"
                        "Numbers: decimal or 0x-prefixed hex. Example: DEVICE 1 0 0x30 0xff 0x0a\n", argv[0]);
        return 2;
    }
    for (i = 0; i < (size_t)argc - 4; ++i) {
        if (!number(argv[i + 4], 255, &value)) {
            fprintf(stderr, "Invalid payload byte: %s\n", argv[i + 4]); return 2;
        }
        payload[i] = (uint8_t)value;
    }
    size = proto_write(tx, sizeof tx, (uint8_t)cmd, (uint8_t)sub, payload, i);
    fd = open(argv[1], O_RDWR | O_CLOEXEC);
    if (fd < 0) { perror("open SPI device"); return 1; }
    if (flock(fd, LOCK_EX | LOCK_NB) < 0) { perror("lock SPI device"); goto done; }
    if (ioctl(fd, SPI_IOC_WR_MODE, &mode) < 0 ||
        ioctl(fd, SPI_IOC_WR_BITS_PER_WORD, &bits) < 0 ||
        ioctl(fd, SPI_IOC_WR_LSB_FIRST, &lsb) < 0 ||
        ioctl(fd, SPI_IOC_WR_MAX_SPEED_HZ, &hz) < 0) {
        perror("configure SPI"); goto done;
    }
    pause_ms(100);
    if (!transfer(fd, tx, rx, size, hz)) goto done;
    pause_ms(10);
    memset(tx, 0xff, PROTO_REPLY_CLOCKS);
    if (!transfer(fd, tx, rx, PROTO_REPLY_CLOCKS, hz)) goto done;
    pause_ms(10);
    if (!proto_parse_reply(rx, PROTO_REPLY_CLOCKS, &response)) {
        fprintf(stderr, "Invalid response: header/length/CRC/tail. Write may have executed; no automatic retry.\n");
        goto done;
    }
    printf("status=%u data[%zu]:", (unsigned)response.status, response.size);
    for (i = 0; i < response.size; ++i) printf(" %02X", (unsigned)response.data[i]);
    putchar('\n');
    result = response.status == 0 ? 0 : 3;
done:
    close(fd);
    return result;
}
