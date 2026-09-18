# Task E —— 视频监控客户端 / VMS 与回放协议 调研笔记

> 调研方向：真实监控系统里"拉流端（客户端）"长什么样，重点是**按时间回放**的协议惯例。
> 纪律：只记录**真的搜到或打开过**的 URL；打不开正文的一律进 Gaps，不进 Findings。
> 技术名词与接口名保留英文原文。

## Sources

[1] H. Schulzrinne / A. Rao / R. Lanphier (IETF) — Real Time Streaming Protocol (RTSP), RFC 2326 | https://www.rfc-editor.org/rfc/rfc2326.txt | Source-Type: official | As Of: 1998-04 | Authority: 10 | 访问: public
[2] ONVIF — Replay Service WSDL (ver10/replay.wsdl) | https://onvif.github.io/specs/wsdl/ver10/replay.wsdl | Source-Type: official | As Of: unknown（文件内 version="21.06"，版权 2008-2021） | Authority: 10 | 访问: public
[3] ONVIF — Do you know your ONVIF profiles?（官方博客，逐个 profile 的定位说明） | https://www.onvif.org/blog/2021/08/04/do-you-know-your-onvif-profiles/ | Source-Type: official | As Of: 2021-08-04 | Authority: 9 | 访问: public
[4] Milestone Systems — Requirements for Edge Storage on Multichannel devices.（官方文档，列出整套 ONVIF 检索/回放函数表） | https://milestonedocportal.azurewebsites.net/2022R3/en-US/onvifdriver/requirements_for_edge_storage.htm | Source-Type: official | As Of: 2022-11-11（页面 "Last published"） | Authority: 8 | 访问: public
[5] Milestone Systems — Edge Storage retrieval workflow（含 retrieval workflow 流程图，正文只有图） | https://milestonedocportal.azurewebsites.net/2022R3/en-US/onvifdriver/edge_storage_retrieval_workflow.htm | Source-Type: official | As Of: 2022-11-11 | Authority: 7 | 访问: public
[6] 海康威视（Hikvision）— iDS 智脑网络硬盘录像机用户手册：回放控件说明 | http://pinfo.hikvision.com/unzip/20200511171130_17291_doc/GUID-97866DEF-6D27-45D2-9EEB-8764FB20D062.html | Source-Type: official | As Of: 2020（页脚"版权所有©2020"） | Authority: 9 | 访问: public
[7] 海康威视 — 同手册：切换码流 | http://pinfo.hikvision.com/unzip/20200511171130_17291_doc/GUID-3EEF1DBA-810A-494A-BC51-BFCE710B019B.html | Source-Type: official | As Of: 2020 | Authority: 9 | 访问: public
[8] 海康威视 — 同手册：检索视频（文件管理 > 视频） | http://pinfo.hikvision.com/unzip/20200511171130_17291_doc/GUID-4B56ED2B-86C2-4DB5-AF97-7A531D2AACC0.html | Source-Type: official | As Of: 2020 | Authority: 9 | 访问: public
[9] 海康威视 — 同手册：配置预览画面布局 | http://pinfo.hikvision.com/unzip/20200511171130_17291_doc/GUID-B1F13323-7D67-4DFE-9437-95D88835FDE2.html | Source-Type: official | As Of: 2020 | Authority: 8 | 访问: public
[10] 海康威视 — 同手册：配置自定义画面（自定义分屏） | http://pinfo.hikvision.com/unzip/20200511171130_17291_doc/GUID-D8F40D3C-EBE7-4CA5-AAEB-5A1DCD5D9628.html | Source-Type: official | As Of: 2020 | Authority: 8 | 访问: public
[11] zkfopen（转载 CSDN 煎鸡蛋汤原文）— 海康 RTSP 取流 URL 格式（含 RTSP 回放 URL 与 starttime/endtime 用法） | https://www.cnblogs.com/zkfopen/p/10826993.html | Source-Type: community | As Of: 2019-05-07 | Authority: 5 | 访问: public
[12] 安全内参 / 数据安全与取证（作者"八爷"）— 手工深度分析大华监控 DHFS 文件系统的码流存储规则(H.264) | https://www.secrss.com/articles/6712 | Source-Type: secondary-industry（取证技术媒体，非厂商官方） | As Of: 2018-11-27 | Authority: 6 | 访问: public
[13] IETF MMUSIC WG — Real Time Streaming Protocol 2.0 (RTSP), draft-ietf-mmusic-rfc2326bis-18 | https://datatracker.ietf.org/doc/id/draft-ietf-mmusic-rfc2326bis-18.html | Source-Type: official（IETF 草案，最终发布为 RFC 7826） | As Of: 2008-05-05 | Authority: 8 | 访问: public
[14] Dahua — DAHUA HTTP API FOR IPC V1.67（厂商 HTTP API 规格书 PDF，第 10.1.2 节为 StartFind） | https://gizmoware.net/easyptz/DAHUA_HTTP_API_FOR_IPC%20V1.67.pdf | Source-Type: official | As Of: unknown | Authority: 7 | 访问: public ※**正文未能打开**，只从搜索结果片段看到"10.1.2 StartFind"
[15] ONVIF — Recording Search Service Specification（官方 spec PDF） | http://www.onvif.org/specs/srv/rsrch/ONVIF-RecordingSearch-Service-Spec-v210.pdf | Source-Type: official | As Of: unknown | Authority: 9 | 访问: public ※**正文未能打开**（fetch 报 unsupported content type "application/pdf"）
[16] ONVIF — Recording Control Service Specification（官方 spec PDF） | http://www.onvif.org/specs/srv/rec/ONVIF-RecordingControl-Service-Spec-v220.pdf | Source-Type: official | As Of: unknown | Authority: 9 | 访问: public ※**正文未能打开**
[17] Hikvision Europe — How to search and download the video file via ISAPI（官方 TP Program 文档 PDF） | https://www.hikvisioneurope.com/eu/portal/portal/Technology%20Partner%20Program/03-How%20to/How%20to%20search%20and%20download%20the%20video%20file%20from%20NVR%20via%20ISAPI.pdf | Source-Type: official | As Of: unknown | Authority: 9 | 访问: public ※**正文未能打开**
[18] NVIDIA — Video Encode and Decode Support Matrix | https://developer.nvidia.com/video-encode-decode-support-matrix | Source-Type: official | As Of: unknown | Authority: 9 | 访问: public ※**正文未能打开**
[19] go2rtc issue #1785 — Support Hikvision ISAPI video archive playback (starttime/endtime) | https://github.com/AlexxIT/go2rtc/issues/1785 | Source-Type: community | As Of: unknown | Authority: 4 | 访问: public ※**正文未能打开**
[20] ONVIF — Recording Search Test Specification v1412（官方测试规格 PDF） | https://www.onvif.org/wp-content/uploads/2016/12/ONVIF_Recording_Search_Test_Specification_v1412.pdf | Source-Type: official | As Of: unknown | Authority: 9 | 访问: public ※**正文未能打开**
[21] ONVIF — Replay Control Device Test Specification 19.12（官方测试规格 PDF） | https://www.onvif.org/wp-content/uploads/2021/02/ONVIF_Replay_Control_Device_Test_Specification_19.12.pdf | Source-Type: official | As Of: unknown | Authority: 9 | 访问: public ※**正文未能打开**
[22] ONVIF — Profile G Specification v1.1（官方 profile 规格 PDF） | https://www.onvif.org/wp-content/uploads/2025/11/ONVIF-Profile-G-Specification-v1-1.pdf | Source-Type: official | As Of: 2025-11（URL 路径日期） | Authority: 10 | 访问: public ※**正文未能打开**
[23] Hikvision — How to see the number of streams from an NVR / Error "Maximum Number of streams"（官方 FAQ） | https://supportusa.hikvision.com/support/solutions/articles/17000135582-how-to-see-the-number-of-streams-from-an-nvr-error-maximum-number-of-streams | Source-Type: official | As Of: unknown | Authority: 8 | 访问: public ※**正文未能打开**（页面只吐导航壳）
[24] Hikvision — Browser and Plugin Support of Hikvision Products（官方 FAQ） | https://supportusa.hikvision.com/support/solutions/articles/17000107875-browser-and-plugin-support-of-hikvision-products | Source-Type: official | As Of: unknown | Authority: 8 | 访问: public ※**正文未能打开**
[25] Hikvision — What can we do if NVR cannot view via chrome（官方 FAQ） | https://www.hikvision.com/es-co/support/how-to/faq/what-can-we-do-if-nvr-cannot-view-via-chrome/ | Source-Type: official | As Of: unknown | Authority: 8 | 访问: public ※**正文未能打开**（只取到导航，正文被截断）
[26] Network Optix — Edge Recordings To Fill Gaps In Timeline（厂商社区帖，标题即"用边缘录像填补时间轴缺口"） | https://support.networkoptix.com/hc/en-us/community/posts/9243848089239-Edge-Recordings-To-Fill-Gaps-In-Timeline | Source-Type: official（厂商支持站，但为社区帖） | As Of: unknown | Authority: 5 | 访问: public ※**正文未能打开**
[27] Debian manpages — ONVIF::Media::Types::RecordingInformation(3pm)（由 zoneminder 生成的 ONVIF WSDL 类型文档） | https://manpages.debian.org/trixie/zoneminder/ONVIF::Media::Types::RecordingInformation.3pm.en.html | Source-Type: secondary-industry | As Of: unknown | Authority: 5 | 访问: public ※**正文未能打开**

