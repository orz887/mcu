/*
 * 应用层：把声音采集、输入处理、游戏更新和界面刷新串起来。
 * 外设寄存器统一放在board.c，游戏规则放在core.c。
 * 阅读顺序：app_init，再按main.c中的任务顺序查找同名函数。
 */
#include "app.h"
#include "app_config.h"
#include "board.h"
#include "command.h"
#include "core.h"
#include "display.h"
#include "pitch.h"
#include <stdio.h>
#include <string.h>

/* 界面模式与游戏状态分开：在游戏界面内还可以处于准备、运行、暂停、结束。 */
typedef enum {
    VIEW_GAME,
    VIEW_DIAGNOSTIC,
    VIEW_TUNER
} View;

/* 模块状态都由结构体保存。初学时把它们理解为各模块的“记事本”。 */
static Game game;
static KeysState keys;
static AudioGate sound_gate;
static AudioStats audio_stats;
static PitchDetector pitch_detector;
static PitchResult last_pitch;
static uint16_t audio_samples[BOARD_AUDIO_SAMPLES]; /* 本轮取得的128个音频采样。 */
static uint16_t pitch_samples[PITCH_FRAME_SAMPLES]; /* 凑够一帧后交给调音模块。 */
static uint16_t pitch_count;                       /* 当前已经攒了多少个采样。 */
static uint16_t pot_raw;                           /* PA1电位器的平均原始值。 */
static uint16_t vdda_mv = APP_DEFAULT_VDDA_MV;
static uint16_t threshold = APP_DEFAULT_SOUND_THRESHOLD;
static View view;
static bool sound_enabled = APP_GAME_SOUND_ENABLED; /* 只控制蜂鸣器音效，不关闭声控。 */
static bool pot_enabled = APP_DEFAULT_POT_ENABLED;
static bool streaming;
static bool vdda_measured;
static bool calibrating = true;
static bool beep_active;
static bool audio_muted; /* 蜂鸣器尾音期间暂时不触发游戏，防止自己触发自己。 */
static uint32_t beep_started;
static uint32_t audio_mute_started;
static uint32_t calibration_started;
static uint32_t calibration_sum;
static uint32_t calibration_blocks;
static uint32_t last_audio;
static uint32_t last_pitch_ms;
static uint32_t last_game_ms;
static uint32_t last_rx_dropped;
static uint32_t last_adc_dropped;
static uint32_t last_frame;
static uint32_t last_stream;
static uint32_t boot_started;
static bool keys_reported;
static char command_line[80]; /* 一次只收集一条短命令；满了就丢弃整行。 */
static uint8_t command_length;
static bool discard_command;

/* 把程序内部状态转换成串口里容易看懂的名称。 */
static const char *state_name(void)
{
    if (view == VIEW_DIAGNOSTIC) {
        return "DIAG";
    }
    if (view == VIEW_TUNER) {
        return "TUNER";
    }
    static const char *names[] = {"READY", "PLAY", "PAUSE", "OVER"};
    return names[game.state];
}

/* 这里只把文字加入发送队列；实际发送由UART中断逐字节完成。 */
static void report_status(void)
{
    const volatile BoardStatus *hardware = board_status();
    char message[384];
    snprintf(message, sizeof(message),
             "STATE=%s CLK=%s/%lu ADC=%u RMS=%u PEAK=%u CLIP=%u POT=%u/%lumV VDDA=%u(%s) TH=%u KEYS_LOW=%X BLOCKED=%X SCORE=%u\r\n",
             state_name(), hardware->hse_ready ? "HSE" : "HSI", (unsigned long)hardware->core_hz,
             audio_stats.mean, audio_stats.rms, audio_stats.peak, audio_stats.clipped,
             pot_raw, (unsigned long)((uint32_t)pot_raw * vdda_mv / 4095U), vdda_mv,
             vdda_measured ? "SET" : "NOMINAL", threshold, board_keys_pressed(), keys_blocked_mask(&keys), game.score);
    board_uart_write(message);
    snprintf(message, sizeof(message),
             "BLOCKS=%lu LOST=%lu RX_LOST=%lu TX_LOST=%lu SPI_ERR=%lu FAULT=%lu FREQ_mHz=%lu VALID=%u\r\n",
             (unsigned long)hardware->adc_blocks, (unsigned long)hardware->adc_dropped,
             (unsigned long)hardware->uart_rx_dropped, (unsigned long)hardware->uart_tx_dropped,
             (unsigned long)hardware->spi_errors, (unsigned long)hardware->faults,
             (unsigned long)last_pitch.frequency_millihz, last_pitch.valid ? 1U : 0U);
    board_uart_write(message);
}

