# IPC Camera 编码规范

> 依据:`embedded-product-engineering` 技能规则 + 本项目实际情况
> 适用范围:`src/` 下所有自研代码。`tools/` 下的测试工具可适当放宽。
> 第三方代码(mp4v2 等)保留原版权、许可证与原始格式,不做本项目风格改造。

---

## 一、总原则

1. **能测的优先做纯逻辑。** 协议层不碰硬件、不做动态分配,保证可在 PC 上原生单测。
2. **运行期零动态分配。** 所有缓冲在初始化阶段分配完毕,运行期只用固定对象。
3. **每个资源有唯一所有者。** 说不清谁拥有,就是还没设计完。
4. **注释解释"为什么",不复述"做了什么"。**
5. **不阻塞关键路径。** 取流线程绝不能被网络或磁盘拖住。

---

## 二、命名规范

### 2.1 语言与大小写

- 语言标准:**C11**
- 文件、函数、变量、类型:**`snake_case`**
- 宏、枚举常量:**`UPPER_SNAKE_CASE`**
- 类型名以 `_t` 结尾

```c
/* ✅ 正确 */
typedef struct { ... } svc_client_t;
static int proto_rtp_send_fragment(...);
uint32_t frame_count;

/* ❌ 错误 */
typedef struct { ... } SvcClient;
static int ProtoRtpSendFragment(...);
uint32_t frameCount;
```

### 2.2 分层前缀(强制)

对外符号**必须**带所属层前缀,一眼看出它在哪一层:

| 前缀 | 层 | 示例 |
|---|---|---|
| `app_` | 应用层 | `app_main()` |
| `svc_` | 服务层 | `svc_media_start()`, `svc_net_stop()` |
| `proto_` | 协议层 | `proto_nalu_foreach()`, `proto_rtp_send()` |
| `infra_` | 基础设施层 | `infra_queue_push()` |
| `bsp_` | 硬件适配层 | `bsp_mpp_get_frame()` |

> `static` 函数可以省前缀,但建议保留以便阅读。

### 2.3 类型与常量

```c
/* 枚举: 类型名 + 成员名都带前缀 */
typedef enum {
    PROTO_NALU_KIND_UNKNOWN = 0,
    PROTO_NALU_KIND_IDR,
    PROTO_NALU_KIND_SLICE,
} proto_nalu_kind_t;

/* 宏: 全大写, 带模块前缀 */
#define PROTO_RTP_MAX_PAYLOAD 1400
#define INFRA_QUEUE_MAX_CLIENTS 8

/* 避免魔法数字 */
if (len > PROTO_RTP_MAX_PAYLOAD) { ... }        /* ✅ */
if (len > 1400) { ... }                          /* ❌ */
```

---

## 三、文件与目录组织

### 3.1 一个模块一对文件

```
src/protocol/proto_rtp.h    ← 接口契约(声明 + 文档)
src/protocol/proto_rtp.c    ← 实现(代码 + 所有权说明)
```

- 头文件**只写接口契约**,不写实现细节、不 `include` 无关头文件
- 头文件必须有 include guard:`#ifndef __PROTO_RTP_H__`
- `.c` 文件顶部**必须**说明四件事:**模块职责、依赖方向、线程模型、资源边界**

### 3.2 `.c` 文件头部模板(强制)

```c
/**
 * @file    proto_rtp.c
 * @brief   RTP 打包实现
 *
 * 【模块职责】把 NALU 切成 RTP 包, 处理 FU-A / FU 分片
 * 【依赖方向】仅依赖 proto_nalu; 不依赖任何 socket / 硬件
 * 【线程模型】无状态全局, 每个会话由调用者独占, 可在多线程中并发使用
 * 【资源边界】无动态分配; 使用文件级静态发送缓冲(1463 字节)
 */
```

---

## 四、注释规范

### 4.1 函数注释(强制,中文 Doxygen)

**每个函数都要有**,包括 `static`、线程入口、回调。至少要有 `@brief`。

```c
/**
 * @brief 发送一个 NALU, 自动决定用单包还是 FU-A 分片
 *
 * @param[in]  sockfd  用于发送的 UDP socket
 * @param[in]  dst     目标地址
 * @param[in]  s       会话状态(序列号、时间戳会在此更新)
 * @param[in]  n       要发送的 NALU(不含起始码)
 * @param[in]  is_last 是否为本帧最后一个 NALU —— 是则置 RTP marker 位
 *
 * @return >=0  已发送的 RTP 包个数
 * @return <0   发送失败
 *
 * @note  调用线程: svc_media 取流线程
 * @note  阻塞行为: 依赖底层 sendto, 正常情况下不阻塞; 内核发送缓冲满时可能短暂阻塞
 * @note  所有权: n 的数据不会被修改, 也不会被保存, 返回后即可释放
 */
int proto_rtp_send_nalu(int sockfd, const struct sockaddr *dst,
                        proto_rtp_session_t *s, const proto_nalu_t *n,
                        int is_last);
```

