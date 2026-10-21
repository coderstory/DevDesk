#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
DevDesk AI 驱动开发实践报告 — HTML 合稿脚本 v2
读取 13-实践报告章节/ 下 8 个 .md + 13 个 .svg，
装配为单文件自包含打印版式 HTML。
2026-10-21 — 修复图6-2匹配
"""
import re, os, sys

SRC = r"D:\project\gui\DevDesk\docs\strategy\13-实践报告章节"
OUT = r"D:\project\gui\DevDesk\docs\strategy\report\DevDesk-AI驱动开发实践报告.html"

# ── 图信息 ────────────────────────────────────────────
FIGURES = {
    "图1-1.svg": ("图 1-1　不可能的三角与七轴机制", 500),
    "图2-1.svg": ("图 2-1　DevDesk 四层技术架构分层框图", 480),
    "图2-2.svg": ("图 2-2　AI 开发环境多会话并行工作示意图", 480),
    "图3-1.svg": ("图 3-1　用户与 AI 协作循环流程图", 500),
    "图4-1.svg": ("图 4-1　AI 六阶段流水线时序图", 520),
    "图4-2.svg": ("图 4-2　子任务并发调度模型", 520),
    "图5-1.svg": ("图 5-1　预防机制全景图：五类风险到六组机制的映射", 500),
    "图5-2.svg": ('图 5-2\u3000四层机制示意图：从\u201c规则在纸上\u201d到\u201c规则在闸上\u201d', 500),
    "图6-1.svg": ("图 6-1　事故到经验的闭环流程", 520),
    "图6-2.svg": ("图 6-2　经验的三层存储结构（倒金字塔）", 520),
    "图7-1.svg": ("图 7-1　多层审计防御塔", 500),
    "图7-2.svg": ("图 7-2　PDCA 多轮递升审计流程", 520),
    "图8-1.svg": ("图 8-1　从零起步最小路线图", 500),
}

def load_svg(fname):
    p = os.path.join(SRC, fname)
    with open(p, "r", encoding="utf-8") as f:
        return f.read()

def svg_figure(fname):
    caption, vbw = FIGURES[fname]
    svg = load_svg(fname)
    svg = re.sub(r'<svg\s+', '<svg ', svg, count=1)
    svg = re.sub(r'\s+width="[^"]*"', '', svg, count=1)
    svg = re.sub(r'\s+height="[^"]*"', '', svg, count=1)
    svg = re.sub(r'\s+max-width="[^"]*"', '', svg, count=1)
    return '<div class="figure">\n' + svg + '\n<div class="caption"><span class="label">' + caption + '</span></div></div>'


# ── Markdown → HTML ──────────────────────────────────
def md2html(text):
    lines = text.split("\n")
    out = []
    i = 0
    in_table = False
    table_rows = []
    in_quote = False
    quote_lines = []
    in_list = False
    list_lines = []

    def flush_quote():
        nonlocal in_quote, quote_lines
        if quote_lines:
            body = "\n".join(quote_lines)
            body = re.sub(r'\*\*(.+?)\*\*', r'<strong>\1</strong>', body)
            out.append('<blockquote>\n' + body + '\n</blockquote>')
        quote_lines = []
        in_quote = False

    def flush_list():
        nonlocal in_list, list_lines
        if list_lines:
            out.append('<ul>')
            for li in list_lines:
                li = re.sub(r'\*\*(.+?)\*\*', r'<strong>\1</strong>', li)
                out.append('<li>' + li + '</li>')
            out.append('</ul>')
        list_lines = []
        in_list = False

    def flush_table():
        nonlocal in_table, table_rows
        if table_rows:
            out.append('<div class="data-table"><table>')
            header = True
            for row in table_rows:
                cells = [c.strip() for c in row.split("|")[1:-1]]
                if all(re.match(r'^[-:]+$', c) for c in cells):
                    header = False
                    continue
                tag = "th" if header else "td"
                out.append('<tr>')
                for c in cells:
                    c_clean = re.sub(r'\*\*(.+?)\*\*', r'<strong>\1</strong>', c)
                    out.append('<' + tag + '>' + c_clean + '</' + tag + '>')
                out.append('</tr>')
                if header:
                    header = False
            out.append('</table></div>')
        table_rows = []
        in_table = False

    while i < len(lines):
        line = lines[i]
        stripped = line.strip()

        if stripped.startswith("|") and stripped.endswith("|"):
            if not in_table:
                flush_quote(); flush_list(); in_table = True
            table_rows.append(stripped)
            i += 1
            continue
        elif in_table:
            flush_table()

        if stripped.startswith("> "):
            if not in_quote:
                flush_list(); in_quote = True
            quote_lines.append(stripped[2:])
            i += 1
            continue
        elif in_quote and stripped.startswith(">"):
            quote_lines.append(stripped[2:])
            i += 1
            continue
        elif in_quote:
            flush_quote()

        if re.match(r'^[-*] ', stripped):
            if not in_list:
                flush_quote(); in_list = True
            li_text = re.sub(r'^[-*] ', '', stripped)
            list_lines.append(li_text)
            i += 1
            continue
        elif in_list:
            flush_list()

        if stripped == "":
            flush_quote(); flush_list()
            i += 1
            continue

        if stripped in ("---", "***"):
            flush_quote(); flush_list()
            out.append('<hr class="section-hr">')
            i += 1
            continue

        m_h1 = re.match(r'^# (.+)', stripped)
        m_h2 = re.match(r'^## (.+)', stripped)
        m_h3 = re.match(r'^### (.+)', stripped)
        if m_h1:
            flush_quote(); flush_list()
            title = re.sub(r'\*\*(.+?)\*\*', r'<strong>\1</strong>', m_h1.group(1))
            out.append('<h1>' + title + '</h1>')
            i += 1
            continue
        if m_h3:
            flush_quote(); flush_list()
            title = re.sub(r'\*\*(.+?)\*\*', r'<strong>\1</strong>', m_h3.group(1))
            out.append('<h3>' + title + '</h3>')
            i += 1
            continue
        if m_h2:
            flush_quote(); flush_list()
            title = re.sub(r'\*\*(.+?)\*\*', r'<strong>\1</strong>', m_h2.group(1))
            out.append('<h2>' + title + '</h2>')
            i += 1
            continue

        flush_quote(); flush_list()
        p = stripped
        p = re.sub(r'\*\*(.+?)\*\*', r'<strong>\1</strong>', p)
        p = re.sub(r'`([^`]+)`', r'<code>\1</code>', p)
        out.append('<p>' + p + '</p>')
        i += 1

    flush_quote(); flush_list(); flush_table()
    return "\n".join(out)


# ── 图占位替换 ─────────────────────────────────────
def replace_figures(html_body, chap_num):
    body = html_body

    # Ch1
    if chap_num == 1:
        body = re.sub(
            r'<p><strong>图1-1[^<]*</strong>[^<]*见同名 SVG 文件[^<]*</p>',
            svg_figure("图1-1.svg"), body)

    # Ch2
    if chap_num == 2:
        body = body.replace(
            '<p>\uff08\u56fe 2-1\uff0c\u89c1\u540c\u540d SVG \u6587\u4ef6\uff09</p>',
            svg_figure("图2-1.svg"))
        body = body.replace(
            '<p>\uff08\u56fe 2-2\uff0c\u89c1\u540c\u540d SVG \u6587\u4ef6\uff09</p>',
            svg_figure("图2-2.svg"))

    # Ch3
    if chap_num == 3:
        body = re.sub(
            r'<h3>\u56fe3-1[^<]*</h3>\s*<p>\u672c\u56fe\u91c7\u7528[^<]*</p>',
            lambda m: svg_figure("图3-1.svg") + '<p class="no-indent note">' + re.search(r'\u672c\u56fe\u91c7\u7528[^<]*', m.group(0)).group(0) + '</p>',
            body, flags=re.DOTALL)

    # Ch4: 图在 4.2 开头和 4.3 开头插入
    if chap_num == 4:
        body = body.replace(
            '<h3>\u9636\u6bb5 1\uff1a\u5b9e\u73b0</h3>',
            svg_figure("图4-1.svg") + '<h3>\u9636\u6bb5 1\uff1a\u5b9e\u73b0</h3>')
        body = body.replace(
            '<h2>4.3 \u8c03\u5ea6\u7eaa\u5f8b\uff1a\u6d41\u6c34\u7ebf\u4e4b\u5916\u7684\u7cfb\u7edf\u7ea6\u675f</h2>\n<p>\u516d\u9636\u6bb5\u7ba1\u7684\u662f',
            '<h2>4.3 \u8c03\u5ea6\u7eaa\u5f8b\uff1a\u6d41\u6c34\u7ebf\u4e4b\u5916\u7684\u7cfb\u7edf\u7ea6\u675f</h2>\n' + svg_figure("图4-2.svg") + '\n<p>\u516d\u9636\u6bb5\u7ba1\u7684\u662f')

    # Ch5
    if chap_num == 5:
        body = body.replace(
            '<p>\uff08\u56fe 5-1\uff1a\u9884\u9632\u673a\u5236\u5168\u666f\u56fe\u2014\u2014\u4e94\u7c7b\u98ce\u9669\u5230\u516d\u7ec4\u673a\u5236\u7684\u6620\u5c04\uff0c\u89c1\u540c\u540d SVG \u6587\u4ef6\uff09</p>',
            svg_figure("图5-1.svg"))
        # 图5-2 用正则（含中文引号）
        body = re.sub(
            r'<p>\uff08\u56fe 5-2[^)]*SVG \u6587\u4ef6\uff09</p>',
            svg_figure("图5-2.svg"), body)

    # Ch6
    if chap_num == 6:
        # 图6-1: 在 6.2 节标题后、第一段"触发"之前
        body = body.replace(
            '<p><strong>\u89e6\u53d1',
            svg_figure("图6-1.svg") + '\n<p><strong>\u89e6\u53d1', 1)
        # 图6-2: 正文中 "(图 6-2)" 引用所在的段落（全文唯一出现此行）
        p62_pat = re.compile(r'(<p>DevDesk .*?\u56fe 6-2[^<]*</p>)')
        m62 = p62_pat.search(body)
        if m62:
            old_p = m62.group(1)
            body = body.replace(old_p, old_p + '\n' + svg_figure("图6-2.svg"))
        else:
            print("  [WARN] \u56fe6-2 \u672a\u627e\u5230\u5339\u914d\u6bb5\u843d")

    # Ch7: regex-based \u2014 use specific context to avoid matching tail reference
    if chap_num == 7:
        svg71 = svg_figure("\u56fe7-1.svg")
        svg72 = svg_figure("\u56fe7-2.svg")
        # \u56fe7-1: paragraph containing "\u9632\u5fa1\u5854" and "\u56fe7-1 \u5c55\u793a\u4e86"
        body = re.sub(r'(<p>[^<]*\u9632\u5fa1\u5854[^<]*\u56fe7-1 \u5c55\u793a\u4e86[^<]*</p>)',
            lambda m: m.group(1) + '\n' + svg71, body)
        # \u56fe7-2: paragraph containing "\u591a\u8f6e\u9012\u5347" and "\u56fe7-2 \u5c55\u793a\u4e86"
        body = re.sub(r'(<p>[^<]*\u591a\u8f6e\u9012\u5347[^<]*\u56fe7-2 \u5c55\u793a\u4e86[^<]*</p>)',
            lambda m: m.group(1) + '\n' + svg72, body)
        # \u5220\u5c3e\u90e8\u5f15\u7528\u884c
        body = re.sub(r'<p>\*?[\uff08\(]\u56fe7-1[^<]*\u56fe7-2[^<]*[\uff09\)]\*?</p>', '', body)
    # Ch8
    if chap_num == 8:
        body = re.sub(
            r'<p>\*?\uff08\u8868 8-1[^<]*\u56fe 8-1[^<]*\uff09\*?</p>',
            svg_figure("图8-1.svg"), body)

    return body


# ── CSS ─────────────────────────────────────────────
CSS = r'''
@page { size: A4 portrait; margin: 2.5cm 2cm 2.5cm 2cm;
  @top-center { content: "DevDesk AI 驱动开发实践报告"; font-family: "SimHei","黑体",sans-serif; font-size: 8pt; color: #666; }
  @bottom-center { content: "第 " counter(page) " 页"; font-family: "SimSun","宋体",serif; font-size: 8pt; color: #666; } }
@page cover { @top-center { content: none; } @bottom-center { content: none; } }
@page toc { @top-center { content: none; } }
* { box-sizing: border-box; margin: 0; padding: 0; }
body { font-family: "SimSun","宋体","Microsoft YaHei",serif; font-size: 11pt; line-height: 1.8; color: #1a1a1a; }
h1 { font-family: "SimHei","黑体","Microsoft YaHei",sans-serif; font-size: 16pt; font-weight: bold; margin: 0.5em 0; page-break-before: always; }
h2 { font-family: "SimHei","黑体","Microsoft YaHei",sans-serif; font-size: 13pt; font-weight: bold; margin: 1.2em 0 0.5em 0; }
h3 { font-family: "SimHei","黑体","Microsoft YaHei",sans-serif; font-size: 11.5pt; font-weight: bold; margin: 1em 0 0.4em 0; }
p { text-indent: 2em; margin: 0.4em 0; }
p.no-indent { text-indent: 0; }
.cover { page: cover; text-align: center; padding-top: 8cm; }
.cover .title { font-family: "SimHei","黑体",sans-serif; font-size: 22pt; font-weight: bold; letter-spacing: 0.1em; margin-bottom: 2em; line-height: 1.6; }
.cover .subtitle { font-family: "SimSun","宋体",serif; font-size: 12pt; margin-bottom: 4em; color: #555; }
.cover .meta { font-family: "SimSun","宋体",serif; font-size: 11pt; line-height: 2.5; color: #555; }
.cover .meta span { display: block; }
.toc { page: toc; page-break-after: always; }
.toc h1 { page-break-before: avoid; }
.toc ul { list-style: none; padding-left: 0; }
.toc li { margin: 0.3em 0; text-indent: 0; }
.toc .l1 { font-weight: bold; margin-top: 0.6em; }
.toc .l2 { padding-left: 2em; }
.toc .l3 { padding-left: 4em; font-size: 10pt; }
.data-table { margin: 1em 0; }
.data-table table { width: 100%; border-collapse: collapse; font-size: 10pt; page-break-inside: avoid; }
.data-table caption { font-family: "SimHei","黑体",sans-serif; font-weight: bold; margin-bottom: 0.5em; text-align: center; }
.data-table th { border-top: 1.5pt solid #1a1a1a; border-bottom: 0.75pt solid #1a1a1a; padding: 5px 8px; text-align: left; font-family: "SimHei","黑体",sans-serif; background: none; }
.data-table td { border-bottom: 0.5pt solid #ccc; padding: 5px 8px; vertical-align: top; }
.data-table tr:last-child td { border-bottom: 1.5pt solid #1a1a1a; }
.figure { margin: 1.2em 0; text-align: center; page-break-inside: avoid; }
.figure svg { max-width: 100%; height: auto; display: block; margin: 0 auto; }
.figure .caption { font-size: 9.5pt; color: #555; margin-top: 0.3em; font-family: "SimSun","宋体",serif; }
.figure .label { font-weight: bold; }
.keypoints { margin: 0.8em 0; padding: 0.6em 1em; border-left: 3px solid #1f4e79; background: #f8f8f8; font-size: 10pt; }
.keypoints strong { font-family: "SimHei","黑体",sans-serif; }
.keypoints p { text-indent: 0; margin: 0.2em 0; }
blockquote { margin: 0.6em 0; padding: 0.4em 1.2em; border-left: 3px solid #ccc; background: #fafafa; font-size: 10.5pt; }
blockquote p { text-indent: 0; margin: 0.2em 0; }
ul, ol { margin: 0.4em 0; padding-left: 3em; }
li { margin: 0.2em 0; }
code { font-family: "Consolas","Courier New",monospace; font-size: 9.5pt; background: #f4f4f4; padding: 1px 4px; border-radius: 2px; }
hr.section-hr { border: none; border-top: 0.5pt solid #ccc; margin: 1.5em 0; }
h1.no-num::before { content: none; }
h2.no-num::before { content: none; }
.small { font-size: 9.5pt; }
.note { font-size: 9.5pt; color: #666; text-indent: 0; }
.page-break { page-break-before: always; }
.num { text-align: center; width: 3em; }
'''


# ── 目录 ────────────────────────────────────────────
TOC = r'''
<div class="toc">
<h1 class="no-num" style="counter-increment:none;">目  次</h1>
<ul>
<li class="l1">第1章 引言：这怎么可能？
</li><li class="l2">1.1 一个"不可能"的开局</li><li class="l2">1.2 真正的悖论</li><li class="l2">1.3 报告结构：七把钥匙</li><li class="l2">1.4 阅读前置说明</li>
<li class="l1">第2章 技术全景：用了什么、做出了什么
</li><li class="l2">2.1 技术栈：四层结构</li><li class="l2">2.2 AI 开发环境</li><li class="l2">2.3 交付物：六个业务模块</li><li class="l2">2.4 技术全景小结</li>
<li class="l1">第3章 人的操作流程：用户从头到尾做什么
</li><li class="l2">3.1 分工前提：代码不可见</li><li class="l2">3.2 用户的实际操作界面</li><li class="l2">3.3 用户的操作节奏</li><li class="l2">3.4 用户不做什么</li><li class="l2">3.5 纪实案例：UAT6</li><li class="l2">3.6 小结</li>
<li class="l1">第4章 AI 的执行流程：任务的六阶段流水线
</li><li class="l2">4.1 主会话铁律</li><li class="l2">4.2 六阶段流水线</li><li class="l2">4.3 调度纪律</li><li class="l2">4.4 为什么这么重？</li>
<li class="l1">第5章 预防机制：用什么解决了什么问题
</li><li class="l2">5.1 防"AI 改了不该改的"</li><li class="l2">5.2 防"多线并行互相踩"</li><li class="l2">5.3 防"假绿测试骗人"</li><li class="l2">5.4 防"静默失败无人知"</li><li class="l2">5.5 防"忙中出错还看不见"</li><li class="l2">5.6 防"AI 不走流程"（hooks）</li>
<li class="l1">第6章 自动复盘：事故如何沉淀为经验
</li><li class="l2">6.1 经验的三层存储结构</li><li class="l2">6.2 经验闭环</li><li class="l2">6.3 cs-retro 辅助机制</li><li class="l2">6.4 四个生命周期案例</li><li class="l2">6.5 小结：系统记忆</li>
<li class="l1">第7章 AI 行为监控：多层防御的审计体系
</li><li class="l2">7.1 核心设计原则</li><li class="l2">7.2 四层防御塔</li><li class="l2">7.3 审计产物标准化</li><li class="l2">7.4 边界与局限</li><li class="l2">7.5 纪实案例</li>
<li class="l1">第8章 成果与可复制清单：别人怎么照抄
</li><li class="l2">8.1 方法论本质</li><li class="l2">8.2 可复制清单</li><li class="l2">8.3 起步指南</li><li class="l2">8.4 适用边界</li><li class="l2">8.5 小结</li>
</ul>
</div>
'''


# ── 主流程 ──────────────────────────────────────────
def main():
    chapters = [
        "第1章-引言.md", "第2章-技术全景.md", "第3章-人类操作流程.md",
        "第4章-AI执行流程.md", "第5章-预防机制.md", "第6章-自动复盘.md",
        "第7章-AI行为监控.md", "第8章-成果与可复制清单.md",
    ]

    chapter_bodies = []
    for idx, fname in enumerate(chapters):
        chap_num = idx + 1
        fpath = os.path.join(SRC, fname)
        with open(fpath, "r", encoding="utf-8") as f:
            md_text = f.read()
        html_body = md2html(md_text)
        html_body = replace_figures(html_body, chap_num)
        chapter_bodies.append(html_body)
        print(f"  [OK] 第{chap_num}章 ({fname}) — {len(html_body)} 字符")

    full = '<!DOCTYPE html>\n<html lang="zh-CN">\n<head>\n<meta charset="UTF-8">\n<title>DevDesk AI 驱动开发实践报告</title>\n<style>\n' + CSS + '\n</style>\n</head>\n<body>\n\n'
    full += '<!-- ====== 封面 ====== -->\n<div class="cover">\n'
    full += '<div class="title">DevDesk AI 驱动<br>开发实践报告</div>\n'
    full += '<div class="subtitle">一个不懂 C++ 的人如何用纯 AI 做出跨平台桌面软件</div>\n'
    full += '<div class="meta"><span>日期：2026 年 10 月</span><span>编制：DevDesk 项目组</span></div>\n</div>\n\n'
    full += '<!-- ====== 目录 ====== -->\n' + TOC + '\n\n<!-- ====== 正文 ====== -->\n'
    full += "".join(chapter_bodies) + '\n</body>\n</html>'

    with open(OUT, "w", encoding="utf-8") as f:
        f.write(full)
    print(f"\n[DONE] HTML => {OUT}")
    print(f"  总大小: {os.path.getsize(OUT):,} 字节")

    # 速检
    with open(OUT, "r", encoding="utf-8") as f:
        html = f.read()
    svg_count = html.count('<svg ')
    fig_count = html.count('class="figure"')
    tbl_count = html.count('class="data-table"')
    residual = html.count('见同名 SVG 文件')
    sens1 = html.count('星锐')
    sens2 = html.count('qwen3')
    print(f"  内嵌SVG: {svg_count}/13 | 图div: {fig_count}/13 | 表: {tbl_count}/6 | 残留占位: {residual} | 敏感词: {sens1+sens2}")


if __name__ == "__main__":
    main()