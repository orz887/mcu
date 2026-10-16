/* 电脑端应用验证：用假硬件输入调用真实应用逻辑，不需要连接开发板。 */
#define FIRMWARE_VERSION "host-test"
#include "../src/app.c"
#include <assert.h>
#include <stdlib.h>

static BoardStatus fake_hardware;
static const char *fake_input = "";
static size_t fake_input_position;
static bool fake_audio_available;
static uint16_t fake_audio_level = 2048U;
static uint16_t fake_audio_amplitude;
static unsigned fake_sound_starts;
static uint16_t fake_sound_frequency;
static uint8_t fake_screen[1024];
static uint8_t fake_page;
static uint8_t fake_column;

bool board_init(void) { return true; }
uint32_t board_millis(void) { return 0U; }
const volatile BoardStatus *board_status(void) { return &fake_hardware; }
void board_led(bool on) { (void)on; }
uint8_t board_keys_pressed(void) { return 15U; }
void board_buzzer(uint16_t frequency)
{
    fake_sound_frequency = frequency;
    if (frequency != 0U) {
        ++fake_sound_starts;
    }
}
void board_display_reset(bool high) { (void)high; }
bool board_uart_write(const char *message) { (void)message; return true; }

bool board_uart_read(char *character)
{
    if (fake_input[fake_input_position] == '\0') {
        return false;
    }
    *character = fake_input[fake_input_position++];
    return true;
}

void board_uart_discard_rx(void)
{
    fake_input_position = strlen(fake_input);
}

bool board_audio_read(uint16_t audio[BOARD_AUDIO_SAMPLES], uint16_t *pot_average)
{
    if (!fake_audio_available) {
        return false;
    }
    fake_audio_available = false;
    for (size_t index = 0U; index < BOARD_AUDIO_SAMPLES; ++index) {
        audio[index] = index % 2U == 0U ? fake_audio_level + fake_audio_amplitude :
                                        fake_audio_level - fake_audio_amplitude;
    }
    *pot_average = 2048U;
    return true;
}

bool board_display_write(bool data, const uint8_t *bytes, size_t length)
{
    if (!data && length == 3U && (bytes[0] & 0xF8U) == 0xB0U) {
        fake_page = bytes[0] & 7U;
        assert((bytes[1] & 0xF0U) == 0U && (bytes[2] & 0xF0U) == 0x10U);
        fake_column = ((bytes[2] & 0x0FU) << 4U) | bytes[1];
        assert(fake_column < DISPLAY_WIDTH);
    }
    if (data) {
        /* 屏幕的一页可以分多次写，按地址重建实际显存而非假定整页事务。 */
        for (size_t index = 0U; index < length; ++index) {
            fake_screen[(size_t)fake_page * DISPLAY_WIDTH + fake_column] = bytes[index];
            fake_column = (fake_column + 1U) % DISPLAY_WIDTH;
        }
    }
    return true;
}

static void test_rx_loss(void)
{
    /* 丢一个0不能把threshold 1000误执行成threshold 100。 */
    threshold = 1000U;
    fake_hardware.uart_rx_dropped = 1U;
    fake_input = "status\nthreshold 100\n";
    app_process_commands(1000U);
    assert(threshold == 1000U);
    fake_input = "\nthreshold 250\n";
    fake_input_position = 0U;
    app_process_commands(1100U);
    assert(threshold == 250U);
    puts("app: corrupted UART line discarded; clean command recovers");
}

static void test_pitch_silence(void)
{
    /* 有效音高之后遇到静音，必须立即撤销旧结果，不能继续显示“调准了”。 */
    view = VIEW_TUNER;
    calibrating = false;
    last_audio = 984U;
    last_pitch.valid = true;
    last_pitch.frequency_millihz = 110000U;
    pitch_init(&pitch_detector);
    pitch_count = PITCH_FRAME_SAMPLES - BOARD_AUDIO_SAMPLES;
    for (size_t index = 0U; index < PITCH_FRAME_SAMPLES; ++index) {
        pitch_samples[index] = 2048U;
    }
    fake_audio_available = true;
    poll_audio(1000U);
    assert(!last_pitch.valid);
    assert(last_pitch.status == PITCH_NO_SIGNAL);
    puts("app: silent frame immediately invalidates old pitch");
}

