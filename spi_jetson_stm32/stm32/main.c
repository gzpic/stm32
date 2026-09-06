#include "SYSTEM/sys/sys.h"
#include "spi_slave.h"

int main(void)
{
    HAL_Init();
    if (sys_stm32_clock_init(336, 8, 2, 7) != 0) {
        while (1) {}
    }
    spi_slave_init();
    while (1) {
        spi_slave_poll();
        /* Keep this loop nonblocking. See PROTOCOL.md timing contract. */
    }
}
