# 任务 D —— 开源录像实现与工具链（带来源调研笔记）

> 调研范围：ffmpeg `-f segment` / fMP4（`-movflags`、MP4Box `-frag`）/ 开源监控项目（ZoneMinder、Frigate、motion、Shinobi）/ 修复工具（untrunc、recover_mp4）/ Linux 嵌入式层（`fallocate`、`fsync`）/ mp4v2 本身的分片能力。
>
> 纪律说明：本文件**只记录本次调研中真实抓取到的官方文档原文、源码行、man page 文本**。
> 打不开或没找到的内容一律写进 Gaps，**不进入 Findings**。

---

## Sources

[1] FFmpeg project — FFmpeg Formats Documentation (`ffmpeg-formats.html`，含 4.4 MOV/MPEG-4 muxer 与 4.73 segment muxer 全文) | https://ffmpeg.org/ffmpeg-formats.html | Source-Type: official | As Of: unknown | Authority: 10 | 访问: public

[2] FFmpeg project — `doc/muxers.texi`（master 分支，`hybrid_fragmented` 官方文档原文） | https://raw.githubusercontent.com/FFmpeg/FFmpeg/master/doc/muxers.texi | Source-Type: official | As Of: unknown | Authority: 10 | 访问: public

[3] FFmpeg project — `libavformat/movenc.c`（master 分支，movflags 选项表与初始化逻辑） | https://raw.githubusercontent.com/FFmpeg/FFmpeg/master/libavformat/movenc.c | Source-Type: official | As Of: unknown | Authority: 10 | 访问: public

[4] FFmpeg project — `libavformat/movenc.h`（`FF_MOV_FLAG_*` 位定义） | https://raw.githubusercontent.com/FFmpeg/FFmpeg/master/libavformat/movenc.h | Source-Type: official | As Of: unknown | Authority: 10 | 访问: public

[5] FFmpeg project — `libavformat/segment.c`（segment muxer 选项默认值与切段判断逻辑） | https://raw.githubusercontent.com/FFmpeg/FFmpeg/master/libavformat/segment.c | Source-Type: official | As Of: unknown | Authority: 10 | 访问: public

[6] FFmpeg project — `libavformat/mov.c`（demuxer，"moov atom not found" 报错点） | https://raw.githubusercontent.com/FFmpeg/FFmpeg/master/libavformat/mov.c | Source-Type: official | As Of: unknown | Authority: 10 | 访问: public

[7] FFmpeg project — `n7.1` 分支 `doc/muxers.texi` / `n7.0` / `n6.1` 分支同名文件（用于判定 `hybrid_fragmented` 从哪个版本起存在） | https://raw.githubusercontent.com/FFmpeg/FFmpeg/n7.1/doc/muxers.texi | Source-Type: official | As Of: unknown | Authority: 10 | 访问: public

[8] FFmpeg-devel 邮件列表（Dennis Mungai / Martin Storsjö 回复） — [PATCH v3] movenc: Add an option for resilient, hybrid fragmented/non-fragmented muxing | https://ffmpeg.org//pipermail/ffmpeg-devel/2024-June/330158.html | Source-Type: official | As Of: 2024-06-24 | Authority: 9 | 访问: public

[9] Michael Kerrisk / Linux man-pages project — fallocate(2) | https://man7.org/linux/man-pages/man2/fallocate.2.html | Source-Type: official | As Of: 2026-02-08 | Authority: 10 | 访问: public

[10] Michael Kerrisk / Linux man-pages project — posix_fallocate(3) | https://man7.org/linux/man-pages/man3/posix_fallocate.3.html | Source-Type: official | As Of: 2026-02-08 | Authority: 10 | 访问: public

[11] Michael Kerrisk / Linux man-pages project — fsync(2)（含 fdatasync） | https://man7.org/linux/man-pages/man2/fsync.2.html | Source-Type: official | As Of: 2026-02-08 | Authority: 10 | 访问: public

[12] GPAC — MP4Box(1) 手册页（Ubuntu jammy 打包版） | https://manpages.ubuntu.com/manpages/jammy/man1/mp4box.1.html | Source-Type: official | As Of: unknown | Authority: 9 | 访问: public

[13] GPAC wiki — Fragmentation, segmentation, splitting and interleaving | https://wiki.gpac.io/Howtos/dash/Fragmentation%2C-segmentation%2C-splitting-and-interleaving/ | Source-Type: official | As Of: unknown | Authority: 9 | 访问: public

