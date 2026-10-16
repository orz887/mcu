#ifndef FLAPPY_APP_CONFIG_H
#define FLAPPY_APP_CONFIG_H

/* 常用参数集中在这里。每次只改一类参数，再编译、烧录、观察效果。 */
/* 串口修改只保存在内存；重新上电恢复这里的默认值。 */

/* 麦克风：门限越小越灵敏；开机安静标定会重新计算实际门限。 */
#define APP_DEFAULT_SOUND_THRESHOLD 100U
#define APP_AUTO_THRESHOLD_MIN 70U
#define APP_SOUND_QUIET_MS 50U
#define APP_SOUND_COOLDOWN_MS 180U
#define APP_CALIBRATION_SETTLE_MS 200U
#define APP_CALIBRATION_DURATION_MS 1200U

/* 3300只是名义值，精度验收时应改为实测VDDA，单位mV。 */
#define APP_DEFAULT_VDDA_MV 3300U
#define APP_DEFAULT_POT_ENABLED 0

/* 每20ms前进一步；速度单位为像素/步，重力为每步增加的速度。 */
#define APP_GAME_STEP_MS 20U
#define APP_BIRD_GRAVITY 0.11f
#define APP_BIRD_FLAP_SPEED (-1.80f)
#define APP_BIRD_MAX_FALL_SPEED 4.0f
#define APP_PIPE_SPEED 1.0f
#define APP_PIPE_GAP 26
#define APP_PIPE_SPACING 64

/* 按键电平稳定20ms才确认，防止机械抖动造成重复动作。 */
#define APP_KEY_DEBOUNCE_MS 20U

/* 正式版默认开启得分和碰撞短音；串口mute on可静音，声控仍可使用。 */
/* 发声后先屏蔽300ms，再等待安静，避免麦克风把蜂鸣器当成一次吹气。 */
#define APP_GAME_SOUND_ENABLED 1
#define APP_BEEP_FREQUENCY_HZ 4000U
#define APP_BEEP_DURATION_MS 60U
#define APP_BEEP_IGNORE_AUDIO_MS 300U

/* 界面和通信节奏；串口终端必须与此处波特率一致。 */
#define APP_DISPLAY_INTERVAL_MS 40U
/* GPIO SPI的边沿等待次数，越大越慢；兼容版由构建参数固定为192。 */
/* 24用于正常游戏，192已通过实屏诊断；具体频率随主频和编译结果变化。 */
#ifndef APP_OLED_GPIO_DELAY_LOOPS
#define APP_OLED_GPIO_DELAY_LOOPS 24U
#endif
#define APP_UART_BAUD 115200U
#define APP_TELEMETRY_PERIOD_MS 1000U

/* 采样中断后丢弃旧音频，避免拼接不连续数据而显示虚假音高。 */
#define APP_AUDIO_GAP_MS 50U
#define APP_ADC_STALE_MS 100U
#define APP_PITCH_STALE_MS 600U

#endif
