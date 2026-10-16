/*
 * SSD1309屏幕驱动与简单绘图。
 * 使用方法：等display_ready为true，clear后画图，再present提交。
 * 后续每轮调用poll最多发送8字节像素，整帧发完之前不修改这张画。
 * 页面/位操作是屏幕协议要求；应用层只需要像素坐标，不用理解这些细节。
 */
#include "display.h"
#include "board.h"

#include <math.h>
#include <string.h>

/* 初始化分成小步骤，等待复位和供电的过程中主程序仍能处理串口。 */
typedef enum {
    DISPLAY_OFF,
    DISPLAY_RESET_HIGH,
    DISPLAY_RESET_LOW,
    DISPLAY_RESET_RELEASE,
    DISPLAY_POWER_WAIT,
    DISPLAY_BOOT_CLEAR,
    DISPLAY_ENABLE,
    DISPLAY_ENABLE_WAIT,
    DISPLAY_IDLE,
    DISPLAY_FRAME,
    DISPLAY_RETRY
} DisplayPhase;

static DisplayPhase phase;       /* 现在处于初始化/待机/发送中的哪一步。 */
static uint32_t phase_started;    /* 本步骤开始时间，单位毫秒。 */
static uint8_t framebuffer[DISPLAY_WIDTH * DISPLAY_HEIGHT / 8]; /* 准备发送的画面。 */
static uint8_t sent_framebuffer[sizeof(framebuffer)]; /* 另存已经发出的内容，不追求省内存。 */
static bool sent_frame_valid;    /* 复位后不知道屏幕内容，必须重新完整发送。 */
static size_t next_offset;       /* 下一个待比较/发送的位置，从0到1024。 */
static size_t initialization_offset; /* 初始化命令也分段，避免启动时长时间占用主循环。 */
#define TRANSFER_BYTES 8U        /* 每轮最多8字节，兼容低速GPIO发送。 */

/* 命令和其参数都用DC=0发送。具体数值来自SSD1309手册与微雪面板参数。 */
static const uint8_t initialization[] = {
    0xFD, 0x12, /* 解锁命令接口。 */
    0xAE,       /* 配置期间关闭显示。 */
    0xD5, 0x80, /* 面板内部振荡与显示时钟。 */
    0xA8, 0x3F, /* 64行复用：参数为行数减1。 */
    0xD3, 0x00, /* 垂直显示偏移为0。 */
    0x40,       /* 起始显示行为0。 */
    0xA0,       /* 列映射方向；实屏方向不同再有意识调整。 */
    0xC0,       /* 行扫描方向。 */
    0xDA, 0x12, /* COM引脚配置，匹配128x64面板。 */
    0xD9, 0x22, /* 像素预充电时间。 */
    0xDB, 0x34, /* VCOM电平，采用芯片手册定义的值。 */
    0x81, 0x7F, /* 对比度，范围0..255，首版使用中等亮度。 */
    0xA4,       /* 根据显存内容显示，而不是强制全亮。 */
    0xA6,       /* 正常显示：数据位为1时亮。 */
    0x2E,       /* 关闭硬件滚动。 */
    0x20, 0x02  /* 页寻址模式，与下面逐页发送方法一致。 */
};

static const char glyph_codes[] =
    "0123456789ABCDEFGHIJKLMNOPQRSTUVWXYZ .,:+-/!?%=<>#_()[]|";

