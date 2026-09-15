# IPC 摄像头项目 —— 环境就绪度与 M0 进展

> 目标:复刻 [IPC-Camera](https://github.com/Qinze7777777/IPC-Camera)
> 推流端 Hi3516DV300(照做)+ 拉流端改用 PC(替代 IMX6ULL)

---

## 一、✅ 环境已完全就绪

| 能力 | 状态 | 验证方式 |
|---|---|---|
| 交叉工具链 | ✅ | `arm-himix200-linux-gcc 6.3.0`,编过内核 |
| 内核源码树 | ✅ | 与板子内核 vermagic 一致,可编驱动 |
| MPP 源码 + 预编译库 | ✅ | `smp/a7_linux/mpp` |
| **MPP sample 编译** | ✅ | **`sample_venc`、`sample_vio` 均一次编过** |
| GC2053 sensor 支持 | ✅ | 编译参数里就是 `GALAXYCORE_GC2053_MIPI_2M_30FPS_10BIT` |
| NFS 快速迭代 | ✅ | 板子开机自动挂载,md5 校验一致 |
| 板子固件 + 恢复镜像 | ✅ | 逐字节验证过 |

**结论:程序能编、能传、能在板子上跑起来(动态库解析成功、打印 usage)。**

---

## 二、✅ M0 已完全跑通(含根因分析)

### 最终结果

```
stream_chn0.h265   12,606,551 字节    H.265 裸流
stream_chn1.h264   12,578,657 字节    H.264 裸流
I2C_WRITE error    0 次
```

文件头验证(标准参数集):

```
h265: 00 00 00 01 40 01 ...   → 起始码 + VPS
h264: 00 00 00 01 67 42 ...   → 起始码 + SPS
      00 00 00 01 68 ce ...   → 起始码 + PPS
```

近 25 秒录出 12.6MB × 2 路 ≈ 4Mbps,数值合理。

> **【2026-09-14 追加修正】** 「25 秒」是估算;实测时长 **24.473 秒**(chn1)/
> **24.507 秒**(chn0),码率 = 12,578,657 B ÷ 24.473 s × 8 = **4.11 Mbps**。
> 见 `docs/问题与解决记录.md` B011。
>
> 另外原文写的 "≈ 4Mbps@1080p30" 也不准确 ——
> 后来用 `work/extract_params.py` 从 MP4 的 avcC/hvcC 盒子里**实测**:

| 通道 | 编码 | **分辨率** | 档次 profile | 级别 level |
|---|---|---|---|---|
| chn0 | H.265 | **1920×1080** | Main | **4.1** |
| chn1 | H.264 | **1280×720** | Baseline | **3.1** |

Level 与分辨率的对应关系完全自洽(3.1→720p30,4.1→1080p30),可信。

### 根因(三层)

**第一层:摄像头接在 `sensor1` / MIPI1 / `i2c-1`,而 SDK 默认用 `sensor0`**

启动日志实测:

```
stPubAttr.enWDRMode=0  ViPipe=1 u32SnsId=1
********GC2053: /dev/i2c-1********
GC2053 init succuss!                    ← sensor1 成功

stPubAttr.enWDRMode=0  ViPipe=0 u32SnsId=0
********GC2053: /dev/i2c-0********
[Func]:gc2053_write_register ... I2C_WRITE error!   ← sensor0 空槽,反复失败
```

> ⚠️ **重要教训**:那 90 条 `I2C_WRITE error` 全部来自**空着的 sensor0**,
> 是**正常现象**,不代表摄像头故障。一开始我误判成硬件问题,差点走弯路。
> **排查时一定要区分是哪一路在报错。**

**第二层:`SAMPLE_COMM_VI_GetComboDevBySensor()` 没有 GC2053 分支**

```c
switch (enMode) {
    case SONY_IMX327_...:  ...  case SONY_IMX415_...:
        if (0 == s32SnsIdx)      dev = 0;
        else if (1 == s32SnsIdx) dev = 2;    // 索尼是 0/2 映射
        break;
    default:
        dev = 0;                              // ← GC2053 掉这里,永远返回 0
}
```

所以传任何 idx 都返回 0 → MIPI 设备号错误 → VI 取不到数据。
(GC2053 的正确映射是 **MipiDev = sensor 序号**,即 0/1)

**第三层:输入提示顺序**

`sample_venc` 是**先问 rc 模式、再问 gop 模式**,自动化脚本喂输入时顺序别搞反。

### 修复(4 处改动,已备份 `sample_venc.c.sdk_bak`)

```c
// SAMPLE_VENC_1080P_CLASSIC  (index 0 分支)
VI_DEV  ViDev  = 1;      // 原 0
VI_PIPE ViPipe = 1;      // 原 0

// SAMPLE_VENC_VI_Init
pstViConfig->astViInfo[0].stSnsInfo.s32BusId = 1;   // 原 0  → i2c-1
pstViConfig->astViInfo[0].stSnsInfo.MipiDev  = 1;   // 原调用 SDK 函数(返回0)
```

### 产物位置

| 文件 | 说明 |
|---|---|
| `sample_venc_sns1` | 改好并编译的样例,在 Ubuntu `~/nfs_share/` |
| `/mnt/nfs/m0/stream_chn0.h265` | 板子上录的 H.265 |
| `/mnt/nfs/m0/stream_chn1.h264` | 板子上录的 H.264 |
| `sample_venc.c.sdk_bak` | SDK 原版备份 |

---

## 三、排查过程中确认的技术细节(有复用价值)

### 1. `#if 0` 陷阱

SDK 的 `sample/vio/smp/sample_vio.c` 里,**mipi0 的正确配置块被 `#if 0` 关掉了**,
默认走的是"双摄"的 `#else` 分支。易百纳的"可用"版把它改成 `#if 1`。

备份已留:`sample_vio.c.sdk_bak`

### 2. I2C 总线是运行时传的

```c
// sample_comm_isp.c:753
SAMPLE_COMM_ISP_BindSns(ISP_DEV IspDev, HI_U32 u32SnsId,
                        SAMPLE_SNS_TYPE_E enSnsType, HI_S8 s8SnsDev)
...
uSnsBusInfo.s8I2cDev = s8SnsDev;
pstSnsObj->pfnSetBusInfo(IspDev, uSnsBusInfo);   // 运行时设置总线
```

`gc2053_cmos.c` 里的 `g_aunGc2053BusInfo[0].s8I2cDev = 0` 只是默认值。

### 3. MPP 库是**静态链接**的

```
sample_vio 的 NEEDED: libpthread / libm / libdl / libstdc++ / libc / ld-linux.so.3
```

**没有 `libmpi.so`** —— MPP 全部静态链进二进制。所以板子上找不到 `libmpi.so` 是正常的。

### 4. 板子传感器连接(易百纳 readme)

```
可执行文件使用sensor0: ./sample_vio_mipi0 0
可执行文件使用sensor1: ./sample_vio_mipi2 0
```

`/app/` 里正是这两个预编译程序。

---

## 四、下一步:M1 写 RTSP/RTP 服务端(现在可以开始了)

M0 已给出可信基线,可以直接把"存文件"换成"发网络":

```
M1  RTSP/RTP 服务端
      ├─ RTSP 交互(RTCP over TCP 控制通道): OPTIONS/DESCRIBE/SETUP/PLAY/TEARDOWN
      ├─ RTP 打包(把 VENC 出来的 NALU 按 RFC 6184 切成 RTP 包)
      ├─ UDP 发送
      └─ 多线程 + epoll 处理并发连接
       ↓
    PC 用 VLC 拉流验证:  rtsp://192.168.16.88:554/live
       ↓
M2  OSD(REGION 模块叠时间)
       ↓
M3  MP4 录制(mp4v2)+ SD 卡环形覆盖
       ↓
M4  PC 拉流端(Qt + FFmpeg)
```

### M1 的关键改动点(相对 M0 基线)

在 `sample_venc` 的取流线程 `SAMPLE_COMM_VENC_GetVencStreamProc` 里,
原本是 `fwrite(...)` 写文件,改成"交给 RTSP 发送队列"即可。

**注意**:M0 里 `SAMPLE_COMM_VENC_GetVencStreamProc` 报 `get venc stream time out`,
是因为 VENC 通道没按预期出流;实际出流在另一条路径。写 M1 时要先确认取流通道。

### 环境使用速查

```powershell
# 板子通道(二选一,可同时用)
python tools/board_serial.py "命令"      # 串口 COM4
python tools/board_telnet.py "命令"      # telnet 192.168.16.88:23 (root/空密码)

# 编译 MPP sample
export PATH=/opt/hisi-linux/toolchain/arm-himix200-linux/arm-himix200-linux/bin:$PATH
cd ~/hi3516_sdk/Hi3516CV500_SDK_V2.0.2.0/smp/a7_linux/mpp/sample/venc && make

# 板子上录制 25 秒码流到 NFS
cd /mnt/nfs/m0 && sh /mnt/nfs/s2_run.sh
```

**坐标**
- 板子 `192.168.16.88`,Ubuntu `192.168.16.100`,NFS 共享 `~/nfs_share` → 板子 `/mnt/nfs`
- 摄像头在 **sensor1 / MIPI1 / i2c-1**
- 内核编译需加 `HOST_EXTRACFLAGS=-fcommon`

---

## 五、M3(MP4 录制)前置条件:已全部就绪 ✅

### 1. TF 卡已挂载

```sh
mount /dev/mmcblk0p1 /mnt/sdcard     # vfat,29.7G
```

> 挂载时会提示 `Volume was not properly unmounted`,可忽略(FAT 未正常卸载的常见提示)。

### 2. mp4v2 已交叉编译并验证

```
/tgj/mp4v2-arm/
├── include/mp4v2/     13 个头文件
└── lib/
    ├── libmp4v2.a     ARM 静态库
    └── libmp4v2.so    ARM 动态库
```

**板子上实测通过**:`mp4v2: link + MP4Create OK`

编译要点(踩过的坑):

| 问题 | 解决 |
|---|---|
| GitHub 直连不稳定 | **走代理 `http://192.168.17.1:7897`**(Clash Verge 在 Windows 上,允许了局域网) |
| `autotools` 构建失败:`undefined reference to MP4Vp09Atom` | autotools 的 `GNUmakefile.am` **漏了 `src/atom_vp09.cpp`**;改用 **CMake**(其 `CMakeLists.txt:185` 有这个文件) |
| `-DBUILD_SHARED_LIBS=OFF` 无效 | 正确选项是 **`-DMP4V2_BUILD_SHARED=OFF`** |
| CMakeLists 没有 install 规则 | 手动拷贝 `include/` 和 `lib/` |

编译命令(留档):

```bash
export PATH=/opt/hisi-linux/toolchain/arm-himix200-linux/arm-himix200-linux/bin:$PATH
cd /tmp/mp4v2-src && mkdir build-arm-static && cd build-arm-static
cmake .. -DCMAKE_SYSTEM_NAME=Linux \
  -DCMAKE_C_COMPILER=arm-himix200-linux-gcc \
  -DCMAKE_CXX_COMPILER=arm-himix200-linux-g++ \
  -DCMAKE_BUILD_TYPE=Release -DMP4V2_BUILD_SHARED=OFF \
  -DCMAKE_C_FLAGS="-mcpu=cortex-a7 -mfloat-abi=softfp -mfpu=neon-vfpv4 -O2" \
  -DCMAKE_CXX_FLAGS="-mcpu=cortex-a7 -mfloat-abi=softfp -mfpu=neon-vfpv4 -O2"
make -j4
```

### 3. 🎁 意外收获:TF 卡上有参考录像,规格已知

板子 TF 卡里有前人录的 4 个 MP4,**把参考实现的规格完全暴露了**:

| 参数 | 值 |
|---|---|
| 编码 | H.264 **1280×720 / 30fps / ~4Mbps** |
| 封装 | mp4v2(`ftyp mp42/isom` + `free`) |
| **分段策略** | **按起始时间命名** `年-月-日-时-分-秒.mp4`,每段 2~4 分钟 |
| 单段大小 | 22~115 MB |

**4 个文件里 2 个完好可播、1 个残缺**(无 `moov`),正是**被强制中断**导致的。

> ⚠️ **M3 的关键设计点**:mp4v2 默认在 `MP4Close` 时才写 `moov` 索引。
> 若进程被杀 / 断电,文件就永久不可播。**参考实现就栽在这里。**
> 你的实现应当处理:① 分段录制(天然降低损失面);② 定时/信号触发优雅关闭;
> ③ 或考虑用分片 MP4(fMP4)。**这是能体现工程细节的加分项。**

---

## 六、环境补充信息

### 代理(重要,以后遇到网络问题走这条)

```
Windows 侧 : http://127.0.0.1:7897        (Clash Verge)
Ubuntu 侧  : http://192.168.17.1:7897     (已开 Allow LAN)
```

用法示例:

```bash
wget -e use_proxy=yes -e https_proxy=http://192.168.17.1:7897 <url>
curl -x http://192.168.17.1:7897 <url>
git -c http.proxy=http://192.168.17.1:7897 clone <repo>
apt-get -o Acquire::https::Proxy::=... install ...
```

### ⚠️ NFS 执行坑

**直接执行 `/mnt/nfs/` 下刚创建的二进制会报 `No such file or directory`**(NFS 属性缓存)。
**先 `cp` 到 `/tmp` 再执行就正常。** 写部署脚本时注意。

### 板子管理通道

```powershell
python tools/board_serial.py "命令"      # 串口 COM4
python tools/board_telnet.py "命令"      # telnet 192.168.16.88:23 (root/空密码)
```
