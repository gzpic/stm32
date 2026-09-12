#define _POSIX_C_SOURCE 200809L
#include <fcntl.h>
#include <linux/gpio.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/ioctl.h>
#include <time.h>
#include <unistd.h>

#define GPIO_CHIP "/dev/gpiochip0"
#define SCK_LINE 106u /* Jetson J12 physical pin 31, PQ.06 */
#define PULSE_COUNT 8u

static int set_line(int fd, uint8_t value)
{
    struct gpiohandle_data data;

    memset(&data, 0, sizeof data);
    data.values[0] = value;
    return ioctl(fd, GPIOHANDLE_SET_LINE_VALUES_IOCTL, &data);
}

int main(void)
{
    const struct timespec settle = {5, 0};
    const struct timespec half_period = {0, 100000000L};
    struct gpiohandle_request request;
    unsigned int i;
    int chip_fd;

    chip_fd = open(GPIO_CHIP, O_RDONLY | O_CLOEXEC);
    if (chip_fd < 0) {
        perror("open gpiochip0");
        return 1;
    }

    memset(&request, 0, sizeof request);
    request.lineoffsets[0] = SCK_LINE;
    request.flags = GPIOHANDLE_REQUEST_OUTPUT;
    request.default_values[0] = 0;
    request.lines = 1;
    snprintf(request.consumer_label, sizeof request.consumer_label,
             "stm32-sck-counter-test");
    if (ioctl(chip_fd, GPIO_GET_LINEHANDLE_IOCTL, &request) < 0) {
        perror("request Pin31 GPIO");
        close(chip_fd);
        return 1;
    }
    close(chip_fd);

    nanosleep(&settle, NULL);
    for (i = 0; i < PULSE_COUNT; ++i) {
        if (set_line(request.fd, 1) < 0) {
            perror("set SCK high");
            close(request.fd);
            return 1;
        }
        nanosleep(&half_period, NULL);
        if (set_line(request.fd, 0) < 0) {
            perror("set SCK low");
            close(request.fd);
            return 1;
        }
        nanosleep(&half_period, NULL);
    }

    close(request.fd);
    printf("Generated %u SCK pulses on physical Pin31\n", PULSE_COUNT);
    return 0;
}