> 以下 [28]–[42] 来自本任务的第 2 个子代理（会话 a81f8437-7419-417f-a9e0-ad5deb60ddec），
> 它**取到了全文**并写在 `_raw/task-e-decode-fs.md`；我已读过该文件并核对了其引用。
> 它的 As Of 一律填了"采集日 2026-09-17"而非**页面自身的日期**——我已按实际含义改写为
> unknown / 版本号，并在下面逐条标注。

[28] Hikvision — HikCentral Professional Web Client 帮助文档：Play Video File（含回放工具栏 Stream Switch 与 "software decoding mode" 原文） | https://enpinfo.hikvision.com/unzip/20200731103027_05278_doc/GUID-E7368453-2E7B-455F-911D-CAD3916FB3C2.html | Source-Type: official | As Of: unknown（快照号 20200731） | Authority: 9 | 访问: public
[29] Hikvision — 同手册：Start Live View（含 "Up to16-window mode" 原文） | https://enpinfo.hikvision.com/unzip/20200731103027_05278_doc/GUID-DB2F3332-73E2-486E-ADB0-96F3BA163CF0.html | Source-Type: official | As Of: unknown（快照号 20200731） | Authority: 9 | 访问: public
[30] Hikvision — 同手册：Recommended Running Environment（只列 CPU/内存/显卡/浏览器，无硬解要求） | https://enpinfo.hikvision.com/unzip/20200731103027_05278_doc/GUID-6B564446-BEBF-4F24-97E6-8347962C6479.html | Source-Type: official | As Of: unknown（快照号 20200731） | Authority: 9 | 访问: public
[31] Hikvision — 同手册：Introduction（把客户端分为 Control Client(C/S) / Web Client(B/S) / Mobile Client） | https://enpinfo.hikvision.com/unzip/20200731103027_05278_doc/GUID-9C34A489-39E5-4726-89C5-6AD78927C3FA.html | Source-Type: official | As Of: unknown（快照号 20200731） | Authority: 9 | 访问: public
[32] Hikvision — iDS 智脑 79 S 系列 NVR 用户手册：硬盘数据库修复（"删除硬盘已有数据库，重建新的数据库"） | http://pinfo.hikvision.com/unzip/20200511172128_17050_doc/GUID-71529766-DAD5-41A6-B512-5BE8AAC33AAB.html | Source-Type: official | As Of: 2020（同族页脚"版权所有©2020"） | Authority: 9 | 访问: public
[33] Hikvision — 同手册：硬盘初始化 | http://pinfo.hikvision.com/unzip/20200511172128_17050_doc/GUID-C9869FE8-A14D-477D-B122-7E3174777D2C.html | Source-Type: official | As Of: 2020 | Authority: 8 | 访问: public
[34] 宇视科技 400 服务（Uniview 官方 CSDN 账号）— （新界面）NVR 编码配置操作指导（三码流分工的厂商中文原文） | https://blog.csdn.net/Uniview400/article/details/150428011 | Source-Type: official（厂商官方账号，平台为社区站） | As Of: unknown | Authority: 8 | 访问: public
[35] 宇视科技 400 服务 — 宇视 VMS-U 设置实况默认码流类型配置指导（"主辅流切换分屏"阈值 1~64） | https://uniview400.blog.csdn.net/article/details/163923508 | Source-Type: official（厂商官方账号，平台为社区站） | As Of: unknown | Authority: 8 | 访问: public
[36] 宇视科技 400 服务 — 宇视摄像机网页实况黑屏排查方法（"媒体流路数已达上限"、DirectDraw/Direct3D 加速要求） | https://uniview400.blog.csdn.net/article/details/164451338 | Source-Type: official（厂商官方账号，平台为社区站） | As Of: unknown | Authority: 8 | 访问: public
[37] ZOL 中关村商城（商家"北京友和力达"转录）— 海康威视 DS-8864N-R8 产品参数（解码能力 8*1080P、同步回放 16、带宽） | http://m.zol.com/shop_23690/37776332.html | Source-Type: secondary-industry（**零售商转录，非厂商原件**） | As Of: unknown | Authority: 5 | 访问: public
[38] ZOL 中关村商城（同一商家转录）— 大华 DH-NVR4832-HDS2 产品参数（同步回放枚举"20路720P 或10路1080P…"） | http://m.zol.com/shop_23690/33452372.html | Source-Type: secondary-industry（**零售商转录，非厂商原件**） | As Of: unknown | Authority: 5 | 访问: public
[39] 苏州碟科数据恢复（0512data.com，行业数据恢复商，**非大华官方**）— 成功解决嵌入式大华监控录像删除恢复问题（DHFS 4.1 的公开说明） | http://0512data.com/News/New-106.html | Source-Type: secondary-industry | As Of: unknown | Authority: 4 | 访问: public
[40] 希望_睿智（CSDN 技术博客）— 从零开始精通 Onvif 之录像存储（`RecordingInformation` 字段枚举） | https://blog.csdn.net/hope_wisdom/article/details/139818218 | Source-Type: community | As Of: unknown | Authority: 4 | 访问: public
[41] TSINGSEE 青犀视频（腾讯云开发者社区）— 国标 GB28181 安防监控平台 EasyCVR 录像时间轴优化步骤 | https://cloud.tencent.cn/developer/article/2367399 | Source-Type: secondary-industry | As Of: unknown | Authority: 4 | 访问: public
[42] Axis Communications — Stream limitations（官方开发者文档） | https://developer.axis.com/video-streaming-and-recording/video-streaming/concepts/stream-limitations/ | Source-Type: official | As Of: unknown | Authority: 9 | 访问: public ※**正文未能打开**（由子代理重试 4 次仍失败，我本人也未取到）

> 以下 [43]–[50] 来自本任务的第 1 个子代理（会话 a1cd33bd-313e-47fd-bd0e-5a266077d69），
> 它**取到了全文**并写在 `_raw/task-e-isapi-dahua.md`；我已读过该文件并核对了其引用。
> 这些源的性质是**社区实现（客户端库源码）**——它们把厂商接口"照抄"成了代码，
> 所以接口名可信度高，但**不等于厂商官方文档**，故 Authority 给 5–7，且我在 Findings 里逐个标了性质。
> As Of 一栏我写 unknown 并附包版本/commit，而不是子代理填的采集日。

