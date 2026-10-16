"""整理已编译文件并验证地址、向量表、HEX内容与压缩包；不连接开发板。"""

import argparse
import hashlib
import json
from pathlib import Path
import re
import shutil
import struct
import subprocess
import zipfile


def sha256(path):
    """计算校验值，让接收者能判断文件是否被改变。"""
    return hashlib.sha256(path.read_bytes()).hexdigest()


def verify_hex(path, binary):
    """检查HEX每行校验和，并确认实际字节与同版本BIN一致。"""
    memory = {}
    upper_address = 0
    ended = False
    for line in path.read_text().splitlines():
        if not line.startswith(":"):
            raise ValueError("Invalid Intel HEX record")
        record = bytes.fromhex(line[1:])
        if len(record) != record[0] + 5 or sum(record) % 256:
            raise ValueError("Invalid Intel HEX checksum")
        offset = int.from_bytes(record[1:3], "big")
        record_type = record[3]
        payload = record[4:-1]
        if record_type == 0:
            for displacement, value in enumerate(payload):
                memory[upper_address + offset + displacement] = value
        elif record_type == 4:
            upper_address = int.from_bytes(payload, "big") << 16
        elif record_type == 2:
            upper_address = int.from_bytes(payload, "big") << 4
        elif record_type == 1:
            ended = True
    if not ended or min(memory) != 0x08000000:
        raise ValueError("HEX origin or EOF invalid")
    if max(memory) >= 0x08010000:
        raise ValueError("HEX exceeds STM32F103C8 flash")
    reconstructed = bytes(memory.get(0x08000000 + offset, 0xFF) for offset in range(len(binary)))
    if reconstructed != binary:
        raise ValueError("HEX data differs from BIN")


