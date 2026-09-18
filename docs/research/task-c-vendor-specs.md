# Task C：视频监控厂商与标准 —— 分段时长 / 单文件大小 / SD 卡选型 / ONVIF Profile G / 海思 sample

调研目标：为 Hi3516DV300 IPC（H.264 1280x720@30fps ≈4 Mbps，32 GB TF 卡 vfat/FAT32，
按时间分段、文件名零填充时间戳、环形覆盖删最旧）找**可引用的行业依据**。

---

## Sources

[1] ONVIF — ONVIF Profile G Specification v1.1 (PDF, 19 页) | https://www.onvif.org/wp-content/uploads/2025/11/ONVIF-Profile-G-Specification-v1-1.pdf | Source-Type: official | As Of: 2025-10（Revision history 载明 1.1 = October 2025；1.0 = June 2014） | Authority: 10 | 访问: public

[2] ONVIF — Profile G（产品页） | https://www.onvif.org/profiles/profile-g/ | Source-Type: official | As Of: unknown | Authority: 9 | 访问: public

[3] Axis Communications — Surveillance cards for edge storage（White paper, June 2025） | https://whitepapers.axis.com/en-us/surveillance-cards-for-edge-storage | Source-Type: official | As Of: 2025-06 | Authority: 9 | 访问: public

[4] SD Association — Capacity (SD/SDHC/SDXC/SDUC) | https://www.sdcard.org/developers/sd-standard-overview/capacity-sd-sdhc-sdxc-sduc/ | Source-Type: official | As Of: unknown（页脚 © 2000–2026） | Authority: 9 | 访问: public

[5] SD Association — Speed Class | https://www.sdcard.org/developers/sd-standard-overview/speed-class/ | Source-Type: official | As Of: unknown | Authority: 9 | 访问: public

[6] Microsoft — Maximum Volume Sizes（Windows 2000 Server 技术参考） | https://learn.microsoft.com/zh-tw/previous-versions/windows/it-pro/windows-2000-server/cc938432(v=technet.10) | Source-Type: official | As Of: 2008-09-11（页脚 Last updated） | Authority: 8 | 访问: public

[7] Microsoft — exFAT file system specification | https://learn.microsoft.com/en-us/windows/win32/fileio/exfat-specification | Source-Type: official | As Of: unknown | Authority: 9 | 访问: public

[8] Microsoft Learn — Define the Windows Server file system | https://learn.microsoft.com/en-us/training/modules/manage-windows-server-file-servers/2-define-windows-server-file-system | Source-Type: official | As Of: unknown | Authority: 7 | 访问: public

[9] Hikvision（海康威视）— 计划录像（NP-V1W 系列安消智能摄像机 Web 手册） | http://pinfo.hikvision.com/unzip/20200512195435_50573_doc/GUID-9AB40A05-949B-41E6-B473-AE87B4FC2AF1.html | Source-Type: official | As Of: unknown（同批手册版权年 2020） | Authority: 8 | 访问: public

[10] Hikvision — 格式化SD卡（同上手册） | http://pinfo.hikvision.com/unzip/20200512195435_50573_doc/GUID-49A7995B-D170-4008-8AE3-A5346FC9846D.html | Source-Type: official | As Of: unknown | Authority: 8 | 访问: public

[11] Hikvision — 本地配置（同上手册） | http://pinfo.hikvision.com/unzip/20200512195435_50573_doc/GUID-F8426344-C507-4E4B-81C8-690CAA176CF8.html | Source-Type: official | As Of: unknown | Authority: 8 | 访问: public

[12] Hikvision — 配置录像高级参数（iDS 智脑网络硬盘录像机 77/86/FA 系列 Web 手册） | https://pinfo.hikvision.com/hkws/unzip/20200511171130_17291_doc/GUID-0C883933-3211-4918-B9A7-D581DCA56661.html | Source-Type: official | As Of: unknown（页脚 版权所有©杭州海康威视数字技术股份有限公司 2020） | Authority: 8 | 访问: public

[13] Hikvision — 设备网络SDK开发使用手册 · NET_DVR_LOCAL_GENERAL_CFG | https://open.hikvision.com/hardware/structures/NET_DVR_LOCAL_GENERAL_CFG.html | Source-Type: official | As Of: unknown | Authority: 9 | 访问: public