/* 每个字符7行、每行5点；#表示亮点，.表示暗点，可以直接看着改字形。 */
static const char glyph_rows[][36] = {
    ".###." "#...#" "#..##" "#.#.#" "##..#" "#...#" ".###.", /* 0 */
    "..#.." ".##.." "..#.." "..#.." "..#.." "..#.." ".###.", /* 1 */
    ".###." "#...#" "....#" "...#." "..#.." ".#..." "#####", /* 2 */
    "####." "....#" "....#" ".###." "....#" "....#" "####.", /* 3 */
    "...#." "..##." ".#.#." "#..#." "#####" "...#." "...#.", /* 4 */
    "#####" "#...." "#...." "####." "....#" "....#" "####.", /* 5 */
    "..##." ".#..." "#...." "####." "#...#" "#...#" ".###.", /* 6 */
    "#####" "....#" "...#." "..#.." ".#..." ".#..." ".#...", /* 7 */
    ".###." "#...#" "#...#" ".###." "#...#" "#...#" ".###.", /* 8 */
    ".###." "#...#" "#...#" ".####" "....#" "...#." ".##..", /* 9 */
    ".###." "#...#" "#...#" "#####" "#...#" "#...#" "#...#", /* A */
    "####." "#...#" "#...#" "####." "#...#" "#...#" "####.", /* B */
    ".###." "#...#" "#...." "#...." "#...." "#...#" ".###.", /* C */
    "###.." "#..#." "#...#" "#...#" "#...#" "#..#." "###..", /* D */
    "#####" "#...." "#...." "####." "#...." "#...." "#####", /* E */
    "#####" "#...." "#...." "####." "#...." "#...." "#....", /* F */
    ".###." "#...#" "#...." "#.###" "#...#" "#...#" ".####", /* G */
    "#...#" "#...#" "#...#" "#####" "#...#" "#...#" "#...#", /* H */
    ".###." "..#.." "..#.." "..#.." "..#.." "..#.." ".###.", /* I */
    "..###" "...#." "...#." "...#." "...#." "#..#." ".##..", /* J */
    "#...#" "#..#." "#.#.." "##..." "#.#.." "#..#." "#...#", /* K */
    "#...." "#...." "#...." "#...." "#...." "#...." "#####", /* L */
    "#...#" "##.##" "#.#.#" "#.#.#" "#...#" "#...#" "#...#", /* M */
    "#...#" "##..#" "##..#" "#.#.#" "#..##" "#..##" "#...#", /* N */
    ".###." "#...#" "#...#" "#...#" "#...#" "#...#" ".###.", /* O */
    "####." "#...#" "#...#" "####." "#...." "#...." "#....", /* P */
    ".###." "#...#" "#...#" "#...#" "#.#.#" "#..#." ".##.#", /* Q */
    "####." "#...#" "#...#" "####." "#.#.." "#..#." "#...#", /* R */
    ".####" "#...." "#...." ".###." "....#" "....#" "####.", /* S */
    "#####" "..#.." "..#.." "..#.." "..#.." "..#.." "..#..", /* T */
    "#...#" "#...#" "#...#" "#...#" "#...#" "#...#" ".###.", /* U */
    "#...#" "#...#" "#...#" "#...#" ".#.#." ".#.#." "..#..", /* V */
    "#...#" "#...#" "#...#" "#.#.#" "#.#.#" "##.##" "#...#", /* W */
    "#...#" "#...#" ".#.#." "..#.." ".#.#." "#...#" "#...#", /* X */
    "#...#" "#...#" ".#.#." "..#.." "..#.." "..#.." "..#..", /* Y */
    "#####" "....#" "...#." "..#.." ".#..." "#...." "#####", /* Z */
    "....." "....." "....." "....." "....." "....." ".....", /* 空格 */
    "....." "....." "....." "....." "....." ".##.." ".##..", /* . */
    "....." "....." "....." "....." "....." "..#.." ".#...", /* , */
    "....." ".##.." ".##.." "....." ".##.." ".##.." ".....", /* : */
    "....." "..#.." "..#.." "#####" "..#.." "..#.." ".....", /* + */
    "....." "....." "....." "#####" "....." "....." ".....", /* - */
    "....#" "....#" "...#." "..#.." ".#..." "#...." "#....", /* / */
    "..#.." "..#.." "..#.." "..#.." "..#.." "....." "..#..", /* ! */
    ".###." "#...#" "....#" "...#." "..#.." "....." "..#..", /* ? */
    "##..#" "##..#" "...#." "..#.." ".#..." "#..##" "#..##", /* % */
    "....." "....." "#####" "....." "#####" "....." ".....", /* = */
    "....#" "...#." "..#.." ".#..." "..#.." "...#." "....#", /* < */
    "#...." ".#..." "..#.." "...#." "..#.." ".#..." "#....", /* > */
    ".#.#." ".#.#." "#####" ".#.#." "#####" ".#.#." ".#.#.", /* # */
    "....." "....." "....." "....." "....." "....." "#####", /* _ */
    "...#." "..#.." ".#..." ".#..." ".#..." "..#.." "...#.", /* ( */
    ".#..." "..#.." "...#." "...#." "...#." "..#.." ".#...", /* ) */
    ".###." ".#..." ".#..." ".#..." ".#..." ".#..." ".###.", /* [ */
    ".###." "...#." "...#." "...#." "...#." "...#." ".###.", /* ] */
    "..#.." "..#.." "..#.." "..#.." "..#.." "..#.." "..#.." /* | */
};

