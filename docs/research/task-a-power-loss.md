# 调研笔记：突然断电下的录像可靠性（IPC / NVR / 行车记录仪）

> 背景：Hi3516DV300 IPC + mp4v2 写 MP4 到 vfat/TF 卡，无 RTC、无超级电容；`MP4Close()` 才写 `moov`。
> 本笔记只记录"工业界怎么做"的事实，不写实现建议。

## Sources

[1] FFmpeg project — FFmpeg Formats Documentation（ffmpeg-formats.html，含 4.4.2 Fragmentation / 4.4.3 Options） | https://ffmpeg.org/ffmpeg-formats.html | Source-Type: official | As Of: unknown（页面无日期，内容随 master 更新） | Authority: 9 | 访问: public
[2] StudioCoast Pty Ltd (vMix) — Fault Tolerant Recordings | https://www.vmix.com/help29/FaultTolerantRecordings.html | Source-Type: official | As Of: unknown | Authority: 6 | 访问: public
[3] Tim Müller (Centricular) — [gstreamer-devel] How to record playable video files in gstreamer even if recording is interrupted unexpectedly (e.g. power disconnects)?（邮件列表回复，2021-09-14） | https://lists.x.org/archives/gstreamer-devel/2021-September/078941.html | Source-Type: community | As Of: 2021-09-14 | Authority: 7 | 访问: public
[4] GStreamer project — qtmux 元素文档（Robust Muxing / Robust Prefill Muxing） | https://gstreamer.freedesktop.org/documentation/isomp4/qtmux.html | Source-Type: official | As Of: unknown | Authority: 8 | 访问: public
[5] GStreamer project — qtmoovrecover 元素文档 | https://gstreamer.freedesktop.org/documentation/isomp4/qtmoovrecover.html | Source-Type: official | As Of: unknown | Authority: 8 | 访问: public
[6] LiveAPI (Castr Live Streaming, Inc) — What Is Fragmented MP4 (fMP4)? How It Works and When to Use It | https://liveapi.com/blog/fragmented-mp4/ | Source-Type: secondary-industry | As Of: 2026-07-17 | Authority: 5 | 访问: public
[7] Tuxera（Eva Rio，Embedded BU Portfolio Manager） — How to design dashcams and DVRs that don't fail: a checklist | https://www.tuxera.com/blog/dashcam-dvr-design-checklist/ | Source-Type: secondary-industry | As Of: 2026-04-20（页面标注） | Authority: 7 | 访问: public
[8] ATP Electronics — Endurance, Latency, and Workload Considerations for Choosing Dashcam Memory Cards (Part 1) | https://www.atpinc.com/tw/blog/choosing-memory-cards-for-dashcam-usage | Source-Type: secondary-industry | As Of: 2026-07-03 | Authority: 7 | 访问: public
[9] ATP Electronics — SD Card Requirements for Mission-Critical Applications | https://www.atpinc.com/tw/blog/industrial-sd-cards-factors-requirements-to-consider | Source-Type: secondary-industry | As Of: 2026-06-30 | Authority: 7 | 访问: public
[10] Ask Ubuntu（社区答案修订版原文） — 掉电时各文件系统的抗损坏等级 | https://askubuntu.com/revisions/6c427df0-1c52-4b78-b6c9-cb2ee740ab24/view-source | Source-Type: community | As Of: unknown | Authority: 5 | 访问: public
[11] anthwlock — untrunc（ponchio/untrunc 的维护分支）README | https://raw.githubusercontent.com/anthwlock/untrunc/master/README.md | Source-Type: community | As Of: unknown | Authority: 7 | 访问: public
[12] bookkojot — mp4fixer README（perl fixer.pl） | https://raw.githubusercontent.com/bookkojot/mp4fixer/master/README.md | Source-Type: community | As Of: unknown | Authority: 6 | 访问: public
[13] BlackboxMyCar — What are the Differences between Dash Cam File Types? | https://www.blackboxmycar.com/pages/differences-between-dash-cam-file-types | Source-Type: secondary-industry | As Of: unknown | Authority: 4 | 访问: public
[14] Jeong-Joo Jeong / Samsung Electronics — US20060007816A1 Digital video recording and reproducing apparatus having data recovery function and method thereof（2006-01-12 公开） | https://patents.google.com/patent/US20060007816 | Source-Type: official | As Of: 2006-01-12 | Authority: 6 | 访问: public
[15] Gillware Data Recovery（Joel Taylor） — Lorex DVR Data Recovery | https://www.gillware.com/data-recovery-services/lorex-dvr-data-recovery/ | Source-Type: secondary-industry | As Of: 2026-06-30 | Authority: 5 | 访问: public
[16] Digital Watchdog — Troubleshooting Recording Errors For A VMAX IP Plus（KB，Last Edit 2024-08-16） | https://digitalwatchdog.happyfox.com/kb/article/366-troubleshooting-recording-errors-for-a-vmax-ip-plus/ | Source-Type: official | As Of: 2024-08-16 | Authority: 6 | 访问: public
[17] mp4v2（TechSmith 镜像） — src/mp4file.cpp（MP4File::Close / MP4File::FinishWrite） | https://raw.githubusercontent.com/TechSmith/mp4v2/master/src/mp4file.cpp | Source-Type: official | As Of: unknown | Authority: 8 | 访问: public
[18] GPAC wiki — MP4Box 概览（docs/MP4Box/MP4Box.md 原文） | https://raw.githubusercontent.com/gpac/wiki/master/docs/MP4Box/MP4Box.md | Source-Type: official | As Of: unknown | Authority: 7 | 访问: public
[19] OBS Forums — Best File Format for Recordings to Save as in OBS?（2024-06-04 起） | https://obsproject.com/forum/threads/best-file-format-for-recordings-to-save-as-in-obs.175935/ | Source-Type: community | As Of: 2024-06-04 | Authority: 4 | 访问: public