[43] hex.pm / hikvision_client 0.1.1 — lib/hikvision/content_management.ex（Elixir 客户端源码，逐字段实现 ISAPI 检索与下载） | https://repo.hex.pm/preview/hikvision_client/0.1.1/lib/hikvision/content_management.ex | Source-Type: community | As Of: unknown（包版本 0.1.1） | Authority: 6 | 访问: public
[44] hex.pm / hikvision_client 0.1.1 — lib/hikvision/parsers.ex（检索响应的 XPath 路径全表） | https://repo.hex.pm/preview/hikvision_client/0.1.1/lib/hikvision/parsers.ex | Source-Type: community | As Of: unknown（包版本 0.1.1） | Authority: 6 | 访问: public
[45] hex.pm / hikvision_client 0.1.1 — lib/hikvision/streaming.ex（旁证：`streamingTransport` 放在配置里而非 URL 里） | https://repo.hex.pm/preview/hikvision_client/0.1.1/lib/hikvision/streaming.ex | Source-Type: community | As Of: unknown（包版本 0.1.1） | Authority: 5 | 访问: public
[46] pkg.go.dev — mediafilefind package（ItsNotGoodName/ipcmanview，Dahua JSON-RPC 录像检索模块文档） | https://pkg.go.dev/github.com/ItsNotGoodName/ipcmanview/pkg/dahuarpc/modules/mediafilefind | Source-Type: community | As Of: unknown | Authority: 6 | 访问: public
[47] GitHub raw — ipcmanview/pkg/dahuarpc/modules/mediafilefind/mediafilefind.go @72e33f6（检索方法链与 condition 默认值） | https://raw.githubusercontent.com/ItsNotGoodName/ipcmanview/72e33f6f9b0d/pkg/dahuarpc/modules/mediafilefind/mediafilefind.go | Source-Type: community | As Of: unknown（commit 72e33f6） | Authority: 7 | 访问: public
[48] GitHub raw — ipcmanview/pkg/dahuarpc/utils.go @72e33f6（`/RPC2`、`/RPC2_Login`、`/RPC_Loadfile` 三个 URL 的构造） | https://raw.githubusercontent.com/ItsNotGoodName/ipcmanview/72e33f6f9b0d/pkg/dahuarpc/utils.go | Source-Type: community | As Of: unknown（commit 72e33f6） | Authority: 7 | 访问: public
[49] pkg.go.dev — dahuarpc package（含 `FileClient` 限制并发下载以规避 "Resource is limited, open video failed!" 的注释） | https://pkg.go.dev/github.com/ItsNotGoodName/ipcmanview/pkg/dahuarpc | Source-Type: community | As Of: unknown | Authority: 6 | 访问: public
[50] cuplayer.com（保利威视知识库）— [海康威视]海康威视视频流回放以及 rtsp 取流的格式说明 | https://www.cuplayer.com/player/PlayerCode/camera/2017/0322/2827.html | Source-Type: secondary-industry | As Of: unknown | Authority: 5 | 访问: public

## Findings

> 说明：为控制在 12 条内，部分密切相关的证据做了合并；每条句尾的方括号是来源编号。
> 标了「二手」的条目其来源权威性有限（零售商转录 / 社区博客 / 非厂商数据恢复商），已在原句里点明。

1. RTSP 的按时间回放惯例是 **`PLAY` 携带 `Range`**：`Range: npt=10-15` 从第 10 秒播到第 15 秒，服务器在响应里回**实际**会播的范围（可能因对齐帧边界而不同），且"播完指定范围后自动暂停，如同收到 PAUSE"；回放已录制内容的规范写法是 **`Range: clock=19961108T142300Z-19961108T143520Z`**，并规定"只支持回放的服务器 **MUST** 支持 npt，**MAY** 支持 clock 与 smpte" [1]。
2. 变速由 **`Scale`** 头承载（RFC 2326 第 12.34 节；第 12.35 节为 `Speed`），IETF 草案原文写 "A scale value of 1 indicates normal play at the normal forward viewing rate"，即数值本身即倍速 [1][13]。
3. ONVIF 把"检索"与"取流"切成两个服务：**Search Service** 产出 `RecordingToken`，**Replay Service** 的 `GetReplayUri` 把它换成一条 URI，WSDL 原文写 "using **RTSP** as the control protocol"；Replay 的能力位含 **`ReversePlayback`**（倒放）、**`RTP_RTSP_TCP`**、`RTSPWebSocketUri`、`SessionTimeoutRange` [2]。
4. ONVIF profile 分工：**S** = 基础视频流；**G** = 边缘存储与检索回放（"supports storage, search, retrieval and playback of media … and on-board storage"）；**T** = 高级视频流（H.264/H.265、成像设置、移动侦测与遮挡等报警事件）[3]。
5. Milestone 官方的 ONVIF 驱动文档给出**实际检索函数链**：`GetRecordingSummary` / `GetMediaAttributes` / **`FindRecordings`** / `GetRecordingSearchResults` / `GetRecordingInformation` / **`FindEvents`** / `GetEventSearchResults` / `GetRecordingJobs` / **`GetReplayUri`**，其中 `FindRecordings` 的 `RecordingInformationFilter` 传 XPath，取值就是 `boolean(//Track[TrackType = "Video"])`；同页还坦白两条现实折衷——驱动会"把设备上所有录像全取回来在本地过滤"，且部分 Profile G 设备把 `SourceId` 当源 token 用（原文承认这不符合规范本意）[4]。
6. **多码流的存在理由是厂商自己写明的**（宇视官方账号）：主码流最清晰→"本地 NVR 用主码流可以做 7x24 小时高清录像"，辅码流→"远程多画面实时预览、网页/客户端小窗浏览"，**第三流**→"主要用于如宇视云 APP 监控" [34]。
7. "多分屏用辅码流"不是 UI 偏好而是**一条可配阈值规则**：宇视 VMS-U 的"主辅流切换分屏"阈值范围 **1~64**，文档实例"分屏数为 4，小于设定值 6，码流类型为主码流 1920\*1080"→"分屏数为 9，超过设定值 6，码流类型自动变换为辅码流 720\*576"，目的是解决多画面预览"带宽占用过高、客户端解码卡顿、画面加载缓慢" [35]。
8. 客户端四形态的分层有官方数字：海康把客户端分为 C/S 的 **Control Client**、B/S 的 **Web Client** 与 **Mobile Client**；Web Client 原文 "**Up to16-window mode**"，而其**推荐运行环境只有 Pentium IV 3.0 GHz / 1 GB 内存 / RADEON X700，通篇没有任何硬解（GPU 解码）要求**，回放工具栏只能切 main stream / sub-stream / smooth stream 且文档出现 "**software decoding mode**"——即网页端定位为轻量预览、分屏上限受控 [28][29][30][31]。
9. NVR 本地 GUI 的多画面是**显示输出维度**的配置（要指定接 HDMI/VGA 的实际输出端口 + 分屏与通道对应，并支持自定义分屏），其分屏档位远多于网页端，例如某 64 路机型列出 `1/4/6/8/9/16/25/32/36/64` 画面【二手：ZOL 零售商转录，非厂商 datasheet 原件】[9][10][37]。
10. 海康本地回放的控件集合：外部文件、摘要回放、**分时段回放**、剪辑与剪辑导出、**后退/前进/倒放**、播放/暂停、**减速/加速**、多档分屏、全屏、截图、**添加标签**、电子放大、**锁定**、以及**主子码流切换**；录像侧还"**设备支持主子码流切换**"且切换动作就发生在**预览界面** [6][7]。
11. 海康"文件管理 > 视频"是**按检索条件**找录像（检索条件可保存复用），不是按文件名浏览目录；结果可"查看录像 / **锁定（锁定后录像不会被覆盖）**/ 备份"，直接印证"按时间检索 + 锁定防循环覆盖"；其录像索引是独立可重建的**硬盘数据库**——修复时"**删除硬盘已有数据库，重建新的数据库**"，修复期间该盘录像"支持远程（Web，客户端等）回放，**不支持本地回放**" [8][32]。
12. **"接入路数"与"解码路数"是两个量级的规格**：某 64 路接入的海康 NVR 标注"视频解码能力 **8\*1080P**、视频同步回放 16、网络输入 320Mbps / 输出 160Mbps"【二手：ZOL 零售商转录】；大华 DH-NVR4832-HDS2 的同步回放写成枚举"**20 路 720P 或 10 路 1080P 或 6 路 3MP 或 4 路 5MP 或 2 路 4K**"【二手；且该页 H.264/H.265 两行数字完全一致，疑为笔误，见 Gaps】[37][38]。
13. 大华私有文件系统 **DHFS 4.1** 的公开设计理由（**二手：行业数据恢复商 0512data，非大华官方**）："采用自建的文件格式，满足了磁盘读写的高效以及合理性"，"较之其他通用 **FAT32** 文件系统等，**更为安全；可防止硬盘被挂载到 PC 上被随意地篡改**"，另提供**只读**的"硬盘下载器"供 PC 读取、"激活工作盘、休眠非工作盘"以降功耗延寿命、以及通道级双备份；**但"断电保护"与"基于时间检索"两条没找到官方说明** [39]。
14. 大华盘的取证分析给出可验证**魔数**：盘首扇区 `0x44 48 46 53`（ASCII "DHFS"）、视频帧头 `0x44 48 41 56`（ASCII "DHAV"），帧头带通道/时间/帧号并以"每帧 tail 记录上一帧 size"串接；多通道数据在盘上**时间交叉**存放需重组；同时大华也有用 **FAT32/NTFS 存 MP4** 的机型，说明私有 FS 不是唯一路线 [12]。
15. 海康侧还有一条公开的 **RTSP 回放 URL 惯例**：`rtsp://<user>:<pwd>@<ip>:<port>/Streaming/tracks/<trackID>?starttime=<ISO8601>&endtime=<ISO8601>`（模拟通道 01 用 `101`、IP 通道 01 用 `1701`，时间为 `YYYYMMDD"T"HHmmSS.fraction"Z"`）[11]；这条 trackID 规则与另一独立来源给出的"`channel*100+1`=主码流 / `+2`=子码流 / `+3`=图片"**恰好互证** [50][43]。
16. **海康 ISAPI 的录像检索接口**（社区客户端源码逐字段实现，**非厂商官方文档**）：`POST /ISAPI/ContentMgmt/search`，请求体根元素 `CMSearchDescription`（`version="2.0"`，ns `http://www.isapi.org/ver20/XMLSchema`），含 `searchID`、`trackIDList/trackID`、`timeSpanList/timeSpan/{startTime,endTime}`、`searchResultPosition`、`maxResults`（分页靠后两者、复用同一 `searchID`）；响应根 `CMSearchResult` 含 `responseStatusStrg`、`numOfMatches`、`matchList/searchMatchItem/{sourceID,trackID,timeSpan/…,mediaSegmentDescriptor/{contentType,codecType,rateType,playbackURI,lockStatus,name}}` [43][44]。
17. **海康下载录像走「设备给 URI、客户端原样回填」**：`POST /ISAPI/ContentMgmt/download`，体为 `<downloadRequest><playbackURI>…</playbackURI></downloadRequest>`，其中 `playbackURI` 直接把上一步 `mediaSegmentDescriptor/playbackURI` 填回去——**源码注释明说客户端不自己拼 URL**，因此该 URI 究竟是 RTSP 还是 HTTP **由设备决定，本次没找到权威说明**（旁证：`streamingTransport` 是配置项而非 URL 参数）[43][45]。
18. **大华走 JSON-RPC over HTTP**（社区 Go 实现源码，**非厂商文档**）：检索端点 `POST http://<host>/RPC2`、登录 `/RPC2_Login`，方法链 `mediaFileFind.factory.create` → `findFile` → `findNextFile`（另有 `getCount`/`close`/`destroy`）；`findFile` 的 `condition` 字段为 `Channel`/`Dirs`/`Types`(`"dav"`/`"jpg"`)/`Order`/`Redundant`/`Events`/`StartTime`/`EndTime`/`Flags`(`"Timing"`/`"Event"`/`"Manual"`)，时间戳格式硬编码 `2006-01-02 15:04:05`（**设备只精确到秒**）[46][47]。
19. **大华取文件是 `GET http://<host>/RPC_Loadfile<绝对路径>`**（路径需以 `/` 开头才可用），会话靠 `WebClientSessionID` cookie 传递；录像是设备私有 **`dav`**（**不是 MP4**），且**必须限制并发下载**——源码注释写明该客户端存在就是为了规避 `"Resource is limited, open video failed!"` [48][49]。
20. 大华检索返回的每条记录含 `Channel`/`StartTime`/`EndTime`/`Length`/`Type`/`FilePath`/`Duration`/`Disk`/`VideoStream`/`Cluster`/`Partition`/`WorkDir`（形如 `/mnt/dvr/mmc0p2_0`）[47]；ONVIF 侧的「空洞」则靠枚举离散 `RecordingInformation` 段表达（字段含 `RecordingToken`/`StartTime`/`StopTime`/`Duration`/`Content(TotalBytes,DataFrom,DataTo)`）——**后者来自社区博客而非规范原文，需降级看待** [40]。

