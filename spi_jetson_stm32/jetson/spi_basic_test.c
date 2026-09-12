#define _POSIX_C_SOURCE 200809L
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/ioctl.h>
#include <time.h>
#include <unistd.h>
#include <linux/gpio.h>
#include <linux/spi/spidev.h>

#define TEST_SIZE 32u
#define EXPECTED_REPLY 0xA5u
#define SOFTWARE_CS_CHIP "/dev/gpiochip0"
#define SOFTWARE_CS_LINE 105u /* Jetson J12 physical pin 29, PQ.05 */

static int set_software_cs(int fd, uint8_t value)
{
    struct gpiohandle_data data;

    memset(&data, 0, sizeof data);
    data.values[0] = value;
    return ioctl(fd, GPIOHANDLE_SET_LINE_VALUES_IOCTL, &data);
}

static int open_software_cs(void)
{
    struct gpiohandle_request request;
    int chip_fd;

    chip_fd = open(SOFTWARE_CS_CHIP, O_RDONLY | O_CLOEXEC);
    if (chip_fd < 0)
        return -1;

    memset(&request, 0, sizeof request);
    request.lineoffsets[0] = SOFTWARE_CS_LINE;
    request.flags = GPIOHANDLE_REQUEST_OUTPUT;
    request.default_values[0] = 1;
    request.lines = 1;
    snprintf(request.consumer_label, sizeof request.consumer_label,
             "stm32-spi-softcs");

    if (ioctl(chip_fd, GPIO_GET_LINEHANDLE_IOCTL, &request) < 0) {
        close(chip_fd);
        return -1;
    }
    close(chip_fd);
    return request.fd;
}