**签名无法表达的约束必须写进 `@note`** —— 调用任务、是否阻塞、所有权、硬件限制、错误恢复。

### 4.2 行内注释

```c
/* ✅ 解释"为什么" */
/* 4 字节起始码 00 00 00 01 也含 00 00 01, 所以统一按 3 字节找,
 * 多出的那个 00 当作 trailing zero 裁掉 */
nalu_start = find_start_code(buf, end);

/* ✅ 解释硬件/协议原因 */
/* 必须在 ReleaseStream 之前取走数据, 否则编码器缓冲会被复用 */
save_frame(&stStream);
HI_MPI_VENC_ReleaseStream(chn, &stStream);

/* ❌ 复述代码, 没有信息量 */
i++;   /* i 自增 */
```

### 4.3 禁止的注释

- ❌ `// TODO` 但不写清楚要做什么
- ❌ 注释掉的死代码(直接删,Git 有历史)
- ❌ 与代码不符的过时注释(**改了代码必须改注释**)

---

## 五、错误处理

### 5.1 用可检查的错误码

```c
/* 统一错误码 */
typedef enum {
    ERR_OK          =  0,
    ERR_INVAL       = -1,   /* 参数非法 */
    ERR_NOMEM       = -2,   /* 资源不足 */
    ERR_AGAIN       = -3,   /* 暂时失败, 可重试 */
    ERR_IO          = -4,   /* 系统调用失败, 详见 errno */
    ERR_PROTOCOL    = -5,   /* 协议格式错误 */
} err_t;
```

### 5.2 规则

- **预期可恢复的错误**返回错误码 + 降级/重试,不要 `abort`
  - 未插卡、网络断开、队列满、客户端异常断开 → 返回 `ERR_AGAIN`/`ERR_IO` 并计数
- **只有**启动完整性、内存破坏、硬件无法继续 → 进入安全退出或复位
- **所有系统调用都要检查返回值**。`sendto`/`write` 的返回值必须处理短写
- `errno` 只在紧邻失败之后读取

```c
/* ✅ */
ssize_t n = sendto(fd, buf, len, 0, dst, dstlen);
if (n < 0) {
    if (errno == EAGAIN || errno == EWOULDBLOCK) {
        stats->tx_drop++;            /* 发送缓冲满, 计一次丢弃 */
        return ERR_AGAIN;
    }
    log_error("sendto failed: %s", strerror(errno));
    return ERR_IO;
}
```

---

## 六、内存与资源

### 6.1 运行期零动态分配(强制)

- ❌ 运行期 `malloc` / `free`
- ✅ 启动时一次性分配;或用静态对象 / 固定数组 / 内存池 / 有界队列

> **例外的唯一情况**:必须 `HI_MPI_VENC_GetStream` 之后 `malloc(stStream.pstPack)` —— 这是 SDK 要求的。
> 这种必须紧邻 `free`,并在注释里说明原因。

### 6.2 每个缓冲必须注明四件事

```c
/*
 * 发送队列: 8 帧 × 256KB ≈ 2MB
 *   容量依据 : 4Mbps 码率下约 0.5 秒的突发量
 *   内存区域 : 启动时一次性堆分配(启动后可视为静态)
 *   唯一所有者: infra_queue 模块
 *   释放时机 : 程序退出时 queue_destroy()
 */
```

### 6.3 禁止大数组放栈上

栈默认 8MB,但线程栈可能更小。**超过 4KB 的数组一律静态或预分配。**

```c
/* ❌ 这段代码在 nalu.c 里, 后续要改 */
uint8_t buf[NALU_MAX_SIZE];        /* 1MB! 会爆栈 */

/* ✅ */
static uint8_t s_nalu_buf[PROTO_NALU_MAX_SIZE];   /* 文件级静态 */
```

> ⚠️ `tools/rtp_test.c` 里目前有这个隐患,后面重构时一并修掉。

### 6.4 队列规则

环形缓冲**必须**明确:
- 生产者/消费者分别是谁、各有几个
- 满 / 空 的语义
- **满了怎么办**(实时流:丢最旧 + 计数,绝不阻塞)

