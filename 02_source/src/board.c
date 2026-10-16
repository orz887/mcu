/*
 * 板级硬件层：把 STM32F103C8T6 外设封装成 board.h 中的简单接口。
 * 主循环处理游戏、命令和音频算法；本文件的中断只搬运数据、记录进度。
 *
 * 引脚与外设：
 *   PA0 / PA1       ADC1 通道 0 / 1：麦克风 / 电位器。
 *   TIM3            每 125 us 触发一次双通道扫描，每通道采样率 8000 Hz。
 *   DMA1 通道 1     将交替的音频、电位器读数写入循环缓冲区。
 *   TIM4            每 1 ms 更新应用时间；SysTick 单独服务 HAL 超时。
 *   PA9 / PA10      USART1 TX / RX：串口命令和状态报告。
 *   PA5 / PA7       OLED CLK / DIN：游戏默认GPIO模拟SPI，保留硬件SPI用于对照。
 *   PA4 / PB8 / PB9 OLED CS / DC / RST：片选、命令数据选择、复位。
 *   PC13            指示灯，低电平点亮。
 *   PA8             TIM1 通道 1，蜂鸣器 PWM。
 *   PB15..PB12      SW1..SW4，上拉输入；低电平可能来自按键或已知接地故障。
 *   PA13 / PA14     保留 SWD 调试接口。
 *
 * 阅读顺序：外设初始化 -> 主循环接口 -> 文件末尾的中断和回调。
 */
#include "board.h"
#include "app_config.h"
#include "stm32f1xx_hal.h"
#include <string.h>
#ifdef BOARD_OLED_GPIO
#include "oled_gpio_spi.h"
#endif

/* 串口数组容量，单位字节。环形队列空出一个位置，以区别“满”和“空”。 */
#define UART_RX_SIZE 256U
#define UART_TX_SIZE 1024U

/* 一半 DMA 缓冲区容纳 128 组双通道结果，即 256 个 16 位数。 */
#define ADC_HALF_WORDS (BOARD_AUDIO_SAMPLES * 2U)

/* HAL 句柄保存外设实例及驱动状态，必须在初始化后一直有效。 */
static volatile BoardStatus status;
static ADC_HandleTypeDef adc_handle;
static DMA_HandleTypeDef dma_handle;
#ifndef BOARD_OLED_GPIO
static SPI_HandleTypeDef spi_handle;
#endif
static UART_HandleTypeDef uart_handle;
static TIM_HandleTypeDef sample_timer;
static TIM_HandleTypeDef tick_timer;
static TIM_HandleTypeDef buzzer_timer;

/*
 * ADC 扫描顺序决定内存布局：
 *   前半区：[音频0, 电位器0, 音频1, 电位器1, ... 音频127, 电位器127]
 *   后半区：下一组同样布局的 128 对采样。
 * DMA 写完前半区时产生半满中断，写完后半区时产生全满中断，然后从头循环。
 * 主循环只复制当前没有被 DMA 写入的半区，不暂停采样来等待算法处理。
 */
static uint16_t adc_buffer[ADC_HALF_WORDS * 2U];
static volatile uint32_t app_milliseconds;  /* TIM4 累计的应用时间，单位 ms。 */
static volatile uint32_t adc_sequence;      /* DMA 完成块的发布序号。 */
static volatile uint32_t adc_completed_half; /* 最新完成的半区编号：0 或 1。 */
static uint32_t adc_consumed;               /* 主循环最后成功复制的块序号。 */

/*
 * 两个队列都是单生产者、单消费者：
 *   RX：中断写 rx_head，主循环读并推进 rx_tail。
 *   TX：主循环写 tx_head，中断读并推进 tx_tail。
 * head 是下一个写入位置，tail 是下一个读取位置；两者相等表示空队列。
 * volatile 让编译器每次读取共享索引，但它不能替代临界区或保证多步操作原子性。
 */
static volatile uint16_t rx_head;
static volatile uint16_t rx_tail;
static volatile uint16_t tx_head;
static volatile uint16_t tx_tail;
static char rx_buffer[UART_RX_SIZE];
static char tx_buffer[UART_TX_SIZE];
/* 记录各外设是否初始化成功，公开接口据此避免访问未准备好的外设。 */
static bool uart_available;
static bool spi_available;
static bool adc_available;
static bool buzzer_available;

/* ===== 启动时的外设配置：只由 board_init 调用 ===== */

