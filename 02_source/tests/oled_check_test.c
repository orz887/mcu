/* 使用真实显示驱动，记录SPI边界，确认诊断固件确实发出可观察的测试画面。 */
#include <assert.h>
#include <stdio.h>
#include <string.h>
#define main oled_check_firmware_main
#include "../src/oled_check.c"
#undef main

static BoardStatus fake_status;
static uint32_t fake_now;
static bool fake_spi_failure;
static unsigned all_on_commands;
static unsigned ram_display_commands;
static unsigned nonzero_pixel_bytes;
static unsigned data_transactions;
static unsigned poll_bytes;
static uint8_t fake_screen[DISPLAY_WIDTH * DISPLAY_HEIGHT / 8U];
static uint8_t fake_page;
static uint8_t fake_column;
static unsigned led_transitions;
static bool led_on;

bool board_init(void) { return true; }
uint32_t board_millis(void) { return fake_now; }
const volatile BoardStatus *board_status(void) { return &fake_status; }
bool board_uart_write(const char *message) { (void)message; return true; }
void board_display_reset(bool high) { (void)high; }
void board_led(bool on)
{
    if (on != led_on) ++led_transitions;
    led_on = on;
}
void board_buzzer(uint16_t frequency)
{
    assert(frequency == 0U); /* 本诊断始终不得打开蜂鸣器。 */
}
bool board_display_write(bool data, const uint8_t *bytes, size_t length)
{
    assert(length > 0U && length <= 8U);
    poll_bytes += length;
    if (fake_spi_failure) {
        ++fake_status.spi_errors;
        return false;
    }
    if (data) {
        ++data_transactions;
        /* 像实物屏幕一样沿当前列写入，不能假定每次收到完整的一页。 */
        for (size_t index = 0U; index < length; ++index) {
            fake_screen[(size_t)fake_page * DISPLAY_WIDTH + fake_column] = bytes[index];
            fake_column = (fake_column + 1U) % DISPLAY_WIDTH;
            if (bytes[index] != 0U) ++nonzero_pixel_bytes;
        }
    } else if (length == 3U && (bytes[0] & 0xF8U) == 0xB0U) {
        assert((bytes[1] & 0xF0U) == 0U && (bytes[2] & 0xF0U) == 0x10U);
        fake_page = bytes[0] & 7U;
        fake_column = ((bytes[2] & 0x0FU) << 4U) | bytes[1];
        assert(fake_column < DISPLAY_WIDTH);
    } else if (length == 1U) {
        if (bytes[0] == 0xA5U) ++all_on_commands;
        if (bytes[0] == 0xA4U) ++ram_display_commands;
    }
    return true;
}

int main(int argc, char **argv)
{
    bool saw_black = false;
    bool saw_checkerboard = false;
    bool saw_text = false;
    fake_spi_failure = argc > 1 && strcmp(argv[1], "spi-failure") == 0;
    memset(fake_screen, 0xFF, sizeof(fake_screen));
    oled_check_init();
    for (fake_now = 0U; fake_now <= 22000U; ++fake_now) {
        poll_bytes = 0U;
        oled_check_poll(fake_now);
        assert(poll_bytes <= 11U);
        if (display_ready() && !frame_pending) {
            /* 检查重建后的整幅画，避免“有发送”却地址错误也算通过。 */
            if (check_phase == OLED_CHECK_BLACK && !saw_black) {
                for (size_t index = 0U; index < sizeof(fake_screen); ++index) {
                    assert(fake_screen[index] == 0U);
                }
                saw_black = true;
            } else if (check_phase == OLED_CHECK_CHECKERBOARD && !saw_checkerboard) {
                for (size_t index = 0U; index < sizeof(fake_screen); ++index) {
                    unsigned page = index / DISPLAY_WIDTH;
                    unsigned column = index % DISPLAY_WIDTH;
                    uint8_t expected = (page + column / 8U) % 2U == 0U ? 0xFFU : 0U;
                    assert(fake_screen[index] == expected);
                }
                saw_checkerboard = true;
            } else if (check_phase == OLED_CHECK_TEXT && !saw_text) {
                for (unsigned column = 0U; column < DISPLAY_WIDTH; ++column) {
                    assert((fake_screen[column] & 1U) != 0U);
                    assert((fake_screen[7U * DISPLAY_WIDTH + column] & 0x80U) != 0U);
                }
                for (unsigned page = 0U; page < 8U; ++page) {
                    assert(fake_screen[page * DISPLAY_WIDTH] == 0xFFU);
                    assert(fake_screen[(page + 1U) * DISPLAY_WIDTH - 1U] == 0xFFU);
                }
                saw_text = true;
            }
        }
    }
    if (fake_spi_failure) {
        assert(fake_status.spi_errors > 0U);
        assert(all_on_commands == 0U && data_transactions == 0U);
        assert(led_transitions > 80U); /* 持续发送失败时，应可从快闪识别。 */
        puts("OLED check: SPI failure produces visible LED indication");
    } else {
        assert(all_on_commands >= 2U); /* 完成一轮后仍会重复，不只在开机闪一次。 */
        assert(ram_display_commands >= 2U); /* 全亮后确实恢复显存模式。 */
        assert(data_transactions > 0U && nonzero_pixel_bytes > 128U);
        assert(saw_black && saw_checkerboard && saw_text);
        assert(led_transitions > 0U);
        puts("OLED check: repeated full-on, RAM images and heartbeat sent");
    }
    return 0;
}
