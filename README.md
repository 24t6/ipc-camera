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
| `src/nalu.c/h` | **Annex-B 解析**:剥离起始码、识别 NALU 类型 |
| `src/rtp.c/h` | **RTP 打包**:RFC 6184(H.264)/ RFC 7798(H.265),含 FU-A 分片 |
| `src/rtsp.c/h` | **RTSP 协议**:OPTIONS/DESCRIBE/SETUP/PLAY/TEARDOWN + SDP 生成 |
| `src/net_loop.c/h` | epoll 事件循环 + 客户端管理 |
| `src/queue.c/h` | 线程安全环形队列 |
| `src/osd.c/h` | (M2)REGION 模块叠加时间 |
| `src/record.c/h` | (M3)mp4v2 录制 + 环形覆盖 |
| `src/mpp_init.c/h` | VI/VPSS/VENC 初始化封装 |

---

## 五、里程碑

| # | 内容 | 状态 |
|---|---|---|
| **M0** | 采集→编码跑通,产出标准 H.264/H.265 | ✅ 已完成 |
| **M1** | RTSP/RTP 服务端,PC VLC 实时播放 | 🚧 进行中 |
| **M2** | OSD 叠加时间(REGION) | ⏳ |
| **M3** | MP4 录制 + SD 卡环形覆盖 | ⏳ 前置条件已就绪 |
| **M4** | PC 拉流端(Qt + FFmpeg) | ⏳ |

---

## 六、编译与运行

```bash
make                      # 交叉编译,产出 ipc_camera
cp ipc_camera ~/nfs_share/
# 板子上:
cp /mnt/nfs/ipc_camera /tmp/ && chmod +x /tmp/ipc_camera
/tmp/ipc_camera
```

> ⚠️ **NFS 执行坑**:直接从 `/mnt/nfs` 执行刚生成的二进制可能报
> `No such file or directory`(NFS 属性缓存)。**先 `cp` 到 `/tmp` 再运行。**

### 验证(RTSP)

```bash
# PC 上用 VLC 打开
rtsp://192.168.16.88:554/live
```
