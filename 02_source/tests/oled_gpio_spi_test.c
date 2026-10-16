/* 记录 GPIO 的真实调用顺序，按时钟上升沿重建字节，不模拟发送算法。 */
#include "oled_gpio_spi.h"
#include "app_config.h"
#include "stm32f1xx_hal.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>

GPIO_TypeDef test_gpio_a;
GPIO_TypeDef test_gpio_b;

typedef struct {
    GPIO_TypeDef *port;
    uint16_t pins;
    GPIO_PinState state;
    uint32_t time;
} GpioEvent;

static GpioEvent events[4096];
static size_t event_count;
static uint16_t port_a_levels = GPIO_PIN_4;
static uint16_t port_b_levels;
static uint32_t nop_count;
static uint32_t last_data_time;
static uint32_t last_rising_time;
static uint32_t last_falling_time;
static uint32_t selected_time;
static bool capturing;
static bool expected_data;
static uint8_t received[128];
static size_t rising_edges;
static unsigned select_count;
static unsigned deselect_count;
static unsigned clock_enables;
static unsigned initialization_count;

void test_gpio_a_clock_enable(void)
{
    ++clock_enables;
}

void test_nop(void)
{
    ++nop_count;
}

void HAL_GPIO_Init(GPIO_TypeDef *port, GPIO_InitTypeDef *configuration)
{
    assert(port == GPIOA);
    assert(configuration->Pin == (GPIO_PIN_5 | GPIO_PIN_7));
    assert(configuration->Mode == GPIO_MODE_OUTPUT_PP);
    assert(configuration->Pull == GPIO_NOPULL);
    assert(configuration->Speed == GPIO_SPEED_FREQ_LOW);
    assert((port_a_levels & (GPIO_PIN_5 | GPIO_PIN_7)) == 0U);
    ++initialization_count;
}

void HAL_GPIO_WritePin(GPIO_TypeDef *port, uint16_t pins, GPIO_PinState state)
{
    assert(port == GPIOA || port == GPIOB);
    /* 碰到 SWD、复位或其他引脚就失败，不允许诊断改动无关硬件。 */
    uint16_t allowed = port == GPIOA ? GPIO_PIN_4 | GPIO_PIN_5 | GPIO_PIN_7 : GPIO_PIN_8;
    assert(pins != 0U && (pins & ~allowed) == 0U);
    assert(event_count < sizeof(events) / sizeof(events[0]));
    events[event_count++] = (GpioEvent){port, pins, state, nop_count};
    uint16_t *levels = port == GPIOA ? &port_a_levels : &port_b_levels;
    uint16_t old_levels = *levels;
    if (state == GPIO_PIN_SET) {
        *levels |= pins;
    } else {
        *levels &= (uint16_t)~pins;
    }
    if (!capturing) {
        return;
    }

    if (port == GPIOA && (pins & GPIO_PIN_4) != 0U) {
        assert((port_a_levels & GPIO_PIN_5) == 0U); /* CS变化时，mode 0时钟处于低电平。 */
        if (state == GPIO_PIN_RESET) {
            assert((old_levels & GPIO_PIN_4) != 0U);
            selected_time = nop_count;
            ++select_count;
        } else {
            assert((old_levels & GPIO_PIN_4) == 0U);
            assert(rising_edges > 0U && nop_count - last_falling_time >= APP_OLED_GPIO_DELAY_LOOPS);
            ++deselect_count;
        }
    }
    if (port == GPIOA && (pins & GPIO_PIN_7) != 0U) {
        assert((port_a_levels & (GPIO_PIN_4 | GPIO_PIN_5)) == 0U);
        last_data_time = nop_count;
    }
    if (port == GPIOA && (pins & GPIO_PIN_5) != 0U) {
        if (state == GPIO_PIN_SET) {
            assert((old_levels & GPIO_PIN_5) == 0U);
            assert((port_a_levels & GPIO_PIN_4) == 0U);
            assert(((port_b_levels & GPIO_PIN_8) != 0U) == expected_data);
            assert(nop_count - selected_time >= APP_OLED_GPIO_DELAY_LOOPS);
            assert(nop_count - last_data_time >= APP_OLED_GPIO_DELAY_LOOPS);
            assert(rising_edges < sizeof(received) * 8U);
            size_t byte_index = rising_edges / 8U;
            received[byte_index] = (uint8_t)((received[byte_index] << 1U) |
                                           ((port_a_levels & GPIO_PIN_7) != 0U ? 1U : 0U));
            ++rising_edges;
            last_rising_time = nop_count;
        } else if ((old_levels & GPIO_PIN_5) != 0U) {
            assert(nop_count - last_rising_time >= APP_OLED_GPIO_DELAY_LOOPS);
            last_falling_time = nop_count;
        }
    }
}

static void check_transfer(bool data, const uint8_t *bytes, size_t length)
{
    event_count = 0U;
    rising_edges = 0U;
    select_count = 0U;
    deselect_count = 0U;
    memset(received, 0, sizeof(received));
    expected_data = data;
    capturing = true;
    assert(oled_gpio_spi_write(data, bytes, length));
    capturing = false;
    assert(rising_edges == length * 8U);
    assert(select_count == 1U && deselect_count == 1U);
    assert(memcmp(received, bytes, length) == 0); /* 按上升沿接收的内容与MSB先发一致。 */
    assert((port_a_levels & GPIO_PIN_4) != 0U);
    assert((port_a_levels & GPIO_PIN_5) == 0U);
    assert(event_count >= 3U);
    assert(events[0].port == GPIOA && events[0].pins == GPIO_PIN_4 && events[0].state == GPIO_PIN_RESET);
    assert(events[1].port == GPIOB && events[1].pins == GPIO_PIN_8);
    assert(events[2].time - events[1].time >= APP_OLED_GPIO_DELAY_LOOPS);
}

static void check_invalid_arguments(void)
{
    const uint8_t value = 0xAFU;
    size_t previous_events = event_count;
    uint32_t previous_nops = nop_count;
    assert(!oled_gpio_spi_write(false, NULL, 1U));
    assert(!oled_gpio_spi_write(false, NULL, 0U));
    assert(!oled_gpio_spi_write(false, &value, 0U));
    assert(!oled_gpio_spi_write(false, &value, 129U));
    assert(!oled_gpio_spi_write(false, &value, SIZE_MAX));
    assert(event_count == previous_events && nop_count == previous_nops);
}

int main(void)
{
    /* 先让 CLK/DIN 为高，确认初始化确实预置为低，而不是依赖默认值。 */
    port_a_levels |= GPIO_PIN_5 | GPIO_PIN_7;
    oled_gpio_spi_init();
    assert(clock_enables == 1U && initialization_count == 1U);
    assert((port_a_levels & (GPIO_PIN_5 | GPIO_PIN_7)) == 0U);
    assert(event_count == 1U && events[0].pins == (GPIO_PIN_5 | GPIO_PIN_7));
    check_invalid_arguments();
    const uint8_t command = 0xA5U;
    check_transfer(false, &command, 1U);
    const uint8_t pattern[] = {0x80U, 0x01U, 0xA5U, 0x00U, 0xFFU};
    check_transfer(true, pattern, sizeof(pattern));
    uint8_t page[128];
    for (size_t index = 0U; index < sizeof(page); ++index) {
        page[index] = (uint8_t)index;
    }
    check_transfer(true, page, sizeof(page));
    check_transfer(false, pattern, sizeof(pattern));
    check_invalid_arguments();
    puts("GPIO SPI: initialization, mode 0/MSB bytes, CS/DC timing and invalid input passed");
    return 0;
}
