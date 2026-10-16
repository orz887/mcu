#include "core.h"

#include <assert.h>
#include <limits.h>
#include <math.h>
#include <stdio.h>
#include <string.h>

/* 对固定输入核对实际音量，避免把直流偏置误当成声音。 */
static void test_audio_statistics(void)
{
    AudioStats stats;
    const uint16_t quiet[] = {2048, 2048, 2048, 2048};
    const uint16_t wave[] = {1948, 2148, 1948, 2148};
    const uint16_t clipped[] = {0, 4095, 2048, 2048};
    /* 恒定的 2048 代表安静：平均值不为零，但有效音量应该为零。 */
    audio_stats_compute(quiet, 4, &stats);
    assert(stats.mean == 2048 && stats.rms == 0 && stats.peak == 0);
    assert(stats.clipped == 0);
    /* 围绕 2048 上下摆动 100，RMS 和峰值都应该是 100。 */
    audio_stats_compute(wave, 4, &stats);
    assert(stats.mean == 2048 && stats.rms == 100 && stats.peak == 100);
    /* 上下轨各出现一次，必须报告两个削顶样本。 */
    audio_stats_compute(clipped, 4, &stats);
    assert(stats.clipped == 2 && stats.peak == 2048);
    assert(stats.rms >= 1447 && stats.rms <= 1449);
    /* 没有采样时，结果清零，不能沿用上一块音频。 */
    audio_stats_compute(NULL, 0, &stats);
    assert(stats.mean == 0 && stats.rms == 0 && stats.peak == 0 && stats.clipped == 0);
}

/* 一次响声只触发一次；冷却期、安静重装和蜂鸣器抑制各自生效。 */
static void test_audio_gate(void)
{
    AudioGate gate;
    audio_gate_init(&gate, 100, 0);
    /* 上电时已经很响不能直接启动，先等满 50ms 安静。 */
    assert(!audio_gate_update(&gate, 200, 0, false));
    assert(!audio_gate_update(&gate, 0, 10, false));
    assert(!audio_gate_update(&gate, 0, 59, false));
    assert(!gate.armed);
    assert(!audio_gate_update(&gate, 0, 60, false));
    assert(gate.armed);
    assert(audio_gate_update(&gate, 200, 61, false));
    /* 同一次持续的响声，时间再长也不能不断触发。 */
    assert(!audio_gate_update(&gate, 200, 500, false));
    assert(!audio_gate_update(&gate, 0, 510, false));
    assert(!audio_gate_update(&gate, 0, 560, false));
    assert(audio_gate_update(&gate, 150, 561, false));
    /* 已经安静重装，但距离上次触发还不到 180ms，也不允许触发。 */
    assert(!audio_gate_update(&gate, 0, 570, false));
    assert(!audio_gate_update(&gate, 0, 620, false));
    assert(!audio_gate_update(&gate, 200, 621, false));
    assert(!audio_gate_update(&gate, 200, 800, false));
    /* 蜂鸣器抑制会清除已武装状态，解除后仍需重新安静。 */
    assert(!audio_gate_update(&gate, 0, 810, false));
    assert(!audio_gate_update(&gate, 0, 860, false));
    assert(!audio_gate_update(&gate, 200, 861, true));
    assert(!audio_gate_update(&gate, 200, 1100, false));
    assert(!audio_gate_update(&gate, 0, 1110, false));
    assert(!audio_gate_update(&gate, 0, 1160, false));
    assert(audio_gate_update(&gate, 200, 1161, false));
    /* 修改阈值也要重新等待安静，避免参数修改本身触发游戏。 */
    audio_gate_set_threshold(&gate, 200, 1170);
    assert(!gate.armed && gate.threshold == 200);
}

/* 毫秒计数从 UINT32_MAX 回到 0 时，时间差仍应正确。 */
static void test_audio_gate_rollover(void)
{
    AudioGate gate;
    uint32_t start = UINT32_MAX - 30U;
    audio_gate_init(&gate, 100, start);
    assert(!audio_gate_update(&gate, 0, start, false));
    assert(!audio_gate_update(&gate, 0, start + 50U, false));
    assert(audio_gate_update(&gate, 100, start + 51U, false));
    assert(!audio_gate_update(&gate, 0, start + 60U, false));
    assert(!audio_gate_update(&gate, 0, start + 110U, false));
    assert(!audio_gate_update(&gate, 100, start + 111U, false));
}