## Findings

- fMP4 把元数据切成每分片一份：每个 `moof`（movie fragment box）只描述紧随其后的 `mdat`（media data box）里的样本，播放器不需要看别的分片就能解码，因此"写入被中断也能解码"（官方原话：a normal MOV/MP4 is undecodable if it is not properly finished）[1]
- fMP4 是流媒体侧的事实标准：MPEG-DASH 从一开始就用它，Apple 2016 给 HLS 加了 fMP4 支持，CMAF 把它固化成 HLS/DASH 共用格式，OBS 也提供 fragmented MP4 录制选项 [6]
- 生成 fMP4 的主流工具是 ffmpeg 与 GPAC：ffmpeg 靠 `frag_keyframe` / `frag_duration` / `frag_size` / `min_frag_duration` 决定分片切点，`empty_moov`+`default_base_moof` 是常见组合；MP4Box 侧的代表命令是 `MP4Box -dash 1000 file.mp4`（按 1000 ms 切片）[1][6][18]
- GStreamer 的 `qtmux` 给出另一条工业路线："Robust Muxing"——录之前先在文件开头预留 header 空间、按可配置周期重写，所以录制被 crash 打断时文件仍可播（要求输出可 seek）；配套 `moov-recovery-file` 属性 + `qtmoovrecover` 元素做事后重建 [3][4][5]
- 谁能扛掉电是**容器属性而不是扩展名**：vMix 的实测表把 vMix AVI / MKV / MPG / TS / WMV / MXF 判为 fault tolerant，把普通 MP4、普通 AVI、以及"indexed MOV / indexed MP4"判为不 tolerant，并建议用 "New File Every" 定期切新文件来限制损失上限 [2]
- 行车记录仪的容器分布是"历史 MP4/MOV/AVI，新机 H.264/H.265（多为 MP4）"：AVI 压缩效率低、文件更大，"不适合高分辨率行车记录仪"，所以 AVI 并不是因为抗掉电才被选中的主流 [13]
- 文件系统层：无日志的 FAT32/FAT16/ext2 在掉电写入后元数据可能处于不一致状态"实际上已损坏、需要修复"；ext4 默认 writeback 模式只保证元数据可回滚，文件数据可能比元数据旧甚至混入后来写入的数据，`data=ordered` 才保证数据先于元数据落盘 [10]
- SD 卡侧：厂商的 SPOR（Sudden Power-Off Recovery）"保护的是 data at rest，不是 in flight"，因为 SD/microSD 卡上没有板载电容，"断电瞬间正在写的那一笔不保证写完"；工业卡耐久按 TBW 标（3D TLC 可达 5,500 TB、pSLC 可达 12,670 TB，高耐久 NAND 约 5,000 P/E 周期），写放大与磨损均衡决定实际寿命 [8][9]
- 旁路裸流是 NVR 数据恢复的现实做法：Dahua 系的 DHFS 私有文件系统用一份 index 把时间戳/通道映射到盘上的 H.264/H.265，index 还在就重建 index；index 没了就从数据区"carve"出裸 H.264/H.265 流，代价是时间戳与通道号丢失（且环形覆盖可能已盖掉数据块）[15]
- 事后修复有硬前提：untrunc 必须有一段"同机型的完好文件"作参照，"否则成功的概率很低"；mp4fixer 同样要求"用同样设置、同一设备"录的 sample；ffmpeg 的 `hybrid_fragmented` 则从设计上让中断后的中间文件可以被 remux 回普通 MP4 [11][12][1]

