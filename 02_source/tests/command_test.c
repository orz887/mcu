/* 命令解析验证：正常参数必须通过，负数、超长数字和多余参数必须被拒绝。 */
#include "command.h"
#include <assert.h>
#include <stdio.h>

int main(void)
{
    Command command;
    assert(command_parse("  PLAY  ", &command) && command.kind == COMMAND_PLAY);
    assert(command_parse("threshold 4095", &command) && command.value == 4095U);
    assert(command_parse("vdda 3150", &command) && command.kind == COMMAND_VDDA && command.value == 3150U);
    assert(command_parse("pot off", &command) && command.value == 0U);
    assert(command_parse("mute ON", &command) && command.value == 1U);
    assert(!command_parse("threshold -1", &command));
    assert(!command_parse("threshold 4096", &command));
    assert(!command_parse("threshold 0", &command));
    assert(!command_parse("vdda 5000", &command));
    assert(!command_parse("play garbage", &command));
    assert(!command_parse("pot on extra", &command));
    assert(!command_parse("threshold 999999999999999999999999999999", &command));
    assert(!command_parse("beep 1", &command));
    assert(!command_parse("", &command));
    puts("command: valid commands and malformed input passed");
    return 0;
}
