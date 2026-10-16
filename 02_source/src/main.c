/*
 * 从这里开始阅读。
 * 程序先初始化一次，然后不断轮流处理几项小任务。
 * 每项任务只做眼前能完成的一小步，不等待按键、声音或整屏刷新。
 * 参数在include/app_config.h，任务实现按同名函数在app.c中查找。
 */
#include "app.h"
#include "board.h"

int main(void)
{
    app_init();

    while (1) {
        /* TIM4中断每毫秒计数加1；这里读取本轮开始的时间。 */
        uint32_t now = board_millis();

        app_process_audio(now);     /* 取麦克风数据、计算音量、判断吹气。 */
        app_process_commands(now);  /* 收集串口字符，收到完整命令后执行。 */
        app_process_keys(now);      /* 按键消抖，故障键不会卡住程序。 */
        app_update_game(now);       /* 到20ms才移动小鸟、管道并检查碰撞。 */
        app_update_tuner(now);      /* 调音模式每轮只完成少量计算。 */
        app_update_display(now);    /* 更新界面，一次最多发送8字节像素。 */
        app_update_status(now);     /* 更新状态灯，按需输出串口数据。 */

        /* 本轮结束就进入下一轮；各任务自己判断有没有新数据、时间是否到了。 */
    }
}