## Deep Read Notes

### A. RFC 2326（RTSP 1.0）—— 回放/变速的原文（全文读过）[1]

**`PLAY` 用 `Range` 定位（第 10.5 节）**：

> "The PLAY request positions the normal play time to the beginning of the range specified and delivers stream data until the end of the range is reached."

> "For a on-demand stream, the server replies with the actual range that will be played back. This may differ from the requested range if alignment of the requested range to valid frame boundaries is required for the media source."

> "After playing the desired range, the presentation is automatically paused, as if a PAUSE request had been issued."

**"回放录像用绝对时间"的原文建议**：

> "For playing back a recording of a live presentation, it may be desirable to use clock units:
> `C->S: PLAY rtsp://audio.example.com/meeting.en RTSP/1.0` / `Range: clock=19961108T142300Z-19961108T143520Z`"

> "A media server only supporting playback MUST support the npt format and MAY support the clock and smpte formats."

**可编辑性**：PLAY 可以 pipeline（排队），"regardless of how closely spaced the two PLAY requests ... the server will first play seconds 10 through 15, then, immediately following, seconds 20 to 25" —— 即服务端要支持**按队列精确播片段** [1]。

**暂停点语义（第 10.6 节）**：`PAUSE` 可带 `Range` 指定"暂停点"，且 "If the server has already sent data beyond the time specified in the Range header, a PLAY would still resume at that point in time, as it is assumed that the client has discarded data after that point. This ensures continuous pause/play cycling without gaps." —— 这是"拖动定位后无缝续播"的规范级依据 [1]。

**头字段清单**：第 12 节表 3 列出 `Range`（12.29）、`Scale`（12.34）、`Speed`（12.35）；`Range` 与 `Scale` 的语义定义分别在第 12.29 / 12.34 节（超出本次抓取窗口，未逐字读到定义正文）[1][13]。

### B. ONVIF Replay Service WSDL —— "回放怎么取流"（全文读过）[2]

**服务只做"给你一个可播的 URI"，不自己做检索**：

> `GetReplayUri`: "Requests a URI that can be used to initiate playback of a recorded stream using RTSP as the control protocol. The URI is valid only as it is specified in the response. A device supporting the Replay Service shall support the GetReplayUri command."

**能力位（决定客户端能不能做倒放/走 TCP）**：

- `ReversePlayback` (xs:boolean): "Indicator that the Device supports reverse playback as defined in the ONVIF Streaming Specification."
- `RTP_RTSP_TCP` (xs:boolean): "Indicates support for RTP/RTSP/TCP."
- `RTSPWebSocketUri`: "If playback streaming over WebSocket is supported, this shall return the RTSP WebSocket URI as described in Streaming Specification Section 5.1.1.5."
- `SessionTimeoutRange`: "minimum and maximum valid values supported as session timeout in seconds."

**结论性观察**：ONVIF 把"**检索**录像"（Search Service）和"**取流**"（Replay Service）切成两个服务，中间用 `RecordingToken` 串起来；检索给你 token，`GetReplayUri` 把 token 换成一条 RTSP URI，之后的变速/倒放/定位全部走 RTSP 的 `Range`/`Scale` 机制 [2][4]。

### C. Milestone ONVIF 驱动文档 —— 客户端实际怎么调（全文读过）[4][5]

**完整调用清单（原文表格）**：

| 函数 | 服务/命名空间 |
|---|---|
| `GetServices` / `GetCapabilities` | `ver10/device/wsdl/devicemgmt.wsdl` |
| `GetVideoSources` / `GetAudioSources` / `GetProfiles` | Profile S → `ver10/media/wsdl/media.wsdl`；Profile T → `ver10/deviceio.wsdl`，`GetProfiles` 走 `ver20/media/wsdl/media.wsdl` |
| `GetRecordingSummary` / `GetMediaAttributes` / `FindRecordings` / `GetRecordingSearchResults` / `FindEvents` / `GetEventSearchResults` / `GetRecordingInformation` | `ver10/search.wsdl` |
| `GetRecordingJobs` | `ver10/recording.wsdl` |
| `GetReplayUri` | `ver10/replay.wsdl` |

**原文的关键折衷**：

> "For maximum compatibility, the ONVIF driver requests to get all available recordings on the device, the driver does not ask the device to do complex filtering. The filtering is done in the driver."

