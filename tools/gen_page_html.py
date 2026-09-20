#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""把 tools/assets/page.html 转成 src/service/svc_http_page.c(内联常量字符串)。

为什么要这个脚本(而不是手写那一大串 `"…\\n"`):
  ① 页面已经涨到 **20KB 级**, 手写 C 字符串拼接等于让人去数引号 —— 迟早错;
  ② 页面必须能被**真正预览**: `tools/assets/page.html` 是标准 HTML, 双击就能看,
     也能直接喂给 headless 浏览器截图做视觉验收(见 work/shot_page.py);
  ③ 硬约束(每行 ≤100 显示列、双引号转义、LF 行尾)交给工具保证, 不靠人记。

它做三件事(顺序不能换):
  ① 读 page.html(**逐字节**, 不重排)
  ② 转义成 C 字符串: 反斜杠 → `\\\\`、双引号 → `\\"`、换行/回车/制表符 → `\\n` 等
     (反斜杠必须第一个换, 否则第二步刚插进去的 `\\"` 会被再转一次)
  ③ 按**显示宽度**折行(中文/emoji 算 2 列, 转义序列按它实际占的列数算),
     每行 ≤ --width 列 —— check_spec §9 就是这么量的

用法(repo 根目录):
  python tools/gen_page_html.py            # 生成/覆盖 src/service/svc_http_page.c
  python tools/gen_page_html.py --check    # 只校验(已提交的 .c 是不是生成器的当前输出)
