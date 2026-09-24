#!/usr/bin/env python3
"""Unifont hex → 平坦字库 bin（DESIGN §5.5：CJK 字库存 SD 流式查表）。

输出格式（assets/fonts/unifont.bin）：
  固定 65536 × 32 = 2097152 字节，码点 cp 的字形 = offset cp*32。
  每字形 16 行 × 16 像素，行优先，每行 2 字节，MSB = 最左像素。
  零开销查表：不需要索引表，seek(cp*32) 即可（SD 单次 32B 读）。

只覆盖 BMP（U+0000–U+FFFF）：中英混排 + 全部 CJK 扩展 A 都在 BMP 内；
  辅助平面（扩展 B+，key 6 位）跳过 —— 生僻字在此工程里可接受。
hex 里存在但数据全 0 的码点（如 U+00A0）保留为全 0 = 空白字形；
hex 里缺失的码点写入 16×16 空心方框 = 屏上可见的"缺字"标记（区别于真空白）。

用法：mk_unifont.py <unifont_all.hex> <out.bin>
"""
import sys

GLYPH_BYTES = 32          # 16 行 × 2 字节
NCP = 65536               # BMP 全覆盖
TOFU = bytes([            # 16×16 空心方框：缺字标记
    0x00, 0x00,
    0x7F, 0xFE, 0x80, 0x01, 0x80, 0x01, 0x80, 0x01, 0x80, 0x01,
    0x80, 0x01, 0x80, 0x01, 0x80, 0x01, 0x80, 0x01, 0x80, 0x01,
    0x80, 0x01, 0x80, 0x01, 0x80, 0x01, 0x7F, 0xFE,
    0x00, 0x00,
])
assert len(TOFU) == GLYPH_BYTES


def main() -> int:
    if len(sys.argv) != 3:
        print(__doc__)
        return 2
    hex_path, out_path = sys.argv[1], sys.argv[2]

    # 缺字 → 方框；hex 覆盖到的 → 原样/空白
    buf = bytearray(TOFU * NCP)
    covered = 0
    halfwidth = 0
    skipped_plane = 0

    with open(hex_path, encoding="ascii", errors="replace") as f:
        for line in f:
            line = line.strip()
            if not line or ":" not in line:
                continue
            key, data = line.split(":", 1)
            if len(key) == 6:          # 辅助平面（U+010000+），BMP 方案不覆盖
                skipped_plane += 1
                continue
            cp = int(key, 16)
            raw = bytes.fromhex(data)
            if len(raw) == GLYPH_BYTES:
                glyph = raw                       # 16×16 全宽
            elif len(raw) == GLYPH_BYTES // 2:
                # 8×16 半宽（Unifont 一半的拉丁/符号）→ 左对齐放进 16 列，
                # 右 8 列留白；渲染按 16px 步进，视觉 = 左半格字形
                glyph = b"".join(raw[i:i + 1] + b"\x00" for i in range(0, len(raw), 1))
                halfwidth += 1
            else:
                print(f"WARN: cp U+{cp:04X} 跳过（异常长度 {len(raw)}）")
                continue
            buf[cp * GLYPH_BYTES:(cp + 1) * GLYPH_BYTES] = glyph
            covered += 1

    with open(out_path, "wb") as f:
        f.write(buf)

    print(f"out={out_path} size={len(buf)} bytes")
    print(f"covered={covered} halfwidth_padded={halfwidth} plane1+_skipped={skipped_plane}")
    # 抽查：中（U+4E2D）不能是方框
    zh = buf[0x4E2D * GLYPH_BYTES:(0x4E2D + 1) * GLYPH_BYTES]
    assert zh != TOFU and zh != bytes(GLYPH_BYTES), "U+4E2D 未覆盖或为空白"
    return 0


if __name__ == "__main__":
    sys.exit(main())