[14] Hikvision — 网络硬盘录像机用户手册（PDF，89 页，自 pinfo.hikvision.com 下载） | http://pinfo.hikvision.com/hkws/unzip/20240702151841_55972_doc/pdf.pdf | Source-Type: official | As Of: unknown | Authority: 7 | 访问: public

[15] Samsung Newsroom — Samsung Electronics Redefines High Endurance Memory Card Market with New PRO Endurance Card | https://news.samsung.com/uk/samsung-electronics-redefines-high-endurance-memory-card-market-with-new-pro-endurance-card | Source-Type: official | As Of: 2018-05-02 | Authority: 8 | 访问: public

[16] 弱电智能网（作者：卡卡）— 海康录像机本地(硬盘)录像文件大小打包规则及如何修改？ | https://www.ruodian360.com/tech/security-monitoring/33548.html | Source-Type: secondary-industry | As Of: 2022-09-14 | Authority: 4 | 访问: public

[17] 天糊土（CSDN 博客，整理自朱有鹏嵌入式课程）— 第二季8：保存编码得到的码流（step6：Save to File） | https://xiefor100.blog.csdn.net/article/details/128289011 | Source-Type: community | As Of: 首发 2022-12-12，修改 2022-12-16 | Authority: 5 | 访问: public

[18] Uniview（宇视）— EZStation 3.0 User Manual V1.17（PDF，74 页） | http://sgcdn.uniview.com/us/res/202103/31/20210331_1790096_EZStation%203.0%20User%20Manual-V1.17_788621_168459_0.pdf | Source-Type: official | As Of: 2021-03 | Authority: 7 | 访问: public

---

## Findings

1. ONVIF Profile G 规范（v1.1，2025-10）全文 19 页里 "Segment"、"Retention"、"Storage"、"Export"、"File"、"duration"、"size" 这些词**命中次数全部为 0**，即该标准**完全不管分段时长、留存期和单文件大小**，只定义 Recording / Track / RecordingJob 抽象模型 [1]
2. Profile G 强制设备实现 FindRecordings、GetRecordingSearchResults、EndSearch、GetRecordingSummary、GetRecordingInformation、GetMediaAttributes，回放必须走 GetReplayUri + `onvif-replay` RTSP feature tag —— 检索与回放都在「时间轴 + RTSP 拉流」层面，与底层文件怎么切完全解耦 [1]
3. 海康官方 SDK 结构体 NET_DVR_LOCAL_GENERAL_CFG 中 `i64FileSize` 的定义是「文件最大限制字节数，单位：Byte」，`byNotSplitRecordFile` 的缺省值是 `0`（切片）—— 官方在本地录像上是**按字节大小切片**而非按时长 [13]
4. 海康 Web 手册明确存在「录像文件打包大小」（= 存放在本地的单个录像文件的大小）与「录像/图片过期时间（天）」（超过即强制删除）两个配置项，但页面**只给参数名，不给可选范围或默认值** [11][12]
5. 行业二手资料称：海康本地（硬盘）定时录像**默认以 1 GB 打包**，事件录像超过 1 GB 则按 1 GB 切成多个文件；网页端「录像文件打包时间」可选 **1–300 分钟**（该数字未能在海康官方页面得到印证）[16]
6. SD 协会官方规定：SDHC（>2 GB–32 GB）的标准文件系统是 **FAT32**，SDXC（>32 GB–2 TB）是 **exFAT** —— 32 GB 卡正落在 SDHC / FAT32 一侧 [4]
7. Microsoft 官方：FAT32 **单文件上限 2^32−1 字节（≈4 GiB）**，这是硬上限；Windows 2000 格式化工具建 FAT32 卷上限 32 GB，但可挂载更大（如 127.53 GB）的 FAT32 卷 [6]
8. Microsoft 官方 exFAT 规范：exFAT 用 **64 位**描述文件大小（DataLength 字段 8 字节），设计目标之一就是「支持非常大的文件」，因此不存在 4 GiB 单文件限制 [7]
9. Axis 官方白皮书（2025-06）明确建议监控卡使用 **ext4**，理由是 journaling 文件系统在系统崩溃或断电后可更快恢复、更不易损坏，从而降低数据丢失风险；同文给出 NAND 寿命量级：SLC ≈100,000、MLC ≈10,000、TLC ≈3,000、QLC ≈1,000 次 P/E cycle [3]
10. Samsung 官方新闻稿（2018-05-02）给出可对账的耐久数字：PRO Endurance **128 GB 在 FHD 26 Mbps 下 43,800 小时连续录像、5 年有限保固**；64 GB 为 26,280 小时/3 年；32 GB 为 17,520 小时/2 年；UHS-I，顺序读 100 MB/s、写 30 MB/s [15]

