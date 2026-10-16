# 选择要烧录的固件

优先选择 **[flappy_f103.hex](flappy_f103.hex)**。这是v1.0.0完整游戏：已验证的正常GPIO SPI方式，默认启用得分/碰撞蜂鸣器。

| 文件 | 用途 |
| --- | --- |
| [flappy_f103.hex](flappy_f103.hex) | 正常游戏，首选 |
| [flappy_f103_compat.hex](flappy_f103_compat.hex) | 更慢的GPIO传输，显示异常时用于对照，功能相同 |
| [board_selftest.hex](board_selftest.hex) | 上电进入自检，内部时钟64MHz，适合查采样与串口 |
| 同名`.bin` | 原始二进制；使用时手动指定起始地址`0x08000000` |
| 同名`.elf` | 含符号的调试文件，后续学习断点调试时使用 |

HEX自带地址，初学者优先使用HEX。STM32CubeProgrammer保持已成功的SWD、100kHz、Normal、Software reset，开启下载后校验。不修改读保护或其他选项字节。

主板USB供电，ST-LINK只接SWDIO、SWCLK、GND；当前小型ST-LINK V2的3.3V/5V输出留空。详细接线见[硬件说明](../02_source/docs/HARDWARE_AS_BUILT_AND_ERRATA.md)。

上电安静约1.5秒，看到`BLOW TO START`后短吹开局。得分或碰撞会短响；普通上跳不响，避免过多声学反馈。音效关闭可发`mute on`，声控仍然有效。

既有正常游戏已经用户实测，新版默认音效仍需听实际响声并观察是否误触发。当前完整功能验证边界见[验证记录](../02_source/docs/VERIFICATION.md)。
