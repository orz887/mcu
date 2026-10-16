#include "core.h"

#include <limits.h>
#include <math.h>
#include <string.h>

/* 参数改得不合理时直接给出编译错误，防止随机范围变成负数或开口过小。 */
_Static_assert(GAME_PIPE_GAP > GAME_BIRD_RADIUS * 2,
               "APP_PIPE_GAP 必须大于小鸟直径");
_Static_assert(GAME_PIPE_GAP <= GAME_FLOOR_Y - GAME_PLAY_TOP - 10,
               "APP_PIPE_GAP 太大，屏幕放不下上下管道");
_Static_assert(GAME_PIPE_SPACING > GAME_PIPE_WIDTH,
               "APP_PIPE_SPACING 必须大于管道宽度");

/* ADC 是 12 位，只能产生 0～4095；异常输入先限制到这个范围。 */
static uint16_t adc_sample(uint16_t value)
{
    return value > 4095U ? 4095U : value;
}

/* 先求平均值，再按“偏差平方的平均值开平方”计算 RMS。 */
void audio_stats_compute(const uint16_t *samples, size_t count, AudioStats *stats)
{
    /* 没有采样时交回零结果，不保留上一次计算的数据。 */
    memset(stats, 0, sizeof(*stats));
    if (samples == NULL || count == 0) {
        return;
    }
    /* 第一次遍历：累加 ADC 值，并记录接近上下供电轨的削顶样本。 */
    float sample_sum = 0.0f;
    for (size_t index = 0; index < count; ++index) {
        uint16_t sample = adc_sample(samples[index]);
        sample_sum += sample;
        if ((sample <= 8U || sample >= 4087U) && stats->clipped < UINT16_MAX) {
            ++stats->clipped;
        }
    }
    float average = sample_sum / (float)count;
    stats->mean = (uint16_t)(average + 0.5f);

    /* 麦克风带有约 1.65V 的直流偏置，必须减掉平均值再计算声音大小。 */
    float squared_sum = 0.0f;
    float peak = 0.0f;
    for (size_t index = 0; index < count; ++index) {
        float deviation = (float)adc_sample(samples[index]) - average;
        float magnitude = fabsf(deviation);
        squared_sum += deviation * deviation;
        if (magnitude > peak) {
            peak = magnitude;
        }
    }

    /* sqrtf 是标准数学库的单精度平方根；显示和门限接口仍使用整数。 */
    stats->rms = (uint16_t)sqrtf(squared_sum / (float)count);
    stats->peak = (uint16_t)(peak + 0.5f);
}

/* 通电初始化：先清空计时状态，再设置阈值，避免启动瞬间就产生事件。 */
void audio_gate_init(AudioGate *gate, uint16_t threshold, uint32_t now_ms)
{
    memset(gate, 0, sizeof(*gate));
    audio_gate_set_threshold(gate, threshold, now_ms);
}

/* 修改阈值后重新等待安静，但保留上次触发时间，不能绕过冷却期。 */
void audio_gate_set_threshold(AudioGate *gate, uint16_t threshold, uint32_t now_ms)
{
    gate->threshold = threshold == 0 ? 1U : adc_sample(threshold);
    gate->armed = false;
    gate->quiet_tracking = false;
    gate->quiet_since_ms = now_ms;
}

/* 每得到一块音频调用一次。返回 true 只代表这一次出现了新的响声事件。 */
bool audio_gate_update(AudioGate *gate, uint16_t level, uint32_t now_ms, bool muted)
{
    /* 蜂鸣器测试音可能被麦克风收进去，抑制期间既不触发，也不提前武装。 */
    if (muted) {
        gate->armed = false;
        gate->quiet_tracking = false;
        return false;
    }
    /* 恢复阈值比触发阈值低，避免音量在同一门槛附近抖动时反复触发。 */
    uint16_t release_level = (uint16_t)((uint32_t)gate->threshold * 3U / 4U);
    if (level <= release_level) {
        if (!gate->quiet_tracking) {
            gate->quiet_since_ms = now_ms;
            gate->quiet_tracking = true;
        }
        /* 无符号时间相减可跨越毫秒计数回绕；本项目比较的都是短时间间隔。 */
        if ((uint32_t)(now_ms - gate->quiet_since_ms) >= AUDIO_GATE_QUIET_MS) {
            gate->armed = true;
        }
    } else {
        gate->quiet_tracking = false;
    }
    /* 一旦出现响声，就消耗这次武装。持续吹气不会在冷却结束后自动连发。 */
    if (level >= gate->threshold && gate->armed) {
        gate->armed = false;
        if (!gate->has_triggered ||
            (uint32_t)(now_ms - gate->last_trigger_ms) >= AUDIO_GATE_COOLDOWN_MS) {
            gate->has_triggered = true;
            gate->last_trigger_ms = now_ms;
            return true;
        }
    }
    return false;
}