---

## Deep Read Notes

### A. ONVIF Profile G Specification v1.1 —— 全文读完（19 页，用 PyMuPDF 抽文本后逐节读）

- **版本与历史**（Revision history 页）：
  - `1.0 | June 2014 | Original release version 1.0`
  - `1.1 | October 2025 | Editorial correction of interface name CreateRecordingJob`
  - 注意：正文页眉仍印着 "Ver. 1.0"，只有修订历史页写明 1.1 —— 引用时建议按 1.1 / 2025-10 写。
- **§5 版本要求**：`Implementation of ONVIF Network Interface Specification Set v2.4 or later is required for conformance to Profile G.`
- **§7.3.1 设备强制项（Recording Search）**：
  `Devices shall support recording search with the FindRecordings, GetRecordingSearchResults and EndSearch operations.`
  `Devices shall support event search with the FindEvents, GetEventSearchResults and EndSearch operations.`
  `Devices shall support retrieval of information related to recordings with the GetRecordingSummary, GetRecordingInformation and GetMediaAttributes operations.`
  `Devices shall deliver notifications for Recording and Track state changes.`
- **§7.4.1 回放强制项**：
  `Device shall support media replay using the GetReplayUri operation according to the ONVIF Streaming Specification v.2.2.1 or later.`
  `Device shall support the "onvif-replay" feature tag as described in 6.3 RTSP Feature Tag of the ONVIF Streaming Specification.`
  `Device may support reverse replay.`（反向回放是 **may**，不是必须）
- **§7.4.2 客户端要求**：`Clients shall implement decoding of all ONVIF supported formats (H.264, MPEG-4, M-JPEG).`
- **§9.1.1 录像控制强制项**：`GetRecordings`、`GetRecordingOptions`、`GetRecordingJobs` / `CreateRecordingJob` / `DeleteRecordingJob`、`GetRecordingJobState` / `SetRecordingJobMode`。
- **§8.1（条件项）动态录像**：`CreateRecording` / `DeleteRecording` / `CreateTrack` / `DeleteTrack` —— 只有设备本身支持动态增删录像/轨道时才必须实现。
- **关键否定结论（可直接引用）**：我对全文做了关键词计数，`Segment`=0、`Retention`=0、`Storage`=0、`Export`=0、`File`=0、`duration`=0、`size`=0。
  → 所以「分段时长该多长」「单文件多大」**在 ONVIF Profile G 里找不到任何依据**，Profile G 不是这个问题的标准来源。
- 另一个可引用点：§3 定义 `Recording` = "Represents the currently stored media (if any) and metadata on the NVS from a single data source. **A recording comprises one or more tracks.**" —— 它把「一段录像」建模成逻辑对象 + 若干 track，而不是文件。

### B. Axis《Surveillance cards for edge storage》(White paper, June 2025) —— 全文读完

这是本次找到的**最贴近「SD 卡选型 + 断电风险」的官方厂商文档**，要点：

- **文件系统（与断电直接相关，原文）**：
  > "Axis recommends you use the file system **ext4** for surveillance cards. This is a journaling file system, which employs a journal — a specific type of data structure — to record changes as they happen. **Should there be a system crash or power outage, this kind of file system can be restored more swiftly and is less prone to corruption, thereby reducing the risk of losing data.** This feature can be especially important in environments where power sometimes goes down, for example for devices installed on buses or trains, but also devices in regions with unreliable power supply."
  → 注意：Axis 并没有写「异常断电会损坏正在录制的文件」这句直白警告，它给的是「用 ext4 来降低风险」，属于间接表述。