/* 优先使用 8 MHz 外部晶振得到 72 MHz；启动失败则切换到内部时钟的 64 MHz。 */
static bool clock_init(void)
{
    RCC_OscInitTypeDef oscillator = {0};
    RCC_ClkInitTypeDef clocks = {0};
    RCC_PeriphCLKInitTypeDef peripheral = {0};
    oscillator.OscillatorType = RCC_OSCILLATORTYPE_HSI;
    oscillator.HSIState = RCC_HSI_ON; /* 保持内部振荡器开启，作为回退时钟。 */
    oscillator.HSICalibrationValue = RCC_HSICALIBRATION_DEFAULT;
    oscillator.PLL.PLLState = RCC_PLL_ON; /* PLL 用于倍频。 */
    oscillator.PLL.PLLSource = RCC_PLLSOURCE_HSI_DIV2;
    oscillator.PLL.PLLMUL = RCC_PLL_MUL16; /* 内部 8 MHz / 2 × 16 = 64 MHz。 */
#ifndef BOARD_FORCE_HSI
    /* 普通固件先尝试外部晶振；自检固件定义 BOARD_FORCE_HSI 后直接走内部时钟。 */
    oscillator.OscillatorType |= RCC_OSCILLATORTYPE_HSE;
    oscillator.HSEState = RCC_HSE_ON;
    oscillator.PLL.PLLSource = RCC_PLLSOURCE_HSE;
    oscillator.PLL.PLLMUL = RCC_PLL_MUL9; /* 外部 8 MHz × 9 = 72 MHz。 */
    if (HAL_RCC_OscConfig(&oscillator) == HAL_OK) {
        status.hse_ready = true;
    } else {
        /* 关闭未能正常启动的外部时钟，再重新配置 PLL。HAL 内部带启动超时。 */
        oscillator.HSEState = RCC_HSE_OFF;
        oscillator.PLL.PLLSource = RCC_PLLSOURCE_HSI_DIV2;
        oscillator.PLL.PLLMUL = RCC_PLL_MUL16;
        if (HAL_RCC_OscConfig(&oscillator) != HAL_OK) {
            return false;
        }
    }
#else
    if (HAL_RCC_OscConfig(&oscillator) != HAL_OK) {
        return false;
    }
#endif
    /* ClockType 的各位选择本次需要更新的时钟，这是 HAL 规定的位标志格式。 */
    clocks.ClockType = RCC_CLOCKTYPE_SYSCLK | RCC_CLOCKTYPE_HCLK |
                       RCC_CLOCKTYPE_PCLK1 | RCC_CLOCKTYPE_PCLK2;
    clocks.SYSCLKSource = RCC_SYSCLKSOURCE_PLLCLK; /* 选 PLL 输出作为系统时钟。 */
    clocks.AHBCLKDivider = RCC_SYSCLK_DIV1;       /* CPU / AHB 保持 72 或 64 MHz。 */
    clocks.APB1CLKDivider = RCC_HCLK_DIV2;        /* APB1 为 36 或 32 MHz，不超过上限。 */
    clocks.APB2CLKDivider = RCC_HCLK_DIV1;        /* APB2 为 72 或 64 MHz。 */
    /* 较高主频访问 Flash 需要两个等待周期，不能沿用低速启动时的设置。 */
    if (HAL_RCC_ClockConfig(&clocks, FLASH_LATENCY_2) != HAL_OK) {
        return false;
    }
    peripheral.PeriphClockSelection = RCC_PERIPHCLK_ADC;
    peripheral.AdcClockSelection = RCC_ADCPCLK2_DIV6; /* ADC 为 12 或约 10.67 MHz，低于 14 MHz。 */
    status.core_hz = HAL_RCC_GetHCLKFreq();
    return HAL_RCCEx_PeriphCLKConfig(&peripheral) == HAL_OK;
}

/* 建立安全的默认引脚状态；串口、SPI 和 PWM 稍后再切换各自需要的复用功能。 */
static void gpio_init(void)
{
    GPIO_InitTypeDef gpio = {0};
    __HAL_RCC_AFIO_CLK_ENABLE();
    __HAL_RCC_GPIOA_CLK_ENABLE();
    __HAL_RCC_GPIOB_CLK_ENABLE();
    __HAL_RCC_GPIOC_CLK_ENABLE();
    __HAL_AFIO_REMAP_SWJ_NOJTAG(); /* 关闭 JTAG，但保留 PA13/PA14 的 SWD 烧录调试。 */
    /* 先设置输出电平再切换输出模式，避免初始化时短暂误亮或误触发。 */
    HAL_GPIO_WritePin(GPIOC, GPIO_PIN_13, GPIO_PIN_SET);
    gpio.Pin = GPIO_PIN_13;
    gpio.Mode = GPIO_MODE_OUTPUT_PP;
    gpio.Speed = GPIO_SPEED_FREQ_LOW; /* LED 不需要高速翻转，选较低输出速度。 */
    HAL_GPIO_Init(GPIOC, &gpio);
    gpio.Pin = GPIO_PIN_12 | GPIO_PIN_13 | GPIO_PIN_14 | GPIO_PIN_15;
    gpio.Mode = GPIO_MODE_INPUT; /* 即使按键被接地，也只作为输入读取。 */
    gpio.Pull = GPIO_PULLUP;     /* 未按下时为高，按下或接地时为低。 */
    HAL_GPIO_Init(GPIOB, &gpio);
    HAL_GPIO_WritePin(GPIOA, GPIO_PIN_4, GPIO_PIN_SET);
    HAL_GPIO_WritePin(GPIOA, GPIO_PIN_8, GPIO_PIN_RESET);
    gpio.Pin = GPIO_PIN_4 | GPIO_PIN_8; /* OLED 片选默认高，蜂鸣器默认低。 */
    gpio.Mode = GPIO_MODE_OUTPUT_PP;
    gpio.Pull = GPIO_NOPULL; /* 推挽输出不需要内部上拉或下拉。 */
    gpio.Speed = GPIO_SPEED_FREQ_LOW;
    HAL_GPIO_Init(GPIOA, &gpio);
    HAL_GPIO_WritePin(GPIOB, GPIO_PIN_8 | GPIO_PIN_9, GPIO_PIN_SET);
    gpio.Pin = GPIO_PIN_8 | GPIO_PIN_9; /* OLED 的 DC、RST 默认高电平。 */
    HAL_GPIO_Init(GPIOB, &gpio);
    gpio.Pin = GPIO_PIN_0 | GPIO_PIN_1;
    gpio.Mode = GPIO_MODE_ANALOG; /* PA0/PA1 交给 ADC，关闭数字输入功能。 */
    HAL_GPIO_Init(GPIOA, &gpio);
}