static void test_adc_gap(void)
{
    /* 样本之间缺了一块，必须丢掉前面累计的数据，不能拼成一段连续声音。 */
    view = VIEW_TUNER;
    calibrating = false;
    last_audio = 984U;
    pitch_count = 256U;
    last_pitch.valid = true;
    fake_hardware.adc_dropped = 1U;
    fake_audio_available = true;
    poll_audio(1000U);
    assert(pitch_count == BOARD_AUDIO_SAMPLES);
    assert(!last_pitch.valid);
    puts("app: ADC discontinuity discards partial pitch frame");
}

static void test_stale_pitch(void)
{
    /* 即使算法刚算完，只要采集已经停止，也不能发布为有效的新结果。 */
    pitch_detector.result.valid = true;
    last_audio = 800U;
    publish_pitch(1000U);
    assert(!last_pitch.valid);
    last_audio = 1000U;
    publish_pitch(1001U);
    assert(last_pitch.valid);
    puts("app: completed pitch cannot republish stale ADC data");
}

/* 一次送入16ms的音频，按真实main的顺序处理本轮任务。 */
static void advance_game_app(uint32_t now, uint16_t amplitude)
{
    fake_audio_amplitude = amplitude;
    fake_audio_available = true;
    app_process_audio(now);
    app_process_commands(now);
    app_process_keys(now);
    app_update_game(now);
    app_update_tuner(now);
    app_update_display(now);
    app_update_status(now);
}

/* 故障按键全部保持低电平，只靠声音和串口完成一局游戏。 */
static void test_complete_game(void)
{
    app_init();
    assert(view == VIEW_GAME && game.state == GAME_READY);
    /* 正式版默认有音效；本用例主动静音，继续验证原来的无声玩法。 */
    fake_input = "mute on\n";
    app_process_commands(0U);
    assert(!sound_enabled);

    /* 安静上电，等待自动标定和声音门重新允许触发。 */
    uint32_t now;
    for (now = 16U; now <= 1408U; now += 16U) {
        advance_game_app(now, 0U);
    }
    assert(!calibrating && game.state == GAME_READY);
    assert(keys_blocked_mask(&keys) == 15U);
    advance_game_app(now, 200U);
    assert(game.state == GAME_PLAYING && game.bird_velocity < 0.0f);

    /* 持续大声不会每一块数据都重新上跳，重力仍会改变速度。 */
    for (unsigned block = 0U; block < 4U; ++block) {
        now += 16U;
        advance_game_app(now, 200U);
    }
    assert(game.bird_velocity > APP_BIRD_FLAP_SPEED);

    fake_input = "pause\n";
    fake_input_position = 0U;
    app_process_commands(now);
    float paused_position = game.bird_y;
    float paused_velocity = game.bird_velocity;
    for (unsigned block = 0U; block < 16U; ++block) {
        now += 16U;
        advance_game_app(now, 0U);
    }
    assert(game.state == GAME_PAUSED);
    assert(game.bird_y == paused_position && game.bird_velocity == paused_velocity);
    fake_input = "resume\n";
    fake_input_position = 0U;
    app_process_commands(now);
    assert(game.state == GAME_PLAYING);

    /* 安排一根刚刚通过的小管道，确认应用层处理了得分但没有启动音效。 */
    game.bird_y = 30.0f;
    game.bird_velocity = 0.0f;
    game.pipes[0].x = GAME_BIRD_X - GAME_BIRD_RADIUS - GAME_PIPE_WIDTH;
    for (unsigned pipe = 1U; pipe < GAME_PIPE_COUNT; ++pipe) {
        game.pipes[pipe].x = 144.0f + pipe * GAME_PIPE_SPACING;
    }
    now += 20U;
    advance_game_app(now, 0U);
    assert(game.score == 1U && game.high_score == 1U);
    assert(fake_sound_starts == 0U);

    /* 撞地结束，再经过安静间隔后吹气重开；本局清零，最高分保留。 */
    game.bird_y = GAME_FLOOR_Y;
    now += 20U;
    advance_game_app(now, 0U);
    assert(game.state == GAME_OVER);
    for (unsigned block = 0U; block < 16U; ++block) {
        now += 16U;
        advance_game_app(now, 0U);
    }
    now += 16U;
    advance_game_app(now, 200U);
    assert(game.state == GAME_PLAYING);
    assert(game.score == 0U && game.high_score == 1U);
    assert(fake_sound_starts == 0U);
    puts("app: boot/calibration/voice/score/pause/retry work with stuck keys and sound off");
}