int main(int argc, char **argv)
{
    struct spi_ioc_transfer transfer;
    uint8_t tx[TEST_SIZE], rx[TEST_SIZE];
    uint8_t mode = SPI_MODE_0;
    uint8_t bits = 8;
    uint32_t hz = 100000;
    size_t i;
    size_t test_size = TEST_SIZE;
    int fd;
    int cs_fd = -1;
    int result;
    int use_software_cs;

    if (argc < 2 || argc > 3 ||
        (argc == 3 && strcmp(argv[2], "zero") != 0 &&
         strcmp(argv[2], "ones") != 0 && strcmp(argv[2], "byte") != 0 &&
         strcmp(argv[2], "hold") != 0 && strcmp(argv[2], "hold3") != 0 &&
         strcmp(argv[2], "softcs") != 0 && strcmp(argv[2], "softone") != 0 &&
         strcmp(argv[2], "softone1") != 0 && strcmp(argv[2], "softone2") != 0 &&
         strcmp(argv[2], "softone3") != 0 && strcmp(argv[2], "mode2") != 0)) {
        fprintf(stderr,
                "Usage: %s /dev/spidevB.C [zero|ones|byte|hold|hold3|softcs|softone|softone1|softone2|softone3|mode2]\n",
                argv[0]);
        return 2;
    }
    use_software_cs = argc == 3 &&
                      (strcmp(argv[2], "softcs") == 0 ||
                       strncmp(argv[2], "softone", 7) == 0);
    if (argc == 3 && strncmp(argv[2], "softone", 7) == 0) test_size = 1;
    if (argc == 3 && strcmp(argv[2], "softone1") == 0) mode = SPI_MODE_1;
    if (argc == 3 && strcmp(argv[2], "softone2") == 0) mode = SPI_MODE_2;
    if (argc == 3 && strcmp(argv[2], "softone3") == 0) mode = SPI_MODE_3;
    if (argc == 3 && strcmp(argv[2], "softcs") == 0) mode = SPI_MODE_2;
    if (argc == 3 && strcmp(argv[2], "mode2") == 0) mode = SPI_MODE_2;
    if (argc == 3 && strcmp(argv[2], "hold3") == 0) mode = SPI_MODE_3;
    for (i = 0; i < TEST_SIZE; ++i) {
        if (argc == 3 &&
            (strcmp(argv[2], "ones") == 0 || strcmp(argv[2], "hold3") == 0))
            tx[i] = 0xff;
        else if (argc == 3 &&
                 (strcmp(argv[2], "zero") == 0 || strcmp(argv[2], "hold") == 0))
            tx[i] = 0;
        else
            tx[i] = (uint8_t)i;
    }
    if (argc == 3 && strncmp(argv[2], "softone", 7) == 0) tx[0] = 0x3c;
    memset(rx, 0, sizeof rx);

    fd = open(argv[1], O_RDWR | O_CLOEXEC);
    if (fd < 0) {
        perror("open SPI device");
        return 1;
    }
    if (ioctl(fd, SPI_IOC_WR_MODE, &mode) < 0 ||
        ioctl(fd, SPI_IOC_WR_BITS_PER_WORD, &bits) < 0 ||
        ioctl(fd, SPI_IOC_WR_MAX_SPEED_HZ, &hz) < 0) {
        perror("configure SPI");
        close(fd);
        return 1;
    }

    memset(&transfer, 0, sizeof transfer);
    transfer.tx_buf = (uintptr_t)tx;
    transfer.rx_buf = (uintptr_t)rx;
    transfer.len = test_size;
    transfer.speed_hz = hz;
    transfer.bits_per_word = bits;
    transfer.cs_change = argc == 3 &&
                         (strcmp(argv[2], "hold") == 0 ||
                          strcmp(argv[2], "hold3") == 0);
    if (use_software_cs) {
        struct timespec settle = {0, 1000000L};
        struct timespec idle = {0, 100000000L};

        cs_fd = open_software_cs();
        if (cs_fd < 0) {
            perror("open software CS (gpiochip0 line 105)");
            close(fd);
            return 1;
        }
        nanosleep(&idle, NULL);
        if (set_software_cs(cs_fd, 0) < 0) {
            perror("assert software CS");
            close(cs_fd);
            close(fd);
            return 1;
        }
        nanosleep(&settle, NULL);
        result = ioctl(fd, SPI_IOC_MESSAGE(1), &transfer);
        nanosleep(&settle, NULL);
        if (set_software_cs(cs_fd, 1) < 0) {
            perror("release software CS");
            close(cs_fd);
            close(fd);
            return 1;
        }
        close(cs_fd);
    } else if (argc == 3 && strcmp(argv[2], "byte") == 0) {
        struct timespec delay = {0, 10000000L};
        for (i = 0; i < TEST_SIZE; ++i) {
            transfer.tx_buf = (uintptr_t)&tx[i];
            transfer.rx_buf = (uintptr_t)&rx[i];
            transfer.len = 1;
            result = ioctl(fd, SPI_IOC_MESSAGE(1), &transfer);
            if (result != 1) break;
            nanosleep(&delay, NULL);
        }
        if (i == TEST_SIZE) result = TEST_SIZE;
    } else {
        result = ioctl(fd, SPI_IOC_MESSAGE(1), &transfer);
    }
    if (transfer.cs_change) {
        puts("Holding CS active for 15 seconds");
        fflush(stdout);
        sleep(15);
    }
    close(fd);
    if (result != (int)test_size) {
        if (result < 0) perror("SPI transfer");
        else fprintf(stderr, "Short SPI transfer: %d\n", result);
        return 1;
    }

    printf("TX:");
    for (i = 0; i < test_size; ++i) printf(" %02X", (unsigned)tx[i]);
    printf("\nRX:");
    for (i = 0; i < test_size; ++i) printf(" %02X", (unsigned)rx[i]);
    putchar('\n');
    for (i = 0; i < test_size; ++i) {
        if (rx[i] != EXPECTED_REPLY) {
            fprintf(stderr, "FAIL: RX[%zu]=%02X, expected %02X\n",
                    i, (unsigned)rx[i], EXPECTED_REPLY);
            return 1;
        }
    }
    puts("PASS: STM32 returned 0xA5 for every byte");
    return 0;
}
