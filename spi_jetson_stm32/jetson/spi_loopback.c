#define _POSIX_C_SOURCE 200809L
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/file.h>
#include <sys/ioctl.h>
#include <unistd.h>
#include <linux/spi/spidev.h>

#define MAX_TRANSFER 256u

static int parse_u32(const char *text, uint32_t min, uint32_t max, uint32_t *value)
{
    char *end;
    unsigned long parsed;
    if (!text || !*text || *text == '-' || *text == '+') return 0;
    errno = 0;
    parsed = strtoul(text, &end, 0);
    if (errno || *end || parsed < min || parsed > max || parsed > UINT_MAX) return 0;
    *value = (uint32_t)parsed;
    return 1;
}

static int configure(int fd, uint32_t hz)
{
    uint8_t mode = SPI_MODE_0;
    uint8_t bits = 8;
    uint8_t lsb = 0;
    if (ioctl(fd, SPI_IOC_WR_MODE, &mode) < 0 ||
        ioctl(fd, SPI_IOC_WR_BITS_PER_WORD, &bits) < 0 ||
        ioctl(fd, SPI_IOC_WR_LSB_FIRST, &lsb) < 0 ||
        ioctl(fd, SPI_IOC_WR_MAX_SPEED_HZ, &hz) < 0) {
        perror("configure SPI");
        return 0;
    }
    return 1;
}

static int transfer(int fd, const uint8_t *tx, uint8_t *rx, size_t size,
                    uint32_t hz)
{
    struct spi_ioc_transfer request;
    int result;
    memset(&request, 0, sizeof request);
    request.tx_buf = (uintptr_t)tx;
    request.rx_buf = (uintptr_t)rx;
    request.len = (uint32_t)size;
    request.speed_hz = hz;
    request.bits_per_word = 8;
    result = ioctl(fd, SPI_IOC_MESSAGE(1), &request);
    if (result < 0) {
        perror("SPI loopback transfer");
        return 0;
    }
    if ((size_t)result != size) {
        fprintf(stderr, "short transfer: expected %zu, got %d\n", size, result);
        return 0;
    }
    return 1;
}

static void fill_pattern(uint8_t *buffer, size_t size, uint32_t iteration)
{
    static const uint8_t marker[] = {0x00, 0xff, 0x30, 0x0a, 0x55, 0xaa};
    size_t i;
    for (i = 0; i < size; ++i)
        buffer[i] = (uint8_t)(0x5au + i * 73u + iteration * 29u);
    if (iteration == 0) {
        size_t marker_size = size < sizeof marker ? size : sizeof marker;
        memcpy(buffer, marker, marker_size);
    }
}

int main(int argc, char **argv)
{
    static const size_t sizes[] = {1, 2, 6, 8, 32, 256};
    uint8_t tx[MAX_TRANSFER], rx[MAX_TRANSFER];
    uint32_t iterations = 1000;
    uint32_t hz = 100000;
    size_t s, i;
    int fd;

    if (argc < 2 || argc > 4 ||
        (argc >= 3 && !parse_u32(argv[2], 1, 1000000, &iterations)) ||
        (argc >= 4 && !parse_u32(argv[3], 1000, 50000000, &hz))) {
        fprintf(stderr, "Usage: %s /dev/spidevB.C [ITERATIONS] [HZ]\n", argv[0]);
        return 2;
    }
    fd = open(argv[1], O_RDWR | O_CLOEXEC);
    if (fd < 0) {
        perror("open SPI device");
        return 1;
    }
    if (flock(fd, LOCK_EX | LOCK_NB) < 0) {
        perror("lock SPI device");
        close(fd);
        return 1;
    }
    if (!configure(fd, hz)) {
        close(fd);
        return 1;
    }

    printf("device=%s mode=0 bits=8 order=MSB hz=%u iterations=%u\n",
           argv[1], (unsigned)hz, (unsigned)iterations);
    for (s = 0; s < sizeof sizes / sizeof sizes[0]; ++s) {
        for (i = 0; i < iterations; ++i) {
            size_t mismatch;
            fill_pattern(tx, sizes[s], (uint32_t)i);
            memset(rx, 0, sizes[s]);
            if (!transfer(fd, tx, rx, sizes[s], hz)) {
                close(fd);
                return 1;
            }
            if (memcmp(tx, rx, sizes[s]) == 0) continue;
            for (mismatch = 0; mismatch < sizes[s]; ++mismatch)
                if (tx[mismatch] != rx[mismatch]) break;
            fprintf(stderr,
                    "mismatch: size=%zu iteration=%zu offset=%zu tx=%02X rx=%02X\n",
                    sizes[s], i, mismatch, (unsigned)tx[mismatch],
                    (unsigned)rx[mismatch]);
            close(fd);
            return 1;
        }
        printf("length=%zu passed=%u\n", sizes[s], (unsigned)iterations);
    }
    puts("SPI loopback passed");
    close(fd);
    return 0;
}
