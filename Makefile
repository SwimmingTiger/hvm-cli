# hvm-cli 构建脚本
#
# 目标平台：aarch64 HarmonyOS PC（OHOS musl 用户态）
# 工具链：系统自带 clang++（OHOS 工具链，Target: aarch64-unknown-linux-ohos）
#
# 本仓库构建两个命令：
#   hvm-cli    控制我们自己创建的虚拟机（vm_manager / SA 65621）
#   openeuler  连入融合开发引擎的 openEuler 环境（fusion PTY 通道）
#
# 两者都通过 dlopen 使用系统自带库，编译期只依赖 libdl。

CXX      ?= clang++
CXXFLAGS ?= -O2 -std=c++17 -Wall -Wextra -Iinclude -Isrc
LDLIBS   ?= -ldl

BIN_VM   := hvm-cli
BIN_OE   := openeuler

VM_SRCS  := src/hvm_client.cpp src/cfginfo.cpp src/main.cpp
VM_HDRS  := src/hvm_client.h src/cfginfo.h
OE_SRCS  := src/fusion_pty.cpp src/openeuler_main.cpp
OE_HDRS  := src/fusion_pty.h

PREFIX   ?= $(HOME)/.local

.PHONY: all clean check check-repo check-headers install

all: $(BIN_VM) $(BIN_OE)

$(BIN_VM): $(VM_SRCS) $(VM_HDRS)
	$(CXX) $(CXXFLAGS) -o $@ $(VM_SRCS) $(LDLIBS)
	@echo "构建完成: $@"

$(BIN_OE): $(OE_SRCS) $(OE_HDRS)
	$(CXX) $(CXXFLAGS) -o $@ $(OE_SRCS) $(LDLIBS)
	@echo "构建完成: $@"

# 自检：分别验证两条通路的只读接口
check: all
	@echo "--- hvm-cli（虚拟机管理）---"
	@./$(BIN_VM) selftest
	@./$(BIN_VM) info
	@echo "--- openeuler（融合开发引擎）---"
	@./$(BIN_OE) selftest

# 逆向出来的公共头文件单独做语法检查（它们不参与两个命令的构建）
check-headers:
	@mkdir -p build
	@printf '#include "ohos/vm_manager_service/cfg_info.h"\n#include "ohos/vm_manager_service/vm_manager_client_wrapper.h"\n#include "ohos/linux_fusion/fusion_pty_ndk.h"\nint main(void){return 0;}\n' > build/headers_check.cpp
	@$(CXX) $(CXXFLAGS) -Iinclude -fsyntax-only build/headers_check.cpp && echo "✓ include/ 头文件语法检查通过"

# 仓库卫生：被跟踪的文件里不允许有二进制（构建产物应被 .gitignore 忽略）
check-repo:
	@scripts/check-no-binary.sh

install: all
	install -d $(PREFIX)/bin
	install -m 0755 $(BIN_VM) $(PREFIX)/bin/$(BIN_VM)
	install -m 0755 $(BIN_OE) $(PREFIX)/bin/$(BIN_OE)
	@echo "已安装到 $(PREFIX)/bin/"

clean:
	rm -f $(BIN_VM) $(BIN_OE)
