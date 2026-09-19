# ═══════════════════════════════════════════════════════════════════════════
#  ipc_camera —— 构建入口
# ═══════════════════════════════════════════════════════════════════════════
#   板子(交叉编译)   make           → build/ipc_app
#   PC 原生单测       make test      → 编 + 跑(**不需要 SDK / 交叉工具链**)
#   清理              make clean
#   说明              make help
#
#  ⚠️ 仓库里**不含**下面三样(体积与许可原因), 用变量指过去即可:
#       SDK_DIR        海思 Hi3516CV500 SDK(头文件 + 静态库 + sample/common)
#       MP4V2_DIR      自己交叉编译的 mp4v2(我们叫它 mp4v2-arm)
#       CROSS_COMPILE  交叉工具链前缀(含结尾的 `-`)
#     例:
#       make SDK_DIR=/opt/Hi3516CV500_SDK_V2.0.2.0 MP4V2_DIR=$HOME/mp4v2-arm \
#            CROSS_COMPILE=/opt/arm-himix200-linux/bin/arm-himix200-linux-
#
#  ⚠️ 为什么 PC 单测是**一等公民**:`protocol` / `infra` / 策略层都不碰硬件,
#     用宿主 gcc 就能编译并断言 —— 拿到仓库的人**不用买板子**也能验证一半代码。
# ═══════════════════════════════════════════════════════════════════════════

# ── 可覆盖的路径与工具 ────────────────────────────────────────────────────
CROSS_COMPILE ?= /opt/hisi-linux/toolchain/arm-himix200-linux/arm-himix200-linux/bin/arm-himix200-linux-
SDK_DIR       ?= $(HOME)/hi3516_sdk/Hi3516CV500_SDK_V2.0.2.0
MP4V2_DIR     ?= $(HOME)/mp4v2-arm
SENSOR        ?= GALAXYCORE_GC2053_MIPI_2M_30FPS_10BIT
HOSTCC        ?= gcc
BUILD         ?= build

CC        := $(CROSS_COMPILE)gcc
SIZE      := $(CROSS_COMPILE)size
SAMPLE_DIR:= $(SDK_DIR)/smp/a7_linux/mpp/sample
REL_LIB   := $(SDK_DIR)/smp/a7_linux/mpp/lib
REL_INC   := $(SDK_DIR)/smp/a7_linux/mpp/include
COMMON_DIR:= $(SAMPLE_DIR)/common
ADP_DIR   := $(SAMPLE_DIR)/audio/adp

OUT := $(BUILD)/ipc_app

