// 自包含 SHA-256 实现。见 sha256.h 顶部的说明（为什么多线程只用在读取上）。
#include "sha256.h"

#include <algorithm>
#include <atomic>
#include <cerrno>
#include <condition_variable>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <thread>
#include <vector>

#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

#if defined(__linux__)
#include <sys/auxv.h>
#endif

// ARMv8 的 SHA-256 加密扩展：目标机型（鸿蒙 PC）全部支持，构建时只给本文件
// 加 -march=armv8-a+crypto（见 Makefile），因此这些内建函数在这里可用。
// 运行时仍用 getauxval(HWCAP_SHA2) 兜一道，万一遇到不支持的环境自动退回标量核。
#if defined(__aarch64__) && (defined(__clang__) || defined(__GNUC__))
#  define HVM_SHA256_ARM_CRYPTO 1
#  include <arm_neon.h>
#endif

namespace hvm {
namespace {

const uint32_t K[64] = {
    0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5, 0x3956c25b, 0x59f111f1, 0x923f82a4, 0xab1c5ed5,
    0xd807aa98, 0x12835b01, 0x243185be, 0x550c7dc3, 0x72be5d74, 0x80deb1fe, 0x9bdc06a7, 0xc19bf174,
    0xe49b69c1, 0xefbe4786, 0x0fc19dc6, 0x240ca1cc, 0x2de92c6f, 0x4a7484aa, 0x5cb0a9dc, 0x76f988da,
    0x983e5152, 0xa831c66d, 0xb00327c8, 0xbf597fc7, 0xc6e00bf3, 0xd5a79147, 0x06ca6351, 0x14292967,
    0x27b70a85, 0x2e1b2138, 0x4d2c6dfc, 0x53380d13, 0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85,
    0xa2bfe8a1, 0xa81a664b, 0xc24b8b70, 0xc76c51a3, 0xd192e819, 0xd6990624, 0xf40e3585, 0x106aa070,
    0x19a4c116, 0x1e376c08, 0x2748774c, 0x34b0bcb5, 0x391c0cb3, 0x4ed8aa4a, 0x5b9cca4f, 0x682e6ff3,
    0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208, 0x90befffa, 0xa4506ceb, 0xbef9a3f7, 0xc67178f2};

inline uint32_t ror(uint32_t x, int n) { return (x >> n) | (x << (32 - n)); }

inline uint32_t bigEndian32(const uint8_t *p) {
    return (static_cast<uint32_t>(p[0]) << 24) | (static_cast<uint32_t>(p[1]) << 16) |
           (static_cast<uint32_t>(p[2]) << 8) | static_cast<uint32_t>(p[3]);
}

// ---------------------------------------------------------------- 标量核
void blockScalar(uint32_t h[8], const uint8_t *p, std::size_t blocks) {
    for (std::size_t b = 0; b < blocks; ++b, p += 64) {
        uint32_t w[64];
        for (int i = 0; i < 16; ++i) w[i] = bigEndian32(p + 4 * i);
        for (int i = 16; i < 64; ++i) {
            const uint32_t s0 = ror(w[i - 15], 7) ^ ror(w[i - 15], 18) ^ (w[i - 15] >> 3);
            const uint32_t s1 = ror(w[i - 2], 17) ^ ror(w[i - 2], 19) ^ (w[i - 2] >> 10);
            w[i] = w[i - 16] + s0 + w[i - 7] + s1;
        }
        uint32_t a = h[0], bb = h[1], c = h[2], d = h[3];
        uint32_t e = h[4], f = h[5], g = h[6], hh = h[7];
        for (int i = 0; i < 64; ++i) {
            const uint32_t S1 = ror(e, 6) ^ ror(e, 11) ^ ror(e, 25);
            const uint32_t ch = (e & f) ^ (~e & g);
            const uint32_t t1 = hh + S1 + ch + K[i] + w[i];
            const uint32_t S0 = ror(a, 2) ^ ror(a, 13) ^ ror(a, 22);
            const uint32_t maj = (a & bb) ^ (a & c) ^ (bb & c);
            const uint32_t t2 = S0 + maj;
            hh = g; g = f; f = e; e = d + t1;
            d = c; c = bb; bb = a; a = t1 + t2;
        }
        h[0] += a; h[1] += bb; h[2] += c; h[3] += d;
        h[4] += e; h[5] += f; h[6] += g; h[7] += hh;
    }
}

#ifdef HVM_SHA256_ARM_CRYPTO
// ---------------------------------------------------------------- ARMv8 加密扩展
// 4 轮一组；STATE0=(a,b,c,d) STATE1=(e,f,g,h)，消息调度用 su0/su1 指令接力。
#define HVM_R4(MSG, KI)                                     \
    do {                                                    \
        uint32x4_t tmp = vaddq_u32((MSG), vld1q_u32(&K[KI])); \
        uint32x4_t n0 = vsha256hq_u32(S0, S1, tmp);         \
        uint32x4_t n1 = vsha256h2q_u32(S1, S0, tmp);        \
        S0 = n0; S1 = n1;                                   \
    } while (0)

__attribute__((target("sha2"))) void blockArmCrypto(uint32_t h[8], const uint8_t *p,
                                                    std::size_t blocks) {
    uint32x4_t S0 = vld1q_u32(&h[0]);
    uint32x4_t S1 = vld1q_u32(&h[4]);
    for (std::size_t b = 0; b < blocks; ++b, p += 64) {
        const uint32x4_t keep0 = S0, keep1 = S1;
        uint32x4_t m0 = vreinterpretq_u32_u8(vrev32q_u8(vld1q_u8(p + 0)));
        uint32x4_t m1 = vreinterpretq_u32_u8(vrev32q_u8(vld1q_u8(p + 16)));
        uint32x4_t m2 = vreinterpretq_u32_u8(vrev32q_u8(vld1q_u8(p + 32)));
        uint32x4_t m3 = vreinterpretq_u32_u8(vrev32q_u8(vld1q_u8(p + 48)));

        HVM_R4(m0, 0);
        HVM_R4(m1, 4);
        HVM_R4(m2, 8);
        HVM_R4(m3, 12);

        m0 = vsha256su1q_u32(vsha256su0q_u32(m0, m1), m2, m3);
        HVM_R4(m0, 16);
        m1 = vsha256su1q_u32(vsha256su0q_u32(m1, m2), m3, m0);
        HVM_R4(m1, 20);
        m2 = vsha256su1q_u32(vsha256su0q_u32(m2, m3), m0, m1);
        HVM_R4(m2, 24);
        m3 = vsha256su1q_u32(vsha256su0q_u32(m3, m0), m1, m2);
        HVM_R4(m3, 28);

        m0 = vsha256su1q_u32(vsha256su0q_u32(m0, m1), m2, m3);
        HVM_R4(m0, 32);
        m1 = vsha256su1q_u32(vsha256su0q_u32(m1, m2), m3, m0);
        HVM_R4(m1, 36);
        m2 = vsha256su1q_u32(vsha256su0q_u32(m2, m3), m0, m1);
        HVM_R4(m2, 40);
        m3 = vsha256su1q_u32(vsha256su0q_u32(m3, m0), m1, m2);
        HVM_R4(m3, 44);

        m0 = vsha256su1q_u32(vsha256su0q_u32(m0, m1), m2, m3);
        HVM_R4(m0, 48);
        m1 = vsha256su1q_u32(vsha256su0q_u32(m1, m2), m3, m0);
        HVM_R4(m1, 52);
        m2 = vsha256su1q_u32(vsha256su0q_u32(m2, m3), m0, m1);
        HVM_R4(m2, 56);
        m3 = vsha256su1q_u32(vsha256su0q_u32(m3, m0), m1, m2);
        HVM_R4(m3, 60);

        S0 = vaddq_u32(S0, keep0);
        S1 = vaddq_u32(S1, keep1);
    }
    vst1q_u32(&h[0], S0);
    vst1q_u32(&h[4], S1);
}
#undef HVM_R4
#endif

bool hwAccel() {
#ifdef HVM_SHA256_ARM_CRYPTO
#if defined(__linux__)
    static const bool ok = [] {
        unsigned long cap = getauxval(AT_HWCAP);
        return (cap & (1UL << 6)) != 0;  // HWCAP_SHA2
    }();
    return ok;
#else
    return true;
#endif
#else
    return false;
#endif
}

}  // namespace

bool sha256HwAccelAvailable() { return hwAccel(); }

// ---------------------------------------------------------------- 流式接口
Sha256::Sha256() : bytes_(0), bufLen_(0) {
    h_[0] = 0x6a09e667; h_[1] = 0xbb67ae85; h_[2] = 0x3c6ef372; h_[3] = 0xa54ff53a;
    h_[4] = 0x510e527f; h_[5] = 0x9b05688c; h_[6] = 0x1f83d9ab; h_[7] = 0x5be0cd19;
}

void Sha256::block(const uint8_t *p, std::size_t blocks) {
#ifdef HVM_SHA256_ARM_CRYPTO
    if (hwAccel() && blocks >= 1) {
        blockArmCrypto(h_, p, blocks);
        return;
    }
#endif
    blockScalar(h_, p, blocks);
}

void Sha256::update(const void *data, std::size_t len) {
    const uint8_t *p = static_cast<const uint8_t *>(data);
    bytes_ += len;
    if (bufLen_ != 0) {
        const std::size_t take = std::min<std::size_t>(64 - bufLen_, len);
        std::memcpy(buf_ + bufLen_, p, take);
        bufLen_ += take;
        p += take;
        len -= take;
        if (bufLen_ == 64) {
            block(buf_, 1);
            bufLen_ = 0;
        }
    }
    const std::size_t whole = len / 64;
    if (whole != 0) {
        block(p, whole);
        p += whole * 64;
        len -= whole * 64;
    }
    if (len != 0) {
        std::memcpy(buf_, p, len);
        bufLen_ = len;
    }
}

void Sha256::final(uint8_t out[32]) {
    const uint64_t bits = bytes_ * 8;
    // 填充：0x80 + 若干 0 + 8 字节大端长度，总长补齐到 64 的倍数
    uint8_t pad[72];
    pad[0] = 0x80;
    std::size_t padLen = 1;
    while ((bufLen_ + padLen) % 64 != 56) pad[padLen++] = 0x00;
    for (int i = 7; i >= 0; --i) pad[padLen++] = static_cast<uint8_t>((bits >> (8 * i)) & 0xff);

    // 直接复用 update：它自带缓冲与分块，且不会再触发 final
    update(pad, padLen);

    for (int i = 0; i < 8; ++i) {
        out[4 * i + 0] = static_cast<uint8_t>(h_[i] >> 24);
        out[4 * i + 1] = static_cast<uint8_t>(h_[i] >> 16);
        out[4 * i + 2] = static_cast<uint8_t>(h_[i] >> 8);
        out[4 * i + 3] = static_cast<uint8_t>(h_[i]);
    }
}

std::string Sha256::toHexUpper(const uint8_t digest[32]) {
    static const char *kHex = "0123456789ABCDEF";
    std::string s;
    s.resize(64);
    for (int i = 0; i < 32; ++i) {
        s[2 * i] = kHex[digest[i] >> 4];
        s[2 * i + 1] = kHex[digest[i] & 0x0f];
    }
    return s;
}

std::string Sha256::normalizeHexUpper(const std::string &hex) {
    std::string s = hex;
    for (char &c : s) {
        if (c >= 'a' && c <= 'f') c = static_cast<char>(c - 'a' + 'A');
    }
    return s;
}

// ---------------------------------------------------------------- 整文件摘要（并行预读）
int sha256File(const std::string &path, std::string &hexUpper, int threads,
               void (*progress)(uint64_t, uint64_t), std::string *err) {
    // 小文件不必报进度（几毫秒就完了，只会刷屏）
    constexpr uint64_t kProgressMinBytes = 32ull << 20;
    auto fail = [&](const std::string &why) {
        if (err != nullptr) *err = why;
        return -1;
    };

    const int fd = ::open(path.c_str(), O_RDONLY);
    if (fd < 0) return fail("打不开文件 " + path + "：" + std::strerror(errno));
    struct stat st {};
    if (::fstat(fd, &st) != 0 || !S_ISREG(st.st_mode)) {
        ::close(fd);
        return fail("不是普通文件：" + path);
    }
    const uint64_t total = static_cast<uint64_t>(st.st_size);

    if (threads <= 0) {
        const unsigned hw = std::thread::hardware_concurrency();
        threads = static_cast<int>(std::min<unsigned>(hw == 0 ? 4u : hw, 8u));
    }
    if (threads < 1) threads = 1;

    // 块大小：不小于 1 MiB；小文件自然退化为单块（此时不开线程）
    const uint64_t kMinChunk = 1ull << 20;
    uint64_t chunk = kMinChunk;
    uint64_t nchunks = total == 0 ? 1 : (total + chunk - 1) / chunk;
    if (threads > 1 && nchunks < static_cast<uint64_t>(threads) * 4) {
        const uint64_t want = std::max<uint64_t>(1, (total + static_cast<uint64_t>(threads) * 4 - 1) /
                                                        (static_cast<uint64_t>(threads) * 4));
        if (want > chunk) chunk = want;
        nchunks = total == 0 ? 1 : (total + chunk - 1) / chunk;
    }

    if (threads == 1 || nchunks <= 1) {  // 单线程直读，省掉线程与队列开销
        Sha256 sha;
        std::vector<uint8_t> buf(1u << 20);
        uint64_t off = 0;
        while (off < total) {
            const std::size_t want = static_cast<std::size_t>(std::min<uint64_t>(buf.size(), total - off));
            const ssize_t n = ::pread(fd, buf.data(), want, static_cast<off_t>(off));
            if (n <= 0) {
                ::close(fd);
                return fail("读取失败：" + path);
            }
            sha.update(buf.data(), static_cast<std::size_t>(n));
            off += static_cast<uint64_t>(n);
            if (progress != nullptr && total >= kProgressMinBytes) progress(off, total);
        }
        uint8_t d[32];
        sha.final(d);
        hexUpper = Sha256::toHexUpper(d);
        ::close(fd);
        return 0;
    }

    // 并行预读：depth 个槽位，多个读者线程按块号抢占，主线程按块号顺序消费
    enum State { FREE, READING, DONE };
    struct Slot {
        std::vector<uint8_t> buf;
        State state = FREE;
        uint64_t idx = 0;
        std::size_t len = 0;
    };
    const int depth = std::min<int>(threads * 2, 16);
    std::vector<Slot> slots(static_cast<std::size_t>(depth));
    for (auto &s : slots) s.buf.resize(static_cast<std::size_t>(chunk));

    std::mutex m;
    std::condition_variable cvFree, cvDone;
    std::atomic<uint64_t> nextIdx{0};
    std::atomic<bool> failed{false};
    // 关键不变量：读者只能认领「消费进度 + depth」之内的块号。
    // 只限制"在飞块数"是不够的 —— 那样读者仍可能认领到很后面的块号（例如第 i+depth 块
    // 与第 i 块共用同一个槽），把还没消费的块覆盖掉，于是消费者把第 i+depth 块当成第 i 块
    // 吃进去，算出**看似正常但错误**的摘要（实测：1.5GB 镜像上算错、633MB 上侥幸算对）。
    // 限制块号跨度后，槽位 i%depth 的上一轮 occupant 必然是第 i-depth 块，
    // 而它一定已被消费，槽位里就只可能是第 i 块。
    uint64_t consumeIdx = 0;   // 消费者接下来要吃第几块（受 m 保护）

    auto reader = [&] {
        for (;;) {
            const uint64_t i = nextIdx.fetch_add(1);
            if (i >= nchunks || failed.load()) return;
            Slot &s = slots[static_cast<std::size_t>(i % static_cast<uint64_t>(depth))];
            {
                std::unique_lock<std::mutex> lk(m);
                // 失败时也要唤醒，否则读者会一直等不到窗口前进
                cvFree.wait(lk, [&] { return i < consumeIdx + static_cast<uint64_t>(depth) || failed.load(); });
                if (failed.load()) return;
                s.state = READING;
                s.idx = i;
            }
            const uint64_t off = i * chunk;
            const std::size_t want =
                static_cast<std::size_t>(std::min<uint64_t>(chunk, total - off));
            std::size_t got = 0;
            while (got < want) {
                const ssize_t n = ::pread(fd, s.buf.data() + got, want - got,
                                          static_cast<off_t>(off + got));
                if (n <= 0) {
                    failed.store(true);
                    break;
                }
                got += static_cast<std::size_t>(n);
            }
            {
                std::lock_guard<std::mutex> lk(m);
                s.len = got;
                s.state = failed.load() ? FREE : DONE;
                cvDone.notify_all();
                cvFree.notify_all();
            }
            if (failed.load()) return;
        }
    };

    std::vector<std::thread> pool;
    pool.reserve(static_cast<std::size_t>(threads));
    for (int t = 0; t < threads; ++t) pool.emplace_back(reader);

    Sha256 sha;
    uint64_t done = 0;
    bool ok = true;
    const char *errKind = "读取失败";
    std::string errDetail;
    for (uint64_t i = 0; i < nchunks; ++i) {
        Slot &s = slots[static_cast<std::size_t>(i % static_cast<uint64_t>(depth))];
        {
            std::unique_lock<std::mutex> lk(m);
            consumeIdx = i;          // 窗口右移：允许读者认领到第 i+depth-1 块
            cvFree.notify_all();
            cvDone.wait(lk, [&] { return s.state == DONE || failed.load(); });
            // 块号兜底校验：一旦不匹配说明槽位复用出了问题，宁可失败也不给错摘要
            if (s.state != DONE) {
                ok = false;
                errKind = "读取失败";
                errDetail = "第 " + std::to_string(i) + " 块读取未完成";
                break;
            }
            if (s.idx != i) {
                ok = false;
                errKind = "内部错误";
                errDetail = "块序错乱：期望第 " + std::to_string(i) + " 块，槽位里是第 " +
                             std::to_string(s.idx) + " 块（共 " + std::to_string(nchunks) +
                             " 块，块长 " + std::to_string(chunk) + "，槽位 " +
                             std::to_string(depth) + "，线程 " + std::to_string(threads) + "）";
                break;
            }
        }
        sha.update(s.buf.data(), s.len);
        done += s.len;
        if (progress != nullptr && total >= kProgressMinBytes) progress(done, total);
        {
            std::lock_guard<std::mutex> lk(m);
            s.state = FREE;
        }
        cvFree.notify_all();
    }

    // 若中途失败，唤醒所有读者让它们退出
    if (!ok) {
        failed.store(true);
        cvFree.notify_all();
        cvDone.notify_all();
    }
    for (auto &t : pool) t.join();
    ::close(fd);

    if (!ok || failed.load())
        return fail(std::string(errKind) + "：" + path + (errDetail.empty() ? "" : "（" + errDetail + "）"));
    uint8_t d[32];
    sha.final(d);
    hexUpper = Sha256::toHexUpper(d);
    return 0;
}

}  // namespace hvm
