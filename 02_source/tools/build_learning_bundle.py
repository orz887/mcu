"""组装离线教学包并逐文件校验；不连接硬件、不执行历史EDA脚本。

用Python 3.12运行，需要Python-Markdown。读取已由package_release.py验证的正式固件。
历史原件保留目录名，以维持CAD脚本的相对依赖；老师应先读硬件勘误。
"""
import hashlib
import html
import json
import os
from pathlib import Path
import re
import shutil
import stat
import tarfile
from urllib.parse import unquote, urlsplit

import markdown

VERSION = "1.0.0"
DATE = "20260923"
PROJECT = Path(__file__).resolve().parents[1]
OUTPUTS = PROJECT.parent / "outputs"
RELEASE = OUTPUTS / f"Flappy_Bird_Firmware_v{VERSION}_{DATE}"
BUNDLE = OUTPUTS / f"Flappy_Bird_Learning_Kit_v{VERSION}_{DATE}"
PCB = OUTPUTS / "flappy-bird-pcb-repair-20260918"
CASE = OUTPUTS / "flappy-bird-enclosure-sync-20260918"

STYLE = """
*{box-sizing:border-box}html{scroll-behavior:smooth}body{margin:0;background:#f7f6f2;color:#243331;
font:17px/1.85 system-ui,'Microsoft YaHei','Noto Sans CJK SC',sans-serif}a{color:#14645c;text-decoration-thickness:1px;text-underline-offset:3px}
header{padding:20px 5vw;border-bottom:1px solid #d9dfd9;background:#fff;display:flex;gap:28px;flex-wrap:wrap;font-size:14px}
header strong{letter-spacing:.04em}main{display:grid;grid-template-columns:250px minmax(0,930px);gap:44px;max-width:1290px;margin:40px auto;padding:0 24px}
aside{font-size:13px;position:sticky;top:20px;align-self:start;max-height:92vh;overflow:auto;padding-right:14px}aside ul{list-style:none;padding-left:13px}aside li{margin:7px 0}
article{min-width:0;padding:28px 42px 56px;background:white;border:1px solid #e1e5df;border-radius:8px}h1{font-size:32px;line-height:1.35;margin-top:0}h2{font-size:24px;margin-top:44px;padding-top:14px;border-top:1px solid #e0e6df}h3{font-size:20px;margin-top:30px}
p,ul,ol{margin:15px 0}li{margin:6px 0}code{font:14px/1.7 Consolas,monospace;background:#edf2ef;padding:2px 5px;border-radius:3px}pre{background:#eef2ef;padding:18px;overflow:auto;border-left:3px solid #459283}pre code{padding:0;background:transparent}
table{width:100%;border-collapse:collapse;font-size:15px;margin:22px 0;display:block;overflow:auto}th,td{border:1px solid #dce4de;padding:9px 13px;vertical-align:top}th{background:#edf3ef;text-align:left}blockquote{margin:24px 0;padding:14px 20px;border-left:4px solid #b9872e;background:#fff7e7}blockquote p{margin:0}
img{max-width:100%;height:auto;border-radius:4px}summary{font-weight:700;cursor:pointer}footer{color:#6b7770;font-size:12px;margin-top:36px}h2,h3{scroll-margin-top:20px}
@media(max-width:1000px){main{display:block;margin:20px auto}aside{position:static;max-height:300px;margin-bottom:20px}article{padding:24px}h1{font-size:27px}}
@media print{header,aside{display:none}main{display:block;margin:0;padding:0}article{border:0;padding:0}body{background:white;font-size:11pt}a{color:inherit}h2,h3{break-after:avoid}pre,blockquote,tr{break-inside:avoid}}
"""


