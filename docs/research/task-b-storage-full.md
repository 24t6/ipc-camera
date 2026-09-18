# Task B — 视频存储生命周期管理：工业界如何处理"存储满了"

> 调研问题：NVR / VMS / IPC 边存（edge storage）在"录像把盘写满"时，**工业界通用的做法是什么**。
> 面向场景：Hi3516DV300 IPC，H.264 720p@30 / ~4 Mbps → MP4 写 32 GB TF 卡（vfat），
> 30 分钟一段（≈900 MB），文件名零填充时间戳，环形覆盖**只在分段收尾时**触发。
>
> 本文件只做"行业怎么做"的事实调研，不含代码实现建议。
>
> ⚠️ 与本次调研最相关的一条行业惯例先放这里：**"盘写满"在主流产品里不是一个边缘情况，
> 而是一个被显式建模的状态**——厂商把它做成"覆盖(overwrite)开/关"这个用户可见的开关，
> 并明确规定两种行为之一（覆盖最旧 / 停止录像），再配一个持续报警。见 [7][8][12][21]。

---

## Sources

说明：`访问:` 指页面的公开程度（public = 无需登录即可读；semi-public = 需登录/订阅或部分受限）。
`Authority:` 为本人对其权威性的主观评分（1-10）。**未成功打开正文的条目一律不计入 Findings**，
只在文末"未能打开的来源"里列出，避免把"搜到标题"当成"读到内容"。

[1] Milestone Systems — General（System mode: Classic vs Evidence collection，XProtect VMS 管理员手册 2018 R2） | https://www.milestonesys.com/globalassets/techcomm/2018-r2/provms/english-united-states/8293.htm | Source-Type: official | As Of: 2018（页脚 © 2018，正文页无独立日期） | Authority: 9 | 访问: public

[2] blakeblackshear/frigate — Recording（Frigate 官方文档，录像保留与目录结构） | https://docs.frigate.video/configuration/record/ | Source-Type: official | As Of: unknown（文档站无发布日期） | Authority: 8 | 访问: public

[3] Fora Soft（Nikolay Sapunov）— How Surveillance Storage Works: The Retention Math | https://www.forasoft.com/learn/video-surveillance/articles-vms/surveillance-storage-retention-math | Source-Type: secondary-industry | As Of: 2026-06-09 | Authority: 6 | 访问: public

[4] Fora Soft（Nikolay Sapunov）— CCTV Recording Modes: Continuous, Motion, Event, and Scheduled | https://www.forasoft.com/learn/video-surveillance/articles-vms/recording-strategies-continuous-motion-event | Source-Type: secondary-industry | As Of: 2026-06-09 | Authority: 6 | 访问: public

[5] IPVM Discussions — Does Disk Fragmentation Exist In NVRs?（Milestone 工程师 Josh Hendricks 回帖，讨论预分配 block file） | https://ipvm.com/discussions/does-disk-fragmentation-exists-in-nvr | Source-Type: community | As Of: 2017-02-13（帖内日期） | Authority: 5 | 访问: semi-public（正文可读，部分讨论需订阅）

[6] 中国电信（发明人 王庆烨 等）— CN104394380A 视频监控管理系统以及视频监控录像的回放方法 | https://patents.google.com/patent/CN104394380A/zh | Source-Type: official（专利文献） | As Of: 2015-03-04（公开日；申请日 2014-12-09） | Authority: 6 | 访问: public

[7] Geutebrück — Edge Recording（G-Core 帮助文档，Axis IPC 边存集成） | https://www.geutebrueck.com/g-help/g-core-ati/Content/IP-Camera%20Plugins/Axis%20IPC/Edge%20Recording/Edge%20Recording.htm | Source-Type: official | As Of: unknown（页内示例日志为 2016–2018） | Authority: 7 | 访问: public