/* 启动时把四键都视为“尚未确认松开”，适配本板按键可能一直短路到地的情况。 */
void keys_init(KeysState *keys, uint8_t high_mask, uint32_t now_ms)
{
    memset(keys, 0, sizeof(*keys));
    for (unsigned index = 0; index < KEY_COUNT; ++index) {
        KeyState *key = &keys->keys[index];
        key->candidate_since_ms = now_ms;
        /* 1 << index 依次得到 1、2、4、8，用来读取对应按键的那一位。 */
        key->candidate_pressed = (high_mask & (1U << index)) == 0;
        key->stable_pressed = true;
    }
}

/* 四键分别计时；电平连续稳定超过消抖时间后，才承认状态真的发生变化。 */
uint8_t keys_update(KeysState *keys, uint8_t high_mask, uint32_t now_ms)
{
    uint8_t events = 0;
    for (unsigned index = 0; index < KEY_COUNT; ++index) {
        KeyState *key = &keys->keys[index];
        bool pressed = (high_mask & (1U << index)) == 0;
        /* 原始电平一变化就重新计时，跳过机械触点的短暂抖动。 */
        if (pressed != key->candidate_pressed) {
            key->candidate_pressed = pressed;
            key->candidate_since_ms = now_ms;
        } else if (pressed != key->stable_pressed &&
                   (uint32_t)(now_ms - key->candidate_since_ms) >= KEY_DEBOUNCE_MS) {
            key->stable_pressed = pressed;
            /* 只有亲眼观察到一次稳定的松开，才解除该按键的启动屏蔽。 */
            if (!pressed) {
                key->enabled = true;
            } else if (key->enabled) {
                /* 仅在“松开变按下”的时刻设置事件位，长按不会重复走到这里。 */
                events |= (uint8_t)(1U << index);
            }
        }
    }
    return events;
}

/* 把仍被屏蔽的键合成 1、2、4、8 四个位，方便显示或串口报告。 */
uint8_t keys_blocked_mask(const KeysState *keys)
{
    uint8_t blocked = 0;
    for (unsigned index = 0; index < KEY_COUNT; ++index) {
        if (!keys->keys[index].enabled) {
            blocked |= (uint8_t)(1U << index);
        }
    }
    return blocked;
}

/* 生成下一根管道的开口高度。游戏只需高度有变化，不需要真正随机或安全随机。 */
static uint8_t game_new_gap(Game *game)
{
    /* 常用的线性同余法：旧值乘一个常数，再加一个常数。无符号溢出会自然回绕。 */
    game->random_state = game->random_state * 1664525U + 1013904223U;

    /* 上方留 4 像素、下方留 6 像素，开口位置随配置的开口大小自动调整。 */
    uint32_t minimum_top = GAME_PLAY_TOP + 4;
    uint32_t maximum_top = GAME_FLOOR_Y - GAME_PIPE_GAP - 6;
    uint32_t possible_positions = maximum_top - minimum_top + 1U;
    return (uint8_t)(minimum_top + game->random_state % possible_positions);
}

/* 重置一局的画面和分数；不清最高分，也不重置管道高度的生成顺序。 */
static void game_reset_scene(Game *game)
{
    game->score = 0;
    game->bird_y = 30.0f;
    game->bird_velocity = 0.0f;
    /* 第一根管道先放在屏幕右边，给玩家留出开始操作的时间。 */
    for (unsigned index = 0; index < GAME_PIPE_COUNT; ++index) {
        game->pipes[index].x = 144.0f + (float)index * GAME_PIPE_SPACING;
        game->pipes[index].gap_top = game_new_gap(game);
        game->pipes[index].passed = false;
    }
}

/* 只有通电初始化会清空最高分，后面的重开和返回菜单都保留它。 */
void game_init(Game *game, uint32_t seed)
{
    memset(game, 0, sizeof(*game));
    game->random_state = seed;
    game_menu(game);
}

/* 回到等待页面，准备一幅新的初始场景。 */
void game_menu(Game *game)
{
    game_reset_scene(game);
    game->state = GAME_READY;
}

/* 串口 play 等入口可以直接调用此函数，开始一局新游戏。 */
void game_start(Game *game)
{
    game_reset_scene(game);
    game->state = GAME_PLAYING;
}

