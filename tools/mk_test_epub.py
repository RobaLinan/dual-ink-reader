#!/usr/bin/env python3
# P4 验收用测试 EPUB 生成器：造一本多章 EPUB，其中一章解压后 >280KB
# （DESIGN §4 P4 验收线：EPUB 读完一章不崩不 OOM，>280KB 章节流式）。
# 内容混入：中文段落（CJK 排版路径）、英文按词断行、HTML 实体（&nbsp; &ldquo; 等）、
# 弯引号/长横线、<br/>、嵌套行内标签、<style>/<script>（应被整棵跳过）。
# 用法：python3 tools/mk_test_epub.py → assets/books/test.epub
import os, zipfile, textwrap, struct, zlib

def test_png():
    """真 PNG（纯标准库造）：400×240 竖向渐变 + 两个圆，验证解码 + 抖动梯度。
    每张扫描线 = filter 字节 0 + RGB888 像素；zlib 压缩后手工组 IHDR/IDAT/IEND。"""
    W, H = 400, 240
    def chunk(typ, data):
        c = struct.pack(">I", len(data)) + typ + data
        return c + struct.pack(">I", zlib.crc32(typ + data) & 0xFFFFFFFF)
    raw = bytearray()
    for y in range(H):
        raw.append(0)
        for x in range(W):
            base = 255 - (y * 255 // H)                    # 上白下黑渐变
            d1 = (x - 120) ** 2 + (y - 120) ** 2
            d2 = (x - 280) ** 2 + (y - 120) ** 2
            if d1 < 60 ** 2 or d2 < 60 ** 2: base = 40     # 两个深圆（硬边考缩放）
            raw += bytes((base, base, base))
    ihdr = struct.pack(">IIBBBBB", W, H, 8, 2, 0, 0, 0)     # 8bit RGB
    return (b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", ihdr) +
            chunk(b"IDAT", zlib.compress(bytes(raw), 9)) + chunk(b"IEND", b""))

OUT = os.path.join(os.path.dirname(__file__), "..", "assets", "books", "test.epub")

PARA_CN = (
    "墨水瓶上的灰落了一层，他仍然没有动笔。窗外的雨声时断时续，像是有人在很远的地方"
    "翻着一本很厚的书。&ldquo;也许该先写结尾。&rdquo;&nbsp;他想。可是结尾藏在开头里，"
    "而开头已经改了十一遍。桌上的灯芯短了一截，光晕缩在稿纸中央，像一口干涸的井。"
    "他忽然记起很多年前的一个下午&mdash;&mdash;母亲把晒好的被子拍松，尘埃在光柱里"
    "缓慢地翻滚，仿佛时间本身也是某种可以掸去的灰。"
)
PARA_EN = (
    "The quick brown fox jumps over the lazy dog. Stream-of-consciousness narration "
    "rewards patient readers: sentences wander, double back, and occasionally find the "
    "door they came in by. &nbsp;Hyphenated words&mdash;and em-dashes&mdash;must not "
    "break the line layout, nor should &ldquo;quoted phrases&rdquo; misbehave at "
    "line boundaries when justification is tight."
)

def chapter_html(title, n_para, body_ratio=(2, 1)):
    parts = [f"""<?xml version="1.0" encoding="utf-8"?>
<!DOCTYPE html PUBLIC "-//W3C//DTD XHTML 1.1//EN" "http://www.w3.org/TR/xhtml11/DTD/xhtml11.dtd">
<html xmlns="http://www.w3.org/1999/xhtml"><head>
<title>{title}</title>
<style type="text/css">p {{ text-indent: 2em; }} .hidden {{ display: none; }}</style>
<script type="text/javascript">/* 这段必须被抽文本跳过 */ var junk = 1;</script>
</head><body>
<h1>{title}</h1>
"""]
    for i in range(n_para):
        if i % (body_ratio[0] + body_ratio[1]) < body_ratio[0]:
            parts.append(f"<p>{PARA_CN}（第{i}段）</p>\n")
        else:
            parts.append(f"<p>{PARA_EN} (para {i})</p>\n")
        if i % 17 == 16:
            parts.append("<p>第一行<br/>第二行<br/>第三行</p>\n")
        if i % 23 == 22:
            parts.append("<blockquote><p>引文：<em>嵌套行内标签</em>里的字也要抽出来。</p></blockquote>\n")
        if i == 7:   # 插一张真图：src 用 ../ 相对路径（考 emitImageMarker 的路径折叠）
            parts.append('<div class="fig"><img src="../images/pic.png" alt="灰阶测试图"/></div>\n')
    parts.append("</body></html>")
    return "".join(parts)

def build():
    # ch1 短章（快速验证翻章），ch2 大章 >280KB（验收线），ch3 中章收尾
    ch1 = chapter_html("第一章 雨夜", 30)
    ch3 = chapter_html("第三章 结尾或开头", 120)
    ch2 = chapter_html("第二章 灰尘与光", 900)   # 约 300KB+ 解压后
    assert len(ch2.encode("utf-8")) > 280 * 1024, len(ch2.encode("utf-8"))

    opf = """<?xml version="1.0" encoding="utf-8"?>
<package xmlns="http://www.idpf.org/2007/opf" unique-identifier="id" version="2.0">
<metadata xmlns:dc="http://purl.org/dc/elements/1.1/">
  <dc:title>灰尘与光</dc:title>
  <dc:language>zh-CN</dc:language>
  <dc:identifier id="id">eink-test-epub-001</dc:identifier>
</metadata>
<manifest>
  <item id="ncx" href="toc.ncx" media-type="application/x-dtbncx+xml"/>
  <item id="ch1" href="text/ch1.xhtml" media-type="application/xhtml+xml"/>
  <item id="ch2" href="text/ch2.xhtml" media-type="application/xhtml+xml"/>
  <item id="ch3" href="text/ch3.xhtml" media-type="application/xhtml+xml"/>
  <item id="img" href="images/pic.png" media-type="image/png"/>
</manifest>
<spine toc="ncx">
  <itemref idref="ch1"/>
  <itemref idref="ch2"/>
  <itemref idref="ch3"/>
</spine>
</package>
"""
    container = """<?xml version="1.0" encoding="utf-8"?>
<container version="1.0" xmlns="urn:oasis:names:tc:opendocument:xmlns:container">
  <rootfiles><rootfile full-path="OEBPS/content.opf" media-type="application/oebps-package+xml"/></rootfiles>
</container>
"""
    os.makedirs(os.path.dirname(OUT), exist_ok=True)
    with zipfile.ZipFile(OUT, "w") as z:
        # mimetype 必须第一个且不压缩（OEPPS 规范）
        z.writestr(zipfile.ZipInfo("mimetype"), "application/epub+zip",
                   compress_type=zipfile.ZIP_STORED)
        z.writestr("META-INF/container.xml", container, compress_type=zipfile.ZIP_DEFLATED)
        z.writestr("OEBPS/content.opf", opf, compress_type=zipfile.ZIP_DEFLATED)
        z.writestr("OEBPS/toc.ncx", "<ncx xmlns='http://www.daisy.org/z3986/2005/ncx/'/>",
                   compress_type=zipfile.ZIP_DEFLATED)
        z.writestr("OEBPS/images/pic.png", test_png(),   # 真 PNG：解码+抖动验证素材
                   compress_type=zipfile.ZIP_DEFLATED)
        z.writestr("OEBPS/text/ch1.xhtml", ch1, compress_type=zipfile.ZIP_DEFLATED)
        z.writestr("OEBPS/text/ch2.xhtml", ch2, compress_type=zipfile.ZIP_DEFLATED)
        z.writestr("OEBPS/text/ch3.xhtml", ch3, compress_type=zipfile.ZIP_DEFLATED)

    print(f"written: {os.path.abspath(OUT)}")
    with zipfile.ZipFile(OUT) as z:
        for info in z.infolist():
            print(f"  {info.filename:36s} comp={info.compress_size:7d} uncomp={info.file_size:7d}")

if __name__ == "__main__":
    build()