/* 按 APP_UART_BAUD 配置 USART1（默认 115200、8N1）；发送中断在有消息入队时再打开。 */
static bool uart_init(void)
{
    GPIO_InitTypeDef gpio = {0};
    __HAL_RCC_USART1_CLK_ENABLE();
    gpio.Pin = GPIO_PIN_9;
    gpio.Mode = GPIO_MODE_AF_PP; /* TX 由串口外设驱动，使用复用推挽输出。 */
    gpio.Speed = GPIO_SPEED_FREQ_HIGH; /* GPIO 边沿速度，不是串口波特率。 */
    HAL_GPIO_Init(GPIOA, &gpio);
    gpio.Pin = GPIO_PIN_10;
    gpio.Mode = GPIO_MODE_INPUT;
    gpio.Pull = GPIO_PULLUP; /* RX 空闲为高，未连接串口线时也维持稳定电平。 */
    HAL_GPIO_Init(GPIOA, &gpio);
    uart_handle.Instance = USART1;
    uart_handle.Init.BaudRate = APP_UART_BAUD; /* 波特率，须与电脑串口工具一致。 */
    uart_handle.Init.WordLength = UART_WORDLENGTH_8B; /* 每个字符 8 个数据位。 */
    uart_handle.Init.StopBits = UART_STOPBITS_1;      /* 1 个停止位。 */
    uart_handle.Init.Parity = UART_PARITY_NONE;      /* 不使用奇偶校验。 */
    uart_handle.Init.Mode = UART_MODE_TX_RX;         /* 同时允许发送和接收。 */
    uart_handle.Init.HwFlowCtl = UART_HWCONTROL_NONE; /* 没有 RTS/CTS 流控接线。 */
    uart_handle.Init.OverSampling = UART_OVERSAMPLING_16; /* 硬件采用 16 倍过采样。 */
    if (HAL_UART_Init(&uart_handle) != HAL_OK) {
        return false;
    }
    HAL_NVIC_SetPriority(USART1_IRQn, 3U, 0U); /* 低于 DMA 的 1 和应用计时的 2。 */
    HAL_NVIC_EnableIRQ(USART1_IRQn);
    __HAL_UART_ENABLE_IT(&uart_handle, UART_IT_RXNE); /* 收到一个字符即可产生中断。 */
    return true;
}

/* 配置只发送的 SPI1；OLED 无需回传数据，片选 PA4 由普通 GPIO 单独控制。 */
static bool spi_init(void)
{
#ifdef BOARD_OLED_GPIO
    /* 当前游戏使用GPIO发送；不定义BOARD_OLED_GPIO时保留原硬件SPI对照路径。 */
    oled_gpio_spi_init();
    return true;
#else
    GPIO_InitTypeDef gpio = {0};
    __HAL_RCC_SPI1_CLK_ENABLE();
    gpio.Pin = GPIO_PIN_5 | GPIO_PIN_7; /* SCK 和 MOSI；不需要连接 MISO。 */
    gpio.Mode = GPIO_MODE_AF_PP;
    gpio.Speed = GPIO_SPEED_FREQ_HIGH;
    HAL_GPIO_Init(GPIOA, &gpio);
    spi_handle.Instance = SPI1;
    spi_handle.Init.Mode = SPI_MODE_MASTER; /* MCU 提供串行时钟。 */
    spi_handle.Init.Direction = SPI_DIRECTION_1LINE; /* 一根数据线，只使用发送方向。 */
    spi_handle.Init.DataSize = SPI_DATASIZE_8BIT;      /* 每次传一个字节。 */
    spi_handle.Init.CLKPolarity = SPI_POLARITY_LOW;   /* 时钟空闲低电平。 */
    spi_handle.Init.CLKPhase = SPI_PHASE_1EDGE;       /* 第一个边沿采样，两项组合为 mode 0。 */
    spi_handle.Init.NSS = SPI_NSS_SOFT;              /* 不启用硬件片选输出。 */
    spi_handle.Init.BaudRatePrescaler = SPI_BAUDRATEPRESCALER_64; /* 72/64 = 1.125 MHz，回退为 1 MHz。 */
    spi_handle.Init.FirstBit = SPI_FIRSTBIT_MSB;     /* 先发送每字节的最高位。 */
    spi_handle.Init.TIMode = SPI_TIMODE_DISABLE;     /* 使用普通 SPI，而非 TI 帧格式。 */
    spi_handle.Init.CRCCalculation = SPI_CRCCALCULATION_DISABLE; /* OLED 不使用 CRC。 */
    spi_handle.Init.CRCPolynomial = 7U;             /* HAL 结构体的默认值，CRC 关闭时不生效。 */
    return HAL_SPI_Init(&spi_handle) == HAL_OK;
#endif
}

