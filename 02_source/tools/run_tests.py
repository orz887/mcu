"""在电脑上编译并运行C测试；不访问开发板，也不会烧录固件。"""

import argparse
import os
from pathlib import Path
import subprocess


def main():
    # 每组测试使用真实模块代码，输出文件集中放在build目录。
    parser = argparse.ArgumentParser()
    parser.add_argument("--sanitize", action="store_true")
    arguments = parser.parse_args()
    project = Path(__file__).resolve().parents[1]
    output = project / "build"
    output.mkdir(exist_ok=True)
    cases = {
        "core": ["src/core.c", "tests/core_test.c"],
        "pitch": ["src/pitch.c", "tests/pitch_test.c"],
        "command": ["src/command.c", "tests/command_test.c"],
        "display": ["src/display.c", "tests/display_test.c"],
        "oled_check": ["tests/oled_check_test.c", "src/display.c"],
        "oled_check_gpio": ["tests/oled_check_test.c", "src/display.c"],
        "oled_gpio_spi": ["src/oled_gpio_spi.c", "tests/oled_gpio_spi_test.c"],
        "oled_gpio_spi_compat": ["src/oled_gpio_spi.c", "tests/oled_gpio_spi_test.c"],
        "app": ["tests/app_test.c", "src/core.c", "src/pitch.c", "src/command.c", "src/display.c"],
    }
    environment = os.environ.copy()
    if arguments.sanitize:
        # 检查越界和未定义行为；当前容器不支持泄漏检测，且业务代码不使用堆。
        environment["ASAN_OPTIONS"] = "detect_leaks=0"
    for name, sources in cases.items():
        executable = output / (name + ("_sanitized" if arguments.sanitize else "_test"))
        command = ["gcc", "-std=c11", "-Wall", "-Wextra", "-Werror", "-Iinclude"]
        if name == "oled_check_gpio":
            command += ["-DBOARD_OLED_GPIO=1"]
        if name in ["oled_gpio_spi", "oled_gpio_spi_compat"]:
            command += ["-Itests/stubs"]
        if name == "oled_gpio_spi_compat":
            command += ["-DAPP_OLED_GPIO_DELAY_LOOPS=192U"]
        if arguments.sanitize:
            command += ["-fsanitize=address,undefined", "-fno-omit-frame-pointer", "-g"]
        command += sources + ["-lm", "-o", str(executable)]
        subprocess.run(command, cwd=project, check=True)
        if name == "app":
            scenarios = ["rx-loss", "pitch-silence", "adc-gap", "pitch-stale", "complete-game", "game-sound", "render"]
        elif name in ["oled_check", "oled_check_gpio"]:
            scenarios = ["normal", "spi-failure"]
        else:
            scenarios = [None]
        for scenario in scenarios:
            invocation = [str(executable)] + ([scenario] if scenario else [])
            subprocess.run(invocation, cwd=project, env=environment, check=True)
    print("PASS: all algorithm, display, command, and application checks")


if __name__ == "__main__":
    main()