- **NAND 寿命量级（原文数字）**：SLC ≈ `100,000 P/E cycles`；MLC ≈ `10,000`；TLC ≈ `3,000`；QLC ≈ `1,000`。`Cards that store more bits per cell generally endure fewer P/E cycles.`
- **卡的寿命与码率关系（模拟值表，原文 Table）**：

  | Card size | 2 MP, 2.5 Mb/s | 5 MP, 3.5 Mb/s | 8 MP, 4.5 Mb/s |
  |---|---|---|---|
  | 128 GB | ~10 years | ~7 years | ~5 years |
  | 256 GB | ~20 years | ~14 years | ~11 years |
  | 512 GB | ~26 years | ~19 years | ~15 years |
  | 1 TB | ~53 years | ~38 years | ~29 years |

  → 可直接引用的结论：**码率越高、卡越小，寿命越短；同码率下寿命大致随容量线性增长**。128 GB 在 2.5 Mb/s 下约 10 年。
- **实测可靠性**：`98.0% of 128 GB cards, 99.9% of 256 GB cards, and >99.9% of 512 GB cards` 5 年后仍在正常工作；`More than 90% of the cards are still functional even after 10 years.`
- **保存期（retention）**：官方示例 —— 128 GB 卡的实际最大保存期**通常 20 到 215+ 天**（取决于分辨率与配置）；`You can configure the retention time you need in the camera's web interface.` 清理任务 `runs once every 60 minutes`，另有持续运行的自动清理。
- **质保**：Axis 监控卡 **5 年质保**（免费 RMA）；`versions with 256 GB storage and higher have shown to generally last even beyond 10 years`。原文还明确指出 `SD cards sometimes come with warranty that does not cover surveillance use cases.` —— 即**消费级卡的质保常常不覆盖监控写入场景**，这是选型时可以直接引用的厂商原话。
- **WAF**：`Ideally, the write amplification factor should be as close to 1 as possible`；高 WAF 会加速磨损。Axis 卡是 TLC/QLC + 低 WAF。

### C. 海康官方文档组（SDK 结构体 + Web 手册 + NVR PDF）+ 宇视手册 —— 逐页读关键节

- **C1. 海康设备网络 SDK（官方）** `NET_DVR_LOCAL_GENERAL_CFG` 原文（我用 GBK 解码后逐字核对）：
  - `byNotSplitRecordFile`：`回放和预览中保存到本地录像文件不切片：0- 切片（默认），1- 不切片`
  - `i64FileSize`：`文件最大限制字节数，单位：Byte，启用切片（byNotSplitRecordFile为0）时，预览和回放保存的录像文件超过这个大小限制会自动切片，即新建文件进行保存`
  - → **这是「厂商官方按字节切片」最硬的证据**，而且是可配置的字节阈值。
- **C2. 海康 Web 手册「计划录像」原文（官方）**：
  - `循环写入`：`若勾选循环写入，当存储空间满之后，将覆盖最早的录像文件；若不勾选，则存储空间满后将停止录像。`
  - `预录时间` / `录像延时` / `码流类型`；说明里写 `码率选择越高，预录时间会变短。`
  - → 与本项目的「环形覆盖删最旧」完全一致，可引用作为行业惯例依据。
- **C3. 海康 Web 手册「配置录像高级参数」原文（官方）**：参数为 记录音频 / 预录时间 / 录像延时 / 码流类型 / **录像/图片过期时间（天）**（`硬盘内文件最长保存时间，超过该时间的文件会被强制删除。`）/ 冗余录像。**页面没有给任何数值范围或默认值**。
- **C4. 海康 Web 手册「本地配置」原文（官方）**：`录像文件打包大小 —— 表示存放在本地的单个录像文件的大小。` **同样没有数值。**
- **C5. 海康 Web 手册「格式化SD卡」原文（官方）**：`初次使用SD卡，请登录设备Web客户端，将SD卡格式化。` 步骤为 存储管理 > 硬盘管理 > 勾选 SD 卡 > `配置磁盘中抓图和录像配额` > 格式化。**手册只说「格式化」，不暴露文件系统类型**（是 FAT32 / exFAT / 私有格式，官方文档未写）。
- **C6. 海康 NVR 用户手册 PDF（89 页，官方）**：全文检索 `打包` 只命中 1 处 —— 手动录像章节的 `进入 配置 → 本地，设置录像文件打包大小和录像文件保存路径`，仍然没有数值。检索 `断电` = 0 次、`异常断电` = 0 次；`掉电` 命中的都是 PTZ「掉电记忆」（记忆云台位置），**与录像文件完整性无关**。
  → 明确结论：**海康这份 NVR 手册里没有任何「异常断电可能损坏录像文件」的警告或建议。**
