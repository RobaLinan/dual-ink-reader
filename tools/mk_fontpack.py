#!/usr/bin/env python3
"""TTF/OTF → EPF1 字库包（字体自定义：离线光栅化，设备端只贴点阵）。

DESIGN §5.5 扩展：任意开源字体（衬线/黑体/楷体…）离线转成 SD 字库包，
设备启动扫 /fonts/font-<name>-<px>.bin 自动发现家族，串口/按键切换。

包格式（little-endian，32B 头 + u32[257] 索引 + 紧凑变长记录）：
  头:
    0   char[4]  magic "EPF1"
    4   u8 ver=1
    5   u8 sizePx       光栅化字号（16/24/32，与 TxtBook 档位对应）
    6   u8 baselineY    基线在版面中的行号 = 全字形最大 ink 上伸
    7   u8 reserved
    8   u32 nGlyph
   12   u8 maxDescent   最大 ink 下伸（行盒/行距计算用）
   13   u8 maxAdv       最大步进（软行宽预算用）
   14   u16 reserved
   16   u32 indexOff    = 32
   20   u32 entriesOff  = 32 + 257*4 = 1060
   24   char[8] name    家族名（NUL 填充，调试用；真源是文件名）
  索引: u32[257]，index[b] = 首个 (cp>>8) >= b 的记录相对 entries 的字节偏移，
        index[256] = entries 总长。查表走一个桶 ≤256 条线性扫。
  记录（按 cp 升序，变长）:
    u16 cp | u8 adv | i8 xoff | i8 yoff | u8 w | u8 h | bits[ceil(w/8)*h]
    bits 行优先，MSB = 最左；ink 左上角相对 (pen, baseline) = (xoff, yoff)；
    w=h=0 = 无墨字形（空格等，只占 adv）。

用法: mk_fontpack.py <font.ttf> <outdir> --name kai [--sizes 16 24 32]
依赖: Pillow >= 9.2
"""
import argparse
import os
import struct
import sys

from PIL import Image, ImageDraw, ImageFont
from fontTools.ttLib import TTFont

MAGIC = b"EPF1"
INDEX_N = 257
INDEX_OFF = 32
ENTRIES_OFF = INDEX_OFF + INDEX_N * 4      # 1060
MAX_BUCKETS = 256


def cps_of(font_path):
    """BMP 且可渲染的码点（跳过 C0/C1 控制符与代理区）。cmap 用 fontTools 读。"""
    tt = TTFont(font_path, fontNumber=0, lazy=True)
    seen = set()
    for table in tt["cmap"].tables:
        seen.update(table.cmap.keys())
    tt.close()
    out = []
    for cp in seen:
        if cp < 0x20 or cp == 0x7F or 0x80 <= cp < 0xA0:
            continue
        if 0xD800 <= cp <= 0xDFFF or cp > 0xFFFF:
            continue
        out.append(cp)
    return sorted(out)


def convert(font_path, out_dir, name, sizes):
    for size in sizes:
        _convert_one(font_path, out_dir, name, size)


