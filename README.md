# IPC Camera —— 海思 Hi3516DV300 网络监控系统

推流端:Hi3516DV300 + 海思 MPP(VI→VPSS→VENC)+ 自研 RTSP/RTP 服务端
拉流端:PC(Qt + FFmpeg)

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
   └────────┬────────┘   └─────────────────┘
            ▼
   ┌─────────────────┐
   │ UDP sendto      │
   └────────┬────────┘
            ▼
      PC 拉流端 (Qt + FFmpeg)
```

### 3.2 线程模型

```
线程 1: MPP 主流程
        VI/VPSS/VENC 初始化 → 启动下面两个线程 → 等待退出信号

线程 2: epoll 事件循环 (net_event_loop)
        ├─ VencFd 就绪   → GetStream → NALU 解析 → 投递队列 → ReleaseStream
        ├─ listen fd 就绪 → accept 新客户端
        └─ client fd 就绪 → 解析 RTSP 请求 → 回响应

线程 3..N: 发送线程 (rtp_sender)
        从队列取 NALU → RTP 打包 → sendto 所有 PLAY 状态的客户端
```

**为什么这样分?**
- **取流线程绝不阻塞**:VENC 缓冲有限,若被网络阻塞会丢帧 → 用队列解耦
- **epoll 而非 select**:无 `FD_SETSIZE` 上限,就绪通知 O(1)
- **RTP 打包独立线程**:UDP `sendto` 也可能短暂阻塞,不能拖累取流

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
| **M1** | RTSP/RTP 服务端,PC VLC 实时播放 | ✅ 已完成(仅**多客户端**未做) |
| **M2** | OSD 叠加时间(REGION) | ✅ 已完成(板子实测) |
| **M3** | MP4 录制 + SD 卡环形覆盖 | ✅ 已完成(验收 A4~A7 全过;分段边界零丢帧) |
| **M4** | PC 拉流端(Qt + FFmpeg) | ⬜ 下一步(实时预览 + 回放) |

---

## 六、编译与运行

交叉编译需要:海思 Hi3516CV500 SDK + `arm-himix200` 交叉工具链 + **自编译的 mp4v2 静态库**
(见"二、开发环境")。

> ⚠️ **仓库目前未附 Makefile** —— 构建脚本还在开发环境里
> (见 [`docs/STATUS.md`](docs/STATUS.md) 已知问题 #8,计划补上)。
> 编译要点:`-I` 指向 SDK 的 `mpp/include` 与我们的 `src/*`;链接厂商 MPP 库与
> `libmp4v2.a`(**注意静态库的分组顺序**,否则会出现"库明明给了却报未定义符号")。

```bash
cp ipc_app ~/nfs_share/                     # 产物拷到 NFS 共享
# 板子上(必须先 cp 到 /tmp, 见下方坑):
cp /mnt/nfs/ipc_app /tmp/ && chmod +x /tmp/ipc_app
/tmp/ipc_app -p 8554 -s 1800 -r 8192        # 默认: 每段 30 分钟 / 环形上限 8 GB
```

> 常用选项:`-s <秒>` 每段时长(默认 1800)、`-r <MB>` 环形上限(默认 8192,
> ⚠️ **必须大于一段**)、`-no-record` 不录、`-no-osd` 不叠水印、`-h265` 取 H.265 那一路
> (mp4v2 不支持 H.265 封装,该模式下**录制会自动关闭**)。

> ⚠️ **NFS 执行坑**:直接从 `/mnt/nfs` 执行刚生成的二进制可能报
> `No such file or directory`(NFS 属性缓存)。**先 `cp` 到 `/tmp` 再运行。**

### 验证(RTSP)

```bash
# 用 VLC 或 ffplay 打开(端口要与 -p 一致; 注意本机防火墙要放行入站 UDP)
ffplay  -rtsp_transport udp rtsp://192.168.16.88:8554/live
# 或用 ffprobe 看编码参数
ffprobe -v error -rtsp_transport udp -i rtsp://192.168.16.88:8554/live \
        -show_entries stream=codec_name,width,height
```
