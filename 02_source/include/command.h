#ifndef FLAPPY_COMMAND_H
#define FLAPPY_COMMAND_H

#include <stdbool.h>
#include <stdint.h>

/* 命令名称对应的编号；它不是引脚，也不是ASCII字符。 */
typedef enum {
    COMMAND_INVALID,
    COMMAND_HELP,
    COMMAND_STATUS,
    COMMAND_PLAY,
    COMMAND_FLAP,
    COMMAND_PAUSE,
    COMMAND_RESUME,
    COMMAND_MENU,
    COMMAND_DIAG,
    COMMAND_TUNER,
    COMMAND_CALIBRATE,
    COMMAND_THRESHOLD,
    COMMAND_POT,
    COMMAND_VDDA,
    COMMAND_BEEP,
    COMMAND_MUTE,
    COMMAND_STREAM
} CommandKind;

typedef struct {
    CommandKind kind; /* 用户希望执行的动作。 */
    uint16_t value;   /* 数字或on/off参数；无参数命令时为0。 */
} Command;

/* 检查整行文字。返回true才允许执行result中的动作。 */
bool command_parse(const char *line, Command *result);

#endif
