#ifndef FLAPPY_BOARD_H
#define FLAPPY_BOARD_H

/*
 * 硬件层的公开接口：上层只使用这些函数，不必直接操作 STM32 寄存器。
 * 初始化在启动时调用一次；读取、发送和控制函数由主循环调用。
 * 中断只在 board.c 内部收发数据、更新时间，游戏逻辑不在中断内运行。
 */
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* 每块包含 128 个音频采样点；每个通道每秒采样 8000 次，一块历时 16 ms。 */
#define BOARD_AUDIO_SAMPLES 128U
#define BOARD_SAMPLE_RATE 8000U

/*
 * 诊断数据。部分字段由中断更新，上层通过只读 volatile 指针观察最新值。
 * 各字段可以独立读取；整个结构体并不是某个瞬间的原子快照。
 */
typedef struct {
    uint32_t core_hz;          /* CPU 主频，单位 Hz。 */
    uint32_t adc_blocks;       /* DMA 半满或全满回调的累计次数，即采样块数。 */
    uint32_t adc_dropped;      /* 主循环未及时处理而跳过的采样块数。 */
    uint32_t uart_rx_dropped;  /* 接收队列满或硬件接收错误的累计次数。 */
    uint32_t uart_tx_dropped;  /* 发送空间不足而拒绝整条消息的累计次数。 */
    uint32_t spi_errors;       /* SPI 发送失败的累计次数。 */
    uint32_t faults;           /* 故障位：1 时钟、2 串口、4 SPI、8 定时器、16 ADC 初始化、32 ADC 运行。 */
    bool hse_ready;            /* true 使用外部晶振；false 使用内部振荡器。 */
} BoardStatus;

/* 启动时配置时钟及外设；任一初始化失败返回 false，并记录 faults。 */
bool board_init(void);

/* 主循环读取毫秒计时；比较时间请使用无符号的“当前值 - 起始值”。 */
uint32_t board_millis(void);

/* 返回只读诊断信息，适合显示状态或通过串口报告。 */
const volatile BoardStatus *board_status(void);

/* 控制 PC13 指示灯；参数 true 表示亮，底层转换为低电平。 */
void board_led(bool on);

/* 读取四个按键的原始电平；位 0..3 对应 SW1..SW4，位为 1 表示引脚为低。 */
uint8_t board_keys_pressed(void);

/*
 * 主循环取得最新完整音频块，同时得到该块电位器读数的平均值，范围均为 0..4095。
 * 成功返回 true；无新块或复制时 DMA 已开始覆盖该半区则返回 false。
 * 只有返回 true 时输出才有效；调用方须提供完整数组和有效的平均值指针。
 */
bool board_audio_read(uint16_t audio[BOARD_AUDIO_SAMPLES], uint16_t *pot_average);

/* 主循环从接收队列取一个字符；没有字符时立即返回 false，不等待输入。 */
bool board_uart_read(char *character);

/* 接收出错时由主循环清空队列；上层还须丢弃后续字符直到遇到新的换行。 */
void board_uart_discard_rx(void);

/* 主循环提交以 '\0' 结尾的文本；入队成功不代表已经发完，空间不足整条拒绝。 */
bool board_uart_write(const char *message);

/* 设置 PA8 蜂鸣器频率，单位 Hz；0 表示关闭，有效发声范围为 100..10000 Hz。 */
void board_buzzer(uint16_t frequency_hz);

/* 由显示状态机控制 PB9；false 拉低 OLED 复位，true 释放复位。 */
void board_display_reset(bool high);

/* 主循环发送 1..128 字节；data 为 true 发送像素数据，false 发送命令。 */
bool board_display_write(bool data, const uint8_t *bytes, size_t length);

#endif
