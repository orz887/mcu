#include "board.h"
/* 显示驱动的电脑端验证：记录发送命令与像素，检查初始化、裁剪和错误恢复。 */
#include "display.h"

#include <assert.h>
#include <stdio.h>
#include <string.h>

typedef struct {
    bool high;
    uint32_t time;
} ResetEvent;

static ResetEvent resets[16];
static unsigned reset_count;
static uint32_t mock_time;
static unsigned writes;
static unsigned data_writes;
static unsigned data_bytes;
static unsigned poll_bytes;
static unsigned poll_commands;
static unsigned failed_writes;
static uint8_t screen[1024];
static uint8_t current_page;
static uint8_t current_column;
static unsigned screen_write_counts[1024];
static bool fail_writes;
static bool fail_data_writes;
static uint8_t init_bytes[64];
static size_t init_length;
static unsigned enable_count;
static uint32_t enabled_at;

void board_display_reset(bool high)
{
    assert(reset_count < sizeof(resets) / sizeof(resets[0]));
    resets[reset_count++] = (ResetEvent){high, mock_time};
}

bool board_display_write(bool data, const uint8_t *bytes, size_t length)
{
    writes++;
    assert(bytes != NULL);
    /* 慢速GPIO发送不能占住主循环：每轮最多8个像素字节加3字节地址。 */
    assert(length > 0 && length <= 8U);
    poll_bytes += length;
    if (!data) {
        ++poll_commands;
    }
    if (fail_writes || (data && fail_data_writes)) {
        ++failed_writes;
        return false;
    }
    if (data) {
        assert(current_page < 8);
        /* 模拟屏幕的页和列指针，允许同一页分成多次SPI事务写入。 */
        for (size_t index = 0U; index < length; ++index) {
            size_t position = (size_t)current_page * DISPLAY_WIDTH + current_column;
            screen[position] = bytes[index];
            ++screen_write_counts[position];
            current_column = (current_column + 1U) % DISPLAY_WIDTH;
        }
        data_writes++;
        data_bytes += length;
    } else if (length == 3 && bytes[0] >= 0xB0 && bytes[0] <= 0xB7) {
        assert((bytes[1] & 0xF0U) == 0x00U);
        assert((bytes[2] & 0xF0U) == 0x10U);
        current_page = bytes[0] - 0xB0;
        current_column = ((bytes[2] & 0x0FU) << 4U) | bytes[1];
        assert(current_column < DISPLAY_WIDTH);
    } else if (length == 1 && bytes[0] == 0xAF) {
        enable_count++;
        enabled_at = mock_time;
    } else {
        assert(init_length + length <= sizeof(init_bytes));
        memcpy(init_bytes + init_length, bytes, length);
        init_length += length;
    }
    return true;
}

static void poll_at(uint32_t now)
{
    unsigned previous_data_writes = data_writes;
    unsigned previous_writes = writes;
    poll_bytes = 0U;
    poll_commands = 0U;
    mock_time = now;
    display_poll(now);
    assert(data_writes - previous_data_writes <= 1);
    assert(writes - previous_writes <= 2);
    assert(poll_commands <= 1U && poll_bytes <= 11U);
}

static void reset_mock(uint32_t now)
{
    memset(resets, 0, sizeof(resets));
    memset(screen, 0xFF, sizeof(screen));
    memset(init_bytes, 0, sizeof(init_bytes));
    memset(screen_write_counts, 0, sizeof(screen_write_counts));
    reset_count = 0;
    writes = 0;
    data_writes = 0;
    data_bytes = 0;
    failed_writes = 0;
    enable_count = 0;
    init_length = 0;
    current_page = 0;
    current_column = 0;
    fail_writes = false;
    fail_data_writes = false;
    mock_time = now;
    display_init(now);
}

static void boot_display(uint32_t start)
{
    reset_mock(start);
    assert(!display_ready());
    assert(reset_count == 1 && resets[0].high);
    for (uint32_t elapsed = 0; elapsed <= 900; elapsed++) {
        poll_at(start + elapsed);
    }
    assert(display_ready());
    assert(reset_count == 3);
    assert(resets[1].time - start == 100 && !resets[1].high);
    assert(resets[2].time - start == 200 && resets[2].high);
    assert(enable_count == 1);
    assert(enabled_at - start >= 500);
    assert(data_bytes == sizeof(screen));
    for (size_t index = 0; index < sizeof(screen); index++) {
        assert(screen[index] == 0);
        assert(screen_write_counts[index] == 1U);
    }
}

static void test_initialization(void)
{
    static const uint8_t expected[] = {
        0xFD, 0x12, 0xAE, 0xD5, 0x80, 0xA8, 0x3F, 0xD3, 0x00,
        0x40, 0xA0, 0xC0, 0xDA, 0x12, 0xD9, 0x22, 0xDB, 0x34,
        0x81, 0x7F, 0xA4, 0xA6, 0x2E, 0x20, 0x02
    };
    boot_display(0);
    assert(init_length == sizeof(expected));
    assert(memcmp(init_bytes, expected, sizeof(expected)) == 0);
    boot_display(UINT32_MAX - 200U);
}