/* 在安静环境记录背景音量，用它决定吹气门限。 */
static void start_calibration(uint32_t now)
{
    calibration_started = now;
    calibration_sum = 0U;
    calibration_blocks = 0U;
    calibrating = true;
    audio_gate_init(&sound_gate, threshold, now);
    board_uart_write("CAL: keep microphone quiet for 1 second\r\n");
}

/* 启动短音后立即返回，由audio任务在到时后关闭，不在此等待。 */
static void tone(uint32_t now)
{
    if (view == VIEW_TUNER) {
        board_uart_write("ERR: buzzer disabled in tuner\r\n");
        return;
    }
    board_buzzer(APP_BEEP_FREQUENCY_HZ);
    beep_started = now;
    audio_mute_started = now;
    beep_active = true;
    audio_muted = true;
}

/* 切换模式时清理旧状态，避免上一模式的半帧音频混入调音结果。 */
static void set_view(View next, uint32_t now)
{
    view = next;
    game_menu(&game);
    last_game_ms = now;
    pitch_count = 0U;
    pitch_init(&pitch_detector);
    memset(&last_pitch, 0, sizeof(last_pitch));
    audio_gate_init(&sound_gate, threshold, now);
    if (next == VIEW_TUNER) {
        board_buzzer(0U);
        beep_active = false;
        audio_muted = true;
        audio_mute_started = now;
    }
}

/* command.c已检查命令格式，这里只执行对应业务动作。 */
static void handle_command(const Command *command, uint32_t now)
{
    switch (command->kind) {
    case COMMAND_HELP:
        board_uart_write("HELLO MCU / Flappy Bird " FIRMWARE_VERSION "\r\n"
                         "help status play flap pause resume menu diag tuner cal beep\r\n"
                         "threshold 1..4095 | pot on/off | vdda 2000..3600\r\n"
                         "mute on/off | stream on/off | UART 115200 8N1, newline\r\n"
                         "SW1 flap, SW2 pause, SW3 mode, SW4 calibrate; stuck keys blocked\r\n");
        break;
    case COMMAND_STATUS:
        report_status();
        break;
    case COMMAND_PLAY:
        set_view(VIEW_GAME, now);
        game_start(&game);
        break;
    case COMMAND_FLAP:
        if (view != VIEW_GAME) {
            set_view(VIEW_GAME, now);
        }
        game_flap(&game);
        break;
    case COMMAND_PAUSE:
        game_set_paused(&game, true);
        break;
    case COMMAND_RESUME:
        game_set_paused(&game, false);
        last_game_ms = now;
        break;
    case COMMAND_MENU:
        set_view(VIEW_GAME, now);
        break;
    case COMMAND_DIAG:
        set_view(VIEW_DIAGNOSTIC, now);
        break;
    case COMMAND_TUNER:
        set_view(VIEW_TUNER, now);
        break;
    case COMMAND_CALIBRATE:
        start_calibration(now);
        break;
    case COMMAND_THRESHOLD:
        threshold = command->value;
        pot_enabled = false;
        calibrating = false;
        audio_gate_set_threshold(&sound_gate, threshold, now);
        break;
    case COMMAND_POT:
        pot_enabled = command->value != 0U;
        break;
    case COMMAND_VDDA:
        vdda_mv = command->value;
        vdda_measured = true;
        break;
    case COMMAND_BEEP:
        if (view == VIEW_TUNER) {
            board_uart_write("ERR: buzzer disabled in tuner\r\n");
            return;
        }
        tone(now);
        break;
    case COMMAND_MUTE:
        sound_enabled = command->value == 0U;
        if (!sound_enabled) {
            board_buzzer(0U);
            beep_active = false;
        }
        break;
    case COMMAND_STREAM:
        streaming = command->value != 0U;
        break;
    default:
        return;
    }
    board_uart_write("OK\r\n");
}