[8] Hikvision — 高级配置（pinfo.hikvision.com 在线手册，含"循环写入"参数说明） | http://pinfo.hikvision.com/unzip/20200715171226_85317_doc/GUID-72919773-BCF5-4422-B3BF-989E35C8A4E7.html | Source-Type: official | As Of: unknown（URL 内含 2020-07-15 构建时间戳） | Authority: 8 | 访问: public

[9] Hikvision — 网络硬盘录像机（78/79/88N-K-R 系列）操作手册 UD21364B（4.30.050 / 2020-09-14） | https://www.hikvision.com/content/dam/hikvision/products/S000000365/S000000641/S000001011/S000000652/OFR003757/M000055998/SM000037048/%E6%93%8D%E4%BD%9C%E6%89%8B%E5%86%8C/UD21364B_%E6%B5%B7%E5%BA%B7%E5%A8%81%E8%A7%86%E7%BD%91%E7%BB%9C%E7%A1%AC%E7%9B%98%E5%BD%95%E5%83%8F%E6%9C%BA%EF%BC%8878-79-88N-K-R%E7%B3%BB%E5%88%97%EF%BC%89_%E6%93%8D%E4%BD%9C%E6%89%8B%E5%86%8C_4.30.050_20200914.pdf | Source-Type: official | As Of: 2020-09-14 | Authority: 8 | 访问: public（⚠️ PDF，本次只读到搜索引擎摘录的原文片段，未能打开全文）

[10] Hikvision — 计划录像（pinfo.hikvision.com 在线手册） | http://pinfo.hikvision.com/unzip/20200512195435_50573_doc/GUID-9AB40A05-949B-41E6-B473-AE87B4FC2AF1.html | Source-Type: official | As Of: unknown（URL 内含 2020-05-12 构建时间戳） | Authority: 8 | 访问: public（⚠️ 仅从搜索结果中读到原文片段）

[11] TP-LINK 视觉安防 — 排障：录像机蜂鸣器一直响 | https://security.tp-link.com.cn/m/service2/detail_article_4097.html | Source-Type: official | As Of: unknown（页脚 © 2026） | Authority: 7 | 访问: public

[12] Infotech（Hikvision 经销商技术文档）— Квоты и дисковые группы на Hikvision NVR | https://infotech.ua/ru/supports/Kvoty-i-diskovye-gruppy-na-Hikvision | Source-Type: secondary-industry | As Of: 2025-09-23 | Authority: 5 | 访问: public

[13] Security Today（Barry Norton）— A Smarter Approach: Video Data Storage Calls For Modern Security Systems | https://securitytoday.com/articles/2026/05/print/a-smarter-approach-video-data-storage-calls-for-modern-security-systems.aspx | Source-Type: journalism（行业媒体） | As Of: 2026-05-15 | Authority: 6 | 访问: public

[14] Team Group Inc. — WHY DOES MY CAMERA STOP VIDEO RECORDING WHENEVER CERTAIN MINUTES ARE REACHED...（FAT32 单文件 4 GB 限制） | https://www.teamgroupinc.com/community/en/questions-and-answers-detail/stop-recording-capacity-full/ | Source-Type: official（存储卡厂商 FAQ） | As Of: unknown | Authority: 6 | 访问: public

[15] SecureView Technologies — Security Camera Storage Full? How to Fix a Full NVR or DVR | https://www.secureviewcameras.com/security-camera-storage-full/ | Source-Type: secondary-industry | As Of: 2026-07-17 | Authority: 4 | 访问: public

[16] ONVIF — ONVIF Profile G Specification v1.1 | https://www.onvif.org/wp-content/uploads/2025/11/ONVIF-Profile-G-Specification-v1-1.pdf | Source-Type: official（标准） | As Of: 2025-10（v1.1 发布月，据 [4] 引用页所载） | Authority: 9 | 访问: public（⚠️ PDF，未能打开全文；操作名与版本信息引自 [4] 的引用条目）

