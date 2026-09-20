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
typedef struct infra_sender infra_sender_t;   /* 不透明句柄, 定义在 infra_netio.h */

struct infra_sender {
    int  (*send)(void *ctx, const void *buf, size_t len);
    void (*destroy)(infra_sender_t **ps);     /* 用法: s->destroy(&s); */
    void  *ctx;
};

/* 两个实现 */
infra_sender_t *infra_sender_udp(const struct sockaddr_in *dst, int sockfd);
infra_sender_t *infra_sender_tcp_interleaved(int fd, uint8_t rtp_channel);
```

好处:
1. **协议逻辑与传输解耦** —— `proto_rtp.c` 只管打包,不管怎么发
2. 将来加 TCP interleaved 只影响 `infra_netio.c`
3. 单测时可以注入一个"计数 sender",统计发了多少字节而不用真发网络

> **⚠️ 上面第 1、2 条在 2026-09-18 之前是"纸面承诺",实际没做到 —— 已修,记在这里。**
>
> 事实是:`svc_sender.emit()` 当时**直接调 `proto_rtp_send_nalu()`**, 而那个函数
> **自己 `sendto`** —— 传输方式被焊死在协议层里。后果是:
> `infra_sender_tcp_interleaved()` 写好了却**从来没有被调用过**, 客户端一勾
> "以 TCP 播放"就**一帧都发不出去**, 而且日志看起来一切正常
> (`SETUP 完成(TCP 交错)`) —— 只有 `加入发送(RTP 端口 0)` 里那个 **0** 是线索。
>
> 修法(commit `378ee1f`):把"打包"与"发送"拆开 —— 新增
> `proto_rtp_pack_nalu(s, n, is_last, fn, user)`(纯打包, 每个 RTP 包交给回调),
> `proto_rtp_send_nalu()` 退化成**薄包装**;`svc_sender.emit()` 改为把每个包交给
> `cli->sender->send()`。这样 UDP 与 TCP 交错才**真正共用同一条数据路径**,
> 协议层也**不再碰 socket**。
>
> **教训**:"抽象写好了"不等于"抽象被用上了"。判断依据只有一个 ——
> **那个实现有没有被真的调用**(grep 调用点 / 看运行期日志),而不是架构图上画了什么。

> **⚠️ `destroy` 为什么收 `infra_sender_t **` 而不是 `void *ctx`(2026-09-14 修订)**
>
> 初版签名是 `void (*destroy)(void *ctx)`,即要求调用方传 `s->ctx`。
> 但调用方手上拿的是 `s`,**极易写成 `s->destroy(s)`** —— 本项目在 M1-6 单测里
> 就真的这么写了, 结果 `free()` 了一个野指针, AddressSanitizer 报
> `SEGV ... in tcp_destroy`。
>
> 改成收二级指针后:
> - 调用方只需 `s->destroy(&s)`,**不可能传错**
> - destroy 内部一次性完成"释放 ctx + 释放 sender 本身 + 把 `s` 置 NULL",
>   **也不会漏掉释放 sender 本身**(初版签名下 sender 结构体会泄漏)
>
> **教训: 接口设计要顺着调用方的直觉,而不是要求调用方记住内部结构。**
> 这类"用错就崩"的签名,靠注释是防不住的。

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

### ADR-5:回放服务 = **单线程串行 + 非阻塞客户端 + 每请求一条连接**

**背景**:阶段 2 要在板子上提供"列分段 / 带 `Range` 取流 / 锁定解锁"(HTTP)。

| 备选 | 为什么不选 |
|---|---|
| **每客户端一个线程** | 线程数不可控(每个连接一个栈);而回放是低频操作,为它引入线程池/上限管理不值 |
| **epoll + 每客户端非阻塞状态机 + `sendfile()`** | 这是"正确"的终局形态, 但要引入"发送游标/半包状态/超时回收"一整套状态机 —— 现在(局域网 1~2 个客户端)收益不抵复杂度 |
| **✅ 单线程串行 + 非阻塞 + `Connection: close`** | 一次只服务一个请求, 代码短到能一眼看完;非阻塞保证**任何一个客户端都卡不住服务** |

**关键三条**(都是踩过坑才写下来的):

1. **客户端 socket 必须非阻塞**。阻塞 `send` 会在"客户端不读数据"时**在内核里无限等** ——
   单线程服务就此永久卡死(实测:`/proc/<tid>/syscall` 里卡在 `send(fd,…)`)。
   非阻塞后 `send` 得到 `EAGAIN` ⇒ `infra_tcp_write_all()` `poll` 等 100 ms ⇒ 等不到就
   **丢掉这个连接**。(B041)
2. **停服务要主动打断在途连接**(`shutdown(cur_fd, SHUT_RDWR)`),否则 `pthread_join`
   会一直等那个线程 —— 表现是"**`kill -TERM` 杀不掉进程**",还占着端口让新进程起不来。(B041)
3. **请求头与 body 必须切开解析**:body 常和头在同一个 TCP 段里到达;
   把头区之后的字节也喂给"逐行解析"会把 body 当成长度非法的头行 ⇒ 400。(B042)

**代价(写在这里,不藏着)**:
- 一个"正在下载"的客户端会占住服务(30 分钟段 ~900 MB,千兆局域网约 80 秒);
- 每个请求一条连接(没有 keep-alive)、没有 chunked、没有 TLS、**没有鉴权** ——
  任何能连上 8080 的人都能下载全部录像、也能改锁定清单。
  **定位是"局域网里的开发板",不声称适合公网。**

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
| **锁定清单缓存**(`g_locks`) | `svc_record` | 录制线程(重读) / HTTP 线程(查与改) | **互斥锁** `g_lock_mtx`,临界区只有几百字节 | 改清单用"写 `.tmp` → `fsync` → `rename`" |
| **分段扫描表**(`g_scan`/`g_del`) | `svc_record` 录制线程 | 无 | — | **刻意不共享**:回放列表走另一条扫描,用调用方数组 |
| **HTTP 客户端连接** | `svc_http` http 线程 | 无 | 串行处理(一次一个) | 非阻塞 + 有界读 + 有界发送(见 ADR-5) |
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
| 回放服务缓冲 | 请求 2 KB + 应答头 1 KB + JSON 16 KB + 分段表 5 KB + 取文件块 32 KB ≈ **56 KB** | JSON 要装下 64 条分段(每条约 190 字节);取文件块取 32 KB(卡上顺序读与一次 `send` 都合适) | 静态(`.bss`) |

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
| `infra_netio` | `infra_sender_udp()`, `infra_sender_tcp_interleaved()`, `infra_tcp_listen()`, `infra_udp_bind()` | 无 |
| `infra_poll` | `infra_poller_create()`, `infra_poller_add()`, `infra_poller_wait()` | epoll(Linux) |
| `infra_log` | `log_info()`, `log_warn()`, `log_error()` | 无 |
| `bsp_mpp` | `bsp_mpp_init()`, `bsp_mpp_get_frame()`, `bsp_mpp_release_frame()` | MPP |
| `svc_media` | `svc_media_start()`, `svc_media_stop()` | `bsp_mpp`, `proto_nalu` |
| `svc_net` | `svc_net_start()`, `svc_net_stop()` | `proto_rtsp`, `proto_sdp`, `infra_*` |
| `svc_record` | `svc_record_start()`, `svc_record_stop()`, **`svc_record_list()` / `svc_record_set_lock()` / `svc_record_make_path()`**(回放服务用) | MPP, mp4v2, `svc_record_policy` |
| `proto_str` | `proto_str_append()`, `proto_str_u32/u64()`, `proto_str_parse_u32/u64()`, `proto_str_eq_ci*()` | 无(协议层共用小工具) |
| `proto_http` | `proto_http_parse()`, `proto_http_match_path()`, `proto_http_name_ok()`, `proto_http_build_head()` | `proto_str` |
| `svc_http` | `svc_http_start()`, `svc_http_stop()`, `svc_http_port()`, `svc_http_get_stats()` | `proto_http`, `svc_record`, `infra_netio` |
| `svc_http_page` | 回放页面(一段常量 HTML;**浏览器就是客户端**) | 无 |

---

## 七、目录结构(与仓库一致)

```
ipc_camera/
├── Makefile             ← make / make test / make clean / make help
├── README.md
├── docs/                PROJECT_PLAN / STATUS / ARCHITECTURE / CODING_STYLE / research/
├── src/
│   ├── app/       app_main.c
│   ├── bsp/       bsp_mpp.c/h  bsp_osd.c/h  bsp_osd_render.c/h
│   ├── service/   svc_media.c/h  svc_net.c/h  svc_sender.c/h  svc_osd.c/h
│   │              svc_record.c/h  svc_record_policy.c/h  svc_record_mp4.h
│   │              svc_http.c/h  svc_http_page.c/h  ← 阶段 2 回放服务 + 板子自带页面
│   ├── protocol/  proto_nalu.c/h  proto_rtp.c/h  proto_rtsp.c/h
│   │              proto_sdp.c/h   proto_str.c/h   proto_http.c/h
│   └── infra/     infra_queue.c/h  infra_netio.c/h  infra_poll.c/h  infra_log.c/h
└── tools/               ← **PC 上原生编译**的测试与诊断工具
    ├── http_test.c              rtp_test.c      sdp_test.c      rtsp_test.c
    ├── svc_record_policy_test.c infra_queue_test.c infra_netio_test.c
    ├── svc_net_test.c           svc_sender_test.c bsp_osd_render_test.c
    └── (media_smoke.c / media_diag.c / nalu_dump.c / rtp_header_dump.c 需要 SDK)
