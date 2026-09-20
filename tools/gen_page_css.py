#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""把第三方压缩版 CSS 转成 src/service/svc_http_page_css.h(内联常量字符串)。

为什么要这个脚本: 头文件里内联了约 1 万字节的第三方 CSS, 而"**不要手改**"这个承诺
必须有**可跑的复现路径** —— 否则下次想升级 Water.css 只能靠手工重排 130 行字符串,
那必然出错(而且一定会有人手改后忘了同步)。

它做四件事(顺序不能换):
  ① 读**逐字节原样**的上游文件 tools/assets/water-dark.min.css
  ② 在最前面注入 Water.css 的 MIT 许可声明(作为 CSS 注释, 发出去的样式自带出处)
  ③ 转义成 C 字符串:  `\\` `"` 换行/回车/制表符 —— 顺序上先转义反斜杠再转义引号,
     否则 `"` 转出的 `\\"` 会被第二步再转一次(`\\\\"`), 字符串就错位了
  ④ 按**显示宽度**折行(中文/emoji 算 2 列, 转义序列按它实际占的列数算),
     保证每一行 ≤ --width 列 —— 本项目的行宽体检(check_spec §9)就是这么量的

用法(repo 根目录):
  python tools/gen_page_css.py           # 生成/覆盖头文件
  python tools/gen_page_css.py --check   # 只校验: 已提交的头文件 == 生成器的当前输出
                                         # (不一致退出码 1; 像 gen_doc_index.py --check)

⚠️ 上游 CSS 从哪来: https://cdn.jsdelivr.net/npm/water.css@2/out/dark.min.css
   (下载后**原样**存成 tools/assets/water-dark.min.css, 一个字节都不要动)
"""

import argparse
import os
import sys
import unicodedata

sys.stdout.reconfigure(encoding='utf-8')

REPO = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
DEFAULT_ASSET = os.path.join(REPO, 'tools', 'assets', 'water-dark.min.css')
DEFAULT_OUT = os.path.join(REPO, 'src', 'service', 'svc_http_page_css.h')

# Water.css v2 的许可声明(MIT 要求"保留版权声明", 所以把它放进发出去的 CSS 里)
LICENSE = ('/*! Water.css v2 (dark) — MIT License — Copyright (c) Kognise — '
           'https://github.com/kognise/water.css */')

DOC = '''/**
 * @file    svc_http_page_css.h
 * @brief   内联的第三方样式基底(Water.css v2 dark, MIT) —— 由脚本生成, **不要手改**
 *
 * 【模块职责】给回放页面提供控件与排版的地板(按钮/表格/输入框/code/滚动条的暗色样式),
 *             页面自己只再写监控大屏的布局层(卡片/播放区/时间轴/数据图)。
 * 【依赖方向】无(纯数据); 被 svc_http.c 的 handle_asset() 作为 /app.css 发出去。
 * 【线程模型】只读常量(在 .rodata), 任何线程读都安全。
 * 【资源边界】{nbytes} 字节常量(CSS 文本长度, 不占 RAM/栈)。
 *
 * 【为什么内联】板子通常**没有外网** —— 引 CDN 就是白屏风险; 而且本项目
 *   "一个常量字符串 + 零前端构建步骤"本身就是优点。发给浏览器时仍是一条
 *   `/app.css` 请求(可被浏览器缓存), 不是把 CSS 塞进 HTML。
 * 【怎么再生成】`python tools/gen_page_css.py`
 *   读 `tools/assets/water-dark.min.css` → 注入许可 → 转义 → 按 ≤100 显示列折行。
 *   `--check` 只校验"已提交的头文件是不是生成器的当前输出"(不一致退出码 1)。
 * 【许可】Water.css v2 是 MIT(© Kognise); 生成时把许可声明注入为 CSS 的**首行注释**,
 *   所以发出去的样式自带出处 —— 不依赖"我们记得它是什么许可"。
 * 【体积取舍】Pico.css 压缩后 83KB、Water.css 10KB —— 选后者(见 docs/STATUS.md)。
 */
#ifndef __SVC_HTTP_PAGE_CSS_H__
#define __SVC_HTTP_PAGE_CSS_H__

const char svc_http_page_css[] =
{body};

#endif /* __SVC_HTTP_PAGE_CSS_H__ */
'''

# C 字符串里必须转义的字符(反斜杠必须第一个做, 见文件头 ③)
ESCAPES = {'\\': '\\\\', '"': '\\"', '\n': '\\n', '\r': '\\r', '\t': '\\t'}


def disp_width(ch):
    """一个字符在终端里占几列(CJK 与 emoji 算 2 列)。"""
    return 2 if unicodedata.east_asian_width(ch) in ('W', 'F') else 1


def wrap_css(css, width):
    """把 CSS 文本折成 C 字符串的若干行(每行 `    "…"`), 返回行列表。"""
    indent = '    "'
    budget = width - len(indent) - 1          # 4 空格 + 开引号 + 结尾引号
    lines = []
    cur = ''
    used = 0
    for ch in css:
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


def build(asset_path, width):
    """读上游 CSS → 生成头文件全文; 同时返回 (全文, CSS 字节数, 行列表)。"""
    with open(asset_path, 'r', encoding='utf-8') as f:
        upstream = f.read()
    css = LICENSE + '\n' + upstream.strip() + '\n'
    lines = wrap_css(css, width)
    body = '\n'.join(lines)
    text = DOC.format(nbytes=len(css.encode('utf-8')), body=body)
    return text, len(css.encode('utf-8')), lines


def main():
    ap = argparse.ArgumentParser(description='生成 svc_http_page_css.h')
    ap.add_argument('--css', default=DEFAULT_ASSET, help='上游压缩版 CSS(原样)')
    ap.add_argument('--out', default=DEFAULT_OUT, help='输出的头文件')
    ap.add_argument('--width', type=int, default=100, help='每行最大显示列数')
    ap.add_argument('--check', action='store_true', help='只校验, 不写文件')
    args = ap.parse_args()

    text, nbytes, lines = build(args.css, args.width)

    over = [ln for ln in text.splitlines() if len(ln) > args.width]
    print(f'上游: {args.css}')
    print(f'CSS : {nbytes} 字节 -> {len(lines)} 行 C 字符串 (每行 <= {args.width} 显示列)')
    print(f'最长行: {max(len(l) for l in text.splitlines())} 列')
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
        print(f'❌ {os.path.relpath(args.out, REPO)} 与生成器输出**不一致** —— 过期了, '
              f'跑一次不带 --check 的生成')
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