/* 每轮最多取32个字符，避免大量串口输入独占CPU。 */
void app_process_commands(uint32_t now)
{
    char character;
    for (uint32_t budget = 0U; budget < 32U; ++budget) {
        uint32_t dropped = board_status()->uart_rx_dropped;
        if (dropped != last_rx_dropped) {
            /* 丢字后不能猜原命令：清队列并等后续新换行重新同步。 */
            board_uart_discard_rx();
            discard_command = true;
            command_length = 0U;
            last_rx_dropped = dropped;
            break;
        }
        if (!board_uart_read(&character)) {
            break;
        }
        if (board_status()->uart_rx_dropped != last_rx_dropped) {
            /* 读取字符时也可能刚好发生中断错误，下轮统一丢弃这批数据。 */
            continue;
        }
        if (character == '\r' || character == '\n') {
            /* 收到换行才执行，半条输入不会改变设备状态。 */
            if (discard_command) {
                board_uart_write("ERR: input lost or line too long; resend\r\n");
            } else if (command_length != 0U) {
                command_line[command_length] = '\0';
                Command command;
                if (command_parse(command_line, &command)) {
                    handle_command(&command, now);
                } else {
                    board_uart_write("ERR: invalid command; type help\r\n");
                }
            }
            command_length = 0U;
            discard_command = false;
        } else if (!discard_command) {
            if (character == '\b' || character == 127) {
                if (command_length != 0U) {
                    --command_length;
                }
            } else if (character >= 32 && character <= 126) {
                if (command_length < sizeof(command_line) - 1U) {
                    command_line[command_length++] = character;
                } else {
                    discard_command = true;
                }
            } else if (character != '\t') {
                discard_command = true;
            } else if (command_length < sizeof(command_line) - 1U) {
                command_line[command_length++] = ' ';
            } else {
                discard_command = true;
            }
        }
    }
}

/* 静音可立即结束计算，正常音高分几轮完成；两种结果都从这里发布。 */
static void publish_pitch(uint32_t now)
{
    last_pitch = *pitch_result(&pitch_detector);
    if (now - last_audio > APP_ADC_STALE_MS) {
        last_pitch.valid = false;
    }
    last_pitch_ms = now;
}

