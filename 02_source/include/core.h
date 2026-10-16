#ifndef FLAPPY_CORE_H
#define FLAPPY_CORE_H

#include "app_config.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* 这些名称沿用算法接口；要调速度、间隔和难度，只需修改 app_config.h。 */
#define AUDIO_GATE_QUIET_MS APP_SOUND_QUIET_MS
#define AUDIO_GATE_COOLDOWN_MS APP_SOUND_COOLDOWN_MS
#define KEY_DEBOUNCE_MS APP_KEY_DEBOUNCE_MS
#define KEY_COUNT 4U
#define GAME_STEP_MS APP_GAME_STEP_MS

/* 屏幕坐标：左上角是 (0, 0)，向右 x 增大，向下 y 增大，单位都是像素。 */
#define GAME_WIDTH 128
#define GAME_HEIGHT 64
#define GAME_PLAY_TOP 8
#define GAME_FLOOR_Y 60
#define GAME_BIRD_X 24
#define GAME_BIRD_RADIUS 3
#define GAME_PIPE_COUNT 3U
#define GAME_PIPE_WIDTH 12
#define GAME_PIPE_GAP APP_PIPE_GAP
#define GAME_PIPE_SPACING APP_PIPE_SPACING

/* 事件采用位标志，用 & 检查所关心的事件；本步死亡时不会再返回计分事件。 */
#define GAME_EVENT_SCORE 1U
#define GAME_EVENT_OVER 2U

/* 一块 ADC 音频的统计结果。所有幅值都使用 12 位 ADC 的原始计数单位。 */
typedef struct {
    uint16_t mean;       /* 平均电平，静音时通常在 2048 附近。 */
    uint16_t rms;        /* 去掉平均电平后的有效音量，适合声音触发。 */
    uint16_t peak;       /* 去掉平均电平后，绝对值最大的瞬时幅度。 */
    uint16_t clipped;    /* 接近 ADC 上下边界的样本数，用于提示削顶。 */
} AudioStats;

/* 声音触发门：一次响声只产生一个事件，安静一段时间后才重新允许触发。 */
typedef struct {
    uint16_t threshold;         /* 触发所需的最小音量，单位与 AudioStats.rms 相同。 */
    uint32_t quiet_since_ms;    /* 本次连续安静开始的毫秒时刻。 */
    uint32_t last_trigger_ms;   /* 上次实际发出触发事件的毫秒时刻。 */
    bool armed;                /* true 表示已等到安静，可以接受下一次响声。 */
    bool quiet_tracking;       /* true 表示正在累计连续安静时间。 */
    bool has_triggered;        /* 区分“尚未触发过”和“曾在第 0 毫秒触发”。 */
} AudioGate;

/* 单个按键的消抖状态。低电平表示按下，高电平表示松开。 */
typedef struct {
    uint32_t candidate_since_ms; /* 当前候选电平第一次出现的时刻。 */
    bool candidate_pressed;      /* 尚在等待消抖的原始按下状态。 */
    bool stable_pressed;         /* 已经过消抖确认的按下状态。 */
    bool enabled;                /* 必须先稳定松开一次，之后才允许上报按下。 */
} KeyState;

/* 四个按键分别消抖，避免一个坏键影响另外三个键。 */
typedef struct {
    KeyState keys[KEY_COUNT]; /* 下标 0～3 对应板上的 SW1～SW4。 */
} KeysState;

/* 游戏状态；只有 GAME_PLAYING 会移动小鸟和管道。 */
typedef enum {
    GAME_READY,   /* 等待声音、按键或串口开始。 */
    GAME_PLAYING, /* 正在游戏，每隔 GAME_STEP_MS 调用一次 game_step。 */
    GAME_PAUSED,  /* 暂停，保留当前画面和速度。 */
    GAME_OVER     /* 已碰撞，等待重新开始。 */
} GameState;

/* 一组上下管道，共用同一个横坐标，中间留出可穿过的开口。 */
typedef struct {
    float x;         /* 管道左边缘横坐标，允许小数像素，也允许移出屏幕后为负。 */
    uint8_t gap_top;  /* 开口上边缘的 y 坐标，下边缘是 gap_top + GAME_PIPE_GAP。 */
    bool passed;     /* 已经给这根管道计过分，防止每一帧重复加分。 */
} GamePipe;

/* 游戏运行数据。不写 Flash，断电后最高分会清零。 */
typedef struct {
    GameState state;                    /* 当前是等待、运行、暂停还是结束。 */
    float bird_y;                       /* 小鸟中心的 y 坐标，单位为像素。 */
    float bird_velocity;                /* 每一步的竖直位移，负值向上，正值向下。 */
    GamePipe pipes[GAME_PIPE_COUNT];     /* 循环使用三组管道，不需要动态申请内存。 */
    uint16_t score;                     /* 本局通过的管道数量。 */
    uint16_t high_score;                /* 本次通电以来的最高分。 */
    uint32_t random_state;              /* 生成管道高度的简单伪随机状态。 */
} Game;

/* 计算一块音频；空指针或 count=0 时返回全零统计。stats 必须指向有效对象。 */
void audio_stats_compute(const uint16_t *samples, size_t count, AudioStats *stats);
/* 初始化触发门，刚开机时必须先安静一段时间，不会立刻触发。 */
void audio_gate_init(AudioGate *gate, uint16_t threshold, uint32_t now_ms);
/* 修改音量阈值并重新等待安静；阈值会限制在 1～4095。 */
void audio_gate_set_threshold(AudioGate *gate, uint16_t threshold, uint32_t now_ms);
/* 输入一次音量读数，返回是否出现新事件；muted 用于蜂鸣器期间抑制声控。 */
bool audio_gate_update(AudioGate *gate, uint16_t level, uint32_t now_ms, bool muted);
/* high_mask 的低四位对应 SW1～SW4；某位为 1 表示该键当前是高电平。 */
void keys_init(KeysState *keys, uint8_t high_mask, uint32_t now_ms);
/* 返回新按下事件的位掩码；长按不重复上报，启动后从未松开的键不响应。 */
uint8_t keys_update(KeysState *keys, uint8_t high_mask, uint32_t now_ms);
/* 返回尚未通过“先稳定松开”检查的按键，用于自检页面。 */
uint8_t keys_blocked_mask(const KeysState *keys);
/* 通电初始化，清除最高分并进入等待状态；seed 只影响管道高度顺序。 */
void game_init(Game *game, uint32_t seed);
/* 返回等待画面，清除本局分数并保留最高分。 */
void game_menu(Game *game);
/* 直接开始新一局，保留最高分。 */
void game_start(Game *game);
/* 让小鸟向上跳；等待或结束状态会自动开新局，暂停状态保持不动。 */
void game_flap(Game *game);
/* 暂停或继续已有的游戏；等待和结束状态不受影响。 */
void game_set_paused(Game *game, bool paused);
/* 前进一个固定时步，返回 GAME_EVENT_SCORE / GAME_EVENT_OVER 事件。 */
uint8_t game_step(Game *game);
/* 把内部小数坐标转换成显示用的整数像素坐标。 */
int16_t game_bird_y(const Game *game);
/* 取得管道显示用的整数横坐标；向零取整与普通 C 类型转换一致。 */
int16_t game_pipe_x(const GamePipe *pipe);

#endif