[14] GPAC wiki — MP4Box Overview | https://wiki.gpac.io/MP4Box/MP4Box/ | Source-Type: official | As Of: unknown | Authority: 8 | 访问: public

[15] ZoneMinder 项目 — FAQ（"How can I stop ZoneMinder filling up my disk?"，PurgeWhenFull 说明） | https://zoneminder.readthedocs.io/en/latest/faq.html | Source-Type: official | As Of: unknown | Authority: 9 | 访问: public

[16] ZoneMinder 项目 — `db/zm_create.sql.in`（master 分支，PurgeWhenFull 过滤器出厂定义原文） | https://raw.githubusercontent.com/ZoneMinder/zoneminder/master/db/zm_create.sql.in | Source-Type: official | As Of: unknown | Authority: 10 | 访问: public

[17] Frigate 项目 — Record 配置文档（含 retention 与存储耗尽时的 emergency cleanup） | https://docs.frigate.video/configuration/record | Source-Type: official | As Of: unknown | Authority: 9 | 访问: public

[18] Motion 项目 — Motion Configuration Options（`movie_*` 选项与默认值） | https://motion-project.github.io/motion_config.html | Source-Type: official | As Of: unknown | Authority: 9 | 访问: public

[19] Motion 项目 — `src/movie.cpp`（master 分支，容器/时间戳/开闭逻辑） | https://raw.githubusercontent.com/Motion-Project/motion/master/src/movie.cpp | Source-Type: official | As Of: unknown | Authority: 9 | 访问: public

[20] anthwlock（untrunc 维护 fork） — README | https://github.com/anthwlock/untrunc | Source-Type: official | As Of: unknown | Authority: 7 | 访问: public

[21] ponchio（untrunc 原始作者） — README | https://github.com/ponchio/untrunc | Source-Type: official | As Of: unknown | Authority: 7 | 访问: public

[22] ponchio/untrunc — Issue #294 "MP4 moov Reconstruction After Abrupt Camera Shutdown - 8 moov Fragments, Mostly Zeros in mdat" | https://github.com/ponchio/untrunc/issues/294 | Source-Type: community | As Of: unknown | Authority: 6 | 访问: public

[23] TechSmith/mp4v2 — 源码仓库（main 分支，我下载了完整 tarball 并检索全部头文件与 `src/`） | https://github.com/TechSmith/mp4v2 | Source-Type: official | As Of: unknown | Authority: 9 | 访问: public

[24] Shinobi 项目 — `libs/cron.js`（dev 分支，GitLab。抓到的 4.7 KB 版本里**没有**视频保留逻辑） | https://gitlab.com/Shinobi-Systems/Shinobi/-/raw/dev/libs/cron.js | Source-Type: official | As Of: unknown | Authority: 6 | 访问: public

[25] DeepWiki（Devin 生成的源码导读） — Shinobi / System Maintenance | https://deepwiki.com/moeiscool/Shinobi/7-system-maintenance | Source-Type: secondary-industry | As Of: unknown | Authority: 4 | 访问: public

[26] marcone/teslausb — README（行车记录仪场景的断电/文件系统损坏处理） | https://github.com/marcone/teslausb | Source-Type: community | As Of: unknown | Authority: 6 | 访问: public

[27] edunavajas/fix-corrupted-videos — README（从 mdat 抽裸 H.264 再重编码的修复脚本） | https://github.com/edunavajas/fix-corrupted-videos | Source-Type: community | As Of: unknown | Authority: 5 | 访问: public

[28] FFmpeg Trac — Ticket #6859 "2pass moov not written and block ffmpeg if RTMP input stream stops before" | https://trac.ffmpeg.org/ticket/6859 | Source-Type: official | As Of: unknown | Authority: 6 | 访问: public

---

## Findings

1. ffmpeg `segment` muxer 的关键默认值（官方文档 + 源码选项表一致）：`segment_time` 默认 **"2"（秒，源码里是 `2000000` 微秒）**、`segment_atclocktime` 默认 **0**、`reset_timestamps` 默认 **0**、`strftime` 默认 **0**、`individual_header_trailer` 默认 **true**、`segment_time_delta` 默认 **"0"**、`min_seg_duration` 默认 **"0"**、`segment_list_size` 默认 **0（=全部）**；给内层 muxer 传参的通道是 `-segment_format_options`（官方定义 "Set output format options using a `:`-separated list of `key=value` parameters. Values containing the `:` special character must be escaped."，官方例子 `-segment_format_options movflags=+faststart`）。[1][5]

