# IPC Camera —— 海思 Hi3516DV300 网络监控系统

推流端:Hi3516DV300 + 海思 MPP(VI→VPSS→VENC)+ 自研 RTSP/RTP 服务端 + MP4 分段录制
回放端:**设备自带的 HTTP 回放服务**(浏览器页面 / VLC 播放列表 / REST 接口)

> **当前进度 / 关键决策 / 已知问题 → [`docs/STATUS.md`](docs/STATUS.md)**
> 规划与验收标准 → [`docs/PROJECT_PLAN.md`](docs/PROJECT_PLAN.md) ·
> 架构 → [`docs/ARCHITECTURE.md`](docs/ARCHITECTURE.md) ·
> 代码规范 → [`docs/CODING_STYLE.md`](docs/CODING_STYLE.md) ·
> 行业调研笔记(133 条来源) → [`docs/research/`](docs/research/)

---

## 一、硬件与系统

| 项 | 值 |
|---|---|
| 芯片 | HiSilicon Hi3516DV300(双核 Cortex-A7 @900MHz) |
| 摄像头 | GC2053,接在 **sensor1 / MIPI1 / i2c-1** |
| 内核 | Linux 4.9.37 SMP |
| 存储 | SPI NOR 32MB(boot 1M / kernel 4M / rootfs 27M) |
| TF 卡 | 32GB,挂载在 `/mnt/sdcard` |
| 调试 | 串口 115200 + telnet(root/空密码) |

## 二、开发环境

```
Ubuntu 22.04 (192.168.16.100)
  ├── 交叉工具链  /opt/hisi-linux/toolchain/arm-himix200-linux/arm-himix200-linux/bin
  ├── MPP SDK     ~/hi3516_sdk/Hi3516CV500_SDK_V2.0.2.0/smp/a7_linux/mpp
  ├── mp4v2       ~/mp4v2-arm   (自编译,ARM 静态库)
  └── NFS 共享    ~/nfs_share   →  板子 /mnt/nfs

开发板 (192.168.16.88)
  └── 从 /mnt/nfs 直接运行新程序(免烧写)
```

---

## 三、软件架构

### 3.1 数据流

```
   GC2053 摄像头
        │  MIPI CSI (2 lane, RAW10)
        ▼
   ┌─────────┐
   │   VI    │  Video Input   —— 采集
   └────┬────┘
        ▼
   ┌─────────┐
   │  VPSS   │  Video Process Sub-System —— 缩放/降噪,分出多路
   └────┬────┘
        ├──────────────┐
        ▼              ▼
   ┌─────────┐   ┌─────────┐
   │ VENC ch0│   │ VENC ch1│   编码器
   │  H.265  │   │  H.264  │
   │ 1080p30 │   │  720p30 │
   └────┬────┘   └────┬────┘
        └──────┬──────┘
               ▼
   ┌───────────────────────┐
   │  NALU 解析(Annex-B)  │  剥离起始码,切出裸 NALU
   └──────────┬────────────┘
              ▼
   ┌───────────────────────┐
   │  发送队列(环形缓冲)  │  收/发解耦,不阻塞编码器
   └──────────┬────────────┘
              ├──────────────────┐
              ▼                  ▼
   ┌─────────────────┐   ┌─────────────────┐
   │ RTP 打包        │   │ MP4 录制        │
   │ RFC 6184 FU-A   │   │ mp4v2 + 环形覆盖│
   └────────┬────────┘   └────────┬────────┘
            ▼                     ▼
   ┌─────────────────┐   ┌──────────────────────────┐
   │ UDP / TCP 交错  │   │ TF 卡上的分段文件         │
   └────────┬────────┘   └────────┬─────────────────┘
            ▼                     ▼
      PC 拉流端            HTTP 回放服务(8080)
      (Qt + FFmpeg)        列分段 / Range 取流 / 锁定 / 播放列表
                                  │
                                  ▼
                       客户端**任选**:浏览器(板子自带页面)
                       · VLC/mpv(playlist.m3u) · 自研 C 客户端
```

### 3.2 线程模型

```
主线程       : 解析参数 → 按序启动各服务 → 每秒汇报 → 收到信号后**反序**停

ipc_media    : MPP 取流(VI→VPSS→VENC),把每帧**扇出**到「发送队列」和「录制队列」
ipc_send     : 从发送队列取帧 → RTP 打包 → 发给所有 PLAY 中的客户端(UDP / TCP 交错)
ipc_net      : epoll 事件循环:accept + 解析 RTSP 请求 + 回响应
ipc_rec      : 从录制队列取帧 → mp4v2 写分段 → 收尾 rename / 环形覆盖 / 启动救残留裸流
ipc_osd      : 1 Hz 把时间渲染成位图并叠加到 VENC
ipc_http     : HTTP 回放服务(串行处理客户端,见 docs/ARCHITECTURE.md 的 ADR-5)
```