def _convert_one(font_path, out_dir, name, size):
    font = ImageFont.truetype(font_path, size)   # 可变字体取默认实例(Regular)

    # 第一遍：度量（ink 盒 + 步进）→ 全局基线 / 行盒 / 最大步进。
    # 行盒度量封顶：竖排假名(U+3031/2 等)ink 远超字号但正常排版不会出现，
    # 参与计算会把行距撑爆；字形照常存，只是画出线盒时可能略微出格（可接受）
    ascent = descent = max_adv = 0
    cap_a = -(-size * 5 // 4)        # ceil(size*1.25)
    cap_d = -(-size * 2 // 5)        # ceil(size*0.4)
    glyphs = []          # (cp, adv, x0, y0, x1, y1)  ink 相对基线
    for cp in cps_of(font_path):
        ch = chr(cp)
        adv = round(font.getlength(ch))
        if adv <= 0:
            adv = 1
        if adv > 255:
            adv = 255
        x0, y0, x1, y1 = font.getbbox(ch, anchor="ls")
        if x1 <= x0 or y1 <= y0:            # 无墨（空格等）
            x0 = y0 = x1 = y1 = 0
        if -y0 <= cap_a:
            ascent = max(ascent, -y0)
        if y1 <= cap_d:
            descent = max(descent, y1)
        max_adv = max(max_adv, adv)
        glyphs.append((cp, adv, x0, y0, x1, y1))

    if ascent > 127 or descent > 127:
        sys.exit(f"FATAL: ink 超出 i8 范围 (ascent={ascent} descent={descent})")

    # 第二遍：逐字形渲染 → 1-bit 紧凑 bits
    entries = bytearray()
    first_off = {}
    for cp, adv, x0, y0, x1, y1 in glyphs:
        b = cp >> 8
        if b not in first_off:
            first_off[b] = len(entries)
        if x1 <= x0 or y1 <= y0:
            w = h = 0
            bits = b""
        else:
            w, h = x1 - x0, y1 - y0
            if w > 255 or h > 255:
                sys.exit(f"FATAL: U+{cp:04X} ink 过大 {w}x{h}")
            g = Image.new("L", (w, h), 0)
            ImageDraw.Draw(g).text((-x0, -y0), chr(cp), font=font,
                                   fill=255, anchor="ls")
            # 阈值 96：Noto 细横笔灰度仅 ~108，128 会砍掉；AA 晕圈 ≤49 安全
            g1 = g.point(lambda v: 255 if v >= 96 else 0).convert("1")
            bits = g1.tobytes()
            rb = (w + 7) // 8
            if len(bits) != rb * h:
                sys.exit(f"FATAL: U+{cp:04X} bits 长度 {len(bits)} != {rb*h}")
        entries += struct.pack("<HBbb", cp, adv, x0, y0)    # u16 cp, u8 adv, i8 xoff, i8 yoff
        entries += struct.pack("<BB", w, h)
        entries += bits

    # 索引：bucket b 的首记录偏移（缺失桶 = 下一个有内容的桶）
    index = [0] * INDEX_N
    index[INDEX_N - 1] = len(entries)
    for b in range(MAX_BUCKETS - 1, -1, -1):
        index[b] = first_off[b] if b in first_off else index[b + 1]

    name8 = name.encode()[:8].ljust(8, b"\0")
    header = MAGIC + struct.pack(
        "<BBBBIBBHII8s",
        1, size, ascent, 0,          # ver, sizePx, baselineY, reserved
        len(glyphs),                  # nGlyph
        descent, max_adv, 0,          # maxDescent, maxAdv, reserved
        INDEX_OFF, ENTRIES_OFF, name8,
    )
    assert len(header) == INDEX_OFF

    os.makedirs(out_dir, exist_ok=True)
    out_path = os.path.join(out_dir, f"font-{name}-{size}.bin")
    with open(out_path, "wb") as f:
        f.write(header)
        f.write(struct.pack(f"<{INDEX_N}I", *index))
        f.write(entries)

    size_bytes = os.path.getsize(out_path)
    zh = any(cp == 0x4E2D for cp, *_ in glyphs)
    print(f"{out_path}: {len(glyphs)} glyphs, ascent={ascent} descent={descent} "
          f"maxAdv={max_adv}, {size_bytes} bytes, U+4E2D={'yes' if zh else 'NO!'}")


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("font", help="TTF/OTF 文件路径")
    ap.add_argument("outdir", help="输出目录（包文件 font-<name>-<px>.bin）")
    ap.add_argument("--name", required=True, help="家族名（serif/sans/kai…，≤8 字符）")
    ap.add_argument("--sizes", nargs="+", type=int, default=[16, 24, 32])
    args = ap.parse_args()
    if not (1 <= len(args.name) <= 8):
        sys.exit("FATAL: --name 需 1-8 字符")
    convert(args.font, args.outdir, args.name, args.sizes)


if __name__ == "__main__":
    main()