/* 配置 TIM4 毫秒计时、TIM3 采样触发和 TIM1 蜂鸣器；这里只启动 TIM4。 */
static bool timers_init(void)
{
    TIM_MasterConfigTypeDef trigger = {0};
    TIM_OC_InitTypeDef pwm = {0};
    /* APB1 已配置为二分频；STM32F1 在该情况下给 TIM3/TIM4 的时钟是 APB1 的两倍。 */
    uint32_t timer_clock = HAL_RCC_GetPCLK1Freq() * 2U;
    __HAL_RCC_TIM1_CLK_ENABLE();
    __HAL_RCC_TIM3_CLK_ENABLE();
    __HAL_RCC_TIM4_CLK_ENABLE();
    /* 调试器暂停 CPU 时也暂停采样触发和应用计时，避免断点期间后台持续推进。 */
    __HAL_DBGMCU_FREEZE_TIM3();
    __HAL_DBGMCU_FREEZE_TIM4();
    tick_timer.Instance = TIM4;
    /* PSC 和 ARR 都按“寄存器值 + 1”计算：1 MHz 计数 1000 次，得到 1 ms。 */
    tick_timer.Init.Prescaler = timer_clock / 1000000U - 1U; /* 分频后每 1 us 加一。 */
    tick_timer.Init.CounterMode = TIM_COUNTERMODE_UP;      /* 从 0 向上计数。 */
    tick_timer.Init.Period = 999U;                         /* 从 0 数到 999 后产生更新事件。 */
    tick_timer.Init.ClockDivision = TIM_CLOCKDIVISION_DIV1; /* 不额外分频数字滤波使用的时钟。 */
    tick_timer.Init.AutoReloadPreload = TIM_AUTORELOAD_PRELOAD_DISABLE; /* 周期设置直接生效。 */
    if (HAL_TIM_Base_Init(&tick_timer) != HAL_OK) {
        return false;
    }
    HAL_NVIC_SetPriority(TIM4_IRQn, 2U, 0U); /* 数字越小优先级越高，DMA 优先于计时。 */
    HAL_NVIC_EnableIRQ(TIM4_IRQn);
    __HAL_TIM_CLEAR_FLAG(&tick_timer, TIM_FLAG_UPDATE); /* 清掉初始化产生的更新标志。 */
    if (HAL_TIM_Base_Start_IT(&tick_timer) != HAL_OK) {
        return false;
    }
    /* 同样先得到 1 MHz，再每 125 个计数触发一次 ADC；这里不用 TIM3 中断。 */
    sample_timer.Instance = TIM3;
    sample_timer.Init.Prescaler = timer_clock / 1000000U - 1U;
    sample_timer.Init.CounterMode = TIM_COUNTERMODE_UP;
    sample_timer.Init.Period = 1000000U / BOARD_SAMPLE_RATE - 1U; /* 8000 Hz 对应 ARR = 124。 */
    sample_timer.Init.ClockDivision = TIM_CLOCKDIVISION_DIV1;
    sample_timer.Init.AutoReloadPreload = TIM_AUTORELOAD_PRELOAD_DISABLE;
    if (HAL_TIM_Base_Init(&sample_timer) != HAL_OK) {
        return false;
    }
    trigger.MasterOutputTrigger = TIM_TRGO_UPDATE; /* 将更新事件送到芯片内部的 ADC 触发输入。 */
    trigger.MasterSlaveMode = TIM_MASTERSLAVEMODE_DISABLE; /* 不级联其他定时器。 */
    if (HAL_TIMEx_MasterConfigSynchronization(&sample_timer, &trigger) != HAL_OK) {
        return false;
    }
    /* TIM1 位于 APB2；本工程 APB2 不分频，因此 TIM1 时钟直接等于 APB2 时钟。 */
    buzzer_timer.Instance = TIM1;
    buzzer_timer.Init.Prescaler = HAL_RCC_GetPCLK2Freq() / 1000000U - 1U; /* 1 MHz 计数。 */
    buzzer_timer.Init.CounterMode = TIM_COUNTERMODE_UP;
    buzzer_timer.Init.Period = 249U; /* 初始周期 250 us，即 4 kHz；此时还不输出声音。 */
    buzzer_timer.Init.ClockDivision = TIM_CLOCKDIVISION_DIV1;
    buzzer_timer.Init.RepetitionCounter = 0U; /* 每个 PWM 周期都允许产生更新事件。 */
    buzzer_timer.Init.AutoReloadPreload = TIM_AUTORELOAD_PRELOAD_DISABLE;
    if (HAL_TIM_PWM_Init(&buzzer_timer) != HAL_OK) {
        return false;
    }
    pwm.OCMode = TIM_OCMODE_PWM1;     /* 计数值小于比较值时输出有效电平。 */
    pwm.Pulse = 125U;                /* 250 个计数中高电平占 125 个，约 50% 占空比。 */
    pwm.OCPolarity = TIM_OCPOLARITY_HIGH; /* 有效电平为高。 */
    pwm.OCNPolarity = TIM_OCNPOLARITY_HIGH; /* 互补输出未使用，保留默认配置。 */
    pwm.OCFastMode = TIM_OCFAST_DISABLE;   /* 不启用快速比较输出模式。 */
    pwm.OCIdleState = TIM_OCIDLESTATE_RESET; /* 通道空闲状态设为低。 */
    pwm.OCNIdleState = TIM_OCNIDLESTATE_RESET; /* 未使用的互补输出空闲态也设为低。 */
    buzzer_available = HAL_TIM_PWM_ConfigChannel(&buzzer_timer, &pwm, TIM_CHANNEL_1) == HAL_OK;
    return buzzer_available;
}