2. **切段边界落在关键帧上、不丢帧但段会变长**：官方原文 "Every segment starts with a keyframe of the selected reference stream, which is set through the `reference_stream` option"，以及 "the segment muxer will start the new segment with the key frame found next after the specified start time"；想让段能切在非关键帧必须显式开 `break_non_keyframes 1`（默认 **0**）。[1]

3. fMP4 抗掉电的**官方理由原话**（不是社区说法）: fragmented 文件 "has the advantage that the file is decodable even if the writing is interrupted (while a normal MOV/MP4 is undecodable if it is not properly finished), and it requires less memory when writing very long files"；官方明确列出开启分片的 5 种方式 = `frag_duration` / `frag_size` / `min_frag_duration` / `movflags +frag_keyframe` / `movflags +frag_custom`。[1]

4. `movflags` 相关原文：`frag_keyframe` = "start a new fragment at each video keyframe"；`empty_moov` = "Make the initial moov atom empty"（源码选项描述）；`default_base_moof` = "avoids writing the absolute `base_data_offset` field in `tfhd` atoms... by using the new default-base-is-moof flag instead. This flag is new from 14496-12:2012"；`delay_moov` = "delay writing the initial moov until the first fragment is cut"；另外 `+empty_moov` 还有两个**官方副作用**：源码会关闭 automatic bitstream filtering（"Empty MOOV enabled; disabling automatic bitstream filtering"），且 `empty_moov` 不带 `delay_moov` 时告警 "No meaningful edit list will be written when using empty_moov without delay_moov"。[1][3]

5. **FFmpeg 7.1 起才有 `+hybrid_fragmented`**（`n7.1` 分支文档里有、`n7.0`/`n6.1` 里都没有）：写的时候是 fragmented（"allows the intermediate file to be read while being written (in particular, if the writing process is aborted uncleanly)"），写完再转成普通非 fragmented 文件；源码里它与 `+faststart` **互斥**，同时给会直接 `AVERROR(EINVAL)` 并打印 "Setting both hybrid_fragmented and faststart is not supported."。[2][3][7][8]

6. **开源监控项目的"卡满"清理策略都是"百分比/剩余时间阈值 + 删最旧"**：ZoneMinder 出厂过滤器 `PurgeWhenFull` 的 JSON 是 `{"sort_field":"Id","terms":[{"val":0,"attr":"Archived","op":"="},{"cnj":"and","val":95,"attr":"DiskPercent","op":">="},...],"limit":100,"sort_asc":1}`（即 **DiskPercent >= 95、按 Id 升序=删最旧、每次限 100 条、AutoDelete=1、Background=1**）；Frigate 则**不用固定百分比**，而是"剩余空间不足约 1 小时录像量（按当前码率估算）就删最旧的"且"removes the oldest recordings first regardless of retention settings"。[16][17]

7. 保留策略的可抄参数：Frigate 用三层 `record.continuous.days` / `record.motion.days`（**默认 continuous 关闭**）/ `record.alerts.retain.days` + `retain.mode`（`mode: motion` 是**默认**，另有 `all` / `active_objects`），官方示例是 continuous 3 天→motion 7 天→事件 30 天；motion 项目的分段参数是 `movie_max_time` 默认 **120 秒**（0=无限）、`movie_codec` 默认 **mkv**、`movie_bps` 默认 **400000**、`movie_output` 默认 **on**。[17][18]

8. **mp4v2 的公开 API 里根本没有分片（fMP4）能力，`moov` 是 `MP4Close()` 时才落盘的** —— 我下载了 TechSmith/mp4v2 完整源码，`include/mp4v2/*.h` 全部 14 个头文件中检索 `Fragment|moof|traf|mvex|tfhd` **零命中**；`src/mp4.cpp` 里只有两行未实现的 TODO `// LATER useExtensibleFormat, moov first, then mvex's`，`src/atom_root.cpp` 只是把 `moof` 标成 `ExpectChildAtom("moof", Optional, Many)`（**能读不能写**）；而 `file.h` 对 `MP4Close()` 的说明是 "MP4Close() will write out all pending information to disk"。[23]