[17] European Data Protection Board — Guidelines 3/2019 on processing of personal data through video devices | https://www.edpb.europa.eu/our-work-tools/our-documents/guidelines/guidelines-32019-processing-personal-data-through-video_en | Source-Type: official（监管指引） | As Of: 2019（指引年份，页面无单独日期） | Authority: 9 | 访问: public（⚠️ 仅读到 [3] 引用的 §120–122 摘录，未打开 PDF 原文）

[18] TrueNAS Community — 80% Rule with all Flash for Surveillance Video | https://www.truenas.com/community/threads/80-rule-with-all-flash-for-surveillance-video.105192/ | Source-Type: community | As Of: 2022-11-10（首帖日期） | Authority: 4 | 访问: public

---

## Findings

1. Milestone XProtect 把"存储满了怎么办"做成了用户可见的**系统模式二选一**：Classic 模式自动删除最旧录像腾空间，Evidence collection 模式则**停止录像、保留所有旧录像**直到人工处理 [1]。
2. 两种模式的其他行为也成对定义：删除设备时 Classic 会连带删除该设备录像，Evidence 模式则保留（因为"录像即证据"）；且 Evidence 模式下**无法设置保留期**，因为系统永不自动删除 [1]。
3. 海康 NVR 用"**循环写入**"复选框表达同一件事：勾选 = 存储满后覆盖最早的录像文件；不勾选 = 存储空间满后**停止录像** [8][9][10]。
4. 海康还提供**存储配额（Quota）与磁盘组（Group）**：Quota 按通道分配容量上限（配额=0 表示不限制、共用整池），Group 把硬盘分组并绑定通道以实现物理隔离 [12]。
5. TP-LINK 官方把"**硬盘空间不足**"（即"硬盘写满且关闭了循环写入"）列为**NVR 异常报警**，与"无硬盘/硬盘出错/IP 冲突/视频信号丢失"并列，蜂鸣器会**持续响到问题解决为止** [11]。
6. 非官方行业指南同样把"写满"归纳为两种可能结果——自动覆盖最旧，或直接停录并报警——并强调停录后"业务可能已经不再录像" [15]。
7. Frigate 的保留策略**按录像类别分层**（continuous / motion / alerts / detections 各配保留天数），官方示例组合为 continuous 3 天 → motion 7 天 → alert/detection 30 天，且保留天数支持小数（如 0.5 天）[2]。
8. 需要长期保留的单段视频在 Frigate 里的正规做法是**导出（export）**而不是调大整机保留期，因为导出的片段"单独保存、永不被保留策略删除"——这是"事件录像不被覆盖"的一种实现 [2]。
9. 存储容量规划的行业惯例是**最多用到约 80%**、并按原始视频量乘 **1.4–1.6 倍**采购容量（RAID 校验 + 文件系统开销 + 留白），因为"把录像盘跑到 100% 满会招致损坏和丢帧" [3][13]。
10. 分段录像的**索引**在工业实现里既有"同名索引文件 + 帧级索引表"（索引表字段含帧类型/帧率/GOP/文件内偏移/PTS/帧大小，并靠文件名中的起止时间与"是否完整写入"标识区分在写/已写完）[6]，也有 Frigate 那样按 `YYYY-MM-DD/HH/<camera>/MM.SS.mp4` 的目录结构 + 数据库记录（数据库条目删除而文件残留会形成"孤儿文件"，需定期 media sync 清理）[2]。

---

## Deep Read Notes

### 1) Milestone XProtect — System mode（[1]，读全文）

来源：<https://www.milestonesys.com/globalassets/techcomm/2018-r2/provms/english-united-states/8293.htm>

这是本次调研里**最直接回答"盘满了怎么办"的官方文档**，而且它把这个问题上升成了一个产品级二选一。

原文关键句（保留存储满时行为）：

- **Classic 模式**："the system automatically deletes the oldest saved recordings in order to make room for new recordings"，
  并说明这是"以往所有版本一直以来的做法"。