**为什么这样分?**
- **取流线程绝不阻塞**:VENC 缓冲有限,若被网络/磁盘阻塞会丢帧 → 用**两条独立队列**解耦
  (磁盘慢只丢录制的帧,网络慢只丢发送的帧)
- **epoll 而非 select**:无 `FD_SETSIZE` 上限,就绪通知 O(1)
- **发送 / 录制 / 回放各自一线程**:慢的环节只能拖累自己那一路,拖不动取流与直播

---

## 四、代码结构

| 文件 | 职责 |
|---|---|
| `src/main.c` | MPP 初始化 + 线程编排 |
| 层 | 文件 | 职责 |
|---|---|---|
| `protocol/` | `proto_nalu.c` | **Annex-B 解析**:剥离起始码、识别 NALU 类型(含 IDR 判定) |
| | `proto_rtp.c` | **RTP 打包**:RFC 6184(H.264)/ RFC 7798(H.265),含 FU-A 分片 |
| | `proto_rtsp.c` | **RTSP** 请求/响应解析与构造(含 `Transport` 交错模式) |
| | `proto_sdp.c` | **SDP** 生成(`sprop-parameter-sets` 等) |
| | `proto_str.c` | 字符串工具(大小写无关比较、安全拼接) |
| `infra/` | `infra_queue.c` | 线程安全环形队列:**队满丢最旧, 永不阻塞** |
| | `infra_poll.c` | epoll 事件循环封装 |
| | `infra_netio.c` | 网络读写 + **RTP over TCP 交错(interleaved)发送** |
| | `infra_log.c` | 分级日志 |
| `service/` | `svc_media.c` | 取帧 → **扇出到两条独立队列**(发送 / 录制) |
| | `svc_net.c` | RTSP 会话与客户端管理(跑在 epoll 线程里) |
| | `svc_sender.c` | 发送线程:按客户端状态发 RTP(等到 IDR 才开始发) |
| | `svc_record.c` | 录制线程:帧 → mp4v2 → 分段 MP4(**边界对齐 IDR**) |
| | `svc_record_policy.c` | **纯函数**:分段命名 + 环形覆盖决策(PC 单测 23 条断言) |
| | `svc_osd.c` | OSD 时间水印(每秒更新) |
| `bsp/` | `bsp_mpp.c` | 海思 MPP:VI→VPSS→VENC 初始化与取流 |
| | `bsp_osd.c` / `bsp_osd_render.c` | REGION 模块叠加 + 点阵字库渲染 |
| `app/` | `app_main.c` | 起停链路 + 信号处理(优雅关闭)+ 运行状态上报 |

> 共 38 个源文件, 分 5 层(**单向依赖**:`app → service → bsp/infra → protocol`;
> `protocol` 层不碰硬件、不碰 `malloc`, 因此可在 PC 上原生单测)。

---

## 五、里程碑

> 当前详细状态、验收证据与已知问题见 [`docs/STATUS.md`](docs/STATUS.md)。

| # | 内容 | 状态 |
|---|---|---|
| **M0** | 采集→编码跑通,产出标准 H.264/H.265 | ✅ 已完成 |
| **M1** | RTSP/RTP 服务端,PC VLC 实时播放 | ✅ 已完成(UDP + **RTP over TCP 交错**, 多客户端已实测) |
| **M2** | OSD 叠加时间(REGION) | ✅ 已完成(板子实测) |
| **M3** | MP4 录制 + SD 卡环形覆盖 | ✅ 已完成(验收 **A4~A16**:掉电可救 / 盘满不停 / 按时间或大小分段 / 锁定段保护) |
| **M4** | PC 拉流端(实时预览 + 回放) | 🔄 **回放已通(A17/A18/A19)**:浏览器页面(自刷新 + "正在录"横幅)/ VLC 播放列表 / REST 接口 + `/status`;自研 C 客户端(SDL2+FFmpeg)与**实时画面进浏览器**为下一步 |

---

## 六、编译与运行

### 6.1 交叉编译(给板子)

需要三样**仓库外**的东西(体积与许可原因没进仓库), 用变量指过去即可:

