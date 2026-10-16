#ifndef FLAPPY_OLED_GPIO_SPI_H
#define FLAPPY_OLED_GPIO_SPI_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* 只配置 PA5 时钟、PA7 数据；CS/DC 和屏幕复位仍由 board.c 配置。 */
void oled_gpio_spi_init(void);

/* 低速 GPIO 模拟 mode 0 SPI，发送 1..128 字节；非法参数不改变引脚。 */
bool oled_gpio_spi_write(bool data, const uint8_t *bytes, size_t length);

#endif