- **Evidence collection 模式**："the system stops recording when you reach full storage capacity. All your old recordings are kept in the storage and the system does not save any new recordings"，
  目的写明是"ensures that video recorded as evidence is never deleted automatically"。

官方给出的对照表（照录要点）：

| 触发条件 | Classic 模式 | Evidence collection 模式 |
|---|---|---|
| 录像所在存储写满 | 删除最旧录像，为新录像腾空间 | 停止保存新录像，保留最旧录像 |
| 在管理端删除某个设备 | 删除该设备的所有录像 | 保留该设备的所有录像 |
| 保留时间（retention time） | 可设置、可自定义 | **不可设置**，因为系统从不删录像 |

**为什么这一页值得单独读**：
它明确写出了两种策略各自的**适用场景**——官方原话是"Most users need the most recent recordings to be available in their storage and should select Classic mode"，
而 Evidence 模式是给"所有录像都被视为证据、因此必须留在存储上"的场景。
另外还有两个可操作细节：试用模式（trial mode）下**只有 Classic 可用**；从旧版本升级上来的系统**默认是 Classic**，要用手动改成 Evidence。

> 对本项目的含义（仅作对应，不含实现建议）：本项目的"环形覆盖"= Classic；"写满即停"在一些证据保全/司法场景反而是**被厂商正式支持的一等选项**，不是退化行为。

---

### 2) Hikvision — 循环写入 / 存储配额（[8][9][10][12]，读全文/摘录）

来源（在线手册，正文可读）：<http://pinfo.hikvision.com/unzip/20200715171226_85317_doc/GUID-72919773-BCF5-4422-B3BF-989E35C8A4E7.html>
来源（PDF 手册，仅读到摘录片段）：UD21364B 网络硬盘录像机（78/79/88N-K-R 系列）操作手册 4.30.050 / 2020-09-14

官方手册里"循环写入"参数的原文说明：

> **循环写入**｜启用后，当硬盘空间用完时，新录像文件按时间顺序覆盖旧录像文件。

同一手册另一处对两种取值的表述（搜索摘录原文）：

> 若勾选循环写入，当存储空间满之后，将覆盖最早的录像文件；若不勾选，则存储空间满后将停止录像。

也就是说，海康用一个复选框把 Milestone 的"两个系统模式"表达成了**策略开关**，且"覆盖"的排序依据明确写了是**按时间顺序**（与文件名/写入时间的时序一致）。

配套的两层容量控制（经销商技术文档 [12] 整理自海康 GUI 4.7x–5.x 官方手册）：

- **Quota（配额）**：`System → Storage Management → Storage Mode`，按通道/对象分配容量上限；
  **配额 = 0 表示不限制**（所有通道共用整池）；GUI 4.7x–4.83 下改动配额**必须重启 NVR** 才生效。
- **Group（磁盘组）**：把 HDD 分组并绑定到指定通道，用来**物理隔离**不同归档（如按部门/区域）。
- **Overwrite（覆盖）**：在 `Advanced Settings` 里独立开关，"ON = 循环写入，OFF = 满则停"。
- 另有一条工程约束：GUI 5.x 里开启 RAID（Array Mode）**要求先把 Storage Mode 设为 Quota**。

**具体数字/数值**：本组来源给的是**配置项与行为**，没有给出"低水位线百分比"这类数值
（海康官方手册也未在该章节给出触发清理的百分比阈值——见 Gaps）。

---

### 3) Geutebrück G-Core × Axis IPC —— 边存（SD 卡）环形覆盖的真实机制（[7]，读全文）

来源：<https://www.geutebrueck.com/g-help/g-core-ati/Content/IP-Camera%20Plugins/Axis%20IPC/Edge%20Recording/Edge%20Recording.htm>