_Static_assert(sizeof(glyph_codes) - 1 == sizeof(glyph_rows) / sizeof(glyph_rows[0]),
               "Font code and bitmap counts must match");

/* 切换步骤时同时记录时间，下一步据此判断是否等待足够久。 */
static void set_phase(DisplayPhase next, uint32_t now)
{
    phase = next;
    phase_started = now;
}

/* 无符号时间差允许毫秒计数自然回绕，不需要“等在这里”。 */
static bool elapsed(uint32_t now, uint32_t duration)
{
    return now - phase_started >= duration;
}

/* 跳过相同的小块；找到变化后只发这一块，立即让出主循环。 */
static bool write_chunk(void)
{
    while (next_offset < sizeof(framebuffer) && sent_frame_valid &&
           memcmp(framebuffer + next_offset, sent_framebuffer + next_offset,
                  TRANSFER_BYTES) == 0) {
        next_offset += TRANSFER_BYTES;
    }
    if (next_offset == sizeof(framebuffer)) {
        return true;
    }
    uint8_t page = (uint8_t)(next_offset / DISPLAY_WIDTH);
    uint8_t column = (uint8_t)(next_offset % DISPLAY_WIDTH);
    uint8_t address[] = {(uint8_t)(0xB0U | page),
                         (uint8_t)(column & 0x0FU), (uint8_t)(0x10U | (column >> 4U))};
    if (!board_display_write(false, address, sizeof(address)) ||
        !board_display_write(true, framebuffer + next_offset, TRANSFER_BYTES)) {
        return false;
    }
    /* 只有实际发送成功，才更新“已经发过”的记录。 */
    memcpy(sent_framebuffer + next_offset, framebuffer + next_offset, TRANSFER_BYTES);
    next_offset += TRANSFER_BYTES;
    return true;
}

/* 启动初始化流程，实际的复位等待由poll分多轮完成。 */
void display_init(uint32_t now)
{
    memset(framebuffer, 0, sizeof(framebuffer));
    next_offset = 0;
    initialization_offset = 0;
    sent_frame_valid = false;
    board_display_reset(true);
    set_phase(DISPLAY_RESET_HIGH, now);
}