9. 修复工具的现实边界与系统层手段：（a）untrunc 的官方 README 明确要求"另一段**同款相机**录的、没坏的文件"（"if not the chances to fix it are slim"），而 Issue #294 是一个 IPC 断电实例——21.4 GB 文件因掉电只写了部分 `moov`（文件里 8 处 "moov" 字样，健康文件有 130 处），**untrunc 只恢复出约 1.2 分钟**，ffmpeg 报 `moov atom not found`（该字符串在 FFmpeg 源码 `libavformat/mov.c` 中）；（b）`fallocate()` 默认模式下 "After a successful call, subsequent writes into the range specified by `offset` and `size` are guaranteed not to fail because of lack of disk space"，`FALLOC_FL_KEEP_SIZE` 用于 "optimizing append workloads"，`posix_fallocate()` 有同样保证但 glibc 在不支持 `fallocate(2)` 的文件系统上会退化成**低效且非 MT-safe 的模拟**，`fsync()` "blocks until the device reports that the transfer has completed" 且连 metadata 一起刷，`fdatasync()` 只刷后续读回数据所必需的 metadata。[6][9][10][11][20][21][22]

---

## Deep Read Notes

### 深读 1：FFmpeg 官方 `ffmpeg-formats.html` §4.4.2 Fragmentation + §4.4.3 movflags + §4.73 segment（全文读过）

**（a）分片抗掉电的原文（§4.4.2）**

> A fragmented file consists of a number of fragments, where packets and metadata about these packets are stored together. Writing a fragmented file has the advantage that **the file is decodable even if the writing is interrupted** (while a normal MOV/MP4 is undecodable if it is not properly finished), and it requires less memory when writing very long files (since writing normal MOV/MP4 files stores info about every single packet in memory until the file is closed).
> The downside is that it is less compatible with other applications.
> Fragmentation is enabled by setting one of the options that define how to cut the file into fragments: `frag_duration` / `frag_size` / `min_frag_duration` / `movflags +frag_keyframe` / `movflags +frag_custom`. If more than one condition is specified, fragments are cut when one of the specified conditions is fulfilled. The exception to this is the option `min_frag_duration`, which has to be fulfilled for any of the other conditions to apply.

**（b）可直接抄的 movflags 原文（§4.4.3，逐条摘）**

| flag | 官方原文 |
|---|---|
| `frag_keyframe` | "start a new fragment at each video keyframe" |
| `empty_moov` | "Make the initial moov atom empty"（movenc.c 选项表原文） |
| `default_base_moof` | "Similarly to the 'omit_tfhd_offset' flag, this flag avoids writing the absolute base_data_offset field in tfhd atoms, but does so by using the new default-base-is-moof flag instead. This flag is new from 14496-12:2012. This may make the fragments easier to parse in certain circumstances" |
| `delay_moov` | "delay writing the initial moov until the first fragment is cut, or until the first fragment flush" |
| `frag_discont` | "signal that the next fragment is discontinuous from earlier ones" |
| `frag_every_frame` | "fragment at every frame" |
| `skip_trailer` | "skip writing the mfra/tfra/mfro trailer for fragmented files" |
| `hybrid_fragmented` | "For recoverability - write the output file as a fragmented file. This allows the intermediate file to be read while being written (in particular, if the writing process is aborted uncleanly). When writing is finished, the file is converted to a regular, non-fragmented file, which is more compatible and allows easier and quicker seeking. If writing is aborted, the intermediate file can manually be remuxed to get a regular, non-fragmented file of what had been written into the unfinished file." |
| `moov_size`（非 flag） | "Reserves space for the moov atom at the beginning of the file instead of placing the moov atom at the end. **If the space reserved is insufficient, muxing will fail.**" |

**（c）segment muxer 可直接抄的选项（§4.73，官方默认值）**

