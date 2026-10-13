#!/usr/bin/env python3
# 漫画 EPUB → 对页合并（DESIGN §12）：calibre 类转换器产出的一图一章 EPUB，
# 每张扫描页是一个独立 XHTML spine 项，双屏阅读器一屏对页 = 两个面板各一页，
# 一章只有一张图时右面板只能画空白页（章尾 "\n\n" 残留页）。
# 本工具把 spine 里连续的"单图章"两两合并成一个 XHTML（同 <p> 内两张 <img>，
# 单个空格分隔）——设备端 collectText 把行内相邻图标记按空格接排，
# 两张整页图正好落满一屏左右面板。非单图章（封面/titlepage、文字章）原样保留。
#
# 设备端配套改动（2026-10-13，DESIGN §12）：
#   - EpubBook::emitImageMarker 块前分隔 nl() → sep()（行首不换行、行内接空格）
#   - TxtBook::renderPage 无内容页（s_hadContent=false）对 EPUB 返回 false
# 合并后的书在老固件上行为退化（仍是左图右空白），不损坏。
#
# 用法：python3 tools/pair_manga_epub.py <in.epub> <out.epub>
import re
import sys
import zipfile
from xml.etree import ElementTree as ET

NS = {"x": "http://www.w3.org/1999/xhtml"}


def norm_zip_path(base_dir, src):
    """与设备端 resolveZipPath 同规则：去锚点、折叠 ./ ../（src 已假设无 %XX）。"""
    if src.startswith("/"):
        p = src[1:]
    else:
        p = base_dir + src
    p = p.split("#")[0]
    out = []
    for seg in p.split("/"):
        if seg == "." or seg == "":
            continue
        if seg == "..":
            if out:
                out.pop()
            continue
        out.append(seg)
    return "/".join(out)


def img_refs(xhtml_bytes, doc_dir):
    """返回 body 内所有 img/svg image 的 zip 内路径；解析失败返回 None。"""
    try:
        root = ET.fromstring(xhtml_bytes.decode("utf-8", "ignore"))
    except ET.ParseError:
        return None
    refs = []
    for el in root.iter():
        tag = el.tag.rsplit("}", 1)[-1]
        if tag == "body":
            continue
        if tag == "img":
            src = el.get("src")
            if src:
                refs.append(norm_zip_path(doc_dir, src))
        elif tag == "image":
            src = el.get("{http://www.w3.org/1999/xlink}href") or el.get("href")
            if src:
                refs.append(norm_zip_path(doc_dir, src))
    return refs


def is_text_only(xhtml_bytes):
    """body 里除图之外还有无文本？（有则不算单图章，不合并）"""
    try:
        root = ET.fromstring(xhtml_bytes.decode("utf-8", "ignore"))
    except ET.ParseError:
        return True  # 解析失败不合并
    body = None
    for el in root.iter():
        if el.tag.rsplit("}", 1)[-1] == "body":
            body = el
            break
    if body is None:
        return True
    for el in body.iter():
        tag = el.tag.rsplit("}", 1)[-1]
        if tag in ("script", "style"):
            continue
        if el.text and el.text.strip():
            return True
        if el.tail and el.tail.strip() and tag not in ("img", "image"):
            # img 的 tail 是图间空白，不算正文
            return True
    return False


def pair_xhtml(title, refs, opf_dir):
    """合并章 XHTML：同 <p> 两张 <img>，单个空格分隔（设备端空格接排的关键）。"""
    def rel(zip_path):
        # href 相对合并章所在目录（沿用原单图章的 text/ 目录）
        base = opf_dir + "text/" if opf_dir else "text/"
        if zip_path.startswith(base):
            return zip_path[len(base):]
        # 图不在 text/ 兄弟树下时退回 ../ 前缀（常规 calibre 布局命中上面分支）
        return "../" + zip_path

    imgs = " ".join(f'<img src="{rel(r)}" class="calibre1"/>' for r in refs)
    return (
        "<?xml version='1.0' encoding='utf-8'?>\n"
        '<html xmlns="http://www.w3.org/1999/xhtml">\n'
        "  <head>\n"
        f"    <title>{title}</title>\n"
        '    <meta http-equiv="Content-Type" content="text/html; charset=utf-8"/>\n'
        '    <link rel="stylesheet" type="text/css" href="../stylesheet.css"/>\n'
        '    <link rel="stylesheet" type="text/css" href="../page_styles.css"/>\n'
        "  </head>\n"
        '  <body class="calibre"><div class="calibre1"><p class="calibre1">'
        + imgs
        + "</p></div></body></html>\n"
    ).encode("utf-8")


def is_mergeable_image_chapter(zin, cp):
    """单图章且不是 calibre 封面页（<svg> 封面带 calibre:cover，图常与第一章重复，
    合并会把同一张封面画两次）。返回 (可否合并, 图 zip 路径列表)。"""
    data = zin.read(cp)
    if b"<svg" in data.lower():
        return False, None
    doc_dir = cp.rsplit("/", 1)[0] + "/" if "/" in cp else ""
    refs = img_refs(data, doc_dir)
    if refs is None or len(refs) != 1 or is_text_only(data):
        return False, refs
    return True, refs