## Deep Read Notes

### A. FFmpeg Formats Documentation（官方，[1]）—— 现成工具支持到什么程度

原文（4.4.2 Fragmentation，逐字摘）：

> "A fragmented file consists of a number of fragments, where packets and metadata about these packets are stored together. Writing a fragmented file has the advantage that the file is decodable even if the writing is interrupted (while a normal MOV/MP4 is undecodable if it is not properly finished), and it requires less memory when writing very long files (since writing normal MOV/MP4 files stores info about every single packet in memory until the file is closed). The downside is that it is less compatible with other applications."

切分条件由这几个选项之一触发：`frag_duration`（微秒）、`frag_size`（字节）、`min_frag_duration`、`movflags +frag_keyframe`、`movflags +frag_custom`；`min_frag_duration` 必须满足，其他条件才生效。

与"降低暴露面"直接相关的三个官方选项原文：

- `moov_size bytes` — "Reserves space for the moov atom at the beginning of the file instead of placing the moov atom at the end. If the space reserved is insufficient, muxing will fail."（= 预留在开头，头部空间不够就 muxing 失败，不是静默继续）
- `movflags +hybrid_fragmented` — "For recoverability - write the output file as a fragmented file... When writing is finished, the file is converted to a regular, non-fragmented file, which is more compatible... If writing is aborted, the intermediate file can manually be remuxed to get a regular, non-fragmented file of what had been written into the unfinished file."
- `movflags +faststart` — "Run a second pass moving the index (moov atom) to the beginning of the file... will not work in various situations such as fragmented output"；`frag_keyframe` = "start a new fragment at each video keyframe"；`skip_trailer` = 不写 `mfra/tfra/mfro` trailer。

另外确认（与第 4 项调研相关）：ffmpeg 有裸码流 demuxer，`h264 video (h264, 264)` / `hevc video (hevc, h265, 265)`，文档注明 "Bitstream shall be converted to Annex B syntax if it's in length-prefixed mode"。同一页也提供 `h264_mp4toannexb` 这个 bitstream filter（MP4 的 length-prefixed → Annex B）。

注意：**当前官方页面上没有 `empty_moov` 这个词**（我对整页文本做过检索，命中 0 次），只有 `delay_moov`（"delay writing the initial moov until the first fragment is cut"）。而 2026 年的第三方文章仍在教 `frag_keyframe+empty_moov+default_base_moof` [6]——见 Gaps。

### B. ATP Electronics 行车记录仪存储卡文章（[8] + [9]）—— SD 卡到底能扛住什么