```
increment_tc 1|0              default 0
reference_stream specifier    default auto
segment_format format         默认按文件名后缀猜
segment_format_options        ":-separated list of key=value"，含 ':' 的值要转义
segment_list_size size        default 0（0 = 列表里保留全部段）
segment_time time             default "2"      ← 注意单位是秒
min_seg_duration time         default "0"，且只在配了 segment_time 时有效
segment_atclocktime 1|0       default "0"
segment_clocktime_offset      default "0"
segment_time_delta delta      default "0"；判据是 PTS >= start_time - time_delta
segment_wrap limit            （环形覆盖用得上：段号到 limit 后回绕）
segment_start_number          default 0
strftime 1|0                  default 0
break_non_keyframes 1|0       default 0
reset_timestamps 1|0          default 0
write_empty_segments 1|0      default 0
write_header_trailer bool     default true
individual_header_trailer bool default true  ← 每个段都是"独立可播文件"的关键
segment_header_filename name  设置它会把 header 只写一次到单独文件
```

官方在 §4.73 结尾还有一句直接相关的警告：

> Make sure to require a closed GOP when encoding and to set the GOP size to fit your segment time constraint.

官方示例里与"分段时给内层 mp4 传参"直接相关的一条：

```
ffmpeg -i in.mkv -f segment -segment_time 10 \
       -segment_format_options movflags=+faststart out%03d.mp4
```

### 深读 2：ZoneMinder `db/zm_create.sql.in` 里的 PurgeWhenFull 出厂定义（逐字抄）

这是 ZoneMinder 新装数据库时插入的默认过滤器（`INSERT INTO Filters ... VALUES` 的 `Query_json` 字段），可直接当作"磁盘阈值清理"的参数模板：

```json
{
  "sort_field": "Id",
  "terms": [
    {"val": 0,   "attr": "Archived",     "op": "="},
    {"cnj": "and", "val": 95,  "attr": "DiskPercent", "op": ">="},
    {"cnj": "and", "obr": "0", "attr": "EndDateTime", "op": "IS NOT", "val": "NULL", "cbr": "0"}
  ],
  "limit": 100,
  "sort_asc": 1
}
```

同一行的其余字段：`AutoDelete = 1`、`Background = 1`、`AutoArchive/AutoVideo/AutoUpload/AutoEmail/AutoMessage/AutoExecute/AutoMove/AutoCopy/UpdateDiskSpace = 0`、`UserId = 1`。

配套的官方 FAQ 说明（[15] 原文要点）：

- 这个过滤器"automatically enabled if you do a fresh install"，但**从旧版本升级上来的数据库会保留旧设置，可能是 disabled**，要手动确认。
- 可以改两处：**"the percentage full you want it to kick in"** 和 **"how many events to delete at a time (it will repeat the filter as many times as needed to clear the space, but will only delete this many events each time to get there)"** —— 这正好对应 JSON 里的 `val: 95` 和 `limit: 100`。
- **只作用于默认存储位置**："If you have added other storage areas, you must create a PurgeWhenFull filter for each one"。
- 必须勾上 **"Run filter in background"**，并且要去看 `zmfilter.log` 确认它真的在跑（"sometimes missing perl modules mean that it never runs but people don't always realize"）。
- 性能相关的官方建议：**"Optional slow delete: limit the number of results"** —— 积压太多时限制每次删的条数，把删除开销摊开，避免 CPU 长时间飙高。

### 深读 3：mp4v2 源码 —— 为什么"正在写的那段缺 moov"是**结构性**的（读遍 `include/mp4v2/*.h` 与 `src/`）

我下载并解压了 `TechSmith/mp4v2` 的完整源码（4.3 MB tarball），得到以下可直接引用的结论：

1. **公开 API 完全没有分片能力。** 对 `include/mp4v2/` 下全部头文件（`chapter.h file.h file_prop.h general.h isma.h itmf_generic.h itmf_tags.h mp4v2.h platform.h project.h sample.h streaming.h track.h track_prop.h`）检索 `Fragment|moof|traf|mvex|tfhd`：**零命中**。也就是说用 mp4v2 写文件的人，**没有任何 API 可以让它按 fragment 落盘**。

2. **`moov` 是 close 时才写的。** `file.h` 对 `MP4Close()` 的原文注释：

   > "MP4Close closes a previously opened mp4 file. If the file was opened writable with MP4Create() or MP4Modify(), then **MP4Close() will write out all pending information to disk**."

   配合 `MP4Create()` 的注释可以确认语义：`MP4Create()` 之后立刻 `MP4Close()` 也会产出一个合法文件——说明 `moov`（以及所有 sample 索引）是在 close 阶段统一写出的。**掉电时 `MP4Close()` 没跑完 ⇒ `moov` 不完整 ⇒ 文件播不了。**