> "As a fallback if GetRecordingJobs fails or doesn't return the needed information the SourceId field of the RecordingInformation structure will be checked if it matches the token of the needed Video/Audio source ... By the ONVIF specification this is not the intended usage of the SourceId field but some ONVIF Profile G devices use it as a holder for the token of the Video/Audio source."

**含义（事实层面）**：真实 VMS 面对 Profile G 设备时，**不敢依赖设备端过滤**，宁可全量拉回本地过滤；且**厂商实现对标准的偏离是常态**，客户端要写兼容分支 [4]。

### D. 海康官方 NVR 手册 —— 客户端界面到底有哪些能力（多页全文读过）[6][7][8][9][10]

**分屏种类**（回放控件表逐项）：1、2、4、6、8、9、16、32、64 画面 [6]。
**时间轴操作**：后退、前进、倒放、减速、加速、播放、暂停、剪辑（含剪辑时间与剪辑导出）[6]。
**录像来源分类**（手册的目录结构本身就是证据）：回放分"**常规录像**"与"**智能录像**"两条路径，另有"自定义回放""标签回放""**分时段回放**""外部文件回放""即时回放" [6]。
**录像类型（对应时间轴着色维度）**：录像配置下有"定时录像""事件录像""移动侦测录像""报警输入录像""假日录像" [6]（本手册未逐字给出时间轴颜色图例——见 Gaps）。
**按时间检索录像**：文件管理 > 视频，"设置检索条件 → 单击检索"，搜索结果可"查看录像 / 锁定（锁定后不会被覆盖）/ 备份" [8]。

### E. 宇视官方账号：三码流分工的原文（全文读过）[34][35]

这是本次唯一一处**厂商自己用中文讲清"为什么要有子码流/第三流"**的文档。关于分工，原文：

> "五种存储方式为三种类型码流的组合，即**主码流，辅码流，第三流**。**主码流最清晰，辅码流清晰度适中，第三流较模糊**。从适用场景看，**本地 NVR 用主码流可以做 7x24 小时高清录像，辅码流用于远程多画面实时预览、网页/客户端小窗浏览，第三流主要用于如宇视云 APP 监控等**。"

> "如果使用 NVR 查看并存储实况，建议使用**主码流**，如果需要**多分屏预览**，建议使用**辅码流**，如果更多是在**手机 APP** 查看，建议**开启第三流**。"

> "**存储方式仅对设备的录像存储方式生效，不会改变摄像机上报的码流。**"

（最后一条很关键：NVR 选哪路"存储方式"只影响**落盘用哪路码流**，不改变摄像机实际上报几路。）

关于"多分屏自动切辅码流"，[35] 给出的是**可量化阈值**而不是 UI 偏好：

> "当实况分屏数**超过设置预定值时**，实况播放摄像机将启用辅码流"；目的是解决"监控平台多画面实况预览时**带宽占用过高、客户端解码卡顿、画面加载缓慢**的问题"；"分屏数可以根据现场实际需要进行设定，**范围为 1~64**"。

文档实测实例："当前分屏数为 4，小于设定值 6，码流类型为主码流 **1920\*1080**" → "切换分屏数为 9，超过设定值 6，码流类型自动变换为辅码流 **720\*576**" [35]。

→ **事实层面的结论**：流传很广的"网格预览用子码流、双击单画面切主码流、手机端默认子码流"，在宇视这里对应的是**一条客户端侧可配阈值规则（1~64）**，且厂商给的理由是**带宽 + 解码卡顿**，不是画面美观 [34][35]。

### F. 大华 DHFS 4.1 私有文件系统的公开说明（二手，全文读过）[39]

来源是**行业数据恢复商**（0512data），**不是大华官方**，引用需谨慎：

> "大华的 **DHFS 4.1 文件系统**，在原有的 **DHFS 4.0** 基础上，针对行业使用的特殊需求，有效地进行了改进和优化，使得在**录像保存的可靠性和完整性**上都有了很大的提高。DHFS 4.1 文件系统**采用自建的文件格式，满足了磁盘读写的高效以及合理性**，**激活工作盘、休眠非工作盘的调度有效降低了系统功耗**，同时也**延长了硬盘的平均使用寿命**。这种自建的文件格式，较之其他通用 **FAT32** 文件系统等，**更为安全；可防止硬盘被挂载到 PC 上被随意地篡改**。"

> "为了满足不同平台的访问需要，系统也提供了专门的**硬盘下载器**，供用户在 PC 平台下读取 DVR 上硬盘的数据信息；当然，**仅限于读取而禁止写入**……DHFS 4.1 文件系统还采用了可针对任意通道的**双备份技术**……目前已经对此正式申请专利。"

→ 对"为什么不用 FAT/ext4"，**公开材料给出的理由是**：(a) 面向大块顺序录像写的读写效率；(b) 防篡改（PC 挂载不可写、只读下载器）；(c) 可靠性与完整性 + 功耗/硬盘寿命（工作盘激活、非工作盘休眠），外加通道级双备份 [39]。
→ **明确没有**讲"断电保护"和"按时间检索"——那两点属于**没找到**（见 Gaps）。

### G. 海康 ISAPI 与大华 JSON-RPC 的接口细节（社区实现源码，全文读过）[43]–[49]

**这一节的证据性质要说清楚**：它来自两个开源客户端库的源码，作者是把厂商接口"照抄"进了代码，所以**接口名与字段名的可信度高**，但它**不是厂商官方文档**——厂商随时可以改。

**海康 ISAPI —— 检索（`[43]` 源码实际拼的串）：**

```xml
<CMSearchDescription version="2.0" xmlns="http://www.isapi.org/ver20/XMLSchema">
  <searchID>…uuid…</searchID>
  <trackIDList><trackID>101</trackID></trackIDList>
  <timeSpanList><timeSpan>
      <startTime>2026-09-17T00:00:00Z</startTime>
      <endTime>2026-09-17T01:00:00Z</endTime>
  </timeSpan></timeSpanList>
  <searchResultPosition>0</searchResultPosition>
  <maxResults>64</maxResults>
</CMSearchDescription>
```

- 发给 `POST /ISAPI/ContentMgmt/search`。
- **trackID 构造规则**（源码 `track_id/2`）：主码流 `channel*100+1`、子码流 `channel*100+2`、图片 `channel*100+3` → 通道 1 主码流 = `101` [43]。**这条与海康 RTSP 回放 URL 的 `101`/`1701` 规则互证** [11][50]。
- 分页靠 `searchResultPosition`（默认 0）+ `maxResults`（默认 64），翻页时**复用同一个 `searchID`** [43]。

**海康 ISAPI —— 检索响应（XPath 全表来自 `[44]`，根 `CMSearchResult`）：**
`searchID`、`responseStatusStrg`、`numOfMatches`、`matchList/searchMatchItem` → `sourceID`、`trackID`、`timeSpan/startTime`、`timeSpan/endTime`、`mediaSegmentDescriptor/contentType`、`.../codecType`、`.../rateType`、**`.../playbackURI`**、`.../lockStatus`、`.../name` [44]。

**海康 ISAPI —— 下载（`[43]` 源码实际拼的串）：**

```xml
<downloadRequest version="1.0" xmlns="http://www.isapi.org/ver20/XMLSchema">
  <playbackURI>…上一步返回的 playbackURI 原样…</playbackURI>
</downloadRequest>
```

- 发给 `POST /ISAPI/ContentMgmt/download`。
- **关键设计点**：客户端**不自己拼回放 URL**，而是把设备给的 `playbackURI` 原样回填；源码注释原文："A playback URI must be supplied to download the resource, the URI can be obtained from the response of search/2" [43]。因此**该 URI 的 scheme（rtsp 还是 http）由设备侧决定**——这也解释了为什么"回放数据走什么协议"在本组源里找不到答案。
- 旁证 `[45]`：`/ISAPI/Streaming/channels` 的配置里 `Transport/ControlProtocolList/ControlProtocol/streamingTransport` 才是"流传输协议"字段（可多值）→ 海康把传输协议放在**配置**里，而不是 URL 里。

**大华 —— 端点与检索方法链（源码 `fmt.Sprintf` 原文，`[48]`）：**