# ── 源码(五层全上;新增 .c 文件会自动被 wildcard 收进来) ─────────────────
SRCS := $(sort $(wildcard src/*/*.c))
OBJS := $(patsubst src/%.c,$(BUILD)/%.o,$(SRCS))

# 厂商 sample 的公共层(**不在我们仓库里**, 从 SDK 里取)
COMMON_SRCS := loadbmp.c sample_comm_audio.c sample_comm_isp.c \
               sample_comm_region.c sample_comm_sys.c sample_comm_vdec.c \
               sample_comm_venc.c sample_comm_vi.c sample_comm_vo.c \
               sample_comm_vpss.c
VND_OBJS := $(addprefix $(BUILD)/vendor/,$(COMMON_SRCS:.c=.o))

# ── 编译参数 ─────────────────────────────────────────────────────────────
INCS := -Isrc/service -Isrc/protocol -Isrc/infra -Isrc/bsp \
        -I$(REL_INC) -I$(COMMON_DIR) -I$(ADP_DIR)
# ⚠️ 故意**不加** -I$(MP4V2_DIR)/include:它的头文件是 C++ 的, 从 C 里编不过。
#    我们用自己的垫片 src/service/svc_record_mp4.h(见该文件头部说明, 也见 B031)。
DEFS := -Dhi3516cv500 -DHI_XXXX -DHI_RELEASE \
        -DVER_X=1 -DVER_Y=0 -DVER_Z=0 -DVER_P=0 -DVER_B=10 \
        -DUSER_BIT_32 -DKERNEL_BIT_32 -DHI_ACODEC_TYPE_INNER \
        -DSENSOR0_TYPE=$(SENSOR) -DSENSOR1_TYPE=$(SENSOR)
ARCH := -mcpu=cortex-a7 -mfloat-abi=softfp -mfpu=neon-vfpv4
WARN := -Wall -Wextra
OPT  := -O2 -g -fno-aggressive-loop-optimizations \
        -ffunction-sections -fdata-sections -fstack-protector-strong
CFLAGS := $(WARN) $(OPT) $(ARCH) $(DEFS) $(INCS)

# ★ **所有库(含系统库)必须在同一个 --start-group 里** —— 静态库之间有循环依赖,
#   分组顺序错了会报"库明明给了却未定义符号"(见项目 bug log B021)。
VENDOR_LIBS := $(REL_LIB)/libmpi.a $(REL_LIB)/libhdmi.a \
    $(REL_LIB)/lib_hiae.a $(REL_LIB)/libisp.a $(REL_LIB)/lib_hidehaze.a \
    $(REL_LIB)/lib_hidrc.a $(REL_LIB)/lib_hildci.a $(REL_LIB)/lib_hicalcflicker.a \
    $(REL_LIB)/lib_hiawb.a \
    $(REL_LIB)/libsns_imx327.a $(REL_LIB)/libsns_imx327_2l.a \
    $(REL_LIB)/libsns_imx307.a $(REL_LIB)/libsns_imx307_2l.a \
    $(REL_LIB)/libsns_imx458.a $(REL_LIB)/libsns_mn34220.a \
    $(REL_LIB)/libsns_os05a.a $(REL_LIB)/libsns_os08a10.a \
    $(REL_LIB)/libsns_gc2053.a $(REL_LIB)/libsns_sc4210.a \
    $(REL_LIB)/libsns_ov12870.a $(REL_LIB)/libsns_os04b10.a \
    $(REL_LIB)/libsns_imx415.a $(REL_LIB)/libsns_imx219.a \
    $(REL_LIB)/libsns_imx274.a $(REL_LIB)/libsns_imx335.a \
    $(REL_LIB)/libupvqe.a $(REL_LIB)/libdnvqe.a \
    $(REL_LIB)/libVoiceEngine.a $(REL_LIB)/libsecurec.a \
    $(MP4V2_DIR)/lib/libmp4v2.a
SYS_LIBS := -lpthread -lm -ldl -lstdc++

# ── PC 原生单测(宿主 gcc;不碰 SDK / 不碰硬件) ───────────────────────────
# ⚠️ `-std=c11` 会让 POSIX 声明(如 `struct timeval`)默认不可见 ⇒ 必须同时
#    定义 `_DEFAULT_SOURCE`。第一次跑就是这样栽的:只加 `-std=c11` 时
#    `tools/rtp_test.c` 报 `storage size of 'tv' isn't known`。
HOST_CFLAGS := $(WARN) -O2 -std=c11 -D_DEFAULT_SOURCE -pthread \
               -Isrc/service -Isrc/protocol -Isrc/infra -Isrc/bsp
# 每条规则的**先决条件就是它的源文件**(recipe 里用 $^ 展开) —— 一眼能看出
# 这个测试到底覆盖了哪几个模块。
PC_TESTS := $(BUILD)/pc/svc_record_policy_test \
            $(BUILD)/pc/http_test \
            $(BUILD)/pc/rtp_test \
            $(BUILD)/pc/sdp_test \
            $(BUILD)/pc/rtsp_test \
            $(BUILD)/pc/infra_netio_test \
            $(BUILD)/pc/infra_queue_test \
            $(BUILD)/pc/svc_sender_test \
            $(BUILD)/pc/svc_net_test \
            $(BUILD)/pc/bsp_osd_render_test

# 裸流样本(**可选**):只有 `rtp_test` / `sdp_test` 需要它 ——
# 这两条要真实的 Annex-B 码流(含 SPS/PPS + 切片)才能验"打包→重组逐字节一致"。
# 仓库里**不放样本**(十几 MB), 用变量传进来即可:
#     make test RAW_STREAM=/path/to/stream_chn1.h264
# ⚠️ 变量名别叫 `SAMPLE` —— 那和上面 SDK 的 `SAMPLE_DIR` 撞过一次
#    (第一次跑就是: `$(SAMPLE)` 展开成了 SDK 的 sample 目录, 于是"跳过"判断永远为假、
#     两个测试在**没有输入文件**的情况下被跑起来并失败)。
RAW_STREAM ?=

# ═══════════════════════════════════════════════════════════════════════════
#  目标
# ═══════════════════════════════════════════════════════════════════════════
.PHONY: all test clean help check-env

all: check-env $(OUT)
	@echo "✅ 产物: $(OUT)"

# 环境自检:缺什么就明确说缺什么(而不是让 gcc 报一堆找不到头文件)
check-env:
	@test -d "$(REL_INC)" || { \
	  echo "❌ 找不到 SDK 头文件: $(REL_INC)"; \
	  echo "   用 make SDK_DIR=/path/to/Hi3516CV500_SDK_V2.0.2.0 指定"; exit 1; }
	@test -f "$(MP4V2_DIR)/lib/libmp4v2.a" || { \
	  echo "❌ 找不到 mp4v2 静态库: $(MP4V2_DIR)/lib/libmp4v2.a"; \
	  echo "   用 make MP4V2_DIR=/path/to/mp4v2-arm 指定"; exit 1; }
	@command -v $(CC) >/dev/null 2>&1 || { \
	  echo "❌ 找不到交叉编译器: $(CC)"; \
	  echo "   用 make CROSS_COMPILE=/path/to/bin/arm-himix200-linux- 指定"; exit 1; }

$(BUILD)/%.o: src/%.c
	@mkdir -p $(@D)
	$(CC) $(CFLAGS) -c $< -o $@

$(BUILD)/vendor/%.o: $(COMMON_DIR)/%.c
	@mkdir -p $(@D)
	$(CC) $(CFLAGS) -c $< -o $@

$(OUT): $(OBJS) $(VND_OBJS)
	$(CC) $(CFLAGS) -o $@ $(OBJS) $(VND_OBJS) \
	  -Wl,--start-group $(VENDOR_LIBS) $(SYS_LIBS) -Wl,--end-group
	@$(SIZE) $@
	@md5sum $@

$(BUILD)/pc/svc_record_policy_test: tools/svc_record_policy_test.c \
                                    src/service/svc_record_policy.c
	@mkdir -p $(@D)
	$(HOSTCC) $(HOST_CFLAGS) -o $@ $^

$(BUILD)/pc/http_test: tools/http_test.c src/protocol/proto_http.c \
                       src/protocol/proto_str.c
	@mkdir -p $(@D)
	$(HOSTCC) $(HOST_CFLAGS) -o $@ $^

$(BUILD)/pc/rtp_test: tools/rtp_test.c \
                      src/protocol/proto_rtp.c src/protocol/proto_nalu.c
	@mkdir -p $(@D)
	$(HOSTCC) $(HOST_CFLAGS) -o $@ $^

$(BUILD)/pc/sdp_test: tools/sdp_test.c src/protocol/proto_sdp.c \
                      src/protocol/proto_str.c src/protocol/proto_nalu.c
	@mkdir -p $(@D)
	$(HOSTCC) $(HOST_CFLAGS) -o $@ $^

$(BUILD)/pc/rtsp_test: tools/rtsp_test.c src/protocol/proto_rtsp.c \
                       src/protocol/proto_sdp.c src/protocol/proto_str.c \
                       src/protocol/proto_nalu.c
	@mkdir -p $(@D)
	$(HOSTCC) $(HOST_CFLAGS) -o $@ $^

$(BUILD)/pc/infra_netio_test: tools/infra_netio_test.c src/infra/infra_netio.c \
                              src/infra/infra_poll.c src/infra/infra_log.c
	@mkdir -p $(@D)
	$(HOSTCC) $(HOST_CFLAGS) -o $@ $^

$(BUILD)/pc/infra_queue_test: tools/infra_queue_test.c src/infra/infra_queue.c
	@mkdir -p $(@D)
	$(HOSTCC) $(HOST_CFLAGS) -o $@ $^

$(BUILD)/pc/svc_sender_test: tools/svc_sender_test.c src/service/svc_sender.c \
                             src/protocol/proto_rtp.c src/protocol/proto_nalu.c \
                             src/infra/infra_queue.c src/infra/infra_log.c \
                             src/infra/infra_netio.c
	@mkdir -p $(@D)
	$(HOSTCC) $(HOST_CFLAGS) -o $@ $^

$(BUILD)/pc/svc_net_test: tools/svc_net_test.c src/service/svc_net.c \
                          src/protocol/proto_rtsp.c src/protocol/proto_str.c \
                          src/infra/infra_poll.c src/infra/infra_netio.c \
                          src/infra/infra_log.c
	@mkdir -p $(@D)
	$(HOSTCC) $(HOST_CFLAGS) -o $@ $^

$(BUILD)/pc/bsp_osd_render_test: tools/bsp_osd_render_test.c \
                                 src/bsp/bsp_osd_render.c
	@mkdir -p $(@D)
	$(HOSTCC) $(HOST_CFLAGS) -o $@ $^

# 编 + 跑:任何一条断言失败都会让 make 以非 0 退出(可以直接进 CI)
#
# ⚠️ 两条"要外部输入"的测试(`rtp_test` / `sdp_test`)在没给 `RAW_STREAM=` 时被**跳过**
#    并打出一行提示 —— 跳过要看得见, 不能静默少跑两条还报"全部通过"。
test: $(PC_TESTS)
	@fail=0; skip=0; \
	for t in $(PC_TESTS); do \
	  printf '\n──────── %s ────────\n' "$$t"; \
	  case "$$t" in \
	    *rtp_test|*sdp_test) \
	      if [ -z "$(RAW_STREAM)" ]; then \
	        echo "   [跳过] 这一条需要真实裸流样本(含 SPS/PPS)"; \
	        echo "          跑法: make test RAW_STREAM=/path/to/stream_chn1.h264"; \
	        skip=$$((skip + 1)); continue; \
	      fi; \
	      "$$t" "$(RAW_STREAM)" || fail=1 ;; \
	    *) "$$t" || fail=1 ;; \
	  esac; \
	done; \
	if [ $$fail -ne 0 ]; then echo ""; echo "❌ 有单测未通过"; exit 1; fi; \
	echo ""; echo "✅ PC 原生单测通过(跳过 $$skip 条需要裸流样本的)"

clean:
	rm -rf $(BUILD)

help:
	@echo "make                      交叉编译板子程序 → $(OUT)"
	@echo "make test                 编 + 跑 PC 原生单测(不需要 SDK)"
	@echo "make clean                删除 $(BUILD)/"
	@echo ""
	@echo "可覆盖的变量(默认值):"
	@echo "  CROSS_COMPILE=$(CROSS_COMPILE)"
	@echo "  SDK_DIR=$(SDK_DIR)"
	@echo "  MP4V2_DIR=$(MP4V2_DIR)"
	@echo "  SENSOR=$(SENSOR)"
	@echo "  HOSTCC=$(HOSTCC)"
	@echo ""
	@echo "我们的源文件($(words $(SRCS)) 个):"
	@echo "  $(SRCS)"