- 定性结论：行车记录仪/Surveillance 的失效模式是"通道写放大 + 磨损不均"，而不是容量不够；官方建议按 TBW 与工作负载选卡，而不是按最大容量。
- 具体数字（2026 年的页面）：ATP 3D TLC microSD "rated up to 5,500 TB written"，同批 NAND 跑 pSLC 模式"up to 12,670 TB"；高耐久 recording NAND 约 5,000 P/E cycles，"pSLC 存 1 bit/cell 把有效耐久提高接近一个数量级"。128 GB 高耐久卡自家测试约 "99,000 hours of continuous HD recording at 12 Mbps"，Full HD 21 Mbps 约 "61,000 hours"。
- 关键限制：**"Power-loss protection on a memory card is firmware-based and protects data at rest — not data in flight."** 以及 **"Because SD/microSD cards have no on-board capacitors, the one write in progress at the instant power is cut is not guaranteed to finish; design a clean shutdown or buffer at the system level for that last write."** SPOR 保护的是 firmware、mapping table 和"已提交"的数据，上电后恢复到一致状态。
- 上电响应时间（Table 2，最坏值，脏态最近 250 次循环）：ATP 卡 @ −20°C 2.25 s、@ +25°C 0.9 s、@ +60°C 7.5 s；对照卡分别为 4 s / 12 s / 12.5 s。
- TBW 是顺序写上限：文中明确"those TBW figures are rated under sequential writes; a random, continuous workload will reach end-of-life sooner"。
- 矛盾点：同一厂商另一篇文章把工业 SD 的 pSLC 写成 "up to 25,000 TB"（[9]），与 [8] 的 12,670 TB 不一致——见 Gaps。

### C. Samsung 专利 US20060007816A1（[14]）—— 早期 DVR 的"掉电恢复信息"设计

这篇 2006 年的专利把问题描述得比任何现代博客都清楚，而且是"录像机厂商侧的官方文本"：

- 根因陈述："the information file and the file system ... are not recorded during video recording but can be recorded after recording"，因为实时录像期间去写这些信息会打断码流、造成 buffer overflow；"If the recording operation is thus not normally completed during recording due to a power breakdown and the like, this information cannot be recorded in the disc, so that video cannot be reproduced."
- 旧方案（**就是"多写一份记录信息"**）：录像期间把 file system information 与 recording information 写进 NVRAM（SRAM/flash），上电后拿盘上的信息与 NVRAM 比对，不一致就回填。代价被明确量化："**300 Kbytes and 512 Kbytes of memories are required for the file system and the recording information, respectively**"——作者认为这 300 KB + 512 KB 是成本负担，所以本专利要改成"不用额外内存、把恢复信息以临时文件形式写在盘上"。
- 该方案的判据是一个 update flag：录像开始置 `recording_info_updated_flag`，正常录完清除；上电时若 flag 仍置位，就判定上次是被强制断电，执行恢复。
- 临时恢复信息文件的位置由一个不等式约束：`Br/Rr > T1s + Tw + T2s`（Br=剩余 buffer、Rr=录制码率、T1s=寻道写恢复信息时间、T2s=寻道回码流时间、Tw=写信息时间）——即"写恢复信息的时间必须小于 buffer 还能撑住的时间"。
- 附带事实：文中还把 DVD 的"信息文件"描述为含 I 帧位置索引，"using location information of video data corresponding to the I-frame ..., a disc scanning can be performed"。

### D. 源码级核对（[17]，短）—— 印证我们实测的 mp4v2 行为

`src/mp4file.cpp` 中：

```cpp
void MP4File::Close(uint32_t options)
{
    if( IsWriteMode() ) {
        SetIntegerProperty( "moov.mvhd.modificationTime", MP4GetAbsTimestamp() );
        FinishWrite(options);
    }
    delete m_file;
    m_file = NULL;
}
```

即：**写模式下 `moov`（及其 mvhd 修改时间）是在 `Close()` → `FinishWrite()` 里才落盘的**，与项目实测"只有 `MP4Close()` 才写 moov"一致。这解释了为什么"进程被 kill -9 / 掉电"会让当前段变成无 `moov` 的文件，而这是 MP4 container 的固有性质、不是 mp4v2 的 bug——[1] 的官方表述、[2] 的格式对照表、[3] 的邮件列表都指向同一结论。

## Gaps

**搜了但没找到**