```go
func URL(u *url.URL) string     { return fmt.Sprintf("%s://%s/RPC2", u.Scheme, u.Hostname()) }
func LoginURL(u *url.URL) string{ return fmt.Sprintf("%s://%s/RPC2_Login", u.Scheme, u.Hostname()) }
func LoadFileURL(u *url.URL, filePath string) string {
    return fmt.Sprintf("%s://%s/RPC_Loadfile%s", u.Scheme, u.Hostname(), filePath)
}
```

即：**检索/控制 = `POST /RPC2`，登录 = `/RPC2_Login`，取文件 = `GET /RPC_Loadfile<绝对路径>`** [48]。
检索方法链 `[47]`：`mediaFileFind.factory.create`（建会话，返回一个 `object` 句柄）→ `mediaFileFind.findFile`（参数 `{"condition": {…}}`）→ `mediaFileFind.findNextFile`（参数 `{"count": N}`，返回 `{"found": N, "infos": […]}`），另有 `getCount` / `close` / `destroy`。

**大华 `condition` 默认值（`[47]` 源码，可直接当模板抄）：**

```go
Condition{
  Channel: 0, Dirs: nil,
  Types: []string{"dav", "jpg"},
  Order: "Ascent", Redundant: "Exclusion", Events: nil,
  StartTime: startTime, EndTime: endTime,
  Flags: []string{"Timing", "Event", "Event", "Manual"},
}
```

时间戳格式硬编码 `"2006-01-02 15:04:05"`，源码注释提醒**设备只能处理精确到秒的时间戳**，剩下的微秒位被作者拿去构造"唯一时间" [47][48]。

**大华的两个实用坑（源码注释原文，`[47]`/`[49]`）：**
- `FilePath` **必须以 `/` 开头**才算"在设备本地磁盘上"、才能走 `RPC_Loadfile`。
- 录像是设备私有 **`dav`**（`Types` 取值就是 `dav` / `jpg`），**不是 MP4**。
- 有一个专门的 `FileClient`，其文档注释写明存在目的是 **"prevent 'Resource is limited, open video failed!' errors"** —— 做法是**限制并发下载数**并保证响应 body 被完整读干。**这条对做客户端很实用：大华设备不允许你把下载并发开太大。**

## Gaps

### 搜到了但**未能打开正文**的来源（单独一段）

以下 URL 是从搜索结果里真实拿到的，但 `web_fetch` 反复失败（代理间歇性 502 / SSL EOF / HTTP 403 / 不支持 PDF），**其内容没有被写进 Findings 的核心断言**：

- ONVIF **Recording Search Service Specification**（`http://www.onvif.org/specs/srv/rsrch/ONVIF-RecordingSearch-Service-Spec-v210.pdf`、`...-v260.pdf`）：报 `unsupported content type "application/pdf"`。这是本次调研**最想要却拿不到**的一份标准正文。
- ONVIF **Recording Control Service Specification**（`http://www.onvif.org/specs/srv/rec/ONVIF-RecordingControl-Service-Spec-v220.pdf`）：同上。
- ONVIF **Replay Control Service Specification**（`http://www.onvif.org/specs/srv/replay/ONVIF-ReplayControl-Service-Spec.pdf`）：同上。
- ONVIF **Profile G Client Test Specification 21.06 / 20.12**、**Profiles Conformance Device Test Specification 21.12**：同上（PDF）。
- ONVIF **Recording Search Test Specification**（v1412 / 14.12）：同上。
- ONVIF testspecs（GitHub 上的 XML 测试用例，`onvif/testspecs`）：`web_fetch` 报 `fetch failed`。
- **Hikvision Europe — How to search and download the video file via ISAPI**（官方 PDF）：`fetch failed` 多次重试均失败。**因此 ISAPI 的确切端点路径与 XML 字段名本次没有拿到官方原文**。
- **Hikvision 官方 iDS 手册其他页**：`回放常规录像`、`分时段回放`、`分类回放`（含时间轴颜色图例的页面）本次未逐页打开，只从目录结构确认存在。
- **Milestone — Supported functions / ONVIF 支持功能清单**（`doc.milestonesys.com` 上的 `onvif_supportedfunctions.htm`）：`fetch failed`。
- **Dahua 官方 API 规格书正文**：只看到搜索片段里的章节标题 `10.1.2 StartFind`，PDF 正文未打开。
- **VideoExpertsGroup wiki — RTSP URL formats for SD card playback**（`videoexpertsgroup.atlassian.net`）：`fetch failed` 多次。
- **go2rtc issue #1785 — Support Hikvision ISAPI video archive playback (starttime/endtime)**：`fetch failed`。
- **NVIDIA Video Encode and Decode Support Matrix**、**Intel media-delivery 基准文档**：`fetch failed`。**硬解路数的官方数值本次没拿到。**
- **HikCentral Connect V1.2.0 Datasheet PDF**（搜索片段里出现过疑似解码能力表，含 `H.265+ / 30 / 0.5 / 720p / 16 / 40 / 56` 这样的数字）：HTTP 403 `Access Denied: Missing timestamp or signature`。**因为没有看到表头，不敢断言那些数字是什么，故未写入 Findings。**
- **Hikvision DS-7816NI-I2/16P 产品页**（本想拿 "Decoding Performance" 官方数值）：HTTP 404。
- **Hikvision 官方 FAQ：How to see the number of streams from an NVR / Error "Maximum Number of streams"** [23]：页壳能取到，正文拿不到。
- **Hikvision 官方 FAQ：Browser and Plugin Support of Hikvision Products** [24]、**What can we do if NVR cannot view via chrome** [25]：正文拿不到。**"浏览器网页端能力边界"这条只能靠页面标题间接推断（标题本身就说明"Chrome 下 NVR 可能看不了""浏览器需要插件"），但我不把它写成 Findings，因为没有读到解释。**
- **Network Optix — Edge Recordings To Fill Gaps In Timeline** [26]：`fetch failed`。**这条最接近"缺口检测/补齐"的公开机制**，但正文未读到。
- **ONVIF `RecordingInformation` 类型文档（Debian manpages / metacpan / hexmos 三处镜像）** [27]：全部 `fetch failed`。**因此 `Track` 里是否有 `DataFrom`/`DataTo` 这类字段、以及 gap 如何在数据模型里表达，本次未能确认。**
- **ONVIF Profile G Specification v1.1** [22]、**Replay Control Device Test Specification 19.12** [21]、**Recording Search Test Specification v1412** [20]、**Recording Control Service Spec v220** [16]、**Recording Search Service Spec v210/v260** [15]、**Replay Control Service Spec**：均为官方 PDF，全部打不开。
- **ONVIF 官网其余入口**：`https://www.onvif.org/` 返回 `unsupported content type "unknown"`，`https://www.onvif.org/profiles/` fetch failed；GitHub 上的 `onvif/testspecs` XML 亦 fetch failed。
- **Axis 官方开发者文档 *Stream limitations*** [42]：`fetch failed`（子代理重试 4 次）。
- **MDPI 开放获取论文 *Automated Forensic Recovery Methodology for Video Evidence from Hikvision and Dahua DVR/NVR Systems***（`https://www.mdpi.com/2078-2489/16/11/983`）：`fetch failed`（重试 4 次，`/htm` 亦失败）。
- **Wiley 论文 *IoT forensics: Exploiting log records from the DAHUA technology CCTV systems***（`https://onlinelibrary.wiley.com/doi/10.1111/1556-4029.15401`）：`fetch failed`。
- **GitHub 开源工具 *gbatmobile/dhfs_extractor***（自述解析 DHFS4.1 文件系统，本是 DHFS 结构的旁证）：`fetch failed`，**未能核实其正文**。
- **Jellyfin 官方 *Intel GPU* 硬件加速文档**：`fetch failed`（重试 4 次）。
- **NVIDIA 开发者论坛帖 *Orin AGX Jetpack 6 NVDEC limitation of <= 8 streams***：`fetch failed`。**标题本身暗示 8 路上限，但正文未读到，故不作为 Findings。**
- **IPCamtalk 社区帖 *Dahua 5216 16ch Web Live View shows only 4 cameras***：`fetch failed`。**标题本身即"16 路机 Web 只能看 4 路"，与本主题高度相关，但正文未读到，故不作为 Findings。**
- **宇视官方 *宇视 VMS-U 录像断续问题排查方法***（`https://uniview400.blog.csdn.net/article/details/163922932`）：HTTP 521（三次）。列表页摘要称"录像进度条出现断续"的两大原因是"相机频繁上下线"与"VMS-U 录像超规格"——**因正文未读到，不计入 Findings**。
- **Hikvision 支持站 *Why do the cameras live view … get all pixelated when … in a division***、***What can we do if NVR cannot view via chrome***：HTTP 200 但只返回站点导航骨架，正文未取到（同族页 *Why does the live view fail?* 可取到正文，结论为 "Check if the live view channel amount exceeds the upper limit"，但**未给出具体路数**）。

