"""把C测试导出的PGM帧缓存拼成文档预览，不生成或模拟额外界面。"""

from pathlib import Path
from PIL import Image, ImageDraw, ImageFont


project = Path(__file__).resolve().parents[1]
views = [("ready", "Ready"), ("playing", "Playing"), ("game_over", "Game over"),
         ("diagnostic", "Board diagnostics"), ("tuner", "Guitar tuner")]
canvas = Image.new("RGB", (848, 530), "#edf0f4")
draw = ImageDraw.Draw(canvas)
font = ImageFont.load_default(size=19)
for index, (filename, title) in enumerate(views):
    origin_x = 20 + index % 2 * 420
    origin_y = 12 + index // 2 * 174
    draw.text((origin_x, origin_y), title, fill="#172b42", font=font)
    framebuffer = Image.open(project / "build" / (filename + ".pgm"))
    framebuffer = framebuffer.resize((256, 128), Image.Resampling.NEAREST)
    canvas.paste(framebuffer, (origin_x, origin_y + 30))
canvas.save(project / "docs" / "screens.png")
print(project / "docs" / "screens.png")
