/**
 * @file    svc_http_page_css.h
 * @brief   内联的第三方样式基底(Water.css v2 dark, MIT) —— 由脚本生成, **不要手改**
 *
 * 【模块职责】给回放页面提供控件与排版的地板(按钮/表格/输入框/code/滚动条的暗色样式),
 *             页面自己只再写监控大屏的布局层(卡片/播放区/时间轴/数据图)。
 * 【依赖方向】无(纯数据); 被 svc_http.c 的 handle_asset() 作为 /app.css 发出去。
 * 【线程模型】只读常量(在 .rodata), 任何线程读都安全。
 * 【资源边界】10106 字节常量(CSS 文本长度, 不占 RAM/栈)。
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
    "/*! Water.css v2 (dark) — MIT License — Copyright (c) Kognise — https://github.com/kognise/wat"
    "er.css */\n:root{--background-body:#202b38;--background:#161f27;--background-alt:#1a242f;--sel"
    "ection:#1c76c5;--text-main:#dbdbdb;--text-bright:#fff;--text-muted:#a9b1ba;--links:#41adff;--f"
    "ocus:rgba(0,150,191,0.67);--border:#526980;--code:#ffbe85;--animation-duration:0.1s;--button-b"
    "ase:#0c151c;--button-hover:#040a0f;--scrollbar-thumb:var(--button-hover);--scrollbar-thumb-hov"
    "er:#000;--form-placeholder:#a9a9a9;--form-text:#fff;--variable:#d941e2;--highlight:#efdb43;--s"
    "elect-arrow:url(\"data:image/svg+xml;charset=utf-8,%3Csvg xmlns='http://www.w3.org/2000/svg' h"
    "eight='63' width='117' fill='%23efefef'%3E%3Cpath d='M115 2c-1-2-4-2-5 0L59 53 7 2a4 4 0 00-5 "
    "5l54 54 2 2 3-2 54-54c2-1 2-4 0-5z'/%3E%3C/svg%3E\")}html{scrollbar-color:#040a0f #202b38;scro"
    "llbar-color:var(--scrollbar-thumb) var(--background-body);scrollbar-width:thin}body{font-famil"
    "y:system-ui,-apple-system,BlinkMacSystemFont,Segoe UI,Roboto,Oxygen,Ubuntu,Cantarell,Fira Sans"
    ",Droid Sans,Helvetica Neue,Segoe UI Emoji,Apple Color Emoji,Noto Color Emoji,sans-serif;line-h"
    "eight:1.4;max-width:800px;margin:20px auto;padding:0 10px;word-wrap:break-word;color:#dbdbdb;c"
    "olor:var(--text-main);background:#202b38;background:var(--background-body);text-rendering:opti"
    "mizeLegibility}button,input,textarea{transition:background-color .1s linear,border-color .1s l"
    "inear,color .1s linear,box-shadow .1s linear,transform .1s ease;transition:background-color va"
    "r(--animation-duration) linear,border-color var(--animation-duration) linear,color var(--anima"
    "tion-duration) linear,box-shadow var(--animation-duration) linear,transform var(--animation-du"
    "ration) ease}h1{font-size:2.2em;margin-top:0}h1,h2,h3,h4,h5,h6{margin-bottom:12px;margin-top:2"
    "4px}h1,h2,h3,h4,h5,h6,strong{color:#fff;color:var(--text-bright)}b,h1,h2,h3,h4,h5,h6,strong,th"
    "{font-weight:600}q:after,q:before{content:none}blockquote,q{border-left:4px solid rgba(0,150,1"
    "91,.67);border-left:4px solid var(--focus);margin:1.5em 0;padding:.5em 1em;font-style:italic}b"
    "lockquote>footer{font-style:normal;border:0}address,blockquote cite{font-style:normal}a[href^="
    "mailto\\:]:before{content:\"📧 \"}a[href^=tel\\:]:before{content:\"📞 \"}a[href^=sms\\:]:befor"
    "e{content:\"💬 \"}mark{background-color:#efdb43;background-color:var(--highlight);border-radiu"
    "s:2px;padding:0 2px;color:#000}a>code,a>strong{color:inherit}button,input[type=button],input[t"
    "ype=checkbox],input[type=radio],input[type=range],input[type=reset],input[type=submit],select{"
    "cursor:pointer}input,select{display:block}[type=checkbox],[type=radio]{display:initial}button,"
    "input,select,textarea{color:#fff;color:var(--form-text);background-color:#161f27;background-co"
    "lor:var(--background);font-family:inherit;font-size:inherit;margin-right:6px;margin-bottom:6px"
    ";padding:10px;border:none;border-radius:6px;outline:none}button,input[type=button],input[type="
    "reset],input[type=submit]{background-color:#0c151c;background-color:var(--button-base);padding"
    "-right:30px;padding-left:30px}button:hover,input[type=button]:hover,input[type=reset]:hover,in"
    "put[type=submit]:hover{background:#040a0f;background:var(--button-hover)}input[type=color]{min"
    "-height:2rem;padding:8px;cursor:pointer}input[type=checkbox],input[type=radio]{height:1em;widt"
    "h:1em}input[type=radio]{border-radius:100%}input{vertical-align:top}label{vertical-align:middl"
    "e;margin-bottom:4px;display:inline-block}button,input:not([type=checkbox]):not([type=radio]),i"
    "nput[type=range],select,textarea{-webkit-appearance:none}textarea{display:block;margin-right:0"
    ";box-sizing:border-box;resize:vertical}textarea:not([cols]){width:100%}textarea:not([rows]){mi"
    "n-height:40px;height:140px}select{background:#161f27 url(\"data:image/svg+xml;charset=utf-8,%3"
    "Csvg xmlns='http://www.w3.org/2000/svg' height='63' width='117' fill='%23efefef'%3E%3Cpath d='"
    "M115 2c-1-2-4-2-5 0L59 53 7 2a4 4 0 00-5 5l54 54 2 2 3-2 54-54c2-1 2-4 0-5z'/%3E%3C/svg%3E\") "
    "calc(100% - 12px) 50%/12px no-repeat;background:var(--background) var(--select-arrow) calc(100"
    "% - 12px) 50%/12px no-repeat;padding-right:35px}select::-ms-expand{display:none}select[multipl"
    "e]{padding-right:10px;background-image:none;overflow-y:auto}button:focus,input:focus,select:fo"
    "cus,textarea:focus{box-shadow:0 0 0 2px rgba(0,150,191,.67);box-shadow:0 0 0 2px var(--focus)}"
    "button:active,input[type=button]:active,input[type=checkbox]:active,input[type=radio]:active,i"
    "nput[type=range]:active,input[type=reset]:active,input[type=submit]:active{transform:translate"
    "Y(2px)}button:disabled,input:disabled,select:disabled,textarea:disabled{cursor:not-allowed;opa"
    "city:.5}::-moz-placeholder{color:#a9a9a9;color:var(--form-placeholder)}:-ms-input-placeholder{"
    "color:#a9a9a9;color:var(--form-placeholder)}::-ms-input-placeholder{color:#a9a9a9;color:var(--"
    "form-placeholder)}::placeholder{color:#a9a9a9;color:var(--form-placeholder)}fieldset{border:1p"
    "x solid rgba(0,150,191,.67);border:1px solid var(--focus);border-radius:6px;margin:0 0 12px;pa"
    "dding:10px}legend{font-size:.9em;font-weight:600}input[type=range]{margin:10px 0;padding:10px "
    "0;background:transparent}input[type=range]:focus{outline:none}input[type=range]::-webkit-slide"
    "r-runnable-track{width:100%;height:9.5px;-webkit-transition:.2s;transition:.2s;background:#161"
    "f27;background:var(--background);border-radius:3px}input[type=range]::-webkit-slider-thumb{box"
    "-shadow:0 1px 1px #000,0 0 1px #0d0d0d;height:20px;width:20px;border-radius:50%;background:#52"
    "6980;background:var(--border);-webkit-appearance:none;margin-top:-7px}input[type=range]:focus:"
    ":-webkit-slider-runnable-track{background:#161f27;background:var(--background)}input[type=rang"
    "e]::-moz-range-track{width:100%;height:9.5px;-moz-transition:.2s;transition:.2s;background:#16"
    "1f27;background:var(--background);border-radius:3px}input[type=range]::-moz-range-thumb{box-sh"
    "adow:1px 1px 1px #000,0 0 1px #0d0d0d;height:20px;width:20px;border-radius:50%;background:#526"
    "980;background:var(--border)}input[type=range]::-ms-track{width:100%;height:9.5px;background:t"
    "ransparent;border-color:transparent;border-width:16px 0;color:transparent}input[type=range]::-"
    "ms-fill-lower,input[type=range]::-ms-fill-upper{background:#161f27;background:var(--background"
    ");border:.2px solid #010101;border-radius:3px;box-shadow:1px 1px 1px #000,0 0 1px #0d0d0d}inpu"
    "t[type=range]::-ms-thumb{box-shadow:1px 1px 1px #000,0 0 1px #0d0d0d;border:1px solid #000;hei"
    "ght:20px;width:20px;border-radius:50%;background:#526980;background:var(--border)}input[type=r"
    "ange]:focus::-ms-fill-lower,input[type=range]:focus::-ms-fill-upper{background:#161f27;backgro"
    "und:var(--background)}a{text-decoration:none;color:#41adff;color:var(--links)}a:hover{text-dec"
    "oration:underline}code,samp,time{background:#161f27;background:var(--background);color:#ffbe85"
    ";color:var(--code);padding:2.5px 5px;border-radius:6px;font-size:1em}pre>code{padding:10px;dis"
    "play:block;overflow-x:auto}var{color:#d941e2;color:var(--variable);font-style:normal;font-fami"
    "ly:monospace}kbd{background:#161f27;background:var(--background);border:1px solid #526980;bord"
    "er:1px solid var(--border);border-radius:2px;color:#dbdbdb;color:var(--text-main);padding:2px "
    "4px}img,video{max-width:100%;height:auto}hr{border:none;border-top:1px solid #526980;border-to"
    "p:1px solid var(--border)}table{border-collapse:collapse;margin-bottom:10px;width:100%;table-l"
    "ayout:fixed}table caption,td,th{text-align:left}td,th{padding:6px;vertical-align:top;word-wrap"
    ":break-word}thead{border-bottom:1px solid #526980;border-bottom:1px solid var(--border)}tfoot{"
    "border-top:1px solid #526980;border-top:1px solid var(--border)}tbody tr:nth-child(2n){backgro"
    "und-color:#161f27;background-color:var(--background)}tbody tr:nth-child(2n) button{background-"
    "color:#1a242f;background-color:var(--background-alt)}tbody tr:nth-child(2n) button:hover{backg"
    "round-color:#202b38;background-color:var(--background-body)}::-webkit-scrollbar{height:10px;wi"
    "dth:10px}::-webkit-scrollbar-track{background:#161f27;background:var(--background);border-radi"
    "us:6px}::-webkit-scrollbar-thumb{background:#040a0f;background:var(--scrollbar-thumb);border-r"
    "adius:6px}::-webkit-scrollbar-thumb:hover{background:#000;background:var(--scrollbar-thumb-hov"
    "er)}::-moz-selection{background-color:#1c76c5;background-color:var(--selection);color:#fff;col"
    "or:var(--text-bright)}::selection{background-color:#1c76c5;background-color:var(--selection);c"
    "olor:#fff;color:var(--text-bright)}details{display:flex;flex-direction:column;align-items:flex"
    "-start;background-color:#1a242f;background-color:var(--background-alt);padding:10px 10px 0;mar"
    "gin:1em 0;border-radius:6px;overflow:hidden}details[open]{padding:10px}details>:last-child{mar"
    "gin-bottom:0}details[open] summary{margin-bottom:10px}summary{display:list-item;background-col"
    "or:#161f27;background-color:var(--background);padding:10px;margin:-10px -10px 0;cursor:pointer"
    ";outline:none}summary:focus,summary:hover{text-decoration:underline}details>:not(summary){marg"
    "in-top:0}summary::-webkit-details-marker{color:#dbdbdb;color:var(--text-main)}dialog{backgroun"
    "d-color:#1a242f;background-color:var(--background-alt);color:#dbdbdb;color:var(--text-main);bo"
    "rder-radius:6px;border:#526980;border-color:var(--border);padding:10px 30px}dialog>header:firs"
    "t-child{background-color:#161f27;background-color:var(--background);border-radius:6px 6px 0 0;"
    "margin:-10px -30px 10px;padding:10px;text-align:center}dialog::-webkit-backdrop{background:rgb"
    "a(0,0,0,.61);-webkit-backdrop-filter:blur(4px);backdrop-filter:blur(4px)}dialog::backdrop{back"
    "ground:rgba(0,0,0,.61);-webkit-backdrop-filter:blur(4px);backdrop-filter:blur(4px)}footer{bord"
    "er-top:1px solid #526980;border-top:1px solid var(--border);padding-top:10px;color:#a9b1ba;col"
    "or:var(--text-muted)}body>footer{margin-top:40px}@media print{body,button,code,details,input,p"
    "re,summary,textarea{background-color:#fff}button,input,textarea{border:1px solid #000}body,but"
    "ton,code,footer,h1,h2,h3,h4,h5,h6,input,pre,strong,summary,textarea{color:#000}summary::marker"
    "{color:#000}summary::-webkit-details-marker{color:#000}tbody tr:nth-child(2n){background-color"
    ":#f2f2f2}a{color:#00f;text-decoration:underline}}\n";

#endif /* __SVC_HTTP_PAGE_CSS_H__ */