这是一份少见的、把 **IPC 本地 SD 卡写满后怎么转圈**写清楚的厂商文档，且直接涉及"用 vfat 还是 ext4"这个与本项目完全同构的取舍。

关键结论与**具体数值**：

1. **环形覆盖由相机侧一个"保留上限"参数控制**，不是由中心服务器控制：
   原文强调"To continuously delete older entries and replace them with new entries, it is important to select a sufficiently large value for the setting **Keep recordings up to** in the section **Onboard storage** under **System > Storage**"。
   —— 即：卡片端存在"最多保留多少（时间/容量）"的上限，够大才会持续滚动覆盖；这是**边存环形覆盖的标准做法**。
2. **文件系统选择的官方建议（直接对应于本项目的 vfat 疑问）**：
   "For longer service life and better storage utilization, formatting the memory card using the **ext4** file system is recommended. However, it will then not be possible to read this card using a Windows computer."
   "If the card will be removed from the camera and read using a Windows computer, the card needs to be formatted using the **vfat** file system."
   —— 即：**vfat 是为了 Windows 可读性而做的兼容取舍，代价是寿命与空间利用率**。这条对本项目"32 GB vfat"的选择是最贴切的行业注释。
3. **写入过程中的"未完成文件"是显式建模的**：日志示例显示相机侧存在一个持续增长的 **`recording.tmp`**，
   "This file contains the most recent recordings that are not yet accessible externally. Only once the file has been copied to an \*.avi file is it accessible for G-Core."
   —— 正在写的那一段以临时名存在、写完才改名/收尾，这与"绝不能删正在写的那段"是同一个担忧的两面。
4. **录像任务的健康检查周期是一个具体数字**："The G-Core server automatically starts permanent recording on the camera. This recording is **checked every 5 minutes** and restarted if necessary."
   通过 ONVIF `SetRecordingJobMode ... to Mode: Active`，日志中 `Result: 0` 即表示"录像已被确保"。
5. **缺口（gap）补齐的判定阈值也是一个具体数字**："If a gap of **10 seconds** is identified from one image to the next, this is considered a gap and is registered as a gap"。
6. 带宽/负载控制：可限制同时从 SD 卡取流的路数（示例值 `EdgeRecBalancingParallelRuns = 5`），
   传输有"realtime（按原始速率，1 小时录像传 1 小时）"和"as fast as possible（例：1 小时 @5 fps 的录像，以 30 fps 传出，10 分钟传完）"两档。

---

### 补充读：容量规划的两个"行业惯例数值"（[3][13]）

来源：<https://www.forasoft.com/learn/video-surveillance/articles-vms/surveillance-storage-retention-math>
来源：<https://securitytoday.com/articles/2026/05/print/a-smarter-approach-video-data-storage-calls-for-modern-security-systems.aspx>

（这两篇是 secondary-industry / 行业媒体，权威性低于厂商手册，但**只有它们给出了具体百分比**，故列出以便交叉验证。）

- **10.8 GB/天/Mbps**：`1 Mbps × 86400 s ÷ 8 = 10,800 MB/天 = 10.8 GB/天`，用于把码率换算成容量 [3]。
- **80% 使用率上限**：两篇独立来源一致——"the standard practice is to size so the array runs at no more than about **80% capacity**" [3]；
  "A practical target is to plan for **no more than 80% use**... those are safety valves, not design targets" [13]。
  理由写明：机械盘写满会因数据摆放物理特性而变慢，SSD 在高填充率下加速磨损，且"Running a recording filesystem to the brim invites corruption and dropped frames" [3]。