/* 取一块DMA采样，先算音量，再按当前模式交给游戏或调音器。 */
static void poll_audio(uint32_t now)
{
    if (!board_audio_read(audio_samples, &pot_raw)) {
        return;
    }
    uint32_t dropped = board_status()->adc_dropped;
    if (dropped != last_adc_dropped || now - last_audio > APP_AUDIO_GAP_MS) {
        /* 频率计算要求时间连续；中间少了一块，就重新开始凑帧。 */
        pitch_count = 0U;
        pitch_init(&pitch_detector);
        memset(&last_pitch, 0, sizeof(last_pitch));
        audio_gate_init(&sound_gate, threshold, now);
        last_adc_dropped = dropped;
    }
    last_audio = now;
    audio_stats_compute(audio_samples, BOARD_AUDIO_SAMPLES, &audio_stats);
    if (calibrating) {
        /* 刚上电先让模拟电路稳定，再统计背景音量。 */
        if (now - calibration_started >= APP_CALIBRATION_SETTLE_MS) {
            calibration_sum += audio_stats.rms;
            ++calibration_blocks;
        }
        if (now - calibration_started >= APP_CALIBRATION_DURATION_MS && calibration_blocks != 0U) {
            uint32_t calculated = calibration_sum / calibration_blocks * 3U + 30U;
            /* 门限至少略高于背景噪声，同时限制在可用的ADC范围内。 */
            if (calculated < APP_AUTO_THRESHOLD_MIN) {
                calculated = APP_AUTO_THRESHOLD_MIN;
            }
            if (calculated > 2000U) {
                calculated = 2000U;
            }
            threshold = (uint16_t)calculated;
            audio_gate_set_threshold(&sound_gate, threshold, now);
            calibrating = false;
            board_uart_write("CAL DONE\r\n");
        }
    }
    if (pot_enabled && !calibrating) {
        /* 把旋钮0..4095映射到门限40..640，忽略小抖动。 */
        uint16_t next_threshold = (uint16_t)(40U + (uint32_t)pot_raw * 600U / 4095U);
        if (next_threshold > threshold + 12U || next_threshold + 12U < threshold) {
            threshold = next_threshold;
            audio_gate_set_threshold(&sound_gate, threshold, now);
        }
    }
    bool quiet_required = calibrating || audio_muted || view != VIEW_GAME;
    if (audio_gate_update(&sound_gate, audio_stats.rms, now, quiet_required)) {
        game_flap(&game);
    }
    if (view == VIEW_TUNER && !audio_muted) {
        /* 每块128点，攒够1024点再提交；算法会保存自己的副本。 */
        memcpy(&pitch_samples[pitch_count], audio_samples, sizeof(audio_samples));
        pitch_count += BOARD_AUDIO_SAMPLES;
        if (pitch_count == PITCH_FRAME_SAMPLES) {
            if (!pitch_busy(&pitch_detector)) {
                pitch_begin(&pitch_detector, pitch_samples, PITCH_FRAME_SAMPLES);
                if (!pitch_busy(&pitch_detector)) {
                    publish_pitch(now);
                }
            }
            pitch_count = 0U;
        }
    }
}

/* 屏幕原点在左上，x向右增加，y向下增加。 */
static void draw_game(void)
{
    char text[24];
    snprintf(text, sizeof(text), "S:%u HI:%u", game.score, game.high_score);
    display_text(0, 0, text, 1U);
    display_line(0, GAME_PLAY_TOP, 127, GAME_PLAY_TOP, true);
    display_line(0, GAME_FLOOR_Y, 127, GAME_FLOOR_Y, true);
    if (game.state == GAME_READY) {
        display_text(28, 15, "FLAPPY", 2U);
        display_text(40, 33, "BIRD", 2U);
        display_text(19, 50, calibrating ? "KEEP QUIET..." : "BLOW TO START", 1U);
        return;
    }
    for (uint32_t pipe = 0U; pipe < GAME_PIPE_COUNT; ++pipe) {
        int16_t horizontal = game_pipe_x(&game.pipes[pipe]);
        int16_t top = game.pipes[pipe].gap_top;
        display_fill_rect(horizontal, GAME_PLAY_TOP + 1, GAME_PIPE_WIDTH,
                          top - GAME_PLAY_TOP - 1, true);
        display_fill_rect(horizontal, top + GAME_PIPE_GAP, GAME_PIPE_WIDTH,
                          GAME_FLOOR_Y - top - GAME_PIPE_GAP, true);
    }
    int16_t bird_y = game_bird_y(&game);
    display_fill_rect(GAME_BIRD_X - 3, bird_y - 2, 6, 5, true);
    display_pixel(GAME_BIRD_X + 3, bird_y, true);
    display_pixel(GAME_BIRD_X + 1, bird_y - 1, false);
    if (game.state == GAME_PAUSED || game.state == GAME_OVER) {
        display_fill_rect(9, 19, 110, 32, false);
        display_rect(9, 19, 110, 32, true);
        display_text(game.state == GAME_OVER ? 16 : 28, 23,
                     game.state == GAME_OVER ? "GAME OVER" : "PAUSED", 1U);
        display_text(16, 39, game.state == GAME_OVER ? "BLOW TO RETRY" : "SW2 / RESUME", 1U);
    }
}