/* 模拟实物短路、释放抖动、按下抖动和长按，确认每键的安全门控。 */
static void test_key_release_and_debounce(void)
{
    KeysState keys;
    keys_init(&keys, 0, 0);
    /* 四键一直低电平，必须全部保持屏蔽，不产生任何点击。 */
    for (uint32_t now = 0; now < 1000; now += 10) {
        assert(keys_update(&keys, 0, now) == 0);
    }
    assert(keys_blocked_mask(&keys) == 15);
    /* 第一键松开过程中出现一次抖动，重新累计 20ms。 */
    assert(keys_update(&keys, 1, 1000) == 0);
    assert(keys_update(&keys, 0, 1010) == 0);
    assert(keys_update(&keys, 1, 1015) == 0);
    assert(keys_update(&keys, 1, 1034) == 0);
    assert(keys_blocked_mask(&keys) == 15);
    assert(keys_update(&keys, 1, 1035) == 0);
    assert(keys_blocked_mask(&keys) == 14);
    /* 通过释放检查后，按下仍须消抖；另外三键继续保持屏蔽。 */
    assert(keys_update(&keys, 0, 1040) == 0);
    assert(keys_update(&keys, 1, 1045) == 0);
    assert(keys_update(&keys, 0, 1050) == 0);
    assert(keys_update(&keys, 0, 1069) == 0);
    assert(keys_update(&keys, 0, 1070) == 1);
    /* 长按不连发，再次完整松开、按下才产生下一次事件。 */
    assert(keys_update(&keys, 0, 2000) == 0);
    assert(keys_update(&keys, 1, 2010) == 0);
    assert(keys_update(&keys, 1, 2030) == 0);
    assert(keys_update(&keys, 0, 2040) == 0);
    assert(keys_update(&keys, 0, 2060) == 1);
}

/* 检查毫秒回绕，以及第二、第四键能在同一帧分别上报。 */
static void test_keys_rollover_and_independence(void)
{
    KeysState keys;
    uint32_t start = UINT32_MAX - 10U;
    keys_init(&keys, 15, start);
    assert(keys_update(&keys, 15, start + 20U) == 0);
    assert(keys_blocked_mask(&keys) == 0);
    assert(keys_update(&keys, 5, start + 30U) == 0);
    assert(keys_update(&keys, 5, start + 50U) == 10);
    assert(keys_update(&keys, 5, start + 80U) == 0);
}

/* 检查等待、跳跃、暂停、继续、撞地和重新开始的完整状态切换。 */
static void test_game_states_and_physics(void)
{
    Game game;
    game_init(&game, 123);
    assert(game.state == GAME_READY && game.score == 0 && game.high_score == 0);
    /* 等待状态下，即使主循环调用更新，也不应改变游戏数据。 */
    Game saved = game;
    assert(game_step(&game) == 0);
    assert(memcmp(&saved, &game, sizeof(game)) == 0);
    game_flap(&game);
    assert(game.state == GAME_PLAYING && game.bird_velocity < 0.0f);
    float before = game.bird_y;
    assert(game_step(&game) == 0);
    assert(game.bird_y < before);
    assert(fabsf(game.bird_velocity - (APP_BIRD_FLAP_SPEED + APP_BIRD_GRAVITY)) < 0.0001f);
    assert(fabsf(game.bird_y - (before + game.bird_velocity)) < 0.0001f);
    /* 暂停后，跳跃输入和物理更新都必须保持数据不变。 */
    game_set_paused(&game, true);
    assert(game.state == GAME_PAUSED);
    saved = game;
    game_flap(&game);
    assert(game_step(&game) == 0);
    assert(memcmp(&saved, &game, sizeof(game)) == 0);
    game_set_paused(&game, false);
    assert(game.state == GAME_PLAYING);
    /* 恢复后不再跳跃，小鸟最终必须因碰撞结束游戏。 */
    bool over = false;
    for (unsigned tick = 0; tick < 200; ++tick) {
        if ((game_step(&game) & GAME_EVENT_OVER) != 0) {
            over = true;
            break;
        }
    }
    assert(over && game.state == GAME_OVER);
    /* 游戏结束后的一次跳跃输入，应开启新的一局。 */
    game_flap(&game);
    assert(game.state == GAME_PLAYING && game.score == 0);
    game_menu(&game);
    assert(game.state == GAME_READY);
}

