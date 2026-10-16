/*
 * 独立亮屏诊断入口；仅由 oled_check 构建环境编译。
 * 烧录后暂时替代游戏，可以随时重新烧回游戏固件。
 * 保持相同引脚和屏幕初始化，依次检查命令与像素显示。
 * oled_check使用原硬件SPI；oled_check_gpio仅将传输改为低速GPIO模拟SPI。
 * 本文件不处理声音、按键或游戏，蜂鸣器始终关闭。
 */
#include "board.h"
#include "display.h"
#include <stdio.h>

#ifdef BOARD_OLED_GPIO
#define CHECK_TRANSPORT "GPIO"
#else
#define CHECK_TRANSPORT "HARDWARE"
#endif

typedef enum {
    OLED_CHECK_INITIALIZING,
    OLED_CHECK_FULL_ON,
    OLED_CHECK_BLACK,
    OLED_CHECK_CHECKERBOARD,
    OLED_CHECK_TEXT,
    OLED_CHECK_RETRY
} OledCheckPhase;

static OledCheckPhase check_phase;
static uint32_t phase_started;
static uint32_t last_report;
static uint32_t previous_spi_errors;
static bool frame_pending;

/* 串口中的阶段名说明现在应该看到什么，不表示已经检测到实物屏幕。 */
static const char *check_phase_name(void)
{
    switch (check_phase) {
    case OLED_CHECK_INITIALIZING: return "INIT";
    case OLED_CHECK_FULL_ON: return "FULL_ON";
    case OLED_CHECK_BLACK: return "BLACK";
    case OLED_CHECK_CHECKERBOARD: return "CHECKERBOARD";
    case OLED_CHECK_TEXT: return "TEXT";
    case OLED_CHECK_RETRY: return "RETRY";
    }
    return "UNKNOWN";
}

/* 失败后等一秒再执行完整初始化，避免不断发送失败命令。 */
static void check_retry(uint32_t now)
{
    check_phase = OLED_CHECK_RETRY;
    phase_started = now;
    frame_pending = false;
    previous_spi_errors = board_status()->spi_errors;
}

/* A5 忽略显存内容，直接点亮全部像素，用来先确认命令通路与面板能否工作。 */
static void check_full_on(uint32_t now)
{
    static const uint8_t command = 0xA5U;
    if (!board_display_write(false, &command, 1U)) {
        check_retry(now);
        return;
    }
    check_phase = OLED_CHECK_FULL_ON;
    phase_started = now;
}

/* 每次只提交一张画；逐页传输仍由原来的 display_poll 完成。 */
static void check_ram_frame(OledCheckPhase next, uint32_t now)
{
    if (next == OLED_CHECK_BLACK) {
        /* A4 恢复显存显示；否则 A5 会让之后的所有画面都保持全亮。 */
        static const uint8_t command = 0xA4U;
        if (!board_display_write(false, &command, 1U)) {
            check_retry(now);
            return;
        }
    }

    display_clear();
    if (next == OLED_CHECK_CHECKERBOARD) {
        /* 8×8 像素的方格同时检查横向、纵向及明暗区域。 */
        for (int16_t row = 0; row < DISPLAY_HEIGHT / 8; ++row) {
            for (int16_t column = 0; column < DISPLAY_WIDTH / 8; ++column) {
                if ((row + column) % 2 == 0) {
                    display_fill_rect(column * 8, row * 8, 8, 8, true);
                }
            }
        }
    } else if (next == OLED_CHECK_TEXT) {
        display_rect(0, 0, DISPLAY_WIDTH, DISPLAY_HEIGHT, true);
#ifdef BOARD_OLED_GPIO
        display_text(22, 16, "GPIO SPI CHECK", 1U);
#else
        display_text(34, 16, "OLED CHECK", 1U);
#endif
        display_text(28, 36, "SPI / RAM OK", 1U);
    }

    if (!display_present()) {
        check_retry(now);
        return;
    }
    check_phase = next;
    frame_pending = true;
    /* 此处不开始停留计时；等八页都发完后再计时，方便肉眼观察。 */
}

static void oled_check_init(void)
{
    board_init();
    board_buzzer(0U);
    uint32_t now = board_millis();
    display_init(now);
    check_phase = OLED_CHECK_INITIALIZING;
    phase_started = now;
    last_report = now;
    previous_spi_errors = board_status()->spi_errors;
    frame_pending = false;
    board_uart_write("\r\nOLED CHECK " CHECK_TRANSPORT ": full-on 2s, black 1s, checkerboard 3s, text 3s; sound off\r\n");
}

static void oled_check_poll(uint32_t now)
{
    if (check_phase == OLED_CHECK_RETRY) {
        if (now - phase_started >= 1000U) {
            display_init(now);
            check_phase = OLED_CHECK_INITIALIZING;
        }
    } else {
        display_poll(now);
        if (board_status()->spi_errors != previous_spi_errors) {
            /* 包括逐页传输失败；重新初始化后从全亮阶段开始，不沿用半张画。 */
            check_retry(now);
        } else if (display_ready()) {
            if (frame_pending) {
                frame_pending = false;
                phase_started = now;
            } else {
                switch (check_phase) {
                case OLED_CHECK_INITIALIZING:
                    check_full_on(now);
                    break;
                case OLED_CHECK_FULL_ON:
                    if (now - phase_started >= 2000U) {
                        check_ram_frame(OLED_CHECK_BLACK, now);
                    }
                    break;
                case OLED_CHECK_BLACK:
                    if (now - phase_started >= 1000U) {
                        check_ram_frame(OLED_CHECK_CHECKERBOARD, now);
                    }
                    break;
                case OLED_CHECK_CHECKERBOARD:
                    if (now - phase_started >= 3000U) {
                        check_ram_frame(OLED_CHECK_TEXT, now);
                    }
                    break;
                case OLED_CHECK_TEXT:
                    if (now - phase_started >= 3000U) {
                        check_full_on(now);
                    }
                    break;
                case OLED_CHECK_RETRY:
                    break;
                }
            }
        }
    }

    const volatile BoardStatus *hardware = board_status();
    bool has_error = hardware->faults != 0U || hardware->spi_errors != 0U;
    uint32_t heartbeat = now % 1000U;
    /* 原SPI诊断双短闪、GPIO诊断三短闪；错误仍为均匀快闪。 */
    bool normal_blink = heartbeat < 80U || (heartbeat >= 180U && heartbeat < 260U);
#ifdef BOARD_OLED_GPIO
    normal_blink = normal_blink || (heartbeat >= 360U && heartbeat < 440U);
#endif
    board_led(has_error ? now % 200U < 100U : normal_blink);
    if (now - last_report >= 1000U) {
        char message[112];
        snprintf(message, sizeof(message), "OLED CHECK " CHECK_TRANSPORT " phase=%s SPI_ERR=%lu FAULT=%lu\r\n",
                 check_phase_name(), (unsigned long)hardware->spi_errors,
                 (unsigned long)hardware->faults);
        board_uart_write(message);
        last_report = now;
    }
}

int main(void)
{
    oled_check_init();
    while (true) {
        oled_check_poll(board_millis());
    }
}