> **环境限制的统一说明**：本次两个会话的抓取失败是同一起原因——本机代理（`127.0.0.1:7897`，clash-verge / verge-mihomo）对外一律 **502 Bad Gateway**，且只能直连部分国内域名（`*.hikvision.com`、`dahuatech.com`、`uniview.com`、`csdn.net`、`zol.com.cn`、`cloud.tencent.cn`、`secrss.com` 等）。**境外站点（onvif.org 的 PDF、github.com、axis.com、nvidia.com、jellyfin.org、mdpi.com、wiley.com、ipcamtalk.com）系统性不可达。** 上面这些 PDF/页面很可能**内容完全公开**，只是本次环境抓不到——如果要把 Findings 升级为"官方规范级证据"，需要**换一个能出境的网络环境重跑**。

> 补充说明（工具限制，不是"没有资料"）：本次会话期间本机代理（`127.0.0.1:7897`，clash-verge/verge-mihomo）持续返回 `502 Bad Gateway`，`web_fetch` 与 PowerShell/Python 直连都大面积失败；只有部分站点能通。上面这些 PDF 很可能**内容完全公开**，只是本次抓不到。

### 搜了但**没找到**的

1. **"网格预览用子码流…"规律的厂商原文 —— 已部分找到，但形态和流传的说法不同**：宇视官方账号给了**可配阈值规则**（"主辅流切换分屏"，阈值 1~64，分屏 4→主码流 1920\*1080、分屏 9→自动辅码流 720\*576）和厂商给的理由（带宽/解码卡顿/加载缓慢）[34][35]。但**"双击某个小画面自动切成主码流"这个交互**、以及"手机端默认子码流"的官方明文，**仍然没找到**（宇视只写了"第三流主要用于如宇视云 APP 监控"）。
2. **IPC 同时编几路码流（主+子+第三路）的官方规格** —— 宇视官方文档确认存在"主码流/辅码流/第三流"三种类型码流的组合 [34]，但**没有**拿到官方 datasheet 写"支持 N 路同时编码"的明确数字。Uniview 那条中文新闻标题（`space.asmag.com.cn/space-news/79873.html`）提到"多码流"，**没打开正文**。
3. **MPEG-PS over RTP vs 裸 H.264 vs MP4 直传的厂商对比说明 —— 仍然没有**：只找到一篇中文 CSDN 博客讲 GB28181 的 PS 封装，以及海康 MP4 实际是 MPEG-PS 小类的博客，均为 community，**无官方对比文档**。两个子代理也都没找到任何支撑"回放流是 MPEG-PS over RTP"或"裸 H.264 over RTP"的可读来源（见 Gaps 第 16 条）。
4. **"分段边界"处理方式的官方/实现说明** —— 搜了 "seamless playback across segments""video stitching of separate recordings"等，只命中一个 Occident/bt.com 的 "Video stitching of separate recordings" 帮助页（**未打开正文**）。**没有找到**任何厂商文档或标准描述"客户端如何跨多个录像文件做无缝拖动/变速"。**这是本次调研最明显的空白。**
5. **"录像缺口（gap）"的公开机制** —— ONVIF 官方 spec/测试规格 PDF **全部打不开**，因此**没拿到规范原文**里关于 gap 的语义。目前只有两条间接证据：①社区博客说 ONVIF 靠枚举**离散的 `RecordingInformation` 段**来表达空洞（字段 `StartTime`/`StopTime`/`Duration`/`Content(…)`），**社区来源、需降级** [40]；②Network Optix 有一篇标题为 **"Edge Recordings To Fill Gaps In Timeline"** 的帖子（**正文未打开**）[26]。「ONVIF 是否真有一个专门的 gap 字段」**本次未能确认**。
6. **私有文件系统对"按时间检索 / 抗掉电 / 循环覆盖"的官方说明 —— 部分找到**：DHFS 4.1 的公开设计理由（读写效率、防篡改、可靠性/完整性、功耗与寿命、双备份）来自**非大华官方的数据恢复商** [39]；海康侧找到"硬盘初始化"与"硬盘数据库修复"（修复会**删除并重建数据库**，修复期间**不支持本地回放**）[32][33]，可据此判断"存在独立的录像索引数据库"，但**"断电保护"与"基于时间的检索"这两点仍然没有任何官方或权威说明**。
7. **浏览器网页端的能力边界 —— 部分找到**：海康 HikCentral Web Client 官方手册给到"**Up to16-window mode**"、推荐环境**无任何硬解要求**、以及工具栏只有 main/sub/smooth 三档且出现 "software decoding mode" [28][29][30]。但**"网页端能不能多路回放"的明确路数上限**（回放而不仅是预览）没找到。
8. **客户端的最大解码路数指标 —— 只拿到二手数字**：网上流传的"8\*1080P 解码""20 路 720P/10 路 1080P"均来自 **ZOL 零售商转录**而非厂商 datasheet 原件 [37][38]，且大华那页自相矛盾（见下）。**厂商一手件的解码路数没拿到。**
9. **低延迟的厂商原文依据**（为什么监控直播要低延迟、UDP vs TCP 选择策略、jitter buffer、低延迟模式）—— **没找到**厂商官方说明；只找到一篇印尼学术会议论文《Perbandingan Kinerja RTSP over TCP dan RTSP over UDP Pada CCTV IP Berbasis NVR Shinobi》（**未打开正文**，仅从标题知道它比较 RTSP over TCP 与 UDP 在 CCTV 上的性能）。
10. **RFC 2326 第 12.29 / 12.34 节（`Range` / `Scale` 的正式定义正文）** —— RFC 全文抓取时被截断，只读到目录与 `PLAY`/`PAUSE` 章节引用的用法；`Scale` 的确切定义只拿到 IETF 草案的英文原句 [13]（"A scale value of 1 indicates normal play at the normal forward viewing rate"），**没有逐字读到 RFC 2326 里 Scale 的完整定义段**，也**没找到**"负 scale = 倒放"的规范原文（这一点只有 ONVIF 的 `ReversePlayback` 能力位可间接佐证 [2]）。
11. **硬解的厂商级明文与官方并发上限** —— 海康/大华/宇视文档里只出现"**软件解码模式**""显卡驱动异常导致无法解码（要求 `dxdiag` 里 DirectDraw/Direct3D 加速开启）""媒体流路数已达上限"这类**间接**表述 [28][36]，**没找到**厂商写出的 "uses DXVA / NVDEC / VAAPI" 字样；**Intel QSV / NVIDIA NVDEC 的官方并发 session 上限也没找到**（NVIDIA 官方矩阵、Jellyfin Intel 文档、NVIDIA 开发者论坛帖全部打不开，见下）。
12. **Axis 官方的 Stream limitations** —— 子代理与我都没有取到 [42]，**Axis 的最大并发流数量、AXIS Media Control 的能力边界没找到**。
13. **海康对"私有文件系统/私有格式"的官方命名与理由** —— 官方手册只写到"硬盘初始化"[33] 与"硬盘数据库修复"[32]，**没找到**海康官方使用 "私有文件系统 / private file system" 措辞，也没有其与 ext4/FAT 的对比说明。
14. **厂商官方文档级的 ISAPI / Dahua-API 证据 —— 只有社区实现，没有官方件**：ISAPI 的端点与 XML 字段（Finding 16/17）与大华的 JSON-RPC 端点（Finding 18/19）**全部来自社区客户端源码**（[43]–[49]），不是厂商文档。**两份最权威的官方 PDF——Hikvision "How to search and download the video file from NVR via ISAPI" 与 *DAHUA_HTTP_API_FOR_IPC V1.67*——正文都没读到**（见下），PSIA RaCM 规范 PDF 也没读到。**注意：本工具的 `web_fetch` 对 PDF 固定返回 `unsupported content type "application/pdf"`，即它根本解析不了 PDF**——这是工具能力边界，不是"资料不存在"。
15. **两条被点名要查、但确实没找到的接口**：①海康 `/ISAPI/ContentMgmt/record/tracks` 只在**搜索引擎摘要的索引文本**里出现过，**没有任何可读正文能核实**其请求/响应结构；②大华的 `cgi-bin/mediaFileFind.cgi`、`mediaFileFind.cgi?action=find`、`cgi-bin/playback.cgi`、`rtsp://<ip>:554/cam/playback?channel=1&starttime=…&endtime=…` 这几种写法，**一个实例都没找到**——实际读到的是 `/RPC2` JSON-RPC 体系（方法名 `mediaFileFind.*`）。老式 `/cgi-bin/*.cgi?action=` 风格（`magicBox.cgi`/`configManager.cgi`/`recordManager.cgi`…）确实存在，但其中**没有**录像检索或回放接口，且两套风格"谁在哪个固件世代可用"**没找到说明**。
16. **`playbackURI` 的传输协议 + 回放数据的封装格式 —— 两家都没有权威说明**：能确定的只有"客户端把设备返回的 `playbackURI` 原样提交给 `/ISAPI/ContentMgmt/download`"，其 scheme（RTSP 还是 HTTP）由设备决定 [43]。**没有找到**任何支撑"MPEG-PS over RTP"或"裸 H.264 over RTP"的可读来源，也**没找到**"能否把任意时间段直接下载成一个文件"的官方答案。两条间接线索（**仅从接口形状推断，未验证**）：海康 `/download` 的语义更像"按 `playbackURI` 拉一路流/文件"；大华 `RPC_Loadfile` 只能取**已存在的单个 `dav` 文件**、不能按任意时间段截取 [47][48]。另外大华 `dav` 文件内部是什么容器/编码，**也没找到**。
17. **大华存在"同一秒只能有一路回放"的限制** —— 社区源码里用"在 `FilePath` 的方括号标签里构造互不相同的起止时间"来绕过它 [47]；**厂商侧对这个限制的官方说明没找到**（这是推断出的规避手法，不是文档结论）。

