# 依据与第三方依赖

- 最终PCB引脚以本机 `outputs/flappy-bird-pcb-repair-20260918/pcb_final.json` 和 `after_routing_pass2.json` 为准；旧交接文档中I²C屏说明已被SPI版本替代。
- 需求读取自用户提供的 C题-HelloMCU.pdf，基础外设、状态和非阻塞要求在 README 逐项对应。
- STM32F103C8 原厂数据表：https://www.st.com/resource/en/datasheet/stm32f103c8.pdf
- STM32F1 编程手册：https://www.st.com.cn/resource/en/programming_manual/pm0075-stm32f10xxx-flash-memory-microcontrollers-stmicroelectronics.pdf
- PlatformIO目标板与64KB/20KB容量：https://docs.platformio.org/en/latest/boards/ststm32/genericSTM32F103C8.html
- 构建平台：https://github.com/platformio/platform-ststm32/tree/v20.0.0
- STM32CubeF1 官方库：https://github.com/STMicroelectronics/STM32CubeF1
- 微雪屏产品与接线：https://www.waveshare.com/wiki/2.42inch_OLED_Module
- 屏幕原理图：https://files.waveshare.com/wiki/2.42inch-OLED-Module/2.42inch-OLED-Module-Schematic.pdf
- SSD1309芯片手册：https://files.waveshare.com/wiki/2.42inch-OLED-Module/SSD1309Datasheet.pdf
- 微雪官方示例包：https://files.waveshare.com/upload/2/2c/OLED_Module_Code.7z

显示驱动和5×7字体为本项目编写，初始化参数依据上述资料整理。页寻址使用20 02，VCOM使用DB 34，对比度使用81 7F；未照搬官方示例中孤立FF及水平/页寻址混用片段。参数仍需实屏验证。

PlatformIO在用户的包目录下载GNU Arm、STM32 HAL、CMSIS等构建依赖，相关授权随原依赖保留。发布包随附对应许可文件，用于随固件分发；源工程不复制或改写这些依赖的许可证。