/* 自检页显示原始数据，先验证硬件，再验证玩法。 */
static void draw_diagnostic(uint32_t now)
{
    char text[24];
    const volatile BoardStatus *hardware = board_status();
    snprintf(text, sizeof(text), "DIAG %s %luMHZ", hardware->hse_ready ? "HSE" : "HSI",
             (unsigned long)(hardware->core_hz / 1000000U));
    display_text(0, 0, text, 1U);
    snprintf(text, sizeof(text), "MIC %u RMS %u", audio_stats.mean, audio_stats.rms);
    display_text(0, 10, text, 1U);
    snprintf(text, sizeof(text), "PEAK %u CLIP %u", audio_stats.peak, audio_stats.clipped);
    display_text(0, 20, text, 1U);
    snprintf(text, sizeof(text), "POT %u %luMV", pot_raw,
             (unsigned long)((uint32_t)pot_raw * vdda_mv / 4095U));
    display_text(0, 30, text, 1U);
    snprintf(text, sizeof(text), "TH %u KEYS %X BAD %X", threshold,
             board_keys_pressed(), keys_blocked_mask(&keys));
    display_text(0, 40, text, 1U);
    if (hardware->faults != 0U || now - last_audio > APP_ADC_STALE_MS) {
        snprintf(text, sizeof(text), "FAULT %lu ADC %s", (unsigned long)hardware->faults,
                 now - last_audio > APP_ADC_STALE_MS ? "STALE" : "OK");
    } else {
        snprintf(text, sizeof(text), "UART115200 VDDA %s", vdda_measured ? "SET" : "NOM");
    }
    display_text(0, 54, text, 1U);
}

/* 调音是选学扩展：显示频率，并用音分标尺表示偏高或偏低。 */
static void draw_tuner(uint32_t now)
{
    char text[24];
    display_text(4, 0, "GUITAR TUNER", 1U);
    if (last_pitch.valid && now - last_pitch_ms < APP_PITCH_STALE_MS) {
        display_text(6, 15, pitch_string_name(last_pitch.string_index), 3U);
        snprintf(text, sizeof(text), "%lu.%luHZ",
                 (unsigned long)(last_pitch.frequency_millihz / 1000U),
                 (unsigned long)((last_pitch.frequency_millihz % 1000U) / 100U));
        display_text(47, 18, text, 1U);
        snprintf(text, sizeof(text), "%+d CENT", last_pitch.cents);
        display_text(47, 31, text, 1U);
        display_line(14, 49, 114, 49, true);
        display_line(64, 44, 64, 54, true);
        int16_t cents = last_pitch.cents;
        if (cents < -50) {
            cents = -50;
        }
        if (cents > 50) {
            cents = 50;
        }
        display_fill_rect(63 + cents, 46, 3, 7, true);
        display_text(1, 57, "FLAT  IN TUNE  SHARP", 1U);
    } else {
        display_text(16, 21, "PLUCK A STRING", 1U);
        display_text(19, 36, audio_stats.clipped != 0U ? "INPUT CLIPPED" : "WAITING...", 1U);
        snprintf(text, sizeof(text), "RMS %u / 65-400HZ", audio_stats.rms);
        display_text(0, 54, text, 1U);
    }
}

/* 先在内存里画好整张画，再交给显示驱动逐块发送变化的部分。 */
static void draw_frame(uint32_t now)
{
    display_clear();
    if (view == VIEW_DIAGNOSTIC) {
        draw_diagnostic(now);
    } else if (view == VIEW_TUNER) {
        draw_tuner(now);
    } else {
        draw_game();
    }
    display_present();
}

/* 上电准备。此函数只执行一次，后续所有工作都分散到各个循环任务中。 */
void app_init(void)
{
    board_init();
    uint32_t now = board_millis();
    display_init(now);
    game_init(&game, 0xF1A99U);
    keys_init(&keys, (uint8_t)(~board_keys_pressed()) & 0x0FU, now);
    audio_gate_init(&sound_gate, threshold, now);
    pitch_init(&pitch_detector);
#ifdef BOARD_SELFTEST
    view = VIEW_DIAGNOSTIC;
#else
    view = VIEW_GAME;
#endif
    board_uart_write("\r\nHELLO MCU / Flappy Bird " FIRMWARE_VERSION "\r\n"
                     "SWD kept enabled. Type help then Enter.\r\n"
                     "Known PCB issue: SW1-SW4 may be grounded; blocked until released.\r\n");
    start_calibration(now);
    report_status();
    last_game_ms = now;
    last_frame = now;
    last_stream = now;
    boot_started = now;
}