static bool screen_pixel(int16_t x, int16_t y)
{
    assert(x >= 0 && x < DISPLAY_WIDTH && y >= 0 && y < DISPLAY_HEIGHT);
    return (screen[(y / 8) * DISPLAY_WIDTH + x] & (1U << (y % 8))) != 0;
}

static void finish_frame(void)
{
    for (unsigned attempt = 0U; attempt < 256U && !display_ready(); ++attempt) {
        poll_at(mock_time + 1U);
    }
    assert(display_ready());
}

static void flush_frame(void)
{
    assert(display_present());
    assert(!display_present());
    assert(!display_ready());
    finish_frame();
}

static void test_pixels_and_frame_lock(void)
{
    boot_display(0);
    display_clear();
    display_pixel(0, 0, true);
    display_pixel(127, 63, true);
    display_pixel(10, 10, true);
    display_pixel(10, 10, false);
    display_pixel(-1, 0, true);
    display_pixel(128, 0, true);
    display_pixel(0, -1, true);
    display_pixel(0, 64, true);
    assert(display_present());
    display_clear();
    display_pixel(0, 0, false);
    display_fill_rect(0, 0, 128, 64, true);
    display_text(20, 20, "LOCKED", 1);
    poll_at(mock_time + 1U);
    assert(!display_ready()); /* 两个相距很远的像素不能在一轮中全部发完。 */
    assert(screen_pixel(0, 0) && !screen_pixel(127, 63));
    display_clear();
    display_pixel(127, 63, false);
    finish_frame();
    for (int16_t row = 0; row < DISPLAY_HEIGHT; row++) {
        for (int16_t column = 0; column < DISPLAY_WIDTH; column++) {
            bool expected = (column == 0 && row == 0) ||
                            (column == 127 && row == 63);
            assert(screen_pixel(column, row) == expected);
        }
    }
    display_clear();
    flush_frame();
    assert(!screen_pixel(0, 0) && !screen_pixel(127, 63));
}

static void test_unchanged_and_partial_frames(void)
{
    boot_display(0);
    display_fill_rect(32, 16, 24, 12, true);
    display_text(70, 40, "KEEP", 1);
    flush_frame();
    uint8_t previous_screen[sizeof(screen)];
    memcpy(previous_screen, screen, sizeof(screen));
    unsigned previous_data_bytes = data_bytes;
    flush_frame();
    assert(data_bytes == previous_data_bytes);
    assert(memcmp(screen, previous_screen, sizeof(screen)) == 0);

    display_pixel(0, 0, true);
    display_pixel(127, 63, true);
    flush_frame();
    assert(data_bytes - previous_data_bytes == 16U);
    previous_screen[0] |= 1U;
    previous_screen[sizeof(screen) - 1U] |= 0x80U;
    assert(memcmp(screen, previous_screen, sizeof(screen)) == 0);
    previous_data_bytes = data_bytes;
    flush_frame();
    assert(data_bytes == previous_data_bytes);
}

static void test_lines_and_rectangles(void)
{
    boot_display(0);
    display_line(INT16_MIN, INT16_MIN, INT16_MAX, INT16_MAX, true);
    display_line(INT16_MIN, 63, INT16_MAX, 63, true);
    display_line(127, INT16_MIN, 127, INT16_MAX, true);
    display_line(-30, 1, -10, 62, true);
    flush_frame();
    for (int16_t row = 0; row < DISPLAY_HEIGHT; row++) {
        for (int16_t column = 0; column < DISPLAY_WIDTH; column++) {
            bool expected = column == row || row == 63 || column == 127;
            assert(screen_pixel(column, row) == expected);
        }
    }
    display_clear();
    display_rect(-2, -2, 5, 5, true);
    display_fill_rect(125, 61, 10, 10, true);
    display_fill_rect(INT16_MAX, INT16_MAX, INT16_MAX, INT16_MAX, true);
    display_fill_rect(5, 5, -1, 4, true);
    display_rect(6, 6, 0, 4, true);
    flush_frame();
    for (int16_t row = 0; row < DISPLAY_HEIGHT; row++) {
        for (int16_t column = 0; column < DISPLAY_WIDTH; column++) {
            bool expected = (column <= 2 && row <= 2 &&
                             (column == 2 || row == 2)) ||
                            (column >= 125 && row >= 61);
            assert(screen_pixel(column, row) == expected);
        }
    }
}

