#include "SYSTEM/sys/sys.h"
#include "SYSTEM/delay/delay.h"
#include "dht11.h"
#include "i2c_slave.h"
#include "sensors.h"

int main(void)
{
    HAL_Init();
    if (sys_stm32_clock_init(336, 8, 2, 7) != 0) {
        while (1) {}
    }
    delay_init(168);
    dht11_init();
    sensors_init();
    i2c_slave_init();
    while (1) {
        i2c_slave_poll();
        /* Keep this loop nonblocking so responses become available promptly. */
    }
}
