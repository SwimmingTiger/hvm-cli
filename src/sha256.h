// 自包含的 SHA-256（不依赖 OpenSSL / 系统库）
//
// 提供两件事：
//   1) Sha256 —— 标准 SHA-256 流式实现（标量核；运行时有 ARMv8 加密扩展则走硬件指令）
//   2) sha256File() —— 计算整个文件的摘要，用【多线程并行预读】喂给顺序的哈希核
//
// 为什么"多线程"只用在读取上：
//   SHA-256 是顺序依赖的 —— 第 N 块的输入包含第 N-1 块的输出，摘要本身无法并行。
//   能真正并行的是 I/O：多个线程 pread 不同区段到缓冲区池，主线程按块序消费，
//   于是哈希不再等磁盘。这是单文件摘要唯一有意义的多线程用法。
#pragma once

#include <cstddef>
#include <cstdint>
#include <string>

namespace hvm {

class Sha256 {
public:
    Sha256();
    void update(const void *data, std::size_t len);
    void final(uint8_t out[32]);

    //: 32 字节摘要 → 64 字符【大写】十六进制
    static std::string toHexUpper(const uint8_t digest[32]);
    //: 大写十六进制字符串规范化：小写转大写；非法字符原样保留（由调用方判断）
    static std::string normalizeHexUpper(const std::string &hex);

private:
    void block(const uint8_t *p, std::size_t blocks);

    uint32_t h_[8];
    uint64_t bytes_;
    uint8_t buf_[64];
    std::size_t bufLen_;
};

//: 本机是否有 ARMv8 SHA-256 加密扩展（运行时检测，同二进制跨机型安全）
bool sha256HwAccelAvailable();

//: 计算整个文件的 SHA-256，输出【大写】十六进制
//:   threads = 0 → 自动（按 CPU 核数与文件大小取一个合适的值）
//:   progress 非空时，每处理若干块回调一次（done, total，单位字节）
//: 返回 0 成功；否则返回负值并把原因写入 err（若非空）
int sha256File(const std::string &path, std::string &hexUpper, int threads = 0,
               void (*progress)(uint64_t done, uint64_t total) = nullptr,
               std::string *err = nullptr);

}  // namespace hvm