/* 建立 ADC1 双通道扫描和 DMA 循环搬运；全部就绪后才启动 TIM3 触发采样。 */
static bool adc_init(void)
{
    ADC_ChannelConfTypeDef channel = {0};
    __HAL_RCC_ADC1_CLK_ENABLE();
    __HAL_RCC_DMA1_CLK_ENABLE();
    dma_handle.Instance = DMA1_Channel1; /* STM32F103 的 ADC1 使用此 DMA 通道。 */
    dma_handle.Init.Direction = DMA_PERIPH_TO_MEMORY; /* 从 ADC 寄存器搬到数组。 */
    dma_handle.Init.PeriphInc = DMA_PINC_DISABLE;     /* 每次都读同一个 ADC 数据寄存器。 */
    dma_handle.Init.MemInc = DMA_MINC_ENABLE;         /* 每次写完后移动到数组下一个元素。 */
    dma_handle.Init.PeriphDataAlignment = DMA_PDATAALIGN_HALFWORD; /* 每次从外设读 16 位。 */
    dma_handle.Init.MemDataAlignment = DMA_MDATAALIGN_HALFWORD;    /* 每次数组写入 16 位。 */
    dma_handle.Init.Mode = DMA_CIRCULAR;             /* 写到数组末尾后自动从头开始。 */
    dma_handle.Init.Priority = DMA_PRIORITY_HIGH;    /* DMA 总线仲裁优先级，不是中断优先级。 */
    if (HAL_DMA_Init(&dma_handle) != HAL_OK) {
        return false;
    }
    adc_handle.Instance = ADC1;
    adc_handle.Init.ScanConvMode = ADC_SCAN_ENABLE;       /* 一个触发依次转换多个通道。 */
    adc_handle.Init.ContinuousConvMode = DISABLE;         /* 不自行连续采样，保持 TIM3 决定的频率。 */
    adc_handle.Init.DiscontinuousConvMode = DISABLE;      /* 不将两个通道拆到不同触发中。 */
    adc_handle.Init.ExternalTrigConv = ADC_EXTERNALTRIGCONV_T3_TRGO; /* 由 TIM3 更新事件启动。 */
    adc_handle.Init.DataAlign = ADC_DATAALIGN_RIGHT;      /* 12 位结果右对齐，数值为 0..4095。 */
    adc_handle.Init.NbrOfConversion = 2U;                /* 每次触发转换音频和电位器两个通道。 */
    __HAL_LINKDMA(&adc_handle, DMA_Handle, dma_handle);   /* HAL 据此找到 DMA 句柄及回调所属 ADC。 */
    if (HAL_ADC_Init(&adc_handle) != HAL_OK) {
        return false;
    }
    channel.Channel = ADC_CHANNEL_0; /* PA0 麦克风。 */
    channel.Rank = ADC_REGULAR_RANK_1; /* 排在扫描序列第一位，所以写入数组偶数位置。 */
    channel.SamplingTime = ADC_SAMPLETIME_55CYCLES_5; /* 采样保持 55.5 个 ADC 时钟周期。 */
    if (HAL_ADC_ConfigChannel(&adc_handle, &channel) != HAL_OK) {
        return false;
    }
    channel.Channel = ADC_CHANNEL_1; /* PA1 电位器，沿用上述采样时间。 */
    channel.Rank = ADC_REGULAR_RANK_2; /* 排在第二位，写入数组奇数位置。 */
    /* STM32F1 的 ADC 启动采样前要做硬件校准，补偿内部转换偏差。 */
    if (HAL_ADC_ConfigChannel(&adc_handle, &channel) != HAL_OK ||
        HAL_ADCEx_Calibration_Start(&adc_handle) != HAL_OK) {
        return false;
    }
    HAL_NVIC_SetPriority(DMA1_Channel1_IRQn, 1U, 0U); /* 及时发布完成半区，比应用计时优先。 */
    HAL_NVIC_EnableIRQ(DMA1_Channel1_IRQn);
    /*
     * HAL 接口的地址参数固定为 uint32_t *，但 DMA 上面已配置为每次搬运 16 位。
     * 最后一个参数是传输次数（512 个 16 位数），不是字节数。
     * 先装好 DMA 并等待外部触发，再启动 TIM3，避免第一次采样结果无人接收。
     */
    if (HAL_ADC_Start_DMA(&adc_handle, (uint32_t *)adc_buffer, ADC_HALF_WORDS * 2U) != HAL_OK) {
        return false;
    }
    return HAL_TIM_Base_Start(&sample_timer) == HAL_OK;
}

/* ===== 主循环使用的硬件接口 ===== */