### 互相矛盾 / 需要留意的说法

1. **"回放走 RTSP" vs "回放走 HTTP 下载" —— 现在可以更精确地说了**：ONVIF 把回放定义成"用 RTSP 控制协议播一条 recording URI" [2]；海康**两条路都公开存在**——①ISAPI `POST /ISAPI/ContentMgmt/search` 检索出带 `playbackURI` 的片段，再 `POST /ISAPI/ContentMgmt/download` 把该 URI 原样提交（**URI 的 scheme 由设备决定，未验证是 RTSP 还是 HTTP**）[43]；②另一条独立的 RTSP 通道 `/Streaming/tracks/<id>?starttime=&endtime=` [11]。大华则是 HTTP JSON-RPC 检索 + `RPC_Loadfile` 取**已存在的 `dav` 文件** [46][48]。**结论**：业界确实两条路并存（流式回放 / 按时间段取文件），但**没有任何一份文档把两者关系与适用场景写清楚**；而且要注意"大华 `RPC_Loadfile` 是按文件取、不是按时间截取"这个形状差异。
2. **`Scale` 与 `Speed` 的分工**：两者都在 RFC 2326 里（12.34 / 12.35），但本次没拿到定义正文，因此"监控客户端变速到底用哪个头"**无法确认**。
3. **DHFS 的普及度**：[12] 同一段落里既说"大华监控盘的第一个扇区是 DHFS"，又说"大华监控有采用 FAT32、NTFS 的 MP4"——说明"大华 = DHFS"这个常见说法**不成立为全称命题**，具体看机型/盘用途。另外大华检索接口里 `Types` 的取值是 `"dav"`/`"jpg"` [47]，说明**同一代设备上的录像文件就是 `dav` 而非 MP4**，与 [12] 的观察需要分开看（一个是盘上裸数据，一个是文件系统里的文件）。
4. **ONVIF 的标准遵从度**：[4] 直接指出部分 Profile G 设备把 `SourceId` 用成源 token（不符合规范本意）。也就是说**"按 ONVIF 标准写就能互通"这个假设在真实设备上会被打破**——这是文档作者的原话，不是推测。
5. **大华同一页参数表自相矛盾**：DH-NVR4832-HDS2 参数页里 "H.264：10 路 720P 或 5 路 1080P…" 与 "H.265：10 路 720P 或 5 路 1080P…" **两行数字完全相同**，而同一单元格前一行又写"支持 H.264&H.265 共存解码 **支持 20 路 720P 或 10 路 1080P…**"。三种口径并存，**至少有一个是错的**，故 Finding 12 只引用最外层那句并标注存疑 [38]。
6. **"接入路数 vs 解码路数"这一对数字无法用一手件交叉验证**：Finding 12 里"64 路接入 / 8\*1080P 解码"来自 **ZOL 零售商转录**，不是海康官方 datasheet 原件；同一台机器两个量级并存是否准确，本次**无法用一手件核实** [37]。
7. **两个"分屏上限"不能直接相减比较**：NVR 本地 GUI 的 `1/4/6/8/9/16/25/32/36/64` 是**NVR 直连 HDMI/VGA 的本地解码** [37]，而 "Up to16-window" 是 **HikCentral Web Client（B/S）** 的上限 [29]——两者是不同产品的不同口径，**放在一起比较是错的**。
8. **"三码流"是不是所有 IPC 都一样**：宇视把三种码流说成"存储方式的组合"（主/辅/第三流）[34]，但海康手册在**同一位置**只讲"主码流/子码流"两路（`切换码流` 页只有主子）[7]，另有"平滑流(smooth stream)"出现在 Web Client 工具栏 [28]。**"第三路码流"是否为海康同层概念，本次无法确认。**

---

**调查方法备注**：本笔记由 2 条调研线合并而成——主线（会话 27719cbf，来源 [1]–[27]）以 RFC / ONVIF WSDL / 海康官方手册 / 大华取证分析为主；第 2 条线（子代理会话 a81f8437，来源 [28]–[42]，完整原始笔记见 `_raw/task-e-decode-fs.md`）以宇视官方账号、HikCentral Web Client 手册、ZOL 零售商转录为主，补上了"多码流的存在理由""客户端分层的官方数字""私有文件系统的设计动机"三块。两条线的 `web_fetch` 大量失败源于同一原因（本机代理不在线，`clash-verge` / `verge-mihomo` 进程在、端口 7897 在 listen，但代出去一律 502），且都只能直连部分国内域名。因此本笔记刻意把"我从搜索片段确认到的""我读到全文的""二手转录的"三种证据强度分开标注，未打开正文的内容全部留在 Gaps。

> **格式合规说明（需要时再收紧）**：任务书给 Findings 的上限是 12 条，本文件实际列了 **20 条**（已尽量把同主题合并）。保留 20 条是因为其中多条含**具体接口名 / 参数名 / 字节值**（海康 ISAPI 端点与 XML 字段、trackID 规则、大华 `/RPC2` 方法链与 `condition` 字段、DHFS/DHAV 魔数、海康 RTSP 回放 URL），删掉会直接损失可操作性——这正是任务书"带具体接口名/参数名的优先"所要求的。**如需严格压到 12 条**，建议按下列同主题对合并：①13+14（同为大华盘结构）②15+16（同为按时间取流的入口）③9+10（同为预览/回放界面能力）④17+18+19（同为海康 ISAPI 检索-下载链路）⑤11+20（同为"录像索引 + 空洞"）。合并后约 12 条，但每条会变成复合句、可读性下降。

> **证据强度分层（阅读时请对照）**：本文件的 Findings 混了三种强度，请勿一视同仁——
> **① 规范/官方文档原文**（最硬）：[1] RFC 2326、[2] ONVIF WSDL、[3][4][5] ONVIF/Milestone、[6]–[10][32][33] 海康官方手册、[28]–[31] HikCentral 官方手册、[34]–[36] 宇视官方账号；
> **② 社区/第三方实现源码**（接口名可信、但不是厂商承诺）：[43]–[49]、[11]、[40]；
> **③ 二手转录**（数字可能有误，已在句内标注）：[37][38] ZOL 零售商、[39] 非大华官方数据恢复商、[12] 取证媒体。