- **没找到 `-frag` 的一手文档**：GPAC wiki 的 MP4Box 概览页（[18]，我拿到的是 gpac/wiki 仓库里的 markdown 原文）只有 `-add / -rem / -par / -inter / -hint / -dash` 这些示例，**通篇没有 `-frag`**；wiki 里名为 "Fragmentation,-segmentation,-splitting-and-interleaving" 的页面多次 404（HTML 版与 raw markdown 版都试过）。**所以"MP4Box 怎么生成 fMP4"这条只有 `-dash` 一个命令级证据，`-frag` 未证实。**
- **没找到 IPC 板端本地录像用 fMP4 的厂商文档**：所有 fMP4 采用证据都来自流媒体侧（HLS/DASH/CMAF/OBS/GStreamer）。**没有任何一家 IPC/NVR/行车记录仪厂商公开写"我们机内录像写 fMP4"**——这条是空白，不能反推说行业普遍这么干。
- **没找到 NVR 掉电后重建索引的一手技术资料**：只有数据恢复公司的二手描述（[15]）和厂商 KB 的"格式化硬盘"建议（[16]）。Dahua DHFS / 海康私有格式均无公开规范。
- **没找到"AVI 为什么抗掉电"的规范级解释**：只有 vMix 的实测对照表（[2]）和论坛说法（[19]）。AVI 的 `idx1` 索引是否可选、播放器能否无索引遍历 `movi` chunk，我没找到可信一手来源，**所以"AVI 抗掉电的原理"这条我答不上来，只能给现象（[2] 的表）。**
- **没找到磨损均衡/写放大对"频繁小写入"的量化影响**：ATP 只给 TBW 与"随机负载更早到寿"（[8][9]）。**没有找到"小块随机写会把写放大放大 N 倍"的具体实测数字。**
- **没找到"无 RTC 的摄像机如何保证录像时间戳"的行业做法**——这与本项目直接相关（板子无 RTC），但本轮检索没有命中任何一手资料。
- 打不开的页面（如实记录，未计入 Sources）：`usenix.org` 的 SSD 掉电鲁棒性论文页（403，Anubis/Cloudflare 拦截）、`lists.ffmpeg.org` 的 ffmpeg-user 讨论串（Anubis 反爬）、stackoverflow / superuser 相关问答（403）、`api.github.com` 的 untrunc issue #294（403）、dashcamtalk 与 swedespeed 的两个论坛帖（返回 200 但正文被截断为空）。

**互相矛盾 / 口径不一致的说法**

- **`empty_moov` 是否还存在**：ffmpeg 官方页当前**检索不到** `empty_moov`（只有 `delay_moov`）[1]，但 2026 年的行业文章仍在推荐 `frag_keyframe+empty_moov+default_base_moof` [6]。要么该 flag 已从文档移除/改名，要么第三方文档过时——**我没能确认，两种说法不能同时当真。**
- **GStreamer "robust muxing" 的机制描述不一致**：邮件列表说"reserves space for **two sets of headers** and then periodically switches between them"（[3]），官方元素文档说"space for the headers **are reserved** at the start ... and **rewritten** at a configurable interval"（[4]）。一个是双份轮换、一个是一份重写，**只凭这两段无法判定实现细节**（"保留两份"的直接后果是掉电只丢最后一个切换周期内的数据，邮件里说 "you'd only lose the last few seconds"）。
- **fMP4 与 MKV 谁更抗掉电**：OBS 论坛里有人凭"3 年论坛反馈"估计 MKV 一年坏 2–3 个、fMP4 一年 1 个，并断言 fMP4 "even better than MKV"（[19]）；但这条自己承认"fragmented mp4 在 OBS 里用的人少，真正的统计做不出来"，且理由是"取决于实现，如果写最终数据时还要额外 seek，格式就没那么鲁棒"。**这是低权威度的个人估计，不能当结论。**
- **同一厂商（ATP）自家数字打架**：[8] 说 pSLC "up to 12,670 TB"，[9] 说工业 SD pSLC "up to 25,000 TB"。两篇文章都在 2026-06/07，**无法判断哪个是当前口径。**
- **NVR 厂商的处置与"保数据"目标相反**：Digital Watchdog 的 KB 把"格式化硬盘"列为解决掉电后不录像的步骤，同时注明"格式化会永久删除所有已归档录像"（[16]）——**"恢复"和"让机器继续录"在工业实践里是两个互相冲突的目标**，这与数据恢复公司主张的"先做镜像、别格式化"（[15]）明显对立。
- 时间口径：多篇厂商博客（Tuxera [7]、LiveAPI [6]、ATP [8][9]、Gillware [15]）页面自述日期落在 2026 年，而 ffmpeg/GStreamer 官方页无日期。引用这些数字时**只能按"页面自述日期"标注，无法独立核实发布/修订时间。**
