/**
 ****************************************************************************************************
 * @file        main.c
 * @author      正点原子团队(ALIENTEK)
 * @version     V1.0
 * @date        2021-12-30
 * @brief       串口通信 实验
 * @license     Copyright (c) 2020-2032, 广州市星翼电子科技有限公司
 ****************************************************************************************************
 * @attention
 *
 * 实验平台:正点原子 探索者 F407开发板
 * 在线视频:www.yuanzige.com
 * 技术论坛:www.openedv.com
 * 公司网址:www.alientek.com
 * 购买地址:openedv.taobao.com
 *
 ****************************************************************************************************
 */

#include "./SYSTEM/sys/sys.h"
#include "./SYSTEM/usart/usart.h"
#include "./SYSTEM/delay/delay.h"
#include "./BSP/LED/led.h"
#include <string.h>

int main(void)
{
    uint8_t len;
    uint16_t index;
    uint32_t rx_count = 0;

    sys_stm32_clock_init(336, 8, 2, 7);     /* 设置时钟,168Mhz */
    delay_init(168);                        /* 延时初始化 */
    usart_init(84, 115200);                 /* 串口初始化为115200 */
    led_init();                             /* 初始化LED */

    LED0(0);                                /* 红灯常亮：程序已经运行 */
    LED1(1);                                /* 绿灯仅在收到完整消息时变化 */
    printf("\r\nSTM32 register UART RX ready, 115200 8N1\r\n");

    while (1)
    {
        if (g_usart_rx_sta & 0x8000)        /* 收到以 CRLF 结尾的完整消息 */
        {
            len = g_usart_rx_sta & 0x3fff;  /* 得到此次接收到的数据长度 */
            rx_count++;
            LED1_TOGGLE();                  /* 每收到一帧，绿灯翻转一次 */

            printf("RX[%lu] (%u bytes): ", (unsigned long)rx_count, len);
            for (index = 0; index < len; index++)
            {
                printf("%c", g_usart_rx_buf[index]); /* 在 MDK5 Viewer 逐字符显示 */
            }
            printf("\r\n");

            if ((len == 6) && (memcmp(g_usart_rx_buf, "LED ON", 6) == 0))
            {
                LED0(0);                    /* 板载 LED 为低电平点亮 */
            }
            else if ((len == 7) && (memcmp(g_usart_rx_buf, "LED OFF", 7) == 0))
            {
                LED0(1);
            }
            else if ((len == 10) && (memcmp(g_usart_rx_buf, "LED TOGGLE", 10) == 0))
            {
                LED0_TOGGLE();
            }

            g_usart_rx_sta = 0;
        }
        delay_ms(1);
    }
}





