def main():
    # 发布文件与源工程分开放，避免用户把测试中间文件误当成烧录文件。
    project = Path(__file__).resolve().parents[1]
    if re.search(r"^#define\s+APP_GAME_SOUND_ENABLED\s+1\s*$",
                 (project / "include/app_config.h").read_text(), re.MULTILINE) is None:
        raise ValueError("Formal release requires game sound enabled in app_config.h")
    parser = argparse.ArgumentParser()
    parser.add_argument("--output", type=Path,
                        default=project.parent / "outputs" / "Flappy_Bird_Firmware_v1.0.0_20260923")
    arguments = parser.parse_args()
    release = arguments.output.resolve()
    firmware = release / "firmware"
    source = release / "source"
    firmware.mkdir(parents=True, exist_ok=True)
    source.mkdir(parents=True, exist_ok=True)
    packages = Path.home() / ".platformio" / "packages"
    tools = packages / "toolchain-gccarmnoneeabi" / "bin"
    executable_suffix = ".exe" if (tools / "arm-none-eabi-objcopy.exe").exists() else ""
    objcopy = str(tools / ("arm-none-eabi-objcopy" + executable_suffix))
    size_tool = str(tools / ("arm-none-eabi-size" + executable_suffix))
    nm_tool = str(tools / ("arm-none-eabi-nm" + executable_suffix))
    verification = {"version": "1.0.0", "mcu": "STM32F103C8T6", "hardware_verified": False,
                    "hardware_observations": {"gpio_oled_diagnostic": "User confirmed all patterns display at delay=192", "normal_game_v0.1.1": "User confirmed normal game works at delay=24", "buzzer_v1.0.0": "Enabled and host-tested; physical sound and acoustic feedback pending"},
                    "game_sound_default": True,
                    "flash_origin": "0x08000000", "flash_limit": 65536, "ram_limit": 20480,
                    "environments": {}, "dependencies": {}}
    required_vectors = {"Reset_Handler": 1, "SysTick_Handler": 15,
                        "DMA1_Channel1_IRQHandler": 27, "TIM4_IRQHandler": 46,
                        "USART1_IRQHandler": 53}
    for environment in ["board_selftest", "flappy_f103", "flappy_f103_compat"]:
        built = project / ".pio" / "build" / environment
        shutil.copy2(built / "firmware.elf", firmware / (environment + ".elf"))
        elf = firmware / (environment + ".elf")
        hex_file = firmware / (environment + ".hex")
        bin_file = firmware / (environment + ".bin")
        # 两种格式对齐空隙都填FF，避免默认BIN填0而HEX保留空洞的差异。
        subprocess.run([objcopy, "-O", "binary", "--gap-fill", "0xFF", str(elf), str(bin_file)], check=True)
        subprocess.run([objcopy, "-O", "ihex", "--gap-fill", "0xFF", str(elf), str(hex_file)], check=True)
        binary = (firmware / (environment + ".bin")).read_bytes()
        initial_stack, reset_vector = struct.unpack_from("<II", binary)
        if not 0x20000000 < initial_stack <= 0x20005000 or initial_stack % 8:
            raise ValueError("Initial stack is outside the target SRAM")
        if not 0x08000000 <= reset_vector < 0x08010000 or not reset_vector & 1:
            raise ValueError("Invalid Cortex-M3 reset vector")
        sizes = subprocess.check_output([size_tool, str(elf)], text=True).splitlines()[1].split()
        text_bytes, data_bytes, bss_bytes = map(int, sizes[:3])
        # 按实际地址统计RAM，避免把位于Flash的初始化表也误算进RAM。
        static_ram_bytes = 0
        reserved_ram_bytes = 0
        sections = subprocess.check_output([size_tool, "-A", str(elf)], text=True)
        for section_line in sections.splitlines():
            fields = section_line.split()
            if len(fields) != 3 or not fields[0].startswith("."):
                continue
            section_size = int(fields[1])
            section_address = int(fields[2])
            if 0x20000000 <= section_address < 0x20005000:
                reserved_ram_bytes += section_size
                if fields[0] != "._user_heap_stack":
                    static_ram_bytes += section_size
        if text_bytes + data_bytes > 65536 or reserved_ram_bytes > 20480 or len(binary) > 65536:
            raise ValueError("Firmware exceeds the target memory")
        nm_output = subprocess.check_output([nm_tool, "--defined-only", str(elf)], text=True)
        symbols = {}
        for line in nm_output.splitlines():
            fields = line.split()
            if len(fields) == 3:
                symbols[fields[2]] = int(fields[0], 16)
        for symbol, vector_index in required_vectors.items():
            actual = struct.unpack_from("<I", binary, vector_index * 4)[0]
            if actual != symbols[symbol] | 1:
                raise ValueError("Unexpected vector for " + symbol)
        if "oled_gpio_spi_write" not in symbols or "HAL_SPI_Transmit" in symbols:
            raise ValueError("Release must use the GPIO OLED transport")
        verify_hex(hex_file, binary)
        verification["environments"][environment] = {
            "flash_used": text_bytes + data_bytes, "static_ram_used": static_ram_bytes,
            "ram_used_including_linker_reserve": reserved_ram_bytes,
            "bin_size": len(binary), "initial_stack": hex(initial_stack),
            "reset_vector": hex(reset_vector), "irq_vectors_match": True, "hex_matches_bin": True,
            "display_transport": "GPIO SPI"}
    for directory in ["src", "include", "tests", "tools", "docs", "third_party"]:
        shutil.copytree(project / directory, source / directory, dirs_exist_ok=True)
    for filename in ["platformio.ini", "README.md", ".gitignore"]:
        shutil.copy2(project / filename, source / filename)
    shutil.copy2(project / "README.md", release / "README.md")
    shutil.copytree(project / "docs", release / "docs", dirs_exist_ok=True)
    shutil.copytree(project / "third_party", release / "third_party", dirs_exist_ok=True)
    for package in ["toolchain-gccarmnoneeabi", "framework-stm32cubef1", "tool-ldscripts-ststm32"]:
        package_metadata = json.loads((packages / package / "package.json").read_text())
        verification["dependencies"][package] = package_metadata["version"]
    (release / "verification.json").write_text(json.dumps(verification, ensure_ascii=False, indent=2) + "\n")
    entries = {}
    for path in sorted(release.rglob("*")):
        if path.is_file() and path.name != "manifest.json":
            entries[str(path.relative_to(release))] = {"bytes": path.stat().st_size, "sha256": sha256(path)}
    (release / "manifest.json").write_text(json.dumps(entries, ensure_ascii=False, indent=2) + "\n")
    archive = release.parent / (release.name + ".zip")
    with zipfile.ZipFile(archive, "w", zipfile.ZIP_DEFLATED) as output:
        for path in sorted(release.rglob("*")):
            if path.is_file():
                output.write(path, str(path.relative_to(release)))
    with zipfile.ZipFile(archive) as output:
        if output.testzip() is not None:
            raise ValueError("ZIP CRC verification failed")
        for filename, details in entries.items():
            if hashlib.sha256(output.read(filename)).hexdigest() != details["sha256"]:
                raise ValueError("ZIP content hash differs: " + filename)
    archive.with_suffix(".zip.sha256").write_text(sha256(archive) + "  " + archive.name + "\n")
    print(json.dumps(verification, ensure_ascii=False, indent=2))
    print("Verified release:", archive)


if __name__ == "__main__":
    main()
