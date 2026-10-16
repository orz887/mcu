#ifndef STM32F1XX_HAL_CONF_H
#define STM32F1XX_HAL_CONF_H

/*
 * STM32 HAL 的工程配置。platformio.ini 中 custom_config_header = yes
 * 让框架使用此文件；若缺少该选项，框架目录的默认配置可能优先被包含。
 * 这里只列本工程使用的外设，便于从初始化代码对应到各 HAL 模块。
 */
#define HAL_MODULE_ENABLED
#define HAL_ADC_MODULE_ENABLED     /* 音频、电位器的模数转换。 */
#define HAL_CORTEX_MODULE_ENABLED  /* 中断优先级和系统滴答时钟。 */
#define HAL_DMA_MODULE_ENABLED     /* 将 ADC 结果自动搬入内存。 */
#define HAL_FLASH_MODULE_ENABLED   /* 配合主频配置 Flash 等待周期。 */
#define HAL_GPIO_MODULE_ENABLED    /* 引脚的输入、输出和复用模式。 */
#define HAL_PWR_MODULE_ENABLED     /* HAL 电源控制基础模块。 */
#define HAL_RCC_MODULE_ENABLED     /* 振荡器、总线和外设时钟。 */
#define HAL_SPI_MODULE_ENABLED     /* OLED 串行通信。 */
#define HAL_TIM_MODULE_ENABLED     /* 采样触发、毫秒计时和蜂鸣器 PWM。 */
#define HAL_UART_MODULE_ENABLED    /* USART1 命令收发。 */

/* 频率单位均为 Hz；这些宏用于 HAL 计算时钟，并不会自行启动振荡器。 */
#ifndef HSE_VALUE
#define HSE_VALUE 8000000U        /* 板上外部晶振为 8 MHz，也允许编译参数指定。 */
#endif
#define HSE_STARTUP_TIMEOUT 100U  /* 外部高速晶振的启动超时，单位 ms。 */
#define HSI_VALUE 8000000U        /* 内部高速振荡器标称 8 MHz，用于无晶振回退。 */
#define LSI_VALUE 40000U          /* 内部低速振荡器的标称值；本工程未启用。 */
#define LSE_VALUE 32768U          /* 外部低速晶振的参考值；本工程未启用。 */
#define LSE_STARTUP_TIMEOUT 5000U /* 外部低速晶振启动超时，单位 ms。 */
#define VDD_VALUE 3300U           /* 供电标称值，单位 mV，不是 ADC 实测电压。 */
#define TICK_INT_PRIORITY 15U     /* SysTick 使用最低抢占优先级；数值越小越优先。 */
#define USE_RTOS 0U               /* 本工程是主循环加中断，没有操作系统。 */
#define PREFETCH_ENABLE 1U        /* 打开 Flash 预取，配合较高主频取指。 */
#define USE_SPI_CRC 0U            /* OLED 通信协议不使用 SPI 硬件 CRC。 */

/* 为上方启用的模块引入对应 HAL 接口。 */
#include "stm32f1xx_hal_rcc.h"
#include "stm32f1xx_hal_gpio.h"
#include "stm32f1xx_hal_dma.h"
#include "stm32f1xx_hal_cortex.h"
#include "stm32f1xx_hal_adc.h"
#include "stm32f1xx_hal_flash.h"
#include "stm32f1xx_hal_pwr.h"
#include "stm32f1xx_hal_spi.h"
#include "stm32f1xx_hal_tim.h"
#include "stm32f1xx_hal_uart.h"

/* 不启用 HAL 参数断言；调用 HAL 后仍须检查返回状态，不能把断言当错误处理。 */
#define assert_param(expr) ((void)0U)

#endif
