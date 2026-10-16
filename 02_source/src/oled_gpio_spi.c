/*
 * 游戏和亮屏诊断共用的GPIO SPI：沿用屏幕命令，逐位发送字节。
 * PA4=CS，PA5=CLK，PA7=DIN，PB8=DC；PB9复位仍由原显示驱动控制。
 * 每一步直接写引脚，方便初学者对照 SPI mode 0 的波形理解。
 */
#include "oled_gpio_spi.h"
#include "app_config.h"
#include "stm32f1xx_hal.h"

/*
 * 给电平变化留出较长间隔，不追求精确波特率。
 * volatile 防止循环被优化掉；中断可以正常执行，只会把某个间隔延长。
 * 这里不使用定时器、中断屏蔽或微秒计数器，以保持诊断实现简单。
 */
static void signal_delay(void)
{
    for (volatile uint32_t wait_count = 0U; wait_count < APP_OLED_GPIO_DELAY_LOOPS; ++wait_count) {
        __NOP();
    }
}

void oled_gpio_spi_init(void)
{
    GPIO_InitTypeDef gpio = {0};
    __HAL_RCC_GPIOA_CLK_ENABLE();
    /* 先预置输出寄存器，再启用输出，避免切换模式时产生额外时钟。 */
    HAL_GPIO_WritePin(GPIOA, GPIO_PIN_5 | GPIO_PIN_7, GPIO_PIN_RESET);
    gpio.Pin = GPIO_PIN_5 | GPIO_PIN_7;
    gpio.Mode = GPIO_MODE_OUTPUT_PP;
    gpio.Pull = GPIO_NOPULL;
    gpio.Speed = GPIO_SPEED_FREQ_LOW;
    HAL_GPIO_Init(GPIOA, &gpio);
}

bool oled_gpio_spi_write(bool data, const uint8_t *bytes, size_t length)
{
    /* 必须先检查参数，错误调用不改变任何引脚电平。 */
    if (bytes == NULL || length == 0U || length > 128U) {
        return false;
    }

    HAL_GPIO_WritePin(GPIOA, GPIO_PIN_4, GPIO_PIN_RESET);
    HAL_GPIO_WritePin(GPIOB, GPIO_PIN_8, data ? GPIO_PIN_SET : GPIO_PIN_RESET);
    signal_delay();

    for (size_t byte_index = 0U; byte_index < length; ++byte_index) {
        /* 从 bit7 开始，一直发送到 bit0；每个上升沿让屏幕采样一位。 */
        for (uint8_t bit_mask = 0x80U; bit_mask != 0U; bit_mask >>= 1U) {
            HAL_GPIO_WritePin(GPIOA, GPIO_PIN_7,
                              (bytes[byte_index] & bit_mask) != 0U ? GPIO_PIN_SET : GPIO_PIN_RESET);
            signal_delay();
            HAL_GPIO_WritePin(GPIOA, GPIO_PIN_5, GPIO_PIN_SET);
            signal_delay();
            HAL_GPIO_WritePin(GPIOA, GPIO_PIN_5, GPIO_PIN_RESET);
        }
    }

    /* 最后一位传输结束后再释放片选；CLK保持mode 0所需的空闲低电平。 */
    signal_delay();
    HAL_GPIO_WritePin(GPIOA, GPIO_PIN_4, GPIO_PIN_SET);
    /* GPIO发送没有应答，此处成功只表示所有电平变化已执行。 */
    return true;
}