- **C7. 宇视 EZStation 3.0 用户手册（官方，74 页）**：有 `Recording file size: The size of a single recording.` 这一项，另有 `Stop: Recording stops when space is used up.` 与 `Recording space: The disk space used to save recordings. Note: It's recommended to set at least 2GB recording space.` —— **只给名字与「建议至少 2 GB 录像空间」，没有默认分段时长/文件大小**。

### D. 补充阅读（community）：海思 SDK venc sample 的取流/存流写法

来源是 CSDN 上整理自朱有鹏嵌入式课程的博文 [17]，它**逐字贴出了 SDK sample 源码**（我按代码块核对过），可作为「海思官方 sample 怎么写」的旁证（非官方直发，已标注 community）：

- `SAMPLE_COMM_VENC_GetVencStreamProc` 的 step1 里对每个通道 `pFile[i] = fopen(aszFileName[i], "wb");`，文件名形如 `stream_chn0.h264`（`sprintf(aszFileName[i], "stream_chn%d%s", i, szFilePostfix)`）；step3 在**线程退出时**才 `fclose(pFile[i])`。
- 保存函数原文：
  ```c
  HI_S32 SAMPLE_COMM_VENC_SaveH264(FILE* fpH264File, VENC_STREAM_S *pstStream)
  {
      HI_S32 i;
      for (i = 0; i < pstStream->u32PackCount; i++)
      {
          fwrite(pstStream->pstPack[i].pu8Addr + pstStream->pstPack[i].u32Offset,
                 pstStream->pstPack[i].u32Len - pstStream->pstPack[i].u32Offset,
                 1, fpH264File);
          fflush(fpH264File);
      }
      return HI_SUCCESS;
  }
  ```
- **关键结论**：官方 sample 是「**一个通道一个文件、开到线程结束才关**」，**没有任何分段 / 滚动 / 大小检查逻辑**，而且每写一个 pack 就 `fflush` 一次。也就是说「分段 + 环形覆盖」在官方 sample 里是**不存在的**，必须自己加。
- 同一份 sample 里官方注释还写着：`suggest to check both u32CurPacks and u32LeftStreamFrames at the same time` —— 但活代码只检查了 `u32CurPacks`（本项目 B027 已经踩过这个坑）。
- **没找到**：搜索 `SAMPLE_COMM_VENC_SaveH264ToMP4` 这个符号**没有任何公开出处**（GitHub / CSDN / 博客均无），公开渠道能看到的海思官方保存函数只有 `SAMPLE_COMM_VENC_SaveH264` / `SAMPLE_COMM_VENC_SaveStream` / `SaveH265`。MP4 封装不在公开可见的 sample_comm_venc.c 里。

### E. 码率 ↔ 容量 ↔ 寿命换算（**这一段是我基于上面引用数字做的算术，不是引用**）

- 通用公式：`每天写入量(GB) = 码率(Mbps) ÷ 8 × 3600 × 24 ÷ 1000`
- 本项目的 4 Mbps：`4 ÷ 8 = 0.5 MB/s` → `1.8 GB/小时` → **`43.2 GB/天`**
- 32 GB 卡装满一轮的时间：`32 ÷ 43.2 ≈ 0.74 天 ≈ 17.8 小时`（即这张卡一天要整卡覆写约 1.35 次）
- 用 Samsung 的官方数字反推对账 [15]：26 Mbps → `3.25 MB/s` → `11.7 GB/h`；×43,800 h ≈ `512 TB`；`512 TB ÷ 128 GB ≈ 4,000 次全卡写入` —— 与 Axis 给的 TLC ≈3,000 次 P/E 量级一致 [3]，说明 Samsung 的「43,800 小时」本质就是「约 4000 次整卡擦写」。
- 反过来算：32 GB 的 TLC 卡按 ~3,000 P/E 计，理论总写入寿命约 `96 TB`；本项目 43.2 GB/天 → 约 `2,220 天 ≈ 6 年`（这是纯算术外推，忽略了 WAF、写入放大与坏块管理，**不能当作承诺值**）。

---

## Gaps

### G1. 明确「没找到」的（尤其厂商文档里没写清的地方）

