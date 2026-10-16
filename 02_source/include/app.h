#ifndef FLAPPY_APP_H
#define FLAPPY_APP_H

#include <stdint.h>

/* 上电只调用一次，初始化硬件、游戏、显示并开始声音标定。 */
void app_init(void);

/* 下面的任务由main按顺序反复调用；now为当前毫秒数。 */
void app_process_audio(uint32_t now);
void app_process_commands(uint32_t now);
void app_process_keys(uint32_t now);
void app_update_game(uint32_t now);
void app_update_tuner(uint32_t now);
void app_update_display(uint32_t now);
void app_update_status(uint32_t now);

#endif