| 变量 | 是什么 | 默认值 |
|---|---|---|
| `SDK_DIR` | 海思 Hi3516CV500 SDK(头文件 + 静态库 + `sample/common`) | `$HOME/hi3516_sdk/Hi3516CV500_SDK_V2.0.2.0` |
| `MP4V2_DIR` | 自己交叉编译的 mp4v2 静态库(见"二、开发环境") | `$HOME/mp4v2-arm` |
| `CROSS_COMPILE` | 交叉工具链前缀(含结尾 `-`) | `/opt/hisi-linux/toolchain/arm-himix200-linux/arm-himix200-linux/bin/arm-himix200-linux-` |

```bash
make                     # → build/ipc_app
# 路径不同就覆盖变量:
make SDK_DIR=/opt/Hi3516CV500_SDK_V2.0.2.0 MP4V2_DIR=$HOME/mp4v2-arm \
     CROSS_COMPILE=/opt/toolchain/bin/arm-himix200-linux-
make help                # 看当前生效的变量
```

> ⚠️ 链接时**所有静态库(含系统库)必须在同一个 `--start-group` 里** —— 厂商库之间有
> 循环依赖, 分组顺序错了会报"库明明给了却未定义符号"(踩过, 见项目 bug log B021)。
> ⚠️ mp4v2 的头文件是 **C++** 的, 所以**故意不把它的 `include/` 加进 `-I`**,
> 我们用自己的垫片 `src/service/svc_record_mp4.h`。

### 6.2 PC 原生单测(**不需要 SDK, 也不需要板子**)

```bash
make test                                 # 跑到 7 条;另外 2 条要裸流样本, 会**显式跳过**
make test RAW_STREAM=stream_chn1.h264     # 9 条全跑(样本 = 任意一段 Annex-B 裸流)
```

`protocol` / `infra` / 录制策略层都不碰硬件, 所以用**宿主 gcc** 就能编译并跑断言:
拿到仓库的人不用买板子也能验证一半代码。任何一条断言失败, `make` 就以非 0 退出(可直接进 CI)。

### 6.3 上板运行

```bash
cp build/ipc_app ~/nfs_share/                # 产物拷到 NFS 共享
# 板子上(必须先 cp 到 /tmp, 见下方坑):
cp /mnt/nfs/ipc_app /tmp/ && chmod +x /tmp/ipc_app
/tmp/ipc_app -p 8554 -s 1800 -m 1024 -r 8192
```

> ⭐ **实时画面也不用装东西(MJPEG, 默认 8081 端口)**:页面上的「看实时」按钮会把
> `http://<板子IP>:8081/live.mjpg` 塞进一个 `<img>` —— 浏览器原生支持
> `multipart/x-mixed-replace`, 一个分片一张 JPEG。板子侧用的是芯片的**硬件 JPEG 编码
> 通道**, 而且**按需启停**:第一个观看者来了才建通道, 最后一个走了就拆掉
> (没人取流的编码通道会把流水线拖死 —— B027 的老教训)。
> 实测(上板 A22 复测):**640x360 @ 17.2 fps, 每帧约 15.3 KB ⇒ ~2.1 Mbps**
> (目标帧率由 `-live-fps` 给, 默认 15;质量 `-live-q`, 默认 80);
> 同一时间窗主路仍是 **31.5 fps**、录像照常、回放服务照常。
> ⚠️ 实时画面那一路上**没有时间水印**(水印挂在主编码通道上, MJPEG 是另一路通道);
> ⚠️ 客户端太慢会被断开(浏览器会自动重连), 上限 4 个观看者。

