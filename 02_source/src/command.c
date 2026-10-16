/*
 * 串口命令解析器：只把文字变成“命令类型+参数”，不直接控制硬件。
 * 这样以后换成按键、蓝牙等入口时，也可以复用相同业务动作。
 */
#include "command.h"
#include <ctype.h>
#include <stddef.h>
#include <string.h>

/* 取一个用空格分隔的单词，并统一转小写；长度超限就报告失败。 */
static bool next_token(const char **cursor, char *output, size_t capacity)
{
    while (isspace((unsigned char)**cursor)) {
        /* cursor指向“当前位置的指针”，因此可以推进调用者的读取位置。 */
        ++*cursor;
    }
    size_t length = 0U;
    bool fits = true;
    while (**cursor != '\0' && !isspace((unsigned char)**cursor)) {
        if (length + 1U < capacity) {
            output[length++] = (char)tolower((unsigned char)**cursor);
        } else {
            fits = false;
        }
        ++*cursor;
    }
    output[length] = '\0';
    return fits;
}

/* 逐位读十进制整数，每加一位就检查范围，避免超长数字溢出。 */
static bool parse_number(const char *text, uint16_t minimum, uint16_t maximum, uint16_t *value)
{
    if (*text == '\0') {
        return false;
    }
    uint32_t parsed = 0U;
    while (*text != '\0') {
        if (*text < '0' || *text > '9') {
            return false;
        }
        parsed = parsed * 10U + (uint32_t)(*text++ - '0');
        if (parsed > maximum) {
            return false;
        }
    }
    if (parsed < minimum) {
        return false;
    }
    *value = (uint16_t)parsed;
    return true;
}

/* 成功时输出命令；失败时不执行任何动作，由应用层回复错误提示。 */
bool command_parse(const char *line, Command *result)
{
    if (line == NULL || result == NULL) {
        return false;
    }
    result->kind = COMMAND_INVALID;
    result->value = 0U;
    char name[16];
    char argument[16];
    char extra[2];
    /* 只允许“命令”或“命令 参数”，多出来的第三个单词一律拒绝。 */
    const char *cursor = line;
    if (!next_token(&cursor, name, sizeof(name)) ||
        !next_token(&cursor, argument, sizeof(argument)) ||
        !next_token(&cursor, extra, sizeof(extra)) || extra[0] != '\0') {
        return false;
    }
    /* 无参数命令列表：新增命令时，还需要在app.c里添加对应动作。 */
    static const struct {
        const char *name;
        CommandKind kind;
    } simple[] = {
        {"help", COMMAND_HELP}, {"status", COMMAND_STATUS},
        {"play", COMMAND_PLAY}, {"flap", COMMAND_FLAP},
        {"pause", COMMAND_PAUSE}, {"resume", COMMAND_RESUME},
        {"menu", COMMAND_MENU}, {"diag", COMMAND_DIAG},
        {"tuner", COMMAND_TUNER}, {"cal", COMMAND_CALIBRATE},
        {"beep", COMMAND_BEEP}
    };
    for (size_t index = 0U; index < sizeof(simple) / sizeof(simple[0]); ++index) {
        if (strcmp(name, simple[index].name) == 0 && argument[0] == '\0') {
            result->kind = simple[index].kind;
            return true;
        }
    }
    if (strcmp(name, "threshold") == 0 && parse_number(argument, 1U, 4095U, &result->value)) {
        result->kind = COMMAND_THRESHOLD;
        return true;
    }
    if (strcmp(name, "vdda") == 0 && parse_number(argument, 2000U, 3600U, &result->value)) {
        result->kind = COMMAND_VDDA;
        return true;
    }
    if (strcmp(argument, "on") == 0 || strcmp(argument, "off") == 0) {
        /* 统一把on/off转换成1/0，后续业务不用再比较字符串。 */
        result->value = strcmp(argument, "on") == 0 ? 1U : 0U;
        if (strcmp(name, "pot") == 0) {
            result->kind = COMMAND_POT;
        } else if (strcmp(name, "mute") == 0) {
            result->kind = COMMAND_MUTE;
        } else if (strcmp(name, "stream") == 0) {
            result->kind = COMMAND_STREAM;
        }
        return result->kind != COMMAND_INVALID;
    }
    return false;
}