/* 只更新音频，便于分辨小鸟速度有没有被蜂鸣器的声音意外改写。 */
static void feed_test_audio(uint32_t now, uint16_t amplitude)
{
    fake_audio_amplitude = amplitude;
    fake_audio_available = true;
    app_process_audio(now);
}

static void send_test_command(const char *text, uint32_t now)
{
    fake_input = text;
    fake_input_position = 0U;
    app_process_commands(now);
}

/* 让真实游戏逻辑产生一次得分，而不是直接调用蜂鸣器函数。 */
static void arrange_next_score(void)
{
    game.bird_y = 30.0f;
    game.bird_velocity = 0.0f;
    game.pipes[0].x = GAME_BIRD_X - GAME_BIRD_RADIUS - GAME_PIPE_WIDTH;
    game.pipes[0].passed = false;
    for (unsigned pipe = 1U; pipe < GAME_PIPE_COUNT; ++pipe) {
        game.pipes[pipe].x = 144.0f + pipe * GAME_PIPE_SPACING;
    }
}

static void test_game_sound(void)
{
    app_init();
    assert(sound_enabled);
    assert(fake_sound_frequency == 0U && fake_sound_starts == 0U);

    /* 开机标定不鸣叫；安静完成后，第一次吹气只开局，不播放音效。 */
    uint32_t now = 0U;
    for (unsigned block = 0U; block < 90U; ++block) {
        now += 16U;
        advance_game_app(now, 0U);
    }
    assert(!calibrating && game.state == GAME_READY);
    assert(fake_sound_starts == 0U);
    now += 16U;
    feed_test_audio(now, 200U);
    assert(game.state == GAME_PLAYING);
    assert(fake_sound_starts == 0U);

    arrange_next_score();
    now += 20U;
    app_update_game(now);
    assert(game.score == 1U);
    assert(fake_sound_starts == 1U);
    assert(fake_sound_frequency == APP_BEEP_FREQUENCY_HZ);
    uint32_t score_time = now;
    float velocity_before_sound = game.bird_velocity;

    /* 把蜂鸣器泄漏到麦克风的声音模拟为持续大声；它不能造成上跳。 */
    for (unsigned block = 0U; block < 3U; ++block) {
        now += 16U;
        feed_test_audio(now, 200U);
        assert(game.bird_velocity == velocity_before_sound);
    }
    feed_test_audio(score_time + APP_BEEP_DURATION_MS - 1U, 200U);
    assert(fake_sound_frequency == APP_BEEP_FREQUENCY_HZ);
    now = score_time + APP_BEEP_DURATION_MS;
    feed_test_audio(now, 200U);
    assert(fake_sound_frequency == 0U);
    while (now - score_time <= APP_BEEP_IGNORE_AUDIO_MS + 16U) {
        now += 16U;
        feed_test_audio(now, 200U);
        assert(game.bird_velocity == velocity_before_sound);
    }
    assert(!audio_muted);

    /* 屏蔽结束仍需先安静；持续大声不能在恢复时凭空成为一次新吹气。 */
    for (unsigned block = 0U; block < 5U; ++block) {
        now += 16U;
        feed_test_audio(now, 0U);
    }
    now += 16U;
    feed_test_audio(now, 200U);
    assert(game.bird_velocity == APP_BIRD_FLAP_SPEED);
    assert(sound_gate.last_trigger_ms == now);

    /* 碰撞也播放一次短音；它不能把结束界面自动变成下一局。 */
    game.bird_y = GAME_FLOOR_Y;
    game.bird_velocity = 0.0f;
    now += 20U;
    app_update_game(now);
    assert(game.state == GAME_OVER && fake_sound_starts == 2U);
    assert(fake_sound_frequency == APP_BEEP_FREQUENCY_HZ);
    for (unsigned block = 0U; block < 22U; ++block) {
        now += 16U;
        feed_test_audio(now, 200U);
        assert(game.state == GAME_OVER);
    }
    assert(fake_sound_frequency == 0U);
    for (unsigned block = 0U; block < 5U; ++block) {
        now += 16U;
        feed_test_audio(now, 0U);
    }
    now += 16U;
    feed_test_audio(now, 200U);
    assert(game.state == GAME_PLAYING && game.score == 0U);
    assert(fake_sound_starts == 2U);

    /* 静音命令立即结束正在播放的短音，并禁止后续得分和碰撞音效。 */
    arrange_next_score();
    now += 20U;
    app_update_game(now);
    assert(fake_sound_starts == 3U && fake_sound_frequency != 0U);
    now += 10U;
    send_test_command("mute on\n", now);
    assert(!sound_enabled && !beep_active && fake_sound_frequency == 0U);
    arrange_next_score();
    now += 20U;
    app_update_game(now);
    assert(game.score == 2U && fake_sound_starts == 3U);
    game.bird_y = GAME_FLOOR_Y;
    game.bird_velocity = 0.0f;
    now += 20U;
    app_update_game(now);
    assert(game.state == GAME_OVER && fake_sound_starts == 3U);

    /* 取消静音可以恢复游戏音效；进入调音器则必须立即停音并拒绝测试音。 */
    send_test_command("mute off\nplay\n", now);
    assert(sound_enabled);
    arrange_next_score();
    now += 20U;
    app_update_game(now);
    assert(fake_sound_starts == 4U && fake_sound_frequency != 0U);
    send_test_command("tuner\n", now);
    assert(view == VIEW_TUNER && !beep_active && fake_sound_frequency == 0U);
    send_test_command("beep\n", now);
    now += 20U;
    app_update_game(now);
    assert(fake_sound_starts == 4U && fake_sound_frequency == 0U);
    puts("app: default game sounds, timed stop, voice isolation, mute and tuner protection passed");
}