/* 启动时调用一次；按“时钟 -> 引脚 -> 外设 -> 采样”的依赖顺序初始化。 */
bool board_init(void)
{
    HAL_Init(); /* 建立 HAL 基础状态和 SysTick，时钟启动超时也依赖这个计时。 */
    if (!clock_init()) {
        status.faults |= 1U; /* 故障位可以同时保留多个错误，具体含义见 BoardStatus。 */
        status.core_hz = HAL_RCC_GetHCLKFreq();
        gpio_init(); /* 即使时钟配置失败，也让 LED 和其余引脚处于安全状态。 */
        return false;
    }
    gpio_init();
    uart_available = uart_init();
    spi_available = spi_init();
    if (!uart_available) {
        status.faults |= 2U;
    }
    if (!spi_available) {
        status.faults |= 4U;
    }
    if (!timers_init()) {
        status.faults |= 8U;
    } else {
        /* ADC 的采样源是 TIM3，定时器配置成功后才允许启动采集。 */
        adc_available = adc_init();
        if (!adc_available) {
            status.faults |= 16U;
        }
    }
    return status.faults == 0U;
}

/* 主循环读取当前毫秒数；定时器或时钟初始化失败时，使用 HAL 的 SysTick 计时。 */
uint32_t board_millis(void)
{
    return (status.faults & (1U | 8U)) != 0U ? HAL_GetTick() : app_milliseconds;
}

/* 提供只读状态供显示和串口使用；不会清零累计计数。 */
const volatile BoardStatus *board_status(void)
{
    return &status;
}

/* 主循环控制状态灯；板上的 PC13 低电平对应亮灯。 */
void board_led(bool on)
{
    HAL_GPIO_WritePin(GPIOC, GPIO_PIN_13, on ? GPIO_PIN_RESET : GPIO_PIN_SET);
}

/* 主循环取得原始按键状态；消抖和“上电持续接地”的屏蔽由上层按键模块处理。 */
uint8_t board_keys_pressed(void)
{
    uint32_t inputs = GPIOB->IDR; /* 一次读取 GPIOB 输入寄存器，四个按键来自同一次读取。 */
    uint8_t pressed = 0U;
    for (uint32_t key = 0U; key < 4U; ++key) {
        /* 硬件寄存器每一位对应一个引脚：PB15..PB12 映射成返回值的位 0..3。 */
        if ((inputs & (1U << (15U - key))) == 0U) {
            pressed |= (uint8_t)(1U << key);
        }
    }
    return pressed;
}

/* 主循环尝试复制最新音频块；复制遇到覆盖风险时放弃这一轮，下一轮再尝试。 */
bool board_audio_read(uint16_t audio[BOARD_AUDIO_SAMPLES], uint16_t *pot_average)
{
    if (!adc_available) {
        return false;
    }
    uint32_t sequence = adc_sequence; /* 先记住本轮准备处理的块序号。 */
    if (sequence == adc_consumed) {
        return false;
    }
    /* 半区由对应回调明确发布，不依赖序号奇偶，以免中断延迟后判断错位。 */
    uint32_t half = adc_completed_half;
    /*
     * DMA 的剩余次数从 512 递减到 0：大于 256 时正在写前半区，否则写后半区。
     * 计数恰为 0 是循环重装载边界，这时保守地放弃复制。
     * 要读的半区若正在写入，也必须放弃，不能把不同时间的采样拼成一块。
     */
    uint32_t count_before = __HAL_DMA_GET_COUNTER(&dma_handle);
    uint32_t active_before = count_before > ADC_HALF_WORDS ? 0U : 1U;
    if (count_before == 0U || active_before == half) {
        return false;
    }
    /* DMB 是内存屏障，约束前后内存访问顺序；它不会停止 DMA，也不是延时函数。 */
    __DMB();
    uint32_t pot_sum = 0U;
    /* 偶数位置取音频，奇数位置累加电位器；32 位总和能容纳 128 个 12 位结果。 */
    for (uint32_t sample = 0U; sample < BOARD_AUDIO_SAMPLES; ++sample) {
        audio[sample] = adc_buffer[half * ADC_HALF_WORDS + sample * 2U];
        pot_sum += adc_buffer[half * ADC_HALF_WORDS + sample * 2U + 1U];
    }
    /* 复制结束后再检查一次：复制期间若切换了半区或发布了新块，输出不能使用。 */
    __DMB();
    uint32_t count_after = __HAL_DMA_GET_COUNTER(&dma_handle);
    uint32_t active_after = count_after > ADC_HALF_WORDS ? 0U : 1U;
    if (count_after == 0U || active_after == half || sequence != adc_sequence) {
        return false;
    }
    /* 算法处理不及时可能跳过旧块；只处理最新完整块，并把跳过数量记入诊断。 */
    uint32_t elapsed_blocks = sequence - adc_consumed;
    if (elapsed_blocks > 1U) {
        status.adc_dropped += elapsed_blocks - 1U;
    }
    adc_consumed = sequence;
    *pot_average = (uint16_t)(pot_sum / BOARD_AUDIO_SAMPLES);
    return true;
}

/* 主循环取走一个字符，空队列立即返回；ISR 只写入新字符，不移动 rx_tail。 */
bool board_uart_read(char *character)
{
    if (rx_tail == rx_head) {
        return false;
    }
    *character = rx_buffer[rx_tail];
    /* 先读取字符，再发布“此位置已空闲”，这样中断不会过早覆盖该位置。 */
    __DMB();
    rx_tail = (uint16_t)((rx_tail + 1U) % UART_RX_SIZE);
    return true;
}