static void test_text(void)
{
    static const uint8_t letter_a[] = {14, 17, 17, 31, 17, 17, 17};
    uint8_t reference[sizeof(screen)];
    boot_display(0);
    display_text(0, 0, "A", 1);
    flush_frame();
    for (int16_t row = 0; row < DISPLAY_HEIGHT; row++) {
        for (int16_t column = 0; column < DISPLAY_WIDTH; column++) {
            bool expected = row < 7 && column < 5 &&
                            (letter_a[row] & (1U << (4 - column))) != 0;
            assert(screen_pixel(column, row) == expected);
        }
    }
    memcpy(reference, screen, sizeof(reference));
    display_clear();
    display_text(0, 0, "a", 1);
    flush_frame();
    assert(memcmp(reference, screen, sizeof(reference)) == 0);
    display_clear();
    display_text(-2, -2, "A", 2);
    display_text(126, 62, "A", 1);
    display_text(20, 20, "INVALID", 0);
    display_text(20, 20, "INVALID", 5);
    display_text(20, 20, NULL, 1);
    flush_frame();
    for (int16_t row = 0; row < DISPLAY_HEIGHT; row++) {
        for (int16_t column = 0; column < DISPLAY_WIDTH; column++) {
            bool expected = row < 12 && column < 8 &&
                            (letter_a[(row + 2) / 2] &
                             (1U << (4 - (column + 2) / 2))) != 0;
            if (column >= 126 && row >= 62) {
                expected = (letter_a[row - 62] & (1U << (4 - (column - 126)))) != 0;
            }
            assert(screen_pixel(column, row) == expected);
        }
    }
    display_clear();
    display_text(0, 0, "0123456789ABCDEFGHIJK\nLMNOPQRSTUVWXYZ .,:+\n-/!?%=<>#_()[]|", 1);
    flush_frame();
    assert(memcmp(reference, screen, sizeof(reference)) != 0);
    display_clear();
    display_text(0, 0, "A\nA", 1);
    flush_frame();
    for (unsigned column = 0; column < 5; column++) {
        assert(screen[column] == screen[128 + column]);
    }
}

static void test_error_recovery(void)
{
    reset_mock(0);
    fail_writes = true;
    for (uint32_t now = 0; now <= 1299; now++) {
        poll_at(now);
    }
    assert(!display_ready());
    assert(writes == 1 && data_writes == 0);
    assert(reset_count == 3);
    fail_writes = false;
    for (uint32_t now = 1300; now <= 2300; now++) {
        poll_at(now);
    }
    assert(display_ready());
    assert(reset_count == 6 && data_bytes == sizeof(screen));
    display_pixel(20, 20, true);
    assert(display_present());
    fail_data_writes = true;
    unsigned previous_failures = failed_writes;
    for (unsigned attempt = 0U; attempt < 256U && failed_writes == previous_failures;
         ++attempt) {
        poll_at(mock_time + 1U);
    }
    assert(failed_writes == previous_failures + 1U);
    assert(!display_ready());
    unsigned writes_after_failure = writes;
    unsigned bytes_before_recovery = data_bytes;
    uint32_t failure_time = mock_time;
    for (uint32_t elapsed = 1U; elapsed < 1000U; ++elapsed) {
        poll_at(failure_time + elapsed);
    }
    assert(writes == writes_after_failure);
    /* 故障后的实际显存不可信，即使上次已缓存黑屏，也必须整屏重写。 */
    memset(screen, 0xFF, sizeof(screen));
    memset(screen_write_counts, 0, sizeof(screen_write_counts));
    fail_data_writes = false;
    for (uint32_t elapsed = 1000U; elapsed <= 2000U; ++elapsed) {
        poll_at(failure_time + elapsed);
    }
    assert(display_ready());
    assert(data_bytes - bytes_before_recovery == sizeof(screen));
    for (size_t index = 0U; index < sizeof(screen); ++index) {
        assert(screen[index] == 0U && screen_write_counts[index] == 1U);
    }
}

static void test_explicit_reset_clears_all_pixels(void)
{
    boot_display(0);
    display_text(20, 20, "OLD FRAME", 1);
    flush_frame();
    unsigned bytes_before_reset = data_bytes;
    memset(screen, 0xFF, sizeof(screen));
    memset(screen_write_counts, 0, sizeof(screen_write_counts));
    init_length = 0U;
    display_init(mock_time);
    uint32_t reset_started = mock_time;
    for (uint32_t elapsed = 0U; elapsed <= 1000U; ++elapsed) {
        poll_at(reset_started + elapsed);
    }
    assert(display_ready());
    assert(data_bytes - bytes_before_reset == sizeof(screen));
    for (size_t index = 0U; index < sizeof(screen); ++index) {
        assert(screen[index] == 0U && screen_write_counts[index] == 1U);
    }
}

static void test_coordinate_extremes(void)
{
    static const int16_t coordinates[] = {
        INT16_MIN, -129, -1, 0, 63, 64, 127, 128, INT16_MAX
    };
    boot_display(0);
    for (size_t first = 0; first < sizeof(coordinates) / sizeof(coordinates[0]); first++) {
        for (size_t second = 0; second < sizeof(coordinates) / sizeof(coordinates[0]); second++) {
            for (size_t third = 0; third < sizeof(coordinates) / sizeof(coordinates[0]); third++) {
                display_line(coordinates[first], coordinates[second],
                             coordinates[third], coordinates[first], true);
                display_rect(coordinates[first], coordinates[second],
                             coordinates[third], coordinates[first], false);
            }
        }
    }
    flush_frame();
}

int main(void)
{
    test_initialization();
    test_pixels_and_frame_lock();
    test_unchanged_and_partial_frames();
    test_lines_and_rectangles();
    test_text();
    test_error_recovery();
    test_explicit_reset_clears_all_pixels();
    test_coordinate_extremes();
    puts("display tests passed");
    return 0;
}