static void export_frame(const char *name, uint32_t now)
{
    /* 把真正绘图函数产生的像素写成PGM，方便在电脑上检查文字布局。 */
    assert(display_ready());
    draw_frame(now);
    for (unsigned attempt = 0U; attempt < 256U && !display_ready(); ++attempt) {
        display_poll(now);
    }
    assert(display_ready());
    char path[128];
    snprintf(path, sizeof(path), "build/%s.pgm", name);
    FILE *output = fopen(path, "wb");
    assert(output != NULL);
    fputs("P5\n128 64\n255\n", output);
    for (size_t vertical = 0U; vertical < 64U; ++vertical) {
        for (size_t horizontal = 0U; horizontal < 128U; ++horizontal) {
            uint8_t value = (fake_screen[vertical / 8U * 128U + horizontal] & (1U << (vertical % 8U))) != 0U ? 255U : 0U;
            fputc(value, output);
        }
    }
    assert(fclose(output) == 0);
}

static void test_render(void)
{
    fake_hardware.core_hz = 72000000U;
    fake_hardware.hse_ready = true;
    audio_stats.mean = 2048U;
    audio_stats.rms = 58U;
    audio_stats.peak = 101U;
    pot_raw = 2048U;
    calibrating = false;
    game_init(&game, 19U);
    keys_init(&keys, 0U, 0U);
    display_init(0U);
    for (uint32_t now = 0U; now < 1000U && !display_ready(); ++now) {
        display_poll(now);
    }
    assert(display_ready());
    view = VIEW_GAME;
    export_frame("ready", 1000U);
    game_start(&game);
    game.pipes[0].x = 64.0f;
    export_frame("playing", 1100U);
    game.state = GAME_OVER;
    export_frame("game_over", 1200U);
    view = VIEW_DIAGNOSTIC;
    last_audio = 1300U;
    export_frame("diagnostic", 1300U);
    view = VIEW_TUNER;
    last_pitch.valid = true;
    last_pitch.frequency_millihz = 110010U;
    last_pitch.cents = 0;
    last_pitch.string_index = 1U;
    last_pitch_ms = 1400U;
    export_frame("tuner", 1400U);
    puts("app: five real framebuffer views exported");
}

int main(int count, char **arguments)
{
    assert(count == 2);
    if (strcmp(arguments[1], "rx-loss") == 0) {
        test_rx_loss();
    } else if (strcmp(arguments[1], "pitch-silence") == 0) {
        test_pitch_silence();
    } else if (strcmp(arguments[1], "adc-gap") == 0) {
        test_adc_gap();
    } else if (strcmp(arguments[1], "render") == 0) {
        test_render();
    } else if (strcmp(arguments[1], "pitch-stale") == 0) {
        test_stale_pitch();
    } else if (strcmp(arguments[1], "complete-game") == 0) {
        test_complete_game();
    } else if (strcmp(arguments[1], "game-sound") == 0) {
        test_game_sound();
    } else {
        return 1;
    }
    return 0;
}