/* 主循环在发现丢字后清空旧队列，避免旧换行被误当成错误之后的新命令边界。 */
void board_uart_discard_rx(void)
{
    /* 临界区内只改索引；保存并恢复原中断状态，不能无条件开启原本已关闭的中断。 */
    uint32_t interrupt_state = __get_PRIMASK();
    __disable_irq();
    rx_tail = rx_head;
    __DMB();
    __set_PRIMASK(interrupt_state);
}

/* 主循环提交一整条文本到发送队列，具体字节由 USART1 中断逐个发送。 */
bool board_uart_write(const char *message)
{
    if (!uart_available) {
        return false;
    }
    size_t length = strlen(message);
    uint16_t write_head = tx_head; /* 使用局部索引，尚未公开的内容不会被发送中断看到。 */
    /* 取余实现数组末尾回到开头；减 1 是为了保留区别队列满和空的那个位置。 */
    uint16_t free_space = (uint16_t)((tx_tail + UART_TX_SIZE - write_head - 1U) % UART_TX_SIZE);
    if (length > free_space) {
        /* 空间不够就拒绝整条消息，不发送残缺的前半句，也不阻塞主循环等待。 */
        ++status.uart_tx_dropped;
        return false;
    }
    /*
     * 复制期间保持中断开启，让 RX 及时读取字符。中断只会推进 tx_tail、释放空间，
     * 所以刚才确认可用的区域不会在复制过程中变小。这依赖“只有主循环发送”的约定。
     */
    for (size_t index = 0U; index < length; ++index) {
        tx_buffer[write_head] = message[index];
        write_head = (uint16_t)((write_head + 1U) % UART_TX_SIZE);
    }
    /* 数据全部写好后才发布新的 tx_head；关中断的时间只覆盖发布和打开 TXE 两步。 */
    __DMB();
    uint32_t interrupt_state = __get_PRIMASK();
    __disable_irq();
    tx_head = write_head;
    __HAL_UART_ENABLE_IT(&uart_handle, UART_IT_TXE);
    __set_PRIMASK(interrupt_state);
    return true;
}

/* 主循环按需要启动或关闭蜂鸣器；时长由上层毫秒计时控制，本函数不等待发声结束。 */
void board_buzzer(uint16_t frequency_hz)
{
    if (!buzzer_available) {
        return;
    }
    GPIO_InitTypeDef gpio = {0};
    gpio.Pin = GPIO_PIN_8;
    gpio.Speed = GPIO_SPEED_FREQ_LOW;
    if (frequency_hz == 0U) {
        /* 停止 PWM 后改回普通低电平输出，确保关闭状态明确。 */
        HAL_TIM_PWM_Stop(&buzzer_timer, TIM_CHANNEL_1);
        HAL_GPIO_WritePin(GPIOA, GPIO_PIN_8, GPIO_PIN_RESET);
        gpio.Mode = GPIO_MODE_OUTPUT_PP;
        HAL_GPIO_Init(GPIOA, &gpio);
        return;
    }
    if (frequency_hz < 100U || frequency_hz > 10000U) { /* 拒绝本工程范围以外的频率。 */
        return;
    }
    gpio.Mode = GPIO_MODE_AF_PP; /* 把 PA8 的控制权交给 TIM1 通道 1。 */
    HAL_GPIO_Init(GPIOA, &gpio);
    uint32_t period = 1000000U / frequency_hz; /* TIM1 每 1 us 计数一次，换算一个周期的计数值。 */
    __HAL_TIM_SET_AUTORELOAD(&buzzer_timer, period - 1U);
    __HAL_TIM_SET_COMPARE(&buzzer_timer, TIM_CHANNEL_1, period / 2U); /* 约 50% 占空比。 */
    __HAL_TIM_SET_COUNTER(&buzzer_timer, 0U);
    HAL_TIM_PWM_Start(&buzzer_timer, TIM_CHANNEL_1);
}

/* 显示状态机调用：只改变复位引脚，复位脉冲与上电等待时间由状态机控制。 */
void board_display_reset(bool high)
{
    HAL_GPIO_WritePin(GPIOB, GPIO_PIN_9, high ? GPIO_PIN_SET : GPIO_PIN_RESET);
}

/* 主循环发送一段 OLED 命令或一页像素，最多 128 字节，避免一次发送整屏长期占用 CPU。 */
bool board_display_write(bool data, const uint8_t *bytes, size_t length)
{
    if (!spi_available || length == 0U || length > 128U) {
        return false;
    }
#ifdef BOARD_OLED_GPIO
    /* 游戏、自检与GPIO诊断都从这里进入模拟SPI，接线及屏幕命令保持一致。 */
    return oled_gpio_spi_write(data, bytes, length);
#else
    HAL_GPIO_WritePin(GPIOA, GPIO_PIN_4, GPIO_PIN_RESET); /* CS 拉低后 OLED 才接收本次数据。 */
    HAL_GPIO_WritePin(GPIOB, GPIO_PIN_8, data ? GPIO_PIN_SET : GPIO_PIN_RESET); /* DC 区分数据与命令。 */
    /*
     * SPI 约 1 MHz，128 字节通常约 1 ms。HAL 发送超时设为 3 ms，等待期间仍允许中断。
     * HAL 成功返回前会等待发送寄存器空、总线不忙，因此最后一位发完后才抬高 CS。
     * 返回成功只说明 MCU 完成发送；单向 SPI 不能确认屏幕是否真正连接或正常显示。
     */
    HAL_StatusTypeDef result = HAL_SPI_Transmit(&spi_handle, (uint8_t *)bytes, (uint16_t)length, 3U);
    HAL_GPIO_WritePin(GPIOA, GPIO_PIN_4, GPIO_PIN_SET);
    if (result != HAL_OK) {
        ++status.spi_errors;
        return false;
    }
    return true;
#endif
}