"""

import argparse
import os
import sys
import unicodedata

sys.stdout.reconfigure(encoding='utf-8')

REPO = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
DEFAULT_SRC = os.path.join(REPO, 'tools', 'assets', 'page.html')
DEFAULT_OUT = os.path.join(REPO, 'src', 'service', 'svc_http_page.c')

DOC = '''/**
 * @file    svc_http_page.c
 * @brief   回放页面(一整段常量 HTML/CSS/JS, **不带任何前端构建步骤**)
 *
 * 【模块职责】把"板子自带的那个网页"作为一个 const 字符串提供给 `svc_http.c`
 * 【依赖方向】无(纯数据);页面自己用 `fetch` 调 `/recordings`、`/status`、
 *             `/recordings/<名>`(Range)、`/recordings/<名>/lock`、`/recent.mp4`;
 *             样式来自 `/app.css`(见 `svc_http_page_css.h`)
 * 【线程模型】只被 `svc_http` 的 http 线程读(只读常量, 无并发问题)
 * 【资源边界】{nbytes} 字节常量(在 .rodata 里, 不占 RAM 也不占栈)
 *
 * ─────────────────────────────────────────────────────────────────
 *  ⚠️ 这个文件是**生成的** —— 不要手改, 改 `tools/assets/page.html`
 * ─────────────────────────────────────────────────────────────────
 *  生成器: `python tools/gen_page_html.py`(`--check` 校验是否过期)
 *
 * ─────────────────────────────────────────────────────────────────
 *  ⚠️ 三条硬约束(改 page.html 之前先读)
 * ─────────────────────────────────────────────────────────────────
 *  ① **不许引外部资源**:不要 CDN / 字体 / 图标库 —— 板子通常**没有外网**,
 *     引了就白屏。CSS/JS/图标全内联(图标用内联 SVG 或字符)。
 *     (参考 Frigate 用 Tailwind + React 构建、Shinobi 用 Node 打包 ——
 *      **我们要的是它们的设计语言, 不是它们的工具链**。)
 *     唯一的例外是 `/app.css`:那是**板子自己**发出去的第三方样式基底(Water.css),
 *     不是外链。
 *  ② **一行不许超过 100 显示列**(`check_spec.py` 按显示宽度算, 中文算 2 列)——
 *     这条由**生成器**保证(它在转义之后按显示宽度折行), 但 page.html 自己
 *     也别写超长行, 否则生成出来的 C 难看。
 *  ③ HTML 属性用单引号(`'`), 双引号统一由生成器转义成 `\\"` —— 手写时代靠"不写双引号"
 *     避免截断 C 字符串, 现在由工具兜住(JS 里的 `"` 是合法且安全的)。
 *
 * ─────────────────────────────────────────────────────────────────
 *  UI 三层结构(2026-09-20 v3 定稿)
 * ─────────────────────────────────────────────────────────────────
 *  ① **底座** `/app.css`(Water.css v2 dark):按钮/表格/输入框/code/滚动条的地板
 *  ② **大屏层** 本文件里的 `<style>`:宽画布(1560px) + 吸顶栏 + KPI 条 +
 *     16:9 画面区 + 时间轴 + 数据表;显式夺回 Water.css 对 body 的 `max-width:800px`
 *  ③ **数据层** 内联 JS:全部手绘(canvas sparkline), 不引图表库;
 *     每秒拉一次 `/stats`, 5 秒拉一次 `/recordings`(看画面/实时时不打扰播放)
 *
 *  可访问性(照 vercel-labs/web-interface-guidelines 过了一遍):
 *    语义 `<button>`(行/时间轴块不再是"带 onclick 的 div")、图标按钮有 `aria-label`、
 *    toast 容器 `aria-live='polite'`、`:focus-visible` 焦点环、`prefers-reduced-motion`
 *    关动效、`color-scheme:dark` + `theme-color`、数字列 `tabular-nums`。
 *    一条**没采纳**的建议: 指南说"列表 >50 项要虚拟化" —— 这里一行只有 6 个节点、
 *    分页一次最多上百行, 实测没有性能问题; 反倒试过 `content-visibility:auto`,
 *    它会让**全页截图/打印里视口外的行变成空白**(`innerText` 也读不到) ——
 *    对"要截图进简历"这个用途是净损失, 所以撤掉了。
 */
#include "svc_http_page.h"

/** 回放页面(整页 HTML;见文件头三条硬约束) */
const char svc_http_page_html[] =
{body};

/* 末尾那一个字节是 '\\0'(C 自动加), 不占用上面的长度 */
'''

ESCAPES = {'\\': '\\\\', '"': '\\"', '\n': '\\n', '\r': '\\r', '\t': '\\t'}


def disp_width(ch):
    """一个字符在终端里占几列(CJK 与 emoji 算 2 列)。"""
    return 2 if unicodedata.east_asian_width(ch) in ('W', 'F') else 1


def wrap(text, width):
    """把文本折成若干 `    "…"` 行(每行 ≤ width 显示列)。"""
    indent = '    "'
    budget = width - len(indent) - 1
    lines, cur, used = [], '', 0
    for ch in text:
        esc = ESCAPES.get(ch, ch)
        w = len(esc) if esc != ch else disp_width(ch)
        if used + w > budget and cur:
            lines.append(indent + cur + '"')
            cur, used = '', 0
        cur += esc
        used += w
    if cur:
        lines.append(indent + cur + '"')
    return lines


def build(src_path, width):
    """读 page.html → 生成 .c 全文; 同时返回 (全文, 页面字节数, 行列表)。"""
    with open(src_path, 'r', encoding='utf-8') as f:
        page = f.read()
    assert page.lstrip().startswith('<!DOCTYPE html>'), 'page.html 不像一个 HTML 文档'
    assert page.count('<script>') == 1, \
        'page.html 里必须恰好一个裸 <script> 标签(验收脚本按它抽 JS)'
    assert page.count('</script>') == 1, 'page.html 里的 </script> 数量不对'
    nbytes = len(page.encode('utf-8'))
    lines = wrap(page, width)
    return DOC.format(nbytes=nbytes, body='\n'.join(lines)), nbytes, lines


def main():
    ap = argparse.ArgumentParser(description='生成 svc_http_page.c')
    ap.add_argument('--src', default=DEFAULT_SRC, help='页面源文件(page.html)')
    ap.add_argument('--out', default=DEFAULT_OUT, help='输出的 .c')
    ap.add_argument('--width', type=int, default=100, help='每行最大显示列数')
    ap.add_argument('--check', action='store_true', help='只校验, 不写文件')
    args = ap.parse_args()

    text, nbytes, lines = build(args.src, args.width)
    js = text.split('<script>')[1].split('</script>')[0]
    print(f'页面源: {args.src}')
    print(f'页面  : {nbytes} 字节 -> {len(lines)} 行 C 字符串 '
          f'(每行 <= {args.width} 显示列)')
    over = [ln for ln in text.splitlines() if len(ln) > args.width]
    if over:
        print(f'❌ 有 {len(over)} 行超宽')
        return 1

    old = ''
    if os.path.exists(args.out):
        with open(args.out, 'r', encoding='utf-8') as f:
            old = f.read()
    if args.check:
        if old == text:
            print(f'✅ {os.path.relpath(args.out, REPO)} 与生成器输出一致(未过期)')
            return 0
        print(f'❌ {os.path.relpath(args.out, REPO)} **过期** —— 重跑一次生成')
        return 1
    if old == text:
        print(f'= {os.path.relpath(args.out, REPO)} 内容没变')
        return 0
    with open(args.out, 'w', encoding='utf-8', newline='\n') as f:
        f.write(text)
    print(f'✅ 已写出 {os.path.relpath(args.out, REPO)}')
    return 0


if __name__ == '__main__':
    sys.exit(main())