3. **分片相关代码只有"读"，没有"写"。** `src/atom_root.cpp:41`：`ExpectChildAtom( "moof", Optional, Many );`（仅表示解析时允许出现 moof）；`src/atom_standard.cpp:243/252/254` 也只是把 `moof`、`mvex` 注册成可解析的 atom 类型。

4. **上游自己标了未实现的 TODO。** `src/mp4.cpp` 第 176 行与第 208 行（分别在 `MP4Create()` 与 `MP4Modify()` 里）各有一行：

   ```cpp
   // LATER useExtensibleFormat, moov first, then mvex's
   ```

   即"以后再做可扩展格式（moov 在前 + mvex）"——截至抓取到的 main 分支，**没做**。

5. 与之对照，`MP4Optimize()`（`file.h`）是"读一个已有 mp4、写出一个新的"（事后把 moov 挪到前面），它是**离线重写**，不是写入期的抗掉电手段。

**这一节对"我们自己写 mp4v2 封装"的直接含义**（仅陈述事实，不给方案决策）：想要"掉电后正在写的那段仍可播"，靠 mp4v2 现有 API 做不到；可行路径只有三条——换一个能写 fMP4 的 muxer、或者自己做"预分配 + 定时把 moov 落盘"、或者**不追求那一段可播**而是保证"已闭合的段"可播 + 可修复。

---

## Gaps

### A. 搜了但**没找到**的

1. **FFmpeg 官方没有一份"缺 moov 的 MP4 能不能救"的说明文档。** 官方侧我能拿到的只有 `libavformat/mov.c` 里 `av_log(s, AV_LOG_ERROR, "moov atom not found\n")` 这一行源码（[6]），以及 Trac #6859 里"写入侧 moov 没写成"的 bug 讨论（[28]）。**"能不能救、救回多少"没有官方结论**，只有社区工具和个案。
2. **`recover_mp4` 的官方页面没打开。** `https://www.videohelp.com/software/recover-mp4-to-h264` 返回 **503 + 人机验证页**（"Human check! Are you a human?"），拿不到任何正文。我只在 untrunc Issue #294 的用户描述里看到这个工具名（[22]），**没有独立核实它的能力与限制**，因此本笔记不给它任何参数或结论。
3. **GPAC 官方文档里没有一句话明确写"fragmented MP4 掉电后仍可播"。** GPAC wiki 关于 moof 的最接近表述是 "Movie Fragments is a tool introduced in the ISO spec **to improve recording of long-running sequences**"（[13]）——这是间接表述，不能等同于"掉电可播"的保证。**做到"掉电可播"这句断言的官方来源只有 FFmpeg（[1] 的 §4.4.2）。**
4. **Blue Iris 官方文档（`https://blueirissoftware.com/help/`）与 Agent DVR 官方文档页都返回 404**，因此**这两个产品本节零信息**，不写任何配置项名或默认值。
5. **Shinobi 的"视频保留天数"没能在官方一手来源核实。** 官方页面 `https://docs.shinobi.video/` 抓到的是纯导航结构（无正文）；GitLab dev 分支的 `libs/cron.js` 我只拿到 4.7 KB 且里面只有 `cloudDiskUse...maxDays` 相关分支，**没有** 视频保留逻辑（[24]）。DeepWiki 那页声称 `details.days` 视频默认 **5 天**、`event_days`/`log_days`/`fileBin_days` 默认 **10 天**、函数名 `s.deleteOld()` / `checkFilterRules()` / `config.cron.deleteOld`、每相机 `max_keep_days`（[25]）——但 **DeepWiki 是 AI 生成的源码导读，权威性只有 4/10，我没能在一手源码里对上这些函数名与默认值，所以这组数字不进 Findings**，只作为"待核实线索"。
6. **"录像文件预分配"的开源先例没找到可信一手来源。** 我搜到了 `github.com/sharow/preallocate` 之类的项目链接，但**没有打开核实**（不满足"真的打开过"的门槛），因此不列为本笔记的来源，也不给结论。FFmpeg 侧最接近的、**有官方原文**的手段是 `moov_size`（预留 moov 空间）与 `hybrid_fragmented`（见 Findings 5、深读 1）。
7. **"卡满正好发生在段中间"这件事，没找到任何一个开源监控项目专门处理它的代码/文档。** ZoneMinder 是"过滤器周期性检查 DiskPercent"、Frigate 是"连续监控剩余空间"（[16][17]），都是**周期性轮询**语义，不是"写每帧前先检查该段剩余配额"。行车记录仪侧我只找到 teslausb，它的公开说明是"automatically repair filesystem corruption produced by the Tesla's current failure to properly dismount the USB drives before cutting power"（[26]）——处理的是**文件系统损坏**，不是段中间写满。
8. 未核实：**Hi3516/板端工具链里 ffmpeg 的实际版本**。这一点很关键，因为 `hybrid_fragmented` 只在 **7.1+** 存在（Findings 5），旧版本用不了。我没有上板核对，故不写结论。