def main():
    if len(sys.argv) != 3:
        sys.exit("usage: pair_manga_epub.py <in.epub> <out.epub>")
    src_path, out_path = sys.argv[1], sys.argv[2]

    zin = zipfile.ZipFile(src_path)
    opf_name = None
    container = zin.read("META-INF/container.xml").decode("utf-8", "ignore")
    m = re.search(r'full-path="([^"]+)"', container)
    if not m:
        sys.exit("container.xml: no rootfile")
    opf_name = m.group(1)
    opf_dir = opf_name.rsplit("/", 1)[0] + "/" if "/" in opf_name else ""
    opf = zin.read(opf_name).decode("utf-8")

    # manifest: id → (href, media-type)；spine: itemref idref 序列
    man = dict(
        (mid, (href, mt))
        for mid, href, mt in re.findall(
            r'<item\b[^>]*\bid="([^"]+)"[^>]*\bhref="([^"]+)"[^>]*\bmedia-type="([^"]+)"',
            opf,
        )
    )
    # item 属性顺序不固定，再扫一遍兜底（href 在 id 前的情况）
    for it in re.findall(r"<item\b[^>]*>", opf):
        mid = re.search(r'\bid="([^"]+)"', it)
        href = re.search(r'\bhref="([^"]+)"', it)
        mt = re.search(r'\bmedia-type="([^"]+)"', it)
        if mid and href and mt and mid.group(1) not in man:
            man[mid.group(1)] = (href.group(1), mt.group(1))

    spine = re.findall(r'<itemref\b[^>]*\bidref="([^"]+)"', opf)
    if not spine:
        sys.exit("spine empty")

    def chapter_path(idref):
        href, mt = man[idref]
        if "xhtml" not in mt and "html" not in mt:
            return None
        return norm_zip_path(opf_dir, href)

    # 分类：单图章（恰好 1 张图且无正文）可合并；其余原样保留
    entries = []  # ("keep", idref) / ("pair", [idref, idref | None])
    run = []  # 连续单图章缓冲

    def flush_run():
        i = 0
        while i < len(run):
            if i + 1 < len(run):
                entries.append(("pair", [run[i], run[i + 1]]))
                i += 2
            else:
                entries.append(("keep", run[i]))  # 奇数尾巴：单图章保留
                i += 1
        run.clear()

    n_single = 0
    for idref in spine:
        cp = chapter_path(idref)
        if cp is None or cp not in zin.namelist():
            flush_run()
            entries.append(("keep", idref))
            continue
        ok, _ = is_mergeable_image_chapter(zin, cp)
        if ok:
            run.append(idref)
            n_single += 1
        else:
            flush_run()
            entries.append(("keep", idref))
    flush_run()

    # 生成合并章 + 新 OPF
    new_files = {}  # zip 路径 → bytes
    drop = set()  # 被合并掉的旧章 zip 路径
    new_items = {}  # id → href（合并章复用每对第一个 id，第二个 id 从 manifest 删）
    pair_seq = 0
    title_m = re.search(r"<dc:title[^>]*>([^<]*)</dc:title>", opf)
    book_title = title_m.group(1) if title_m else ""

    for kind, val in entries:
        if kind == "keep":
            continue
        a, b = val
        pa, pb = chapter_path(a), chapter_path(b)
        _, ra = is_mergeable_image_chapter(zin, pa)
        _, rb = is_mergeable_image_chapter(zin, pb)
        href = f"text/pair{pair_seq:04d}.html"
        new_files[opf_dir + href] = pair_xhtml(book_title, ra + rb, opf_dir)
        new_items[a] = href
        drop.add(pa)   # 旧单图章文件都被 pair 文件取代（第一个复用 id 改 href，第二个删条目）
        drop.add(pb)
        man.pop(b, None)
        pair_seq += 1

    # 重组 manifest / spine 文本
    def repl_item(mo):
        tag = mo.group(0)
        mid = re.search(r'\bid="([^"]+)"', tag).group(1)
        if mid in new_items:
            return re.sub(r'\bhref="[^"]*"', f'href="{new_items[mid]}"', tag)
        if mid not in man:
            return ""  # 被合并掉的第二个 id
        return tag

    new_opf = re.sub(r"<item\b[^>]*/>", repl_item, opf)

    def repl_ref(mo):
        tag = mo.group(0)
        idref = re.search(r'\bidref="([^"]+)"', tag).group(1)
        if idref not in man:
            return ""  # 被合并掉的章的 itemref
        return tag

    new_opf = re.sub(r"<itemref\b[^>]*/>", repl_ref, new_opf)

    # 写新 zip：mimetype 头条目不压缩，其余照原样复制/替换
    zout = zipfile.ZipFile(out_path, "w")
    zout.writestr("mimetype", zin.read("mimetype"), compress_type=zipfile.ZIP_STORED)
    for info in zin.infolist():
        name = info.filename
        if name == "mimetype":
            continue
        if name in drop:
            continue
        if name == opf_name:
            zout.writestr(name, new_opf)
        elif name in new_files:
            zout.writestr(name, new_files[name])
        else:
            zout.writestr(name, zin.read(name))
    for name, data in new_files.items():
        if name not in zin.namelist():
            zout.writestr(name, data)
    zout.close()

    n_pairs = sum(1 for k, _ in entries if k == "pair")
    print(f"{src_path}: {n_single} 单图章 → {n_pairs} 对页章 "
          f"+ {len(entries) - n_pairs} 保留章 → {out_path}")


if __name__ == "__main__":
    main()