void app_process_audio(uint32_t now)
{
    /* 记录开始时间，再检查时间差，不用延时等待蜂鸣器响完。 */
    if (beep_active && now - beep_started >= APP_BEEP_DURATION_MS) {
        board_buzzer(0U);
        beep_active = false;
    }
    if (audio_muted && now - audio_mute_started >= APP_BEEP_IGNORE_AUDIO_MS) {
        audio_muted = false;
    }
    poll_audio(now);
    if (now - last_audio > APP_ADC_STALE_MS || now - last_pitch_ms >= APP_PITCH_STALE_MS) {
        last_pitch.valid = false;
    }
}

void app_process_keys(uint32_t now)
{
    /* bit0..3为SW1..SW4；按下mask取反得到算法所需的高电平mask。 */
    uint8_t pressed_keys = board_keys_pressed();
    uint8_t high_keys = (uint8_t)(~pressed_keys) & 0x0FU;
    uint8_t events = keys_update(&keys, high_keys, now);
    if ((events & 1U) != 0U) {
        if (view == VIEW_GAME) {
            game_flap(&game);
        } else {
            set_view(VIEW_GAME, now);
        }
    }
    if ((events & 2U) != 0U && view == VIEW_GAME) {
        game_set_paused(&game, game.state != GAME_PAUSED);
    }
    if ((events & 4U) != 0U) {
        if (view == VIEW_GAME) {
            set_view(VIEW_DIAGNOSTIC, now);
        } else if (view == VIEW_DIAGNOSTIC) {
            set_view(VIEW_TUNER, now);
        } else {
            set_view(VIEW_GAME, now);
        }
    }
    if ((events & 8U) != 0U) {
        start_calibration(now);
    }
}

void app_update_game(uint32_t now)
{
    if (view != VIEW_GAME || now - last_game_ms < GAME_STEP_MS) {
        return;
    }
    /* 调试暂停太久时丢掉积压帧，避免恢复后突然连续移动很长距离。 */
    if (now - last_game_ms > GAME_STEP_MS * 4U) {
        last_game_ms = now - GAME_STEP_MS;
    }
    last_game_ms += GAME_STEP_MS;
    uint8_t game_events = game_step(&game);
    if (game_events != 0U && sound_enabled) {
        tone(now);
    }
}

void app_update_tuner(uint32_t now)
{
    /* 一轮只比较两个候选周期，算不完下轮继续，串口和屏幕不会饿死。 */
    if (view == VIEW_TUNER && pitch_busy(&pitch_detector)) {
        if (pitch_step(&pitch_detector, 2U)) {
            publish_pitch(now);
        }
    }
}

void app_update_display(uint32_t now)
{
    display_poll(now);
    if (display_ready() && now - last_frame >= APP_DISPLAY_INTERVAL_MS) {
        draw_frame(now);
        last_frame = now;
    }
}

void app_update_status(uint32_t now)
{
    /* 等待消抖完成，再提醒用户哪些按键始终未释放。 */
    if (!keys_reported && now - boot_started >= 100U) {
        if (keys_blocked_mask(&keys) != 0U) {
            board_uart_write("WARN: startup-low keys blocked; repair SW1-SW4 or use voice/UART\r\n");
        }
        keys_reported = true;
    }
    if (streaming && now - last_stream >= APP_TELEMETRY_PERIOD_MS) {
        report_status();
        last_stream = now;
    }
    /* 正常时每秒亮100ms；错误时改为每200ms亮100ms。 */
    bool has_error = board_status()->faults != 0U || now - last_audio > APP_ADC_STALE_MS;
    bool led_on;
    if (has_error) {
        led_on = now % 200U < 100U;
    } else {
        led_on = now % 1000U < 100U;
    }
    board_led(led_on);
}