def digest(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def copy_file(source, target):
    # Windows资料可能带只读位。只调整生成副本，原件内容与权限不变。
    target = Path(target)
    if target.exists():
        target.chmod(target.stat().st_mode | stat.S_IWUSR)
    shutil.copy2(source, target)
    target.chmod(target.stat().st_mode | stat.S_IWUSR)
    return str(target)


def copy_item(source, target):
    if not source.exists():
        raise FileNotFoundError(source)
    target.parent.mkdir(parents=True, exist_ok=True)
    if source.is_dir():
        if any(path.is_symlink() for path in source.rglob("*")):
            raise ValueError(f"Unexpected symbolic link in {source}")
        shutil.copytree(source, target, dirs_exist_ok=True, copy_function=copy_file)
    else:
        copy_file(source, target)


def write(relative, text):
    path = BUNDLE / relative
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(text.strip() + "\n", encoding="utf-8")


def render_document(path):
    converter = markdown.Markdown(extensions=["tables", "fenced_code", "toc", "sane_lists"],
                                  extension_configs={"toc": {"toc_depth": "2-3"}})
    body = converter.convert(path.read_text(encoding="utf-8"))
    # 只把存在对应网页的本地Markdown链接改成HTML，源码/模型等文件链接保持原样。
    def link(match):
        value = html.unescape(match.group(1))
        parts = urlsplit(value)
        if not parts.scheme and not parts.netloc and parts.path.endswith(".md"):
            target = (path.parent / unquote(parts.path)).resolve()
            if target in document_set:
                value = parts.path[:-3] + ".html" + ("#" + parts.fragment if parts.fragment else "")
        return 'href="' + html.escape(value, quote=True) + '"'
    body = re.sub(r'href="([^"]+)"', link, body)
    title_match = re.search(r"<h1[^>]*>(.*?)</h1>", body, re.S)
    title = re.sub("<[^>]+>", "", title_match.group(1)) if title_match else path.stem
    home = os.path.relpath(BUNDLE / "index.html", path.parent).replace(os.sep, "/")
    source = path.name
    output = ("<!doctype html><html lang=\"zh-CN\"><meta charset=\"utf-8\">"
              '<meta name="viewport" content="width=device-width,initial-scale=1">'
              f"<title>{html.escape(title)}</title><style>{STYLE}</style><body>"
              f'<header><strong>声控小鸟 · 嵌入式入门 / v{VERSION}</strong><a href="{home}">学习包首页</a>'
              f'<a href="{source}">Markdown原文</a><span>离线可读 · 中文源码 · 实验记录</span></header>'
              f'<main><aside><details open><summary>本页目录</summary>{converter.toc}</details></aside>'
              f'<article>{body}<footer>教学正式版 {VERSION} · 2026-09-23。硬件生产前先核对勘误。</footer>'
              '</article></main></body></html>')
    path.with_suffix(".html").write_text(output, encoding="utf-8")


def check_local_links(paths):
    failures = []
    for path in paths:
        for value in re.findall(r'(?:href|src)="([^"]+)"', path.read_text(encoding="utf-8")):
            parts = urlsplit(html.unescape(value))
            if parts.scheme or parts.netloc or not parts.path:
                continue
            target = (path.parent / unquote(parts.path)).resolve()
            if not target.is_relative_to(BUNDLE) or not target.exists():
                failures.append((str(path.relative_to(BUNDLE)), value))
    if failures:
        raise ValueError("Broken local links: " + json.dumps(failures, ensure_ascii=False))


def main():
    global document_set
    for name in ["COURSE_ZERO_TO_EMBEDDED.md", "HARDWARE_AS_BUILT_AND_ERRATA.md"]:
        if not (PROJECT / "docs" / name).is_file():
            raise FileNotFoundError(name)
    # 防止拿旧固件或校验失败的文件生成“正式版”。
    verified = json.loads((RELEASE / "verification.json").read_text())
    if verified["version"] != VERSION or not verified["game_sound_default"]:
        raise ValueError("Expected formal release with default sound enabled")
    for relative, expected in json.loads((RELEASE / "manifest.json").read_text()).items():
        if digest(RELEASE / relative) != expected["sha256"]:
            raise ValueError("Release manifest mismatch: " + relative)
    copy_item(RELEASE / "firmware", BUNDLE / "01_firmware")
    copy_item(RELEASE / "verification.json", BUNDLE / "01_firmware/verification.json")
    copy_item(RELEASE / "source", BUNDLE / "02_source")
    for name in ["Flappy_Bird_REVIEWED_20260918.epro2", "Flappy_Bird_PCB1_20260918_Gerber.zip",
                 "Flappy_Bird_BOM_20260918.csv", "Flappy_Bird_PCB1_CPL_20260918.csv",
                 "render_final_board.png", "render_final_mcu.png", "render_final_power.png"]:
        copy_item(PCB / name, BUNDLE / "03_hardware/as_built" / name)
    for name in ["print", "step", "assembly", "Flappy_Bird_V4_CurrentPCB_20260918.epro2",
                 "v4_synced_source.epcb", "validation.json", "release_validation.json"]:
        copy_item(CASE / name, BUNDLE / "04_enclosure/V4" / name)
    # 原始工作档保留相邻目录关系，CAD脚本读取的历史V3依赖也一起归档。
    for source in sorted(OUTPUTS.glob("flappy-bird-*")):
        if source.is_dir():
            copy_item(source, BUNDLE / "06_history/original_workspace" / source.name)
    for source in sorted(OUTPUTS.glob("Flappy_Bird_*.zip")):
        if source.name.startswith(("Flappy_Bird_Firmware_v1.", "Flappy_Bird_Learning_Kit_")):
            continue
        copy_item(source, BUNDLE / "06_history/past_deliveries" / source.name)

    references = {
        "SSD1309_Datasheet.pdf": Path("/tmp/waveshare-SSD1309Datasheet.pdf"),
        "Waveshare_2in42_schematic.pdf": Path("/tmp/waveshare-2in42-schematic.pdf"),
        "Waveshare_2in42_dimensions.pdf": Path("/tmp/waveshare-2in42-sku25742.pdf"),
        "Waveshare_original_OLED_examples.7z": Path("/tmp/waveshare-OLED_Module_Code.7z"),
        "TS1187_switch_C318884.pdf": Path("/tmp/sw5_xkb_ts1187a.pdf"),
        "USB_TypeC_6pin.pdf": OUTPUTS / "flappy-bird-pcb-update-review/XY-TYPEC-6PIN.pdf",
    }
    for name in ["AP2114.pdf", "LMV358_TECH_PUBLIC.pdf", "GSC1030RB_INGHAI.pdf"]:
        references[name] = OUTPUTS / "flappy-bird-hardware-review-20260917" / name
    for name, source in references.items():
        copy_item(source, BUNDLE / "07_references" / name)
    # 离线查阅本项目实际用到的HAL和CMSIS源码；构建仍由PlatformIO按固定版本管理。
    framework = Path.home() / ".platformio/packages/framework-stm32cubef1"
    for relative in ["Drivers/STM32F1xx_HAL_Driver", "Drivers/CMSIS/Include",
                     "Drivers/CMSIS/Device/ST/STM32F1xx"]:
        copy_item(framework / relative, BUNDLE / "07_references/STM32CubeF1_1.8.7" / relative)
    for relative in ["package.json", "License.md", "Release_Notes.html"]:
        if (framework / relative).is_file():
            copy_item(framework / relative, BUNDLE / "07_references/STM32CubeF1_1.8.7" / relative)
    copy_item(PROJECT / "third_party", BUNDLE / "07_references/third_party_licenses")
    exam = Path("/mnt/d/applications/wechat/chat_documents/xwechat_files/wxid_o0mezwwo8fx422_453a/msg/file/2026-09/C题-HelloMCU.pdf")
    if exam.is_file():
        copy_item(exam, BUNDLE / "07_references/HelloMCU_original_task.pdf")
    # 老师和学生的起始页面由同一套可编辑Markdown模板生成。
    for name, target in {"LEARNING_KIT_START.md": "README.md",
                         "LEARNING_KIT_FIRMWARE.md": "01_firmware/README.md",
                         "LEARNING_KIT_HARDWARE.md": "03_hardware/README.md",
                         "LEARNING_KIT_ENCLOSURE.md": "04_enclosure/README.md",
                         "LEARNING_KIT_LEARNING.md": "05_learning/README.md",
                         "LEARNING_KIT_HISTORY.md": "06_history/README.md",
                         "LEARNING_KIT_REFERENCES.md": "07_references/README.md"}.items():
        copy_item(PROJECT / "tools/bundle_pages" / name, BUNDLE / target)
    copy_item(PROJECT / "tools/bundle_pages/project_flow.svg", BUNDLE / "05_learning/project_flow.svg")

    document_set = set((BUNDLE / "02_source/docs").rglob("*.md"))
    document_set.add(BUNDLE / "02_source/README.md")
    document_set.update(BUNDLE.glob("*/README.md"))
    document_set.add(BUNDLE / "04_enclosure/V4/assembly/ASSEMBLY_GUIDE.md")
    document_set.add(BUNDLE / "README.md")
    document_set = {path.resolve() for path in document_set}
    for path in sorted(document_set):
        render_document(path)
    shutil.copy2(BUNDLE / "README.html", BUNDLE / "index.html")
    html_paths = [path.with_suffix(".html") for path in document_set] + [BUNDLE / "index.html"]
    check_local_links(html_paths)

    provenance = {"version": VERSION, "date": DATE, "source_project": str(PROJECT),
                  "firmware_release": str(RELEASE), "references": {name: str(path) for name, path in references.items()},
                  "hardware_status": "As-built records; known switch wiring defect not revised",
                  "game_status": "Normal v0.1.1 reported working by user; v1.0.0 sound needs physical check",
                  "offline_build": "Toolchain installers/caches are not bundled; first PlatformIO build needs network"}
    write("BUNDLE_BUILD.json", json.dumps(provenance, ensure_ascii=False, indent=2))
    write("VALIDATION.json", json.dumps({"rendered_html_pages": len(html_paths), "local_links_checked": True,
          "firmware_manifest_checked": True, "test_record": "02_source/docs/VERIFICATION.md", "full_hardware_verified": False}, indent=2))
    files = [path for path in sorted(BUNDLE.rglob("*")) if path.is_file() and path not in [BUNDLE / "BUNDLE_MANIFEST.json", BUNDLE / "SHA256SUMS"]]
    if any(path.is_symlink() for path in BUNDLE.rglob("*")):
        raise ValueError("Symlinks are not supported in the portable package")
    manifest = {str(path.relative_to(BUNDLE)): {"bytes": path.stat().st_size, "sha256": digest(path)} for path in files}
    write("BUNDLE_MANIFEST.json", json.dumps(manifest, ensure_ascii=False, indent=2))
    write("SHA256SUMS", "\n".join(value["sha256"] + "  " + name for name, value in manifest.items()))
    archive = OUTPUTS / (BUNDLE.name + ".tar.gz")
    with tarfile.open(archive, "w:gz", compresslevel=6) as output:
        output.add(BUNDLE, arcname=BUNDLE.name, recursive=True)
    with tarfile.open(archive, "r:gz") as output:
        members = output.getmembers()
        for item in members:
            if item.name.startswith("/") or ".." in Path(item.name).parts or not (item.isfile() or item.isdir()):
                raise ValueError("Unsafe archive member: " + item.name)
        for name, expected in manifest.items():
            content = output.extractfile(BUNDLE.name + "/" + name).read()
            if len(content) != expected["bytes"] or hashlib.sha256(content).hexdigest() != expected["sha256"]:
                raise ValueError("Archive verification failed: " + name)
    archive.with_suffix(".gz.sha256").write_text(digest(archive) + "  " + archive.name + "\n")
    print(json.dumps({"archive": str(archive), "bytes": archive.stat().st_size,
                      "file_count": len(manifest), "html_pages": len(html_paths), "verified": True}, ensure_ascii=False, indent=2))


if __name__ == "__main__":
    main()
