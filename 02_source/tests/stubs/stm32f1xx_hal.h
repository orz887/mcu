#ifndef FLAPPY_TEST_STM32F1XX_HAL_H
#define FLAPPY_TEST_STM32F1XX_HAL_H

/* 仅用于电脑端 GPIO SPI 测试，不参与 STM32 固件构建。 */
#include <stdint.h>

typedef struct {
    uint32_t unused;
} GPIO_TypeDef;

typedef struct {
    uint32_t Pin;
    uint32_t Mode;
    uint32_t Pull;
    uint32_t Speed;
} GPIO_InitTypeDef;

typedef enum {
    GPIO_PIN_RESET = 0,
    GPIO_PIN_SET = 1
} GPIO_PinState;

extern GPIO_TypeDef test_gpio_a;
extern GPIO_TypeDef test_gpio_b;
#define GPIOA (&test_gpio_a)
#define GPIOB (&test_gpio_b)

#define GPIO_PIN_4 (1U << 4)
#define GPIO_PIN_5 (1U << 5)
#define GPIO_PIN_7 (1U << 7)
#define GPIO_PIN_8 (1U << 8)
#define GPIO_MODE_OUTPUT_PP 1U
#define GPIO_NOPULL 0U
#define GPIO_SPEED_FREQ_LOW 2U

void test_gpio_a_clock_enable(void);
void test_nop(void);
void HAL_GPIO_WritePin(GPIO_TypeDef *port, uint16_t pins, GPIO_PinState state);
void HAL_GPIO_Init(GPIO_TypeDef *port, GPIO_InitTypeDef *configuration);

#define __HAL_RCC_GPIOA_CLK_ENABLE() test_gpio_a_clock_enable()
#define __NOP() test_nop()

#endif