---

## 七、并发与线程

- 优先用**状态机 + 有界队列**;禁止无界队列
- 临界区必须短小:**不含 socket 收发、文件读写、日志风暴、耗时计算**
- **不在生产者仍访问时重置队列**;用状态位/会话代数让旧数据自然失效
- 停止/重连用**状态标志**,不用强制 `cancel`
- 每个新线程创建后要**检查栈高水位**并按实测峰值调整

### 7.1 线程命名(方便调试)

```c
prctl(PR_SET_NAME, "ipc_net", 0, 0, 0);    /* 顶层能看到线程名 */
```

---

## 八、日志规范

### 8.1 分级与格式

```c
log_error("venc ch%d: %s failed with %#x", chn, "GetStream", ret);
log_warn ("client %s:%u send failed, drop %u pkts", ip, port, drops);
log_info ("client connected: %s:%u (total %d)", ip, port, count);
log_debug("nalu type=%d len=%zu frag=%d", type, len, npkts);
```

- **固定前缀 + 可搜索关键词**:`client connected`、`first frame sent`、`record file opened`
- 同一问题只打一次或限频(不要刷屏)
- **不要在每个 RTP 包打日志**(每帧最多一条 debug)
- 板子 rootfs 只剩 10MB → 日志输出到 stdout 或 `/tmp`(tmpfs),**不要写 flash**

### 8.2 必须监测的指标(每 30 秒一次)

```c
log_info("stats: txq=%d/%d rxq=%d/%d drop_frame=%u drop_pkt=%u clients=%d",
         txq_watermark, TXQ_CAP, recq_watermark, RECQ_CAP,
         drop_frames, drop_pkts, client_count);
```

---

## 九、格式与排版

| 项 | 规定 |
|---|---|
| 缩进 | **4 空格**,不用 Tab |
| 行宽 | **不超过 100 列** |
| 大括号 | 函数左括号另起一行;控制语句同行 |
| 空格 | `if (x)`、`for (i = 0; ...)`、`,` 后加空格 |
| 指针 | `char *p`(星号靠变量) |

```c
int proto_rtp_send_nalu(/* ... */)          /* 函数: 括号另起一行 */
{
    if (n == NULL) {                        /* 控制: 括号同行 */
        return ERR_INVAL;
    }

    for (i = 0; i < count; i++) {
        process(item[i], len, buf);
    }

    return npkt;
}
```

- **编译必须 0 warning**:`-Wall -Wextra`,且不允许靠 `-Wno-` 掩盖
- 每次提交前跑一次 `gcc -Wall -Wextra -O2`(主机)确认无告警

### 9.1 函数规模(2026-09-14 新增)

> **为什么加这一节**:`svc_net_start()` 一度长到 **101 行代码**,
> 是我"量了一下"才发现的 —— 在那之前只规定了行宽,函数规模全靠自觉。
> 项目规矩 8 是「能用工具消除的错误,不要靠纪律避免」,所以规矩 + 工具一起加。

| 项 | 硬约束 | 依据 |
|---|---|---|
| **函数代码行** | **≤ 50 行** | Linux 内核 coding-style:函数应"fit on one or two screenfuls"(80x24 → 一屏 24 行) |
| **函数局部变量** | **≤ 10 个** | 同上:"shouldn't exceed 5-10";人脑只能同时记住约 7 个东西 |
| **缩进层数** | **≤ 5 层**(本文件口径) | 同上:"more than 3 levels of indentation → you're screwed" |

> ⚠️ **缩进口径说明**(避免和内核的"3 层"对不上):
> 内核从**函数体往里**数,本节从**文件左边距**数(函数体本身算第 1 层)。
> 所以 **本节的 5 层 = 内核口径的 4 层**,留了一层余量给
> "确实需要一个 for + 一个 if"的正常写法。

**工具**:`python tools/check_style.py` —— 与 `check_encoding.py` 一样,改完代码跑一次。

```
硬约束: 代码行 ≤ 50 / 局部变量 ≤ 10 / 缩进 ≤ 5 层
```

**明确不设文件行数上限。** 依据(见 `docs/代码模块地图.md` 第 211 行):
**抽模块的依据是"同一份逻辑写了两遍",不是"文件超过 N 行"。**
上游也没有任何一个项目限制文件长度 —— Redis 的 `src/module.c` 有 2 万多行,
其中位数是 552 行。

