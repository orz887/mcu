# Formal Learning Release Implementation Plan

> **For agentic workers:** Use executing-plans for root integration and bounded parallel tasks for sound regression, hardware inventory and the beginner course. Do not commit or create branches.

**Goal:** 发布默认启用蜂鸣器的v1.0.0，并把本项目硬件、软件、外壳、历史证据和零基础中文教程整理成可校验的tar.gz学习包。

**Architecture:** 保留用户已实测正常的GPIO SPI正常游戏路径，蜂鸣器复用既有得分/碰撞短音与防自触发逻辑。学习包提供离线HTML入口、完整源码、可烧录固件、原生产文件和明确的硬件勘误；历史版本与当前学习入口分开，防止旧接线表误用。

**Tech Stack:** STM32F103C8T6、C11、STM32CubeF1 HAL、PlatformIO、Python、Markdown、离线HTML、tar.gz、SHA256。

**Spec:** 用户本轮要求“正常游戏版测试正常，制作正式版本，启用蜂鸣器，将所有硬件软件资料打包tar.gz，面向零基础入门嵌入式”；既有约束为中文注释、优先易读易改、避免不必要高阶优化。

## Global Constraints

- 正常游戏GPIO等待24已由用户实物确认；兼容192保留。
- 蜂鸣器得分/碰撞4kHz、60ms，默认开启；300ms声控屏蔽与安静重启门保留，调音模式禁音。
- 发布动作仅生成本地文件，不代替用户烧录、下单或对外发送。
- PCB仍有已知按键触点分组错误；未找到已修订制造文件，不可宣称可直接复产。
- 已实物验证的软件版本与新增蜂鸣器待实测范围分别记录。
- 固件源码与教程保持中文说明；不打包用户无关目录、凭据、工具链缓存或编译临时文件。

## Review Focus

- 蜂鸣器声音被麦克风听到时不能触发上跳或自动重开。
- 静音与调音切换必须停止音效，结束屏蔽后能恢复正常声控。
- 初学者应能从包入口确定唯一推荐HEX、当前针序与有缺陷的硬件文件。
- HTML与Markdown的相对链接和图片在解压后离线可用。
- tar只包含包内普通文件，校验每个文件大小与SHA256；原生产和历史资料可追溯。

## Task 1: 正式音效行为与回归

- [x] 在tests/app_test.c新增默认开音、得分/结束短音、到期关闭、声控防自触发、静音与调音禁音的行为测试，先观察默认关闭导致失败。
- [x] 在include/app_config.h启用APP_GAME_SOUND_ENABLED，现有声音门行为经测试无需修改。
- [x] 原静音游戏流程改成显式mute on，保留原覆盖；运行本机测试与ASan/UBSan。

## Task 2: 当前硬件、历史资料和教学

- [x] 逐项清点PCB源工程、Gerber、BOM、坐标文件、V4外壳打印件/STEP/装配模型、CAD脚本依赖与参考手册。
- [x] 创建docs/HARDWARE_AS_BUILT_AND_ERRATA.md，明确针序、按键缺陷、返修/复产边界和已知实物验证范围。
- [x] 创建docs/COURSE_ZERO_TO_EMBEDDED.md，串联基础电学、硬件、C、主循环、GPIO、UART、ADC/DMA、TIM/NVIC、SPI、PWM、游戏、装配和10个实验。

## Task 3: 发布元数据与离线学习包

- [x] 把platformio.ini、README、现行设计/验证/快速上手文档更新为v1.0.0，记录正常游戏已验证及新音效待验证。
- [x] 编译flappy_f103、flappy_f103_compat和board_selftest，检查GPIO传输、向量、HEX/BIN一致与内存限制。
- [x] 实现tools/build_learning_bundle.py，组装01_firmware到07_references，保留CAD脚本相邻依赖和历史原件。
- [x] 生成离线HTML教程、包入口与目录清单，检查相对链接、图片与代码片段；浏览器验证电脑/手机宽度无溢出、图片加载正常。
- [x] 生成Flappy_Bird_Learning_Kit_v1.0.0_20260923.tar.gz、SHA256与逐文件manifest，重新读取压缩包逐项校验。

## Task 4: 最终交付审查

- [x] 复核推荐HEX、默认音效、硬件缺陷提示与教程参数一致，确认没有把历史“可下单”描述当成现行结论。
- [x] 准备tar.gz、正式HEX和离线教程入口，交付说明覆盖蜂鸣器新行为与实际验证边界。