/* 跳跃就是把竖直速度改成向上的负数；等待或结束状态会自动重开。 */
void game_flap(Game *game)
{
    if (game->state == GAME_READY || game->state == GAME_OVER) {
        game_start(game);
    }
    if (game->state == GAME_PLAYING) {
        game->bird_velocity = APP_BIRD_FLAP_SPEED;
    }
}

/* 暂停只改变状态，不改坐标和速度，所以恢复后能接着原来的画面继续。 */
void game_set_paused(Game *game, bool paused)
{
    if (paused && game->state == GAME_PLAYING) {
        game->state = GAME_PAUSED;
    } else if (!paused && game->state == GAME_PAUSED) {
        game->state = GAME_PLAYING;
    }
}

/* 主循环每隔 GAME_STEP_MS 调用一次，用普通浮点数完成一小步游戏物理。 */
uint8_t game_step(Game *game)
{
    /* 等待、暂停和结束状态完全冻结，不受主循环调用频率影响。 */
    if (game->state != GAME_PLAYING) {
        return 0;
    }
    /* 第一步：重力使向下速度增加，然后用本步速度移动小鸟。 */
    game->bird_velocity += APP_BIRD_GRAVITY;
    if (game->bird_velocity > APP_BIRD_MAX_FALL_SPEED) {
        game->bird_velocity = APP_BIRD_MAX_FALL_SPEED;
    }
    game->bird_y += game->bird_velocity;

    /* 第二步：计算小鸟包围盒。碰到顶部边界或地面，立即结束本局。 */
    float bird_top = game->bird_y - GAME_BIRD_RADIUS;
    float bird_bottom = game->bird_y + GAME_BIRD_RADIUS;
    float bird_left = GAME_BIRD_X - GAME_BIRD_RADIUS;
    float bird_right = GAME_BIRD_X + GAME_BIRD_RADIUS;
    bool collision = bird_top <= GAME_PLAY_TOP || bird_bottom >= GAME_FLOOR_Y;

    /* 第三步：管道向左移动，横向重叠时检查小鸟是否完全位于开口内。 */
    for (unsigned index = 0; index < GAME_PIPE_COUNT; ++index) {
        GamePipe *pipe = &game->pipes[index];
        pipe->x -= APP_PIPE_SPEED;
        bool overlaps_horizontally = pipe->x < bird_right && pipe->x + GAME_PIPE_WIDTH > bird_left;
        bool outside_opening = bird_top < pipe->gap_top || bird_bottom > pipe->gap_top + GAME_PIPE_GAP;
        if (overlaps_horizontally && outside_opening) {
            collision = true;
        }
    }

    /* 碰撞优先于计分：同一步撞死时，不再增加分数。 */
    if (collision) {
        game->state = GAME_OVER;
        return GAME_EVENT_OVER;
    }
    /* 第四步：整根管道越过小鸟后只计分一次，再回收已经移出屏幕的管道。 */
    uint8_t events = 0;
    for (unsigned index = 0; index < GAME_PIPE_COUNT; ++index) {
        GamePipe *pipe = &game->pipes[index];
        if (!pipe->passed && pipe->x + GAME_PIPE_WIDTH < bird_left) {
            pipe->passed = true;
            /* 分数到达变量能存储的最大值后保持不变，不能回绕成 0。 */
            if (game->score < UINT16_MAX) {
                ++game->score;
            }
            if (game->score > game->high_score) {
                game->high_score = game->score;
            }
            events |= GAME_EVENT_SCORE;
        }
        if (pipe->x + GAME_PIPE_WIDTH < 0.0f) {
            /* 找到当前最右边的管道，在它后面留出一个固定间距。 */
            float farthest = 0.0f;
            for (unsigned other = 0; other < GAME_PIPE_COUNT; ++other) {
                if (game->pipes[other].x > farthest) {
                    farthest = game->pipes[other].x;
                }
            }
            pipe->x = farthest + GAME_PIPE_SPACING;
            pipe->gap_top = game_new_gap(game);
            pipe->passed = false;
        }
    }
    return events;
}

/* 物理计算保留小数，只有绘图时才转成整数，避免小速度被提前截成 0。 */
int16_t game_bird_y(const Game *game)
{
    return (int16_t)game->bird_y;
}

/* 管道也在最后绘图时转换像素坐标，游戏内部仍保留完整的浮点位置。 */
int16_t game_pipe_x(const GamePipe *pipe)
{
    return (int16_t)pipe->x;
}