/* 每轮最多8字节命令，或3字节地址加8字节像素，不等待整屏发完。 */
void display_poll(uint32_t now)
{
    static const uint8_t enable = 0xAF;
    switch (phase) {
    case DISPLAY_RESET_HIGH:
        /* 先保持高电平100ms，让模块电源稳定。 */
        if (elapsed(now, 100)) {
            board_display_reset(false);
            set_phase(DISPLAY_RESET_LOW, now);
        }
        break;
    case DISPLAY_RESET_LOW:
        /* 拉低100ms执行硬复位，再释放复位脚。 */
        if (elapsed(now, 100)) {
            board_display_reset(true);
            set_phase(DISPLAY_RESET_RELEASE, now);
        }
        break;
    case DISPLAY_RESET_RELEASE:
        /* 复位释放后再等100ms，再分几轮发送相同的配置命令。 */
        if (elapsed(now, 100)) {
            size_t count = sizeof(initialization) - initialization_offset;
            if (count > TRANSFER_BYTES) {
                count = TRANSFER_BYTES;
            }
            if (!board_display_write(false, initialization + initialization_offset, count)) {
                set_phase(DISPLAY_RETRY, now);
            } else {
                initialization_offset += count;
                if (initialization_offset == sizeof(initialization)) {
                    set_phase(DISPLAY_POWER_WAIT, now);
                }
            }
        }
        break;
    case DISPLAY_POWER_WAIT:
        /* 等待面板电源稳定后，先把随机显存清为黑色。 */
        if (elapsed(now, 200)) {
            next_offset = 0;
            set_phase(DISPLAY_BOOT_CLEAR, now);
        }
        break;
    case DISPLAY_BOOT_CLEAR:
    case DISPLAY_FRAME:
        /* 清屏与正常刷新都分小块；复位后第一次清屏不跳过任何块。 */
        if (!write_chunk()) {
            set_phase(DISPLAY_RETRY, now);
        } else if (next_offset == sizeof(framebuffer)) {
            sent_frame_valid = true;
            set_phase(phase == DISPLAY_BOOT_CLEAR ? DISPLAY_ENABLE : DISPLAY_IDLE,
                      now);
        }
        break;
    case DISPLAY_ENABLE:
        set_phase(board_display_write(false, &enable, 1)
                      ? DISPLAY_ENABLE_WAIT : DISPLAY_RETRY, now);
        break;
    case DISPLAY_ENABLE_WAIT:
        if (elapsed(now, 100)) {
            set_phase(DISPLAY_IDLE, now);
        }
        break;
    case DISPLAY_RETRY:
        /* SPI报错后隔1秒重试，不在这里无限循环阻塞主程序。 */
        if (elapsed(now, 1000)) {
            display_init(now);
        }
        break;
    case DISPLAY_OFF:
    case DISPLAY_IDLE:
        break;
    }
}

/* 这只表示驱动允许画下一帧，单向SPI无法证明屏幕实物存在。 */
bool display_ready(void)
{
    return phase == DISPLAY_IDLE;
}

/* 清掉内存中的旧画面，直到present之后才会发送到实屏。 */
void display_clear(void)
{
    if (display_ready()) {
        memset(framebuffer, 0, sizeof(framebuffer));
    }
}

/* 越界坐标直接忽略，管道移出屏幕时不需要调用者自己裁剪。 */
static void put_pixel(int32_t x, int32_t y, bool on)
{
    if (x < 0 || x >= DISPLAY_WIDTH || y < 0 || y >= DISPLAY_HEIGHT) {
        return;
    }
    /* y/8选择页，x选择列，y%8选择该字节里的哪一个垂直像素。 */
    size_t offset = (size_t)(y / 8) * DISPLAY_WIDTH + (size_t)x;
    uint8_t mask = (uint8_t)(1U << (y % 8));
    if (on) {
        framebuffer[offset] |= mask;
    } else {
        framebuffer[offset] &= (uint8_t)~mask;
    }
}

/* 对外的画像素接口：发送期间不允许改变正在发送的帧。 */
void display_pixel(int16_t x, int16_t y, bool on)
{
    if (display_ready()) {
        put_pixel(x, y, on);
    }
}

/*
 * 用直线公式画线：沿变化较大的方向走一像素，算出另一坐标。
 * 只遍历屏幕内的列或行，所以很远的屏外端点也不会带来长循环。
 */
void display_line(int16_t x0, int16_t y0, int16_t x1, int16_t y1, bool on)
{
    if (!display_ready()) {
        return;
    }
    int32_t delta_x = (int32_t)x1 - x0;
    int32_t delta_y = (int32_t)y1 - y0;
    int32_t width = delta_x < 0 ? -delta_x : delta_x;
    int32_t height = delta_y < 0 ? -delta_y : delta_y;
    if (width == 0 && height == 0) {
        put_pixel(x0, y0, on);
    } else if (width >= height) {
        int32_t start = x0 < x1 ? x0 : x1;
        int32_t end = x0 > x1 ? x0 : x1;
        if (start < 0) {
            start = 0;
        }
        if (end >= DISPLAY_WIDTH) {
            end = DISPLAY_WIDTH - 1;
        }
        for (int32_t column = start; column <= end; ++column) {
            float progress = (float)(column - x0) / (float)delta_x;
            int32_t row = (int32_t)lroundf(y0 + progress * delta_y);
            put_pixel(column, row, on);
        }
    } else {
        int32_t start = y0 < y1 ? y0 : y1;
        int32_t end = y0 > y1 ? y0 : y1;
        if (start < 0) {
            start = 0;
        }
        if (end >= DISPLAY_HEIGHT) {
            end = DISPLAY_HEIGHT - 1;
        }
        for (int32_t row = start; row <= end; ++row) {
            float progress = (float)(row - y0) / (float)delta_y;
            int32_t column = (int32_t)lroundf(x0 + progress * delta_x);
            put_pixel(column, row, on);
        }
    }
}