- **没有找到任何官方文档规定「IPC 上默认分段时长是多少分钟」。** 海康、大华、宇视的 Web 手册我都翻到了对应章节：海康只给「录像文件打包大小」「录像文件打包时间」这两个**参数名**，宇视只给「Recording file size」这个参数名，**全都没有默认值和可选范围**。「海康默认 1 GB」这一条只有二手站点 [16]，**没能追到海康官方页面**。
- **大华（Dahua）的 `Pack Duration` 具体范围与默认值没拿到**（参数本身确实存在，见 G2 未打开清单）。所以「大华默认多少分钟」在本笔记里**属于未知**。
- **厂商对「SD 卡该用什么文件系统」几乎不表态。** 海康手册只说「首次使用请在 Web 端格式化」，不写 FAT32/exFAT/私有格式；只有 Axis 一家明确写了「推荐 **ext4**」[3]。这与 SD 协会的 SDHC=FAT32 / SDXC=exFAT [4] **是三套不同答案**。
- **没有找到海康/大华官方手册里「异常断电可能损坏正在录制的文件」这类直接警告句。** 海康 NVR 手册全文 `断电`=0、`异常断电`=0（`掉电` 命中的全是 PTZ 掉电记忆）[14]。最接近的官方表述是 Axis 白皮书从**正向**说的：「用 ext4，因为断电后能更快恢复、更不易损坏、降低丢数据风险」[3]。
- **没有找到海思官方的「推荐录制/分段方式」文档。** 海思 SDK 文档不是公开分发（`HiMPP IPC V2.0 媒体处理软件开发参考` 只在百度网盘/CSDN 流转），我只能通过社区转贴的 sample 源码反推（见 Deep Read D）。**「官方推荐怎么分段」这个问题，没找到答案。**
- **没有找到 `SAMPLE_COMM_VENC_SaveH264ToMP4` 这个函数名的任何公开出处。** 公开可查的海思 sample 保存函数是 `SAMPLE_COMM_VENC_SaveH264` / `SaveH265` / `SaveStream`。如果任务描述里的这个名字来自某份内部/特定版本 SDK，那它在公开渠道**查无此名**。
- **没有找到 Axis / Bosch 的具体「默认分段时长」数字。** Axis 只给 retention（保存期，128 GB 约 20–215+ 天）和清理周期（`once every 60 minutes`），不给单文件时长 [3]。Bosch 这一轮我**没有有效检索到官方正文**（见 G2），所以 Bosch 在本笔记中**完全没有覆盖**。
- **没有找到「高耐久卡」的 TBW 官方数字。** Samsung 用「小时数 @ 26 Mbps」而不是 TBW [15]；WD Purple / SanDisk High Endurance 的官方 TBW 页**没打开成功**（见 G2）。想引 TBW 的话这一轮**缺数据**。

### G2. 抓取失败 / 未计入 Findings 的来源清单

（以下 URL 我**点开过但没拿到正文**，或者只有导航壳，**其中的任何数字都没有被写进 Findings**。）

- `https://documents.westerndigital.com/content/dam/doc-library/.../product-brief-wd-purple-sc-qd101-ultra-endurance-microsd.pdf`
  现象：连接被意外关闭（en_US 与 pl_PL 两个路径都试过）。→ WD Purple microSD 的耐久/TBW 官方数字**未取到**。
- `https://dahuatac.zendesk.com/hc/en-gb/articles/12622058729618-IPC-video-file-loss-problem-local-SD-card`
  现象：HTTP 403 + Cloudflare "Just a moment..." 挑战页（web_fetch 与带 UA 的 Invoke-WebRequest 都失败）。
  → 标题恰好就是「IPC video file loss problem (local SD card)」，**很可能正是断电/掉卡导致文件丢失的官方说明，但正文没读到**，不计入 Findings。**这是本次最遗憾的一条**。