/* 直接布置几个可判断的画面，检查碰撞、一次计分、回收和最高分。 */
static void test_game_collision_score_and_recycling(void)
{
    Game game;
    game_init(&game, 456);
    game_start(&game);
    /* 管道与小鸟重叠，开口在小鸟下面，必须判定碰撞。 */
    game.bird_y = 32.0f;
    game.bird_velocity = 0.0f;
    game.pipes[0].x = GAME_BIRD_X;
    game.pipes[0].gap_top = 40;
    assert((game_step(&game) & GAME_EVENT_OVER) != 0);
    assert(game.state == GAME_OVER);
    game_start(&game);
    /* 整根管道越过小鸟左边缘后计一分，之后不能重复计分。 */
    game.bird_y = 32.0f;
    game.bird_velocity = 0.0f;
    game.pipes[0].x = GAME_BIRD_X - GAME_BIRD_RADIUS - GAME_PIPE_WIDTH;
    game.pipes[0].gap_top = 20;
    assert((game_step(&game) & GAME_EVENT_SCORE) != 0);
    assert(game.score == 1 && game.high_score == 1);
    assert((game_step(&game) & GAME_EVENT_SCORE) == 0);
    assert(game.score == 1);
    /* 管道完全离开左侧屏幕后，循环放回最右侧。 */
    game.pipes[0].x = -GAME_PIPE_WIDTH;
    assert((game_step(&game) & GAME_EVENT_OVER) == 0);
    assert(game_pipe_x(&game.pipes[0]) > GAME_WIDTH && !game.pipes[0].passed);
    assert(game.pipes[0].gap_top >= GAME_PLAY_TOP);
    assert(game.pipes[0].gap_top + GAME_PIPE_GAP <= GAME_FLOOR_Y);
    /* 撞地会结束本局，重开清除本局分数并保留最高分。 */
    game.bird_y = GAME_FLOOR_Y;
    assert((game_step(&game) & GAME_EVENT_OVER) != 0);
    game_start(&game);
    assert(game.score == 0 && game.high_score == 1);
}

/* 小数坐标不能改变碰撞规则：开口边缘可贴着通过，地面和顶边不能碰。 */
static void test_game_float_boundaries(void)
{
    Game game;
    game_init(&game, 789);
    game_start(&game);

    /* 抵消本步重力，使小鸟上边缘恰好等于开口上边缘。 */
    game.pipes[0].x = GAME_BIRD_X;
    game.pipes[0].gap_top = 20;
    game.bird_y = 20.0f + GAME_BIRD_RADIUS;
    game.bird_velocity = -APP_BIRD_GRAVITY;
    assert(game_step(&game) == 0);
    assert(game.state == GAME_PLAYING);

    /* 只超出开口八分之一像素，也必须碰撞，不能先截成整数再判断。 */
    game.bird_y = 20.0f + GAME_BIRD_RADIUS - 0.125f;
    game.bird_velocity = -APP_BIRD_GRAVITY;
    assert((game_step(&game) & GAME_EVENT_OVER) != 0);

    /* 落到地面边界和飞到顶部边界，都在接触的这一帧结束。 */
    game_start(&game);
    game.bird_y = GAME_FLOOR_Y - GAME_BIRD_RADIUS;
    game.bird_velocity = -APP_BIRD_GRAVITY;
    assert((game_step(&game) & GAME_EVENT_OVER) != 0);
    game_start(&game);
    game.bird_y = GAME_PLAY_TOP + GAME_BIRD_RADIUS;
    game.bird_velocity = -APP_BIRD_GRAVITY;
    assert((game_step(&game) & GAME_EVENT_OVER) != 0);

    /* 限制最大下落速度，且显示接口仍返回直观的整数像素。 */
    game_start(&game);
    game.bird_velocity = APP_BIRD_MAX_FALL_SPEED;
    assert(game_step(&game) == 0);
    assert(fabsf(game.bird_velocity - APP_BIRD_MAX_FALL_SPEED) < 0.0001f);
    game.bird_y = 31.75f;
    assert(game_bird_y(&game) == 31);
    game.pipes[0].x = -1.75f;
    assert(game_pipe_x(&game.pipes[0]) == -1);
}

/* 在电脑上运行所有算法测试，不依赖 HAL、串口或真实开发板。 */
int main(void)
{
    test_audio_statistics();
    test_audio_gate();
    test_audio_gate_rollover();
    test_key_release_and_debounce();
    test_keys_rollover_and_independence();
    test_game_states_and_physics();
    test_game_collision_score_and_recycling();
    test_game_float_boundaries();
    puts("core: 8 behavior groups passed");
    return 0;
}
