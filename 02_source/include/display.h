#ifndef FLAPPY_DISPLAY_H
#define FLAPPY_DISPLAY_H

#include <stdbool.h>
#include <stdint.h>

#define DISPLAY_WIDTH 128
#define DISPLAY_HEIGHT 64

/* 启动复位流程；poll在主循环中推进流程，时间单位为ms。 */
void display_init(uint32_t now);
void display_poll(uint32_t now);
/* true表示可以画下一帧，不表示已经检测到屏幕实物。 */
bool display_ready(void);
/* 下列函数只修改内存画面；坐标原点在左上，超出屏幕的部分自动忽略。 */
void display_clear(void);
void display_pixel(int16_t x, int16_t y, bool on);
void display_line(int16_t x0, int16_t y0, int16_t x1, int16_t y1, bool on);
void display_rect(int16_t x, int16_t y, int16_t width, int16_t height, bool on);
void display_fill_rect(int16_t x, int16_t y, int16_t width, int16_t height, bool on);
void display_text(int16_t x, int16_t y, const char *text, uint8_t scale);
/* 提交一帧后，要等ready重新变true才能修改画面。 */
bool display_present(void);

#endif