- `https://www.manualslib.com/manual/3565071/Dahua-Technology-Nvr100-Series.html?page=298`（Dahua NVR100 "Basic Setups; Device Setup"）→ 只返回标题，正文被截断。
- `https://www.manualslib.com/manual/4184973/Dahua-Technology-Dh-Xvr1bxh-I-T-Series.html?page=398`（**Appendix 2 HDD Capacity Calculation**）→ 正文被截断。**这一页标题就是「硬盘容量计算」，正是码率↔容量公式的官方出处，但没读到。**
- `https://manualzz.com/doc/o/nimru/dahua-network-camera-operation-manual-record-control`（大华网络摄像机操作手册 Record Control 节）→ 403 Cloudflare。
- `http://dahuawiki.com/index.php?title=IPCStorageRecordControl`（大华自家 Wiki 的 IPC 存储/录像控制页）→ HTTP 502。
- `https://www.ipcamtalk.com/threads/dahua-ipc-t5842t-ze-record-pack-duration-setting-not-working.63184/` → 403（且本来就是 community 论坛，只能作补充）。
- `https://www.dahuasecurity.com/about-dahua/news-events/notice/262`（NVR Interface-Setting-System）→ **打开了，但只有站点导航，没有正文**。
- `https://www.sandisk.com/.../sandisk-high-endurance-uhs-i-microsd` → 跨域重定向到 shop.sandisk.com，未跟随；SanDisk High Endurance 的官方「小时数/质保」数字**未取到**。
- `https://www.hikvision.com/sa/products/IP-Products/Network-Cameras/Pro-Series-EasyIP-/ds-2cd2626g2-izs/` → 打开了但正文被截断，**没拿到「microSD 支持 Class 10 / 最大 256 GB」的原文**，所以这一条不写进 Findings。
- `https://help.axis.com/download/um_c1710_t10225649_en_2509.pdf` → 404（英文版路径不存在）；法文版 `..._fr_2509.pdf` **下载成功（584 KB）但本机文本抽取脚本崩溃**，没读成 → 不计入 Findings。
- `https://learn.microsoft.com/zh-cn/training/modules/explore-windows-client-file-systems/2-examine-file-allocation-table-file-system` → 本轮未打开（FAT32 的 4 GiB 限制我改用 [6] 更老的官方页面，原文明确写 `Maximum file size | 2^32 - 1 bytes`）。

> 环境备注（供父会话参考）：本次调研中 **ONVIF / Axis / SD 协会 / Microsoft / Samsung / Uniview / pinfo.hikvision.com / open.hikvision.com 都成功取到全文**（多次通过本机代理 `127.0.0.1:7897`）。失败集中在 **Cloudflare 保护站点（manualzz、manualslib 正文、dahuatac.zendesk、ipcamtalk）和部分大厂 CDN（documents.westerndigital.com）**，以及 dahuawiki 的 502。所以「境外站点系统性不可达」这个判断与我这边的实测**不完全吻合** —— 更准确的说法是：**主流标准组织与厂商官网可达，被 Cloudflare 与手册镜像站挡住的是大华/西部数据那几条**。

### G3. 互相矛盾 / 口径不一致的说法

1. **文件系统：三方三个答案。** SD 协会规定 SDHC(≤32 GB)=FAT32、SDXC(>32 GB)=exFAT [4]；Axis 官方推荐监控卡用 **ext4**（journaling，抗断电）[3]；海康/大华/宇视的文档**只让你点「格式化」按钮，根本不暴露文件系统** [10][11][18]。本项目 32 GB 卡用 vfat 是**符合 SD 协会规定**的，但**不符合 Axis 的官方建议**（Axis 建议 ext4 正是为了抗断电）。
2. **「安全的分段大小」与「FAT32 硬上限」的关系没人在文档里说清。** FAT32 单文件上限 ≈4 GiB [6]，而海康的默认打包是 1 GB [16]（只有二手中介来源）。厂商**没有解释**为什么是 1 GB（是历史惯性、还是为了掉电只丢 1 GB、还是为 FAT32 留余量？）—— 文档里找不到理由。
3. **耐久度的口径不统一，不能直接横比。** Axis 用「P/E cycles + 模拟年限表」[3]；Samsung 用「小时数 @ 特定码率」[15]；WD/SanDisk 用（本来应该是）TBW —— 而这三个口径**互相换算需要假设 WAF 和码率**，厂商不给换算关系。我上面 E 节的换算只是**算术对账**，不是厂商承诺。
4. **ONVIF Profile G 与厂商文档之间的口径差**：Profile G 只谈「Recording / Track / RecordingJob / 检索 / RTSP 回放」，**完全不提文件** [1]；而厂商文档（海康 SDK）谈的是「文件按字节切片」[13]。也就是说，**Profile G 的一致性（conformance）与「文件怎么切」没有任何关系** —— 想用「符合 ONVIF Profile G」来论证分段策略，是**论证不成立的**。