**`tools/` 下的一次性工具与测试程序例外**:
- `*_test.c` / `*dump.c` 里的 **`main()` 不限长度** —— 它是"一长串断言的容器",
  不是复杂逻辑(内核同一节也说:"最大长度与复杂度成反比;
  一个又长又**简单**的列表是可以接受的")。
  抽 helper 反而会把"能一口气读完的线性流程"打散,增加核对成本。
- 但**缩进深仍然算问题** —— 深缩进是理解障碍,不能用"它是工具"来解释。

> **这是一条"有意的取舍",不是漏检。** 检查器会把这些函数单独列出来并注明原因,
> 而不是默默放过。

---

## 十、代码审查清单

每完成一个模块,逐条自检:

```
[ ] 分层正确? protocol 层有没有混进 MPP/socket 调用?
[ ] 所有函数都有中文 Doxygen? @brief 都有?
[ ] @note 里写清了调用线程 / 阻塞行为 / 所有权?
[ ] 运行期有没有 malloc/free?
[ ] 大数组有没有放栈上?
[ ] 所有系统调用返回值都检查了?短写处理了?
[ ] 缓冲的四要素(容量依据/内存区域/所有者/释放时机)写了吗?
[ ] 队列满了的策略明确吗?会阻塞吗?
[ ] 临界区里有没有网络/文件/日志操作?
[ ] 注释解释的是"为什么"吗?
[ ] -Wall -Wextra 零告警?
[ ] 字符串/中文编码正确吗?(UTF-8)
[ ] 跑过 `python tools/check_style.py` 吗?(函数 ≤ 50 代码行 / 局部变量 ≤ 10 / 缩进 ≤ 5 层)
[ ] 跑过 `python tools/check_encoding.py` 吗?(UTF-8 / LF)
[ ] 跑过 `python tools/check_spec.py` 吗?(§2.2 前缀 / §3.1 guard / §3.2 文件头四要素 /
    §4.1 Doxygen / §4.3 TODO / §6.3 栈数组 / §7.1 线程命名 / §9 行宽 ≤100 列)
[ ] 核心模块过了 ASan + UBSan 吗?(PC 侧能做的一定要做)
[ ] 抽 helper 之后, 原来的**语义**还在吗?(尤其是回调返回值、错误码、提前返回)
```

> ⚠️ 最后一条是血泪:重构 `proto_nalu.c` 时我一度把回调的返回值吞掉了
> (原意是"回调要求提前终止"),**编译器不会报错,类型也完全对**。
> 只有重跑 `rtp_test`(809/809 + 835/835 逐字节一致)才证明没改坏。
> **重构必须重跑测试 —— 这是唯一能发现"语义静默丢失"的办法。**

> 💡 工作区级(`D:\linux_hi`)还有一条:`python tools/gen_doc_index.py --check`
> —— 加了文档/改了标题之后要重生成 `docs/文档索引.md`。见 `AGENTS.md` §7.3。

---

## 十一、Git 提交规范

采用 **Conventional Commits**:

```
<type>(<scope>): <简短描述>

type : feat | fix | docs | refactor | test | build | chore
scope: nalu | rtp | sdp | rtsp | queue | netio | media | record | app | build
```

示例:

```
feat(rtp): 实现 H.264 FU-A 分片打包
fix(nalu): 修正 4 字节起始码的 trailing zero 裁剪
test(rtp): 增加本机 UDP 自发自收的重组校验
docs(arch): 补充传输链抽象设计与 ADR 决策记录
```

**提交前必须**:编译通过 + 相关单测通过。只暂存本模块文件。

---

## 十二、当前代码的待整改项

按本文规范审一遍已完成的两个模块:

| 文件 | 问题 | 处理 |
|---|---|---|
| `nalu.c/h` | ✅ 基本符合 | 重命名加 `proto_` 前缀 |
| `rtp.c/h` | ✅ 基本符合 | 重命名加 `proto_` 前缀 |
| `rtp.c` | `g_pktbuf` 是文件级静态,多线程共享 → **必须在注释里声明"仅在单线程调用"或改为上下文内缓冲** | 修正 |
| `rtp_test.c` | `uint8_t txbuf[NALU_MAX_SIZE]` 1MB 在栈上(且未使用) | 删除该变量 |
| `tools/*.c` | 无 Doxygen 头 | 补齐或标注为测试工具 |

---

## 十三、2026-09-16:把"没人查的规矩"变成机器可查(并清零)

起因:用户问"代码文件的编写应该按照我的规范来写吧?" ——
这件事**不能靠嘴回答**。查下来发现:§2.2 / §3.1 / §3.2 / §4.1 / §4.3 / §6.3 / §7.1 / §9
**这八条一直是没人查的**,而"没人查的规矩等于没有"(规矩 8)。

于是补了 **`tools/check_spec.py`**:

```
python tools/check_spec.py            # 只报有问题的
python tools/check_spec.py -v         # 连通过的文件也列
python tools/check_spec.py --detail   # 每条违规都列出位置
```

### 13.1 首轮结果 → 现状

| 条目 | 首轮 | 现在 |
|---|---|---|
| §2.2 分层前缀 | ⚠️ 4 处(全在 `bsp/osd_render.h`) | ✅ **0**(改名 `bsp_osd_render.*`,104 处符号) |
| §3.1 include guard | (误报 16) | ✅ **0** |
| §3.2 文件头四要素 | ⚠️ **14 个 `.c` 缺**(56 行) | ✅ **0**(逐个按模块实际情况写,非套模板) |
| §4.1 Doxygen | ⚠️ 缺 `@brief` 161 + 完全没文档 15 | ✅ **0**(脚本补 `@brief`;15 个手写) |
| §4.3 无说明的 TODO | ✅ 0 | ✅ 0 |
| §6.3 栈上大数组 | ✅ 0 | ✅ 0 |
| §7.1 线程命名 | ⚠️ 4 个线程没 `prctl` | ✅ **0**(`ipc_media`/`ipc_net`/`ipc_send`/`ipc_osd`) |
| §9 行宽 ≤100 列 | ✅ 0(曾误报 29) | ✅ **0** |

> **§6.1 动态分配**单独说明:全项目 14 处 `malloc/calloc` 经人工核对,**全部在
> `*_start` / `*_create` 里且都有配对的 free** —— 属于规范明确允许的"启动时一次性分配"。
> 机器判不出"启动期 vs 运行期",所以这一条**降级为提示、不计违规**
> (宁可报"需人工确认",不要误报)。

### 13.2 ⭐ 比结论更值钱的:工具本身被质疑了四次

| # | 现象 | 真相 |
|:--:|---|---|
| 1 | 报"**29 行超 100 列**"(最长 217 列) | **量错口径** —— PS 5.1 把 UTF-8 中文当 GBK 读,长度虚高。改成按 UTF-8 + **显示宽度**(CJK=2 列)后**一行都没超** |
| 2 | 报"**16 个头文件全缺 include guard**" | 扫描窗口写成"第 1 行" —— 而我们的头文件都以 `/** @file */` 开头 |
| 3 | 报"**179 个函数缺 Doxygen**" | 不认"public 函数的文档写在 `.h` 里"这个通行做法 |
| 4 | 又把 `/* ─── 分节线 ─── */` 当成文档块 | 跳过逻辑只判"前一行以 `*/` 结尾",没判是不是 `/**` |

**结论**:`测量口径错了, 结论一定错, 而且会带着你去改正确的东西。`
这与 **B020**(度量工具错三次)/ **B026**(验证脚本自己骗人)是同一类错。
**所以工具本身也要被质疑** —— 落盘前抽查 diff, 是这次唯一挡住事故的动作
(补 `@brief` 的脚本第一版把 `/** 单行注释 */` 改成了 `* @brief /** … */`)。

### 13.3 怎么防复发

- ⭐ **已装 `pre-commit` 钩子**(2026-09-16):提交前自动跑三件套, **红了不让提交**。
  起因是用户那句"以后写代码还会忘记我这个代码规范吗" —— **诚实答案是会**
  (同一天我在明知规范的情况下写出 4 处前缀违规), 所以正解不是"记住",
  而是**让工具挡在提交路径上**。装的脚本:`work/install_git_hook.py`(可重装/换机)。
  已实测:塞一个违规函数 → `git commit` **退出码 1、提交被拒**;清理后仓库干净。
  ⚠️ 它只挡机器查得出的条目;§4.2(注释讲"为什么")、§5.2(系统调用返回值)这些
  **机器查不了**, 仍靠 review。紧急绕过:`git commit --no-verify`。
- `check_spec.py` 与 `check_style.py` / `check_encoding.py` 并列为**必跑体检**
- 批量改写脚本必须:**在原行上做手术**(不重建整行)+ **不变量自检**
  (`/**` 与 `*/` 数量必须一致)+ **落盘前抽查一处 diff**
- 修完必须重跑:PC 全量单测(7 套)+ 交叉编译 + `check_*` 三件套
