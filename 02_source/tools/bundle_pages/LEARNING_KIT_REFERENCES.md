# 手册与参考资料

新手不需要从第一页读到最后一页。先提出具体问题，再查对应章节，例如“这个引脚能做什么”“这个命令设置什么”“这个开关哪两脚常通”。

| 包内文件 | 用途 |
| --- | --- |
| [SSD1309手册](SSD1309_Datasheet.pdf) | 屏幕命令、地址与通信时序 |
| [微雪屏幕原理图](Waveshare_2in42_schematic.pdf) | 供电、电平转换及SPI/I²C接口选择 |
| [微雪模块尺寸](Waveshare_2in42_dimensions.pdf) | 外壳设计的机械依据 |
| [微雪原始示例包](Waveshare_original_OLED_examples.7z) | 厂商示例，保留原许可；本项目接线不同，不直接烧原示例 |
| [四脚开关手册](TS1187_switch_C318884.pdf) | 核对内部常通触点分组 |
| [AP2114稳压器](AP2114.pdf) | 3.3V供电器件 |
| [LMV358放大器](LMV358_TECH_PUBLIC.pdf) | 麦克风模拟前端 |
| [蜂鸣器](GSC1030RB_INGHAI.pdf) | BZ1器件参数 |
| [USB接口](USB_TypeC_6pin.pdf) | 本板6脚Type-C封装 |

下列官方资料保留在线链接。本次从命令行下载ST PDF未成功，**不把未取得的PDF冒充已离线收录**；课程和项目源码仍可离线阅读。

- [STM32F103C8数据手册](https://www.st.com/resource/en/datasheet/stm32f103c8.pdf)：型号、引脚、电气条件。
- [STM32F1参考手册RM0008](https://www.st.com/resource/en/reference_manual/cd00171190.pdf)：GPIO、定时器、ADC、DMA、SPI、USART寄存器说明。
- [STM32CubeProgrammer](https://www.st.com/en/development-tools/stm32cubeprog.html)：下载软件与官方文档。
- [PlatformIO VS Code安装](https://docs.platformio.org/en/latest/integration/ide/vscode.html)：首次工具环境配置。
- [微雪屏幕资料页](https://www.waveshare.com/wiki/2.42inch_OLED_Module)：模块与官方示例。

`STM32CubeF1_1.8.7/`还保留本项目对应版本的HAL驱动、CMSIS内核头文件和STM32F1设备文件，可离线查阅底层实现；不要把它当作另一份独立应用工程。正式构建仍由PlatformIO按工程固定版本管理依赖。

项目使用的依赖版本与第三方授权见`02_source/third_party/`、本目录`third_party_licenses/`以及`01_firmware/verification.json`。本包保留既有资料供本项目学习，第三方文档与示例的权利仍归原作者；没有给原项目额外添加未经指定的开源许可证。