> 常用选项:`-s <秒>` 每段时长(默认 1800)、`-m <MB>` 每段**大小上限**(默认 1024,
> 与 `-s` 谁先到算谁)、`-r <MB>` 环形容量上限(默认 8192, ⚠️ **必须大于一段**)、
> `-d <目录>` **录制目录**(默认 `/mnt/sdcard`)—— ⚠️ **必须是已挂载的目录**:
> 不是挂载点(比如 TF 卡没挂上, 那时 `/mnt/sdcard` 只是 rootfs 里的一个空目录)
> 程序会**拒绝录制**并报错, 但推流/回放照常 —— 因为往那儿写等于**写板子的 flash**;
> `-H <端口>` **回放服务端口**(默认 8080)、`-no-http` 不起回放服务、
> `-L <端口>` **实时画面(MJPEG)端口**(默认 8081)、`-no-live` 不起实时画面、
> `-live-fps <n>` 实时帧率(默认 15)、`-live-q <n>` 实时 JPEG 质量(默认 80)、
> `-no-record` 不录、`-no-raw` 不写旁路裸流侧车、`-no-osd` 不叠水印、
> `-h265` 取 H.265 那一路(mp4v2 不支持 H.265 封装, 该模式下**录制会自动关闭**)。
>
> ⭐ **长选项两种横线都认**:`-no-record` 与 `--no-record` 等价(`-h` 仍是短选项帮助)。
> ⚠️ 这条是 2026-09-20 才真正做到的(B045):B040 那次只把 `getopt()` 换成 `getopt_long()`,
> 而 glibc 的 `getopt_long()` **只认双横线** ⇒ 文档里写的单横线写法**一直都没生效过**。
> 现在解析前先把已知名字的单横线写法改写成双横线(见 `app_main.c` 的 `normalize_long_opts()`)。
>
> ⭐ **`-s` 不只是"切文件"**:它同时决定"**最新画面要等多久才能回放**" ——
> 正在写的那一段在收尾前对客户端不可见(下节 6.4 有解释)。演示时建议 `-s 300`。
>
> **锁定段**(重要录像不被覆盖):往录制目录里的 `.locked` 写文件名, 一行一个;
> 被锁的分段**环形覆盖会跳过**, 清理会继续删下一个最旧的。全被锁定时会明确报错但不停录。

> ⚠️ **NFS 执行坑**:直接从 `/mnt/nfs` 执行刚生成的二进制可能报
> `No such file or directory`(NFS 属性缓存)。**先 `cp` 到 `/tmp` 再运行。**

### 6.4 回放(HTTP,阶段 2)

板子同时提供一个小 HTTP 服务(默认 **8080**,`-H <端口>` 改、`-no-http` 关),形状是
"**设备给 URI、客户端原样回填**":

| 接口 | 说明 |
|---|---|
| `GET /` | **回放页面**(浏览器直接当客户端用,**零安装**):分段列表 + 播放 + 锁定/另存 + 时间轴 + "● 正在录"横幅,**每 5 秒自刷新** |
| `GET /status` | **正在录**的那一段:名字 / 已写字节 / 帧数 / 起始时间 / 直播地址(JSON) |
| `GET /recent.mp4` | **"刚录的这段"立刻能看**:请录制线程把当前段收尾(≈1 秒), 再把**刚收尾的整段**发给你(支持 `Range`);应答头 `X-Recent-Clip` 告诉你拿到的是哪一段;没在录 ⇒ 503 |
| `GET /status` | 里面还带一个 **`live_mjpg`** 字段 = 实时画面的 URL(页面用它显示 `<img>`) |
| `GET /playlist.m3u` | **整段回放播放列表**(旧→新)—— VLC/mpv 打开它就能"一整天连着看" |
| `GET /help` | 纯文本帮助(curl / 终端友好) |
| `GET /recordings` | 列出现有分段(JSON,**新→旧**),默认**最新 64 条**,支持 `?limit=&before=` 翻页 |
| `GET /recordings/<名字>` | 取流,**支持 `Range: bytes=…`** ⇒ `206` + `Content-Range`;不存在 ⇒ 404 |
| `HEAD /recordings/<名字>` | 只取头(播放器常用来探大小) |
| `PUT /recordings/<名字>/lock` | body `1` = 锁定、`0` = 解锁(不写 body 默认锁定);锁定后**环形覆盖不会删它** |

```bash
# ① 浏览器(推荐先试这个, 什么都不用装)
用浏览器打开  http://<板子IP>:8080/

# ② 播放器:一整天连着回放 + 可拖进度条
vlc  http://<板子IP>:8080/playlist.m3u
mpv  http://<板子IP>:8080/playlist.m3u

# ③ 手工 / 脚本
curl -s http://<板子IP>:8080/status                    # 正在录的那一段(没在录时 recording=0)
curl -sI http://<板子IP>:8080/recent.mp4                # "刚录的这段"立刻能看(看 X-Recent-Clip)
ffplay http://<板子IP>:8080/recent.mp4                 # 同上, 直接播(会先收尾当前段)
curl -s http://<板子IP>:8080/recordings                # 列分段(JSON, 最新 64 条)
curl -s 'http://<板子IP>:8080/recordings?before=<当前页最后一条>&limit=64'   # 翻到更早
curl -r 0-1023 http://<板子IP>:8080/recordings/<名字> -o head.bin   # 取前 1 KB
ffplay http://<板子IP>:8080/recordings/<名字>           # 直接播(拖动走 Range)
curl -X PUT  http://<板子IP>:8080/recordings/<名字>/lock        # 锁定
curl -X PUT -d 0 http://<板子IP>:8080/recordings/<名字>/lock    # 解锁
```