- **采购容量 ≈ 原始量的 1.4–1.6 倍**（RAID 校验 + 文件系统开销 + 留白），示例：26 TB 原始 → 约 39 TB 实际采购 [3]。
- **分配单元（allocation unit）建议 64 KB**：Security Today 引 Milestone 的建议——简单录像盘格式化为 **64 KB** 分配单元以提升写入效率、延长寿命；用 RAID 控制器时 stripe size 要匹配，否则收益被抵消 [13]。
- **分层归档的一个常见模型**："keeps full-quality video for **seven days**, then archives it at **five frames per second**" [13]。
- 保留天数的**常见区间**（非法律）：一般企业/零售/医疗 **30–90 天**；银行金融 **90+ 天**（部分记录多年）[3]；
  GDPR 侧则给出**上限**逻辑：EDPB 指引认为录像"should in most cases be erased, ideally automatically, after a few days"，且**超过 72 小时**就需要提供更充分的必要性论证 [3][17]。

---

## Gaps

### A. 搜了但没找到（明确写"没找到"）

1. **主流厂商的"低水位线/预留空间"具体数值（百分比或 MB）——没找到可引用的官方数字。**
   找到的最接近证据是：① 多家二手来源一致给出"**不要超过 80%**"这条容量规划惯例 [3][13][18]；
   ② 海康的 Quota 是"用户自己填容量"，手册章节没有给"到 xx% 触发清理"的默认阈值 [12]；
   ③ TP-LINK 只描述"硬盘空间不足"这个**报警状态**，未给出触发它的百分比 [11]。
   —— 也就是说，"**预留 xx MB / xx%**"在消费级与工程级 NVR 里更像是**容量规划建议**，而**不是**一个普遍公开的运行时水位参数。**本项目想找的"行业惯例水位数值"没有找到权威出处。**
2. **"按事件保留、事件录像不被覆盖"在 ONVIF/厂商层面的标准机制——没找到。**
   找到的替代做法是：Frigate 用"**导出（export）** = 单独保存、永不被保留策略删除" [2]；
   Milestone 用 **Evidence collection 模式**（整体停录以保全全部录像）[1]。
   两者都不是"给某一段打锁标记、其余照常覆盖"的那种精细机制。**未找到**可引用的"lock / protect 单段录像"官方文档。
3. **QNAP QVR Elite 的录像目录/文件结构细节——没读到。** 官方 FAQ 页面打开了，但正文在该页的
   脚本渲染部分，抓取到的只是导航与页头（"最後修訂日期:"后内容未取到）。见"未能打开"清单。
4. **Frigate 的数据库表结构（segments 的起止时间字段）——工具没打开。** 尝试的 DeepWiki 页面返回空壳
   （客户端渲染）。只从官方文档确认了"目录结构 + 数据库条目 + 孤儿文件同步"这套机制存在 [2]。
5. **Milestone 的"低磁盘空间警告"百分比阈值——没读到。** 只从搜索摘录看到 Milestone 手册存在
   "hard drive warnings"/"low disk space"相关内容与"建议录像数据库用独立硬盘"的建议，未能打开正文确认数值。
6. **"双存储/备份存储（SD 卡满 → 上传 NAS/云）"的具体阈值与架构——只找到机制，没找到阈值。**
   找到的机制是 **edge recording / 缺口回补**：G-Core 持续在相机 SD 卡上录像，检测到与中心录像的
   gap（>10 s）后，事后通过 ONVIF Profile G 的 playback 通道把 SD 卡上的那段取回并补进中心存储 [7]。
   这是"卡先兜底、中心事后补齐"，**不是**"卡满时把最旧的推到 NAS"。
   **未找到**任何来源说明"卡剩余空间低于 xx 时开始上传/卸载"。

### B. 互相矛盾或口径不一致的说法

1. **"80%"到底指什么口径，各来源不一致。**
   [13] 说的是**设计目标**（"plan for no more than 80% use"，出于性能与磨损）；
   [3] 在同一句里既说"standard practice 是跑到不超过约 80% 容量"，又说"从不超过 100% 满"；
   [18]（TrueNAS 社区）讨论的是**存储池/ZFS 性能**意义上的 80%，发帖人自己也在质疑这个规则是否适用；
   另有社区回复指出"80% 规则其实是针对 HDD 的性能损失缓解策略，对 SSD 只有部分成立"。
   —— 结论：**80% 是一个容量规划的经验值，不是一个跨厂商统一的运行时清理水位**，引用时必须说明口径。
