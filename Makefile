# hvm-cli 构建脚本
#
# 目标平台：aarch64 HarmonyOS PC（OHOS musl 用户态）
# 工具链：系统自带 clang++（OHOS 工具链，Target: aarch64-unknown-linux-ohos）
#
# 运行期用 dlopen 加载系统自带的 /system/lib64/libvm_manager_kits.z.so，
# 编译期只依赖 libdl，不需要任何 OHOS 私有头文件。

CXX      ?= clang++
CXXFLAGS ?= -O2 -std=c++17 -Wall -Wextra
LDLIBS   ?= -ldl

BIN      := hvm-cli
SRCS     := src/hvm_client.cpp src/main.cpp
HDRS     := src/hvm_client.h
PREFIX   ?= $(HOME)/.local

.PHONY: all clean check install

all: $(BIN)

$(BIN): $(SRCS) $(HDRS)
	$(CXX) $(CXXFLAGS) -Isrc -o $@ $(SRCS) $(LDLIBS)
	@echo "构建完成: $@"

# 自检：加载客户端 kit 并读取只读状态
check: $(BIN)
	@./$(BIN) selftest
	@./$(BIN) info

install: $(BIN)
	install -d $(PREFIX)/bin
	install -m 0755 $(BIN) $(PREFIX)/bin/$(BIN)
	@echo "已安装到 $(PREFIX)/bin/$(BIN)"

clean:
	rm -f $(BIN)