```

---

## 八、下一步

| 顺序 | 任务 | 状态 |
|---|---|---|
| 1 | M1 RTSP/RTP 服务端(UDP + TCP 交错 + 多客户端) | ✅ 完成 |
| 2 | M2 OSD 时间水印 | ✅ 完成 |
| 5 | M3 MP4 分段录制 + 环形覆盖 + 掉电可救 + 大小上限 + 锁定段 | ✅ 完成(验收 A4~A15) |
| 6 | **阶段 2 回放(服务端)**:列分段 / `Range` 取流 / 锁定解锁 | ✅ 完成(验收 A17) |
| 7 | 仓库自带构建脚本 + PC 单测入口 | ✅ 完成(A16: `make test`) |
| 8 | **回放客户端(零安装路线)**:板子自带页面 + `playlist.m3u` | ✅ 完成(A18) |
| 9 | **列表只给最新一页 + `?before=` 翻页 + `/status`(正在录的那段) + 页面自刷新** | ✅ 完成(A19, 修 B044) |
| 10 | **② `/recent.mp4`**:"刚录的这段"立刻能看 —— 请录制线程在下一个 IDR 处把当前段**收尾**, 再把刚收尾的整段发出去(缺口从"段长"降到 1 秒) | ✅ 完成(A20: 上板 17/17) |
| 11 | **命令行长选项两种横线都认**(`-no-record` == `--no-record`) | ✅ 完成(A21, 修 B045) |
| 12 | **③ 实时画面进浏览器**:硬件 JPEG 通道 + MJPEG(`/live.mjpg`)。⚠️ 通道必须**按需启停** —— 没人取流的编码通道会拖死整条流水线(B027) | ⬜ 未开始 |
| 13 | (可选)自研 C 客户端:**SDL2 + FFmpeg**(可选 ImGui);seek 直接映射成 HTTP `Range` | ⬜ 想讲"解复用→解码→上屏"时再做 |
| 14 | (可选)接入第三方 NVR(Frigate / ZoneMinder)证明标准可接入 | ⬜ |