> ⭐ **"刚录的这段"也能看了(`/recent.mp4`)**:它请录制线程在**下一个关键帧**处把当前段
> **收尾**(复用已有的切段路径 ⇒ 一帧不丢、可独立解码), 然后把这刚收尾的整段发出去 ——
> **缺口从"段长"降到 1 秒左右**, 页面上的"看最新"按钮就是它。
> 代价如实说:每请求一次**多一个分段边界**;所以刚开的段(< 20 秒)不会被切,
> 这时直接给你上一段。这个请求还会**占住服务线程最多 5 秒**(串行模型)。

> ⭐ **"最新那几分钟看不到"是设计行为, 不是坏了** —— 正在写的段叫
> `<时间戳>.mp4.tmp`:MP4 的索引(`moov`)只在**关文件时**才写, 没索引的文件给播放器也
> 打不开, 所以它**不出现在列表里**(行业同样如此, Axis 叫 `recording.tmp`)。
> 缺口大小 = **段长**:默认 1800 秒 ⇒ 最多落后半小时;想看得更"实时"就把段长调短
> (板上现在跑的是 `-s 300`, 最多落后 5 分钟)。**当前进度**去 `GET /status` 看
> (它如实报出 `.tmp` 写了多少字节、多少帧、从几点开始)。
> 想让"刚录的这段"也能立刻看, 需要做 `/recent.mp4`(把旁路裸流尾部重封成小文件),
> 目前**未实现**, 见 `docs/STATUS.md` 技术债第 9 条。

**客户端路线是可以换的**(这是有意的取舍 —— 不该把项目最有价值的部分绑死在某个 GUI 框架上):

| 路线 | 成本 | 说明 |
|---|---|---|
| ✅ **浏览器**(板子自己发页面) | **零安装** | 真实监控产品的客户端就是浏览器;页面调的就是上面那几个 REST 接口 |
| ✅ **VLC / mpv**(播放列表) | 零代码 | 证明"服务端不挑客户端";拖动进度条走我们的 `Range` |
| ⬜ **自研 C 客户端**(SDL2 + FFmpeg,可选 ImGui) | 中等 | 想讲"解复用→解码→YUV 上屏"这条链路时再做;比 Qt 依赖小得多 |
| ⬜ **接第三方开源 NVR**(Frigate / ZoneMinder / Agent DVR) | 零代码 | 证明设备**标准可接入**;它们的回放读自己的盘, 不读板子的分段 |
| ❌ Qt + FFmpeg(原计划) | 大 | 框架本身要学一遍;对嵌入式岗的加分不如"协议+解码"本身 |

> ⚠️ **没有鉴权** —— 任何能连上这个端口的人都能下载全部录像、改锁定清单。
> 定位是**局域网里的开发板演示**,不适合公网。
> ⚠️ 服务是**串行**的:一个正在下载大分段的客户端会占住它几十秒(见 `docs/ARCHITECTURE.md` 的 ADR-5)。
> ✅ 但"客户端不读数据"**不会**卡死服务:超时会主动丢掉那个连接(验收 A17 第 ⑨ 条)。
> ⚠️ 播放列表里每段的时长写的是 `-1`(未知):要报准确时长得解析每个 MP4 的 `mvhd`,
> 而我们不存这个信息 —— **宁可写"未知"也不写假数字**。
> ⚠️ 页面和列表响应都带 `Cache-Control: no-store`,分段文件带 `no-cache`:否则浏览器会
> 拿缓存,出现"明明在录、页面却纹丝不动"的假象(B044 排查中的一段弯路)。
> ℹ️ `curl -I http://<板子IP>:8080/recordings`(**HEAD 列表**)会得到 **405** ——
> 列表只允许 GET,想看它请去掉 `-I`;单段文件的 `HEAD` 是支持的。

### 6.5 验证(RTSP)

```bash
# 用 VLC 或 ffplay 打开(端口要与 -p 一致; 注意本机防火墙要放行入站 UDP)
ffplay  -rtsp_transport udp rtsp://192.168.16.88:8554/live
# 或用 ffprobe 看编码参数
ffprobe -v error -rtsp_transport udp -i rtsp://192.168.16.88:8554/live \
        -show_entries stream=codec_name,width,height
```
