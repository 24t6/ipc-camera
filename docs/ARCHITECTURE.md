# IPC Camera 架构设计(定稿)

> 参考:[gtxaspec/compy](https://github.com/gtxaspec/compy)(嵌入式 IP 摄像头 RTSP 服务端库,C99)
> 原则:分层单向依赖、运行期零动态分配、协议逻辑与硬件解耦

---

## 一、分层架构

```
┌─────────────────────────────────────────────────────────────────────┐
│ app        应用层                                                    │
│   app_main.c        产品流程编排:MPP 初始化 → 启动服务 → 等待退出     │
├─────────────────────────────────────────────────────────────────────┤
│ service    服务层(有状态、有线程、依赖硬件)                          │
│   svc_media.c       VI/VPSS/VENC 通路封装 + 取流 + OSD               │
│   svc_net.c         RTSP 会话管理 + epoll 事件循环                   │
│   svc_record.c      (M3) mp4v2 录制 + 分段 + 环形覆盖                │
├─────────────────────────────────────────────────────────────────────┤
│ protocol   协议层(纯逻辑、无硬件、可在 PC 上单测)★                    │
│   proto_nalu.c      Annex-B 解析:剥起始码、识别 NALU 类型     ✅已完成 │
│   proto_rtp.c       RTP 打包 + FU-A / FU 分片                 ✅已完成 │
│   proto_sdp.c       SDP 会话描述生成                                 │
│   proto_rtsp.c      RTSP 请求解析 / 响应构造                         │
├─────────────────────────────────────────────────────────────────────┤
│ infra      基础设施层(与业务无关的通用件)                            │
│   infra_queue.c     线程安全环形队列(SPSC/MPSC)                     │
│   infra_netio.c     socket 封装 + TCP/UDP 发送抽象                   │
│   infra_log.c       分级日志                                         │
├─────────────────────────────────────────────────────────────────────┤
│ bsp        硬件适配层                                                │
│   bsp_mpp.c         HI_MPI_* 调用封装、错误码转换                     │
└─────────────────────────────────────────────────────────────────────┘
```

### 1.1 依赖方向铁律

```
app  →  service  →  protocol  →  infra
                       ↓
                     bsp
```

**绝对禁止:**
- ❌ `protocol` 层引用任何 `HI_MPI_*` 或 MPP 头文件
- ❌ `protocol` / `infra` 层调用 `malloc/free`
- ❌ 上层直接调 `HI_MPI_*`(必须经 `bsp_`)
- ❌ 下层反向依赖上层

**为什么这条最重要:** `protocol` 层一旦不碰硬件和动态内存,就能**直接在 Ubuntu 上原生编译 + 单元测试** —— 我们已经用这个办法在联网之前就把 FU-A 分片验证到逐字节一致。这是本项目最有效的质量手段。

### 1.2 分层依据

| 层 | 判定标准 | 测试方式 |
|---|---|---|
| `protocol` | 纯函数式,输入输出都是字节缓冲 | **PC 原生单测**(无需板子) |
| `infra` | 通用数据结构/系统调用封装 | PC 原生单测 |
| `bsp` | 只有它认识 `HI_MPI_*` | 只能板上验 |
| `service` | 有线程、有状态、编排多方 | 板上验 + 日志观察 |
| `app` | 只做流程编排,不含逻辑 | 板上验 |

---

## 二、传输链设计(借鉴 compy)

compy 的做法是把"发送"设计成**可组合的管道**:

```
Application
    ↓
NalTransport     —— NALU 分片(FU-A / FU)
    ↓
RtpTransport     —— RTP 头、序号、时间戳
    ↓
SrtpTransport    —— [可选] 加密
    ↓
Transport        —— TCP interleaved / UDP sendmsg
    ↓
Network
```

**我们采用同样的思路,但简化为:**

```c
/* 发送抽象 —— 让 RTP 层不关心底层是 UDP 还是 TCP */
typedef struct {
    int  (*send)(void *ctx, const void *buf, size_t len);
    void (*destroy)(void *ctx);
    void  *ctx;
} infra_sender_t;

/* 两个实现 */
infra_sender_t *infra_sender_udp(const struct sockaddr_in *dst, int sockfd);
infra_sender_t *infra_sender_tcp_interleaved(int fd, uint8_t rtp_channel);
```

好处:
1. **协议逻辑与传输解耦** —— `proto_rtp.c` 只管打包,不管怎么发
2. 将来加 TCP interleaved 只影响 `infra_netio.c`
3. 单测时可以注入一个"计数 sender",统计发了多少字节而不用真发网络

---

## 三、协议决策记录(ADR)

### ADR-1:UDP 为主,TCP interleaved 作为附加

| | UDP | RTP over TCP (interleaved) |
|---|---|---|
| 实时性 | ✅ 好 | ⚠️ 有队头阻塞 |
| 丢包 | 会丢,但只影响局部 | 不丢,但延迟累积 |
| 穿越防火墙/NAT | ❌ 差 | ✅ 好 |
| 实现复杂度 | 简单 | 中(要处理 `$` 分帧) |

**决策:先实现 UDP(符合项目原描述),`infra_sender_t` 抽象为 TCP 预留接口。**
如果 VLC 联调时遇到网络问题,可低成本切到 TCP。

> compy 两种都支持,且 `ffplay -rtsp_transport tcp` 是常用调试手段。**建议 M1 完成后补 TCP interleaved 作为加分项。**

### ADR-2:epoll 而非 select

- `select` 有 `FD_SETSIZE=1024` 上限,且每次调用要传全量 fd 集合(O(n))
- `epoll` 用红黑树注册 + 就绪链表,只返回就绪的 fd(**O(1)**)
- **但注意**:VENC 的 fd 要加进 epoll 需要用 `epoll_ctl(EPOLL_CTL_ADD)`,这没问题

**决策:用 epoll。** 简历上 epoll 也比 select 值钱。

### ADR-3:发送走队列,不直接发

VENC 的编码缓冲有限(实测 `HI_MPI_VENC_GetStream` 后必须尽快 `ReleaseStream`)。
如果取流线程里直接 `sendto` 给多个客户端,网络一慢就会拖住编码器 → **丢帧**。

**决策:取流线程只做「解析 + 入队」,发送和录制各自独立线程。**
队列满时**丢弃最旧的帧并计数**(实时流允许丢帧,不允许阻塞)。

### ADR-4:单进程多线程(而非 HIVIEW 的多进程)

参考 [NightAroundDay/HIVIEW](https://github.com/NightAroundDay/HIVIEW) 用的是多进程框架。

| | 单进程多线程 | 多进程 |
|---|---|---|
| 复杂度 | 低 | 高(IPC、共享内存) |
| 崩溃隔离 | 差(一个线程挂全挂) | 好 |
| MPP 兼容 | ✅ 官方 sample 都是单进程 | 需要额外适配 |
| 本项目适用 | ✅ **选它** | 产品级才需要 |

**决策:单进程多线程。** 海思官方所有 sample 都是单进程模型,跟它对着干没有收益。

---

## 四、线程与所有权(定稿)

| 对象 | 唯一所有者 | 访问者 | 通信/同步 | 停止与错误恢复 |
|---|---|---|---|---|
| **MPP 通路**(VI/VPSS/VENC) | `svc_media` | 无(取流线程独占) | — | 退出时按反序 UnBind/Stop/Destroy |
| **VENC fd** | `svc_media` 取流线程 | epoll | epoll 边沿触发 | 句柄失效则退出线程并标记服务不可用 |
| **NALU 缓冲** | 取流线程 | 队列 | 栈上固定数组 + 入队即拷贝 | 单帧超长则丢弃并计数 |
| **发送队列** | `infra_queue` | 取流线程(生产者) / 发送线程(消费者) | 环形缓冲 + 互斥锁 + 条件变量 | 满则丢最旧 + 计数 |
| **录制队列** | `infra_queue` | 取流线程(生产) / 录制线程(消费) | 同上 | 满则丢帧 + 计数(录制可容忍) |
| **客户端会话** | `svc_net` net 线程 | 发送线程只读 RTP 会话状态 | 会话表 + 引用计数 | `TEARDOWN`/超时/`send` 失败即销毁 |
| **RTP 会话**(每客户端每通道) | `svc_net` | 发送线程 | 每会话独立,无共享 | 会话销毁时一并释放 |
| **MP4 文件句柄** | `svc_record` | 无 | — | 信号触发时**必须先 `MP4Close`** |
| **日志** | `infra_log` | 所有线程 | 互斥锁,极短临界区 | 写失败只降级不崩溃 |

> **"唯一所有者"是硬约束**:任何对象同一时刻只能有一个线程能改它。
> 不能确定所有者,就是还没设计完。

---

## 五、内存与缓冲区预算

**铁律:运行期不做动态分配。** 所有缓冲在初始化阶段一次性分配好。

| 缓冲 | 容量 | 依据 | 内存区域 |
|---|---|---|---|
| 单帧码流缓冲 | **256 KB** | 实测最大 NALU:H.265 IDR 115 KB;一帧可能含多个 NALU,留 2 倍余量 | 栈? **不行,太大** → 静态/堆预分配 |
| RTP 发送包缓冲 | 1400 + 15 B | MTU 推导 | 栈上(小) |
| 发送队列 | 8 帧 × 256 KB ≈ 2 MB | 容忍 8 帧的突发;按 4Mbps 算约 0.5 秒 | **堆预分配(启动时)** |
| 录制队列 | 4 帧 × 256 KB ≈ 1 MB | 磁盘临时变慢时缓冲 | 堆预分配 |
| 客户端会话表 | 8 个 | 够用;超过拒绝新连接 | 静态数组 |

**板子内存约束**:`mem=128M`,MMZ 384M。用户态可用约 100MB,规划占用 < 5MB 很安全。

**必须监测的指标**(每 30 秒打一次日志):
- 发送队列 / 录制队列**水位**
- 丢弃帧计数
- 各客户端发送失败计数

---

## 六、模块清单与接口契约

| 模块 | 公开接口 | 依赖 |
|---|---|---|
| `proto_nalu` | `nalu_foreach()`, `nalu_has_idr()` | 无 ✅已完成 |
| `proto_rtp` | `rtp_session_init()`, `rtp_session_next_frame()`, `rtp_send_nalu()` | `proto_nalu` ✅已完成 |
| `proto_sdp` | `sdp_build_video(buf, size, params)` | 无 |
| `proto_rtsp` | `rtsp_parse_request()`, `rtsp_build_response()` | 无 |
| `infra_queue` | `queue_create()`, `queue_push()`, `queue_pop()`, `queue_destroy()` | 无 |
| `infra_netio` | `infra_sender_udp()`, `infra_sender_tcp_interleaved()` | 无 |
| `infra_log` | `log_info()`, `log_warn()`, `log_error()` | 无 |
| `bsp_mpp` | `bsp_mpp_init()`, `bsp_mpp_get_frame()`, `bsp_mpp_release_frame()` | MPP |
| `svc_media` | `svc_media_start()`, `svc_media_stop()` | `bsp_mpp`, `proto_nalu` |
| `svc_net` | `svc_net_start()`, `svc_net_stop()` | `proto_rtsp`, `proto_sdp`, `infra_*` |
| `svc_record` | `svc_record_start()`, `svc_record_stop()` | MPP, mp4v2 |

---

## 七、目录结构(定稿)

```
ipc_camera/
├── 项目计划.md
├── 架构设计.md          ← 本文
├── 编码规范.md
├── Makefile
├── src/
│   ├── app_main.c
│   ├── bsp/     bsp_mpp.c/h
│   ├── service/ svc_media.c/h  svc_net.c/h  svc_record.c/h
│   ├── protocol/ proto_nalu.c/h  proto_rtp.c/h
│   │             proto_sdp.c/h   proto_rtsp.c/h
│   └── infra/   infra_queue.c/h  infra_netio.c/h  infra_log.c/h
├── tools/               ← 可在 PC 上原生编译的测试工具
│   ├── nalu_dump.c
│   ├── rtp_test.c
│   └── sdp_test.c
└── tests/               ← 单元测试
    └── test_all.c
```

---

## 八、下一步

| 顺序 | 任务 | 能否 PC 单测 |
|---|---|---|
| 1 | `proto_sdp.c` SDP 生成 | ✅ 能 |
| 2 | `proto_rtsp.c` RTSP 解析/构造 | ✅ 能 |
| 3 | `infra_queue.c` 环形队列 | ✅ 能 |
| 4 | `infra_netio.c` 发送抽象 | ✅ 能 |
| 5 | `svc_net.c` epoll 事件循环 | ⚠️ 部分 |
| 6 | `bsp_mpp.c` + `svc_media.c` | ❌ 只能板上 |
| 7 | `app_main.c` 集成 | ❌ |
| 8 | VLC 联调 | ❌ |