/*
 * ===== 中断与 HAL 回调 =====
 * 中断越长，其他外设越容易错过服务时机。因此这里只清标志、搬字符、发布完成信息。
 * 不在中断中做音频计算、解析命令、刷新屏幕或等待一段时间，这些都留给主循环。
 */

/* Cortex-M 的 SysTick 中断每毫秒调用一次，为 HAL 启动等待和 SPI 超时提供时间。 */
void SysTick_Handler(void)
{
    HAL_IncTick();
}

/* TIM4 更新中断：确认来源并清除标志后，只给应用时间加 1 ms。 */
void TIM4_IRQHandler(void)
{
    if (__HAL_TIM_GET_FLAG(&tick_timer, TIM_FLAG_UPDATE) != RESET &&
        __HAL_TIM_GET_IT_SOURCE(&tick_timer, TIM_IT_UPDATE) != RESET) {
        __HAL_TIM_CLEAR_IT(&tick_timer, TIM_IT_UPDATE);
        ++app_milliseconds;
    }
}

/* DMA 中断交给 HAL 识别半满、全满或错误，再由 HAL 调用下方对应回调。 */
void DMA1_Channel1_IRQHandler(void)
{
    HAL_DMA_IRQHandler(&dma_handle);
}

/* DMA 写满前半区后调用，此时正常采样已转向后半区；只发布前半区编号。 */
void HAL_ADC_ConvHalfCpltCallback(ADC_HandleTypeDef *handle)
{
    if (handle == &adc_handle) {
        adc_completed_half = 0U;
        __DMB(); /* 先写半区编号，再增加序号，让主循环看到“有新块可读”。 */
        ++adc_sequence;
        ++status.adc_blocks;
    }
}

/* DMA 写满后半区后调用，循环采样转回前半区；不在这里复制或处理整块数据。 */
void HAL_ADC_ConvCpltCallback(ADC_HandleTypeDef *handle)
{
    if (handle == &adc_handle) {
        adc_completed_half = 1U;
        __DMB();
        ++adc_sequence;
        ++status.adc_blocks;
    }
}

/* ADC / DMA 运行出错时由 HAL 调用，保存故障位供主循环显示，不在中断内重新初始化。 */
void HAL_ADC_ErrorCallback(ADC_HandleTypeDef *handle)
{
    if (handle == &adc_handle) {
        status.faults |= 32U;
    }
}

/* 串口中断最多处理一次接收和一次发送；队列使主循环无需等一个字符真正发完。 */
void USART1_IRQHandler(void)
{
    /*
     * SR 是状态寄存器：RXNE 表示有接收数据，TXE 表示发送数据寄存器可接受新字符。
     * ORE/FE/NE/PE 分别是接收溢出、帧格式、噪声和奇偶校验错误。
     * 位与运算用于检查硬件标志，不能把这些寄存器当成普通的单一真假值。
     */
    uint32_t flags = USART1->SR;
    if ((flags & (USART_SR_RXNE | USART_SR_ORE | USART_SR_FE | USART_SR_NE | USART_SR_PE)) != 0U) {
        /* F1 按“先读 SR，再读 DR”的顺序清除接收和错误标志；错误字符也必须读走。 */
        char character = (char)USART1->DR;
        if ((flags & (USART_SR_ORE | USART_SR_FE | USART_SR_NE | USART_SR_PE)) != 0U) {
            ++status.uart_rx_dropped; /* 不把损坏的字符交给命令解析器，上层会重新同步整行。 */
        } else if ((flags & USART_SR_RXNE) != 0U) {
            uint16_t next = (uint16_t)((rx_head + 1U) % UART_RX_SIZE);
            if (next == rx_tail) { /* 再写一个字符将追上读取位置，说明队列已满。 */
                ++status.uart_rx_dropped;
            } else {
                rx_buffer[rx_head] = character;
                __DMB(); /* 先写字符，再公开新 head，避免主循环读到尚未写好的数据。 */
                rx_head = next;
            }
        }
    }
    /* 同时检查“发送中断已开启”和“寄存器可写”，避免无任务时一直触发空中断。 */
    if ((USART1->CR1 & USART_CR1_TXEIE) != 0U && (flags & USART_SR_TXE) != 0U) {
        if (tx_tail == tx_head) {
            __HAL_UART_DISABLE_IT(&uart_handle, UART_IT_TXE); /* 队列发空，等下次入队再开启。 */
        } else {
            USART1->DR = (uint8_t)tx_buffer[tx_tail];
            tx_tail = (uint16_t)((tx_tail + 1U) % UART_TX_SIZE);
        }
    }
}