/* 填充矩形前先限制在屏幕范围，再一行一行画，便于理解。 */
static void fill_area(int32_t x, int32_t y, int32_t width, int32_t height, bool on)
{
    if (width <= 0 || height <= 0) {
        return;
    }
    int32_t end_x = x + width;
    int32_t end_y = y + height;
    if (x < 0) {
        x = 0;
    }
    if (y < 0) {
        y = 0;
    }
    if (end_x > DISPLAY_WIDTH) {
        end_x = DISPLAY_WIDTH;
    }
    if (end_y > DISPLAY_HEIGHT) {
        end_y = DISPLAY_HEIGHT;
    }
    for (int32_t row = y; row < end_y; row++) {
        for (int32_t column = x; column < end_x; column++) {
            put_pixel(column, row, on);
        }
    }
}

/* 空心矩形由上、下、左、右四条边组成。 */
void display_rect(int16_t x, int16_t y, int16_t width, int16_t height, bool on)
{
    if (!display_ready() || width <= 0 || height <= 0) {
        return;
    }
    fill_area(x, y, width, 1, on);
    fill_area(x, (int32_t)y + height - 1, width, 1, on);
    fill_area(x, y, 1, height, on);
    fill_area((int32_t)x + width - 1, y, 1, height, on);
}

/* 实心矩形，on=false时也可以用来擦除一块区域。 */
void display_fill_rect(int16_t x, int16_t y, int16_t width, int16_t height, bool on)
{
    if (display_ready()) {
        fill_area(x, y, width, height, on);
    }
}

/* 按字符查找自编字库；暂不支持中文，小写字母按大写显示。 */
static const char *find_glyph(char character)
{
    static const char unknown[] = ".###." "#...#" "....#" "...#." "..#.." "....." "..#..";
    if (character >= 'a' && character <= 'z') {
        character = (char)(character - 'a' + 'A');
    }
    for (size_t index = 0; index < sizeof(glyph_codes) - 1; index++) {
        if (glyph_codes[index] == character) {
            return glyph_rows[index];
        }
    }
    return unknown;
}

/* 字形5x7像素，字符间隔1列；scale=2表示每个像素画成2x2方块。 */
void display_text(int16_t x, int16_t y, const char *text, uint8_t scale)
{
    if (!display_ready() || text == NULL || scale < 1 || scale > 4) {
        return;
    }
    int32_t cursor_x = x;
    int32_t cursor_y = y;
    while (*text != '\0' && cursor_y < DISPLAY_HEIGHT) {
        char character = *text++;
        if (character == '\n') {
            cursor_x = x;
            cursor_y += 8 * scale;
            continue;
        }
        if (cursor_x < DISPLAY_WIDTH && cursor_x + 5 * scale > 0 &&
            cursor_y + 7 * scale > 0) {
            const char *glyph = find_glyph(character);
            for (unsigned row = 0; row < 7; row++) {
                for (unsigned column = 0; column < 5; column++) {
                    if (glyph[row * 5U + column] == '#') {
                        fill_area(cursor_x + (int32_t)column * scale,
                                  cursor_y + (int32_t)row * scale,
                                  scale, scale, true);
                    }
                }
            }
        }
        if (cursor_x < DISPLAY_WIDTH) {
            cursor_x += 6 * scale;
        }
    }
}

/* 提交后冻结本帧，后续poll逐块发送变化；完成后才允许再画。 */
bool display_present(void)
{
    if (!display_ready()) {
        return false;
    }
    next_offset = 0;
    phase = DISPLAY_FRAME;
    return true;
}