2. **"满了以后是删最旧还是停录"没有统一默认值，默认值因厂商/代际而异。**
   Milestone：**升级上来的系统默认 Classic（删最旧）**，但试用模式只有 Classic；Evidence 需手动改 [1]。
   海康：取决于"循环写入"复选框，且[12]明确把它列为"独立策略，ON = 循环、OFF = 满则停"。
   二手来源 [15] 则称"**大多数商用系统默认自动覆盖最旧**"，但同时承认"若该功能被关闭则会停录"。
   —— 这三者并不冲突，但"**默认值是什么**"在不同来源里给出的印象不同：厂商文档强调"由配置决定"，二手文章强调"多数默认覆盖"。
3. **"边删边录"这类能力在不同中国厂商产品间的成熟度说法不一致。**
   EasyNVR 官方专栏文章称：磁盘满就无法继续录像是**老版本 EasyNVR 的已知缺陷**，"边删边录"功能"其实已经开发，
   不过没有加入到老版本 EasyNVR 中，新增在了 **EasyCVR** 中，但是 EasyNVR 新版本加入了**阈值**的配置"，
   并指引在 `easynvr.ini` 中配置几个参数即可实现。
   ⚠️ **该文章的关键参数是截图（图片），正文没有给出参数字面名与数值**，因此本条**不能**用来支撑任何具体阈值数字，
   只能支撑一个事实："**磁盘满即无法续录**"是真实产品里出现过的、需要靠新增配置项来修的缺陷。
   来源：[EasyNVR 腾讯云开发者社区专栏](https://cloud.tencent.cn/developer/article/2298176)（Source-Type: secondary-industry/vendor，As Of: 2023-06-27，Authority: 4，访问: public）。

### C. 本次调研未能打开正文的来源（**不计入 Findings / Deep Read**，列此以免被误当作已核实）

以下 URL 确实来自搜索结果，但抓取时被拒绝或内容为空，**本人的结论未依赖它们**：

- Axis 官方用户手册（P5655 / P1377 等，含 "Retention time" 说明）— PDF，工具不支持该内容类型。
- Dahua 官方支持文章《IPC video file loss problem (local SD card)》— HTTP 403（Cloudflare）。**这条最可惜**：它极可能正面回答"SD 卡录像丢失"的成因。
- Hanwha Vision WAVE《How to Configure Reserved Space》《Adjusting the Reserved Disk Space in Wisenet WAVE》— HTTP 403。**这两条同样可惜**：标题表明 WAVE 里存在明确的"预留空间（reserved space）"配置项，是本项目最想找的那类机制，但正文未取到，故本文件**不声称**WAVE 的预留空间如何配置。
- Hikvision Dahua 等厂商手册的 manualslib / manualowl / manualzz 镜像页 — 内容被截断或 HTTP 403。
- QNAP QVR Elite 文件夹结构 FAQ（en / zh-hk / ja / vi 多语言版本）— 正文未渲染出来。
- SnapStream《Expiration》帮助页 — 抓取失败（fetch failed）。
- DIGIEVER FAQ #93 — 抓取失败（fetch failed）。
- ONVIF Profile G Specification v1.1 与 EDPB Guidelines 3/2019 — PDF，工具不支持。
  （其中 Profile G 的操作名 `CreateRecordingJob` / `SetRecordingJobMode` / `FindEvents` 与 v1.1 的 2025-10 版本信息，
  来自 [4] 的引用条目与 [7] 的实际日志，属**二手转引**，本文件不当作已读原文。）

---

*调研日期：2026-09-16 前后。凡标注 As Of: unknown 的条目表示页面上未能定位到可核对的日期。*