### B. 互相**矛盾**或**需要小心**的说法

1. **`hybrid_fragmented` 的版本门槛（矛盾的具体形态）**：`n7.1` 分支的 `doc/muxers.texi` 里有这个 flag，`n7.0` 与 `n6.1` 分支的同一文件里**都没有**（[7]）——所以"ffmpeg 支持 hybrid_fragmented"这句话**必须带版本号**，否则会误导。同时 [8] 的邮件列表显示该 patch 于 **2024-06-24** 才被 push，与 7.1 的时间线吻合。**任何"某台机器上的 ffmpeg 能不能用这个选项"的判断，都必须以那台机器的实际版本为准。**
2. **fMP4 的兼容性代价**：FFmpeg 官方同时说了两件方向相反的话——fragmented "the file is decodable even if the writing is interrupted"（好处），但 "The downside is that it is less compatible with other applications"；而 `hybrid_fragmented` 的文档又说非 fragmented 版本 "is more compatible and allows easier and quicker seeking"（[1][2]）。**这不是文档打架，而是同一枚硬币的两面**：抗掉电 ↔ 兼容性/可 seek 性，两个目标不能同时最大化。记录在此以免被误读成"fMP4 全面更好"。
3. **`segment_time` 的"按时切"是近似**：官方原文先说 "This muxer outputs streams to a number of separate files of **nearly** fixed duration"，又说 "Note that splitting may not be accurate, unless you force the reference stream key-frames at the given time"（[1]）。**"设了 `segment_time 60` 就一定是 60 秒一段"是错的**；持续时间取决于 GOP 结构。
4. **untrunc 的"能修"与"修回多少"是两件事**：[20][21] 的 README 给的是积极版本（"hopefully produce a playable file"），而 [22] 的 21.4 GB 实例只修回约 1.2 分钟。**同一工具在不同文件上结果差两个数量级**，不存在"untrunc 能救断电 MP4"这种一般性结论。
5. **`fsync` 的成本**：`fsync(2)` 官方说它 "blocks until the device reports that the transfer has completed"，而 `fdatasync()` "is aimed to reduce disk activity"（[11]）。**"多调 fsync 更安全"没有官方背书**——官方只说明了语义差异，**没有给任何"每个 segment 该不该 fsync、该调哪个"的推荐频率**。这个取舍我在本次调研中**没有找到权威依据**。

### C. 尝试过但打不开的 URL（不计入 Sources）

| URL | 结果 |
|---|---|
| `https://zoneminder.readthedocs.io/en/stable/userguide/filters.html` | HTTP 404 |
| `https://docs.frigate.video/configuration/reference` 与 `.../reference/` | HTTP 404 |
| `https://blueirissoftware.com/help/` | HTTP 404 |
| `https://www.ispyconnect.com/userguide-agent-dvr.aspx` | HTTP 404 |
| `https://www.videohelp.com/software/recover-mp4-to-h264` | HTTP 503（人机验证拦截） |
| `https://wiki.gpac.io/Howtos/dash/MP4Box-DASH/` | HTTP 404（SPA 回退页） |
| `https://raw.githubusercontent.com/gpac/gpac-wiki/.../Fragmentation.md` | HTTP 404（该路径不存在） |
| `https://codeload.github.com/TechSmith/mp4v2/tar.gz/master` | HTTP 404（该仓库默认分支是 `main` 不是 `master`；换 `main` 后成功） |
| `https://raw.githubusercontent.com/Motion-Project/motion/master/src/movie.c` | HTTP 404（实际文件名是 `src/movie.cpp`，已抓到） |
| `https://api.github.com/repos/Motion-Project/motion/contents/src` | API 限流（改用 jsDelivr 文件清单成功） |
