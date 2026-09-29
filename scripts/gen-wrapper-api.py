#!/usr/bin/env python3
"""
gen-wrapper-api.py —— 生成 OHOS::VmManagerService::VmManagerClientWrapper 的公共头文件

解决的问题：设备上的系统库用 libc++ 的 inline namespace `std::__h`，而 SDK 默认是
`std::__n1`，所以"照着设备符号手抄 mangled 字符串"既不可读也容易抄错。
本脚本改成：

  1. 从 docs/abi/vm_manager_client_wrapper.symbols.txt（设备符号快照）取出方法名与参数；
  2. 生成 **真正的 C++ 声明**（include/ohos/vm_manager_service/vm_manager_kits.h），
     别人 #include 即可正常写代码、正常链接（在设备侧构建时）；
  3. 生成一个探测 TU，**用 ABI 命名空间 shim（`__h`）编译**，再用 llvm-nm 取出
     编译器产出的 mangled 名 —— 即符号表来自声明而非手抄；
  4. 把编译产物与设备快照**逐一核对**，全部一致才写出
     include/ohos/vm_manager_service/vm_manager_kits.syms.h（供 dlopen + dlsym 使用，
     用 abi::FindSym(方法名) 取符号）。

返回类型说明：Itanium 名字改编不含返回类型，因此
  * 带出参（引用）的方法一律是 ErrCode（int32_t）—— 已实测；
  * 少数按值返回的（GetHashName / GetSharedFolder / Get*Enabled / Is* …）在
    下表中显式给出，来源见每条注释。

用法:
  python3 scripts/gen-wrapper-api.py            # 生成并核对
  python3 scripts/gen-wrapper-api.py --check    # 只核对，不写文件
"""
from __future__ import annotations

import os
import re
import subprocess
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
SNAPSHOT = os.path.join(ROOT, "docs/abi/vm_manager_client_wrapper.symbols.txt")
HDR = os.path.join(ROOT, "include/ohos/vm_manager_service/vm_manager_kits.h")
SYMS_HDR = os.path.join(ROOT, "include/ohos/vm_manager_service/vm_manager_kits.syms.h")
SHIM = os.path.join(ROOT, "scripts/abi-shim")
BUILD = os.path.join(ROOT, "build")
PROBE = os.path.join(BUILD, "abi_probe.cpp")
PROBE_O = os.path.join(BUILD, "abi_probe.o")

#: 按值返回的方法 —— 返回类型不是 ErrCode，需显式给出（依据见注释）
RET_OVERRIDES = {
    "GetInstance": ("sptr<VmManagerClientWrapper>", "static", "实测：sret 返回 sptr"),
    "GetHashName": ("std::string", "", "实测：sret 返回 std::string"),
    "GetSharedFolder": ("std::string", "", "实测：sret 返回 std::string"),
    # ⚠️ 实测否定：按 vector<string> 解释会段错误（析构不匹配），元素是私有类，
    #    需像 PortInfoList 那样从服务端 Unmarshalling 还原后才能接。见 api-notes §14.3
    "GetAllSharedVolume": ("std::vector<std::string>", "", "实测否定：元素不是 string，待还原"),
    "GetStratovirtMem": ("int64_t", "", "实测：返回字节数（11061624 B ≈ 10.5 MiB）"),
    "GetSharedFolderEnabled": ("bool", "", "实测：返回 bool"),
    "IsQuickStartScenario": ("bool", "", "实测：返回 bool（CLI 打印 否）"),
    "CheckIsInstalling": ("bool", "", "实测：返回 bool（CLI 打印 否）"),
    "GetPasteboardEnableState": ("bool", "", "实测：返回 bool（CLI 打印 开）"),
    "GetPasteboardUsableState": ("bool", "", "实测：返回 bool（CLI 打印 可用）"),
}

#: 不放进头文件的方法（析构等）
SKIP = {"~VmManagerClientWrapper"}

#: 枚举类型（用不透明声明即可满足函数声明）
ENUMS = {"EventType", "FormDimension", "FocusState", "HapticSceneType", "BackgroundState"}

STD_TYPES = {
    "std::basic_string<char,std::char_traits<char>,std::allocator<char>>": "std::string",
    "std::basic_string<char, std::char_traits<char>, std::allocator<char>>": "std::string",
}


def split_top(text: str) -> list[str]:
    """按顶层逗号切分模板/函数参数"""
    out, depth, cur = [], 0, ""
    for ch in text:
        if ch in "<([":
            depth += 1
        elif ch in ">)]":
            depth -= 1
        if ch == "," and depth == 0:
            out.append(cur.strip())
            cur = ""
        else:
            cur += ch
    if cur.strip():
        out.append(cur.strip())
    return out


def norm_type(t: str) -> str:
    """把 llvm-cxxfilt 的完整类型名规范成可读的 C++ 类型"""
    t = re.sub(r"\s+", " ", t.strip())
    # 1) 先把 __h / __n1 之类的 inline namespace 拿掉，后面的折叠才好匹配
    t = t.replace("std::__h::", "std::").replace("std::__n1::", "std::")
    # 2) 折叠标准库模板
    t = re.sub(r"std::basic_string<char, std::char_traits<char>, std::allocator<char>>",
               "std::string", t)
    t = re.sub(r"std::vector<(.*?), std::allocator<\1>>", r"std::vector<\1>", t)
    t = re.sub(r"std::map<(.*?), (.*?), std::less<\1>, std::allocator<std::pair<.*?>>>",
               r"std::map<\1, \2>", t)
    t = re.sub(r"std::shared_ptr<(.*?)>", r"std::shared_ptr<\1>", t)
    t = re.sub(r"std::function<(.*?)>", r"std::function<\1>", t)
    # 3) 去掉 OHOS 命名空间前缀
    t = t.replace("OHOS::VmManagerService::", "").replace("OHOS::", "")
    # 4) 基本类型
    t = re.sub(r"\bunsigned long\b", "uint64_t", t)
    t = re.sub(r"\bunsigned int\b", "uint32_t", t)
    t = re.sub(r"\blong\b", "int64_t", t)
    t = re.sub(r"\bint\b", "int32_t", t)
    # 5) 引用/const 统一风格
    m = re.match(r"^(.*?) const&$", t)
    if m:
        return "const " + m.group(1).strip() + " &"
    m = re.match(r"^(.*?)&$", t)
    if m:
        return m.group(1).strip() + " &"
    return t


def referenced_types(params):
    """从参数类型里挑出需要前置声明的 OHOS 类型"""
    out = set()
    for p in params:
        # 只取未被 :: 限定的、首字母大写的标识符
        for name in re.findall(r"(?<![\w:])([A-Z][A-Za-z0-9_]*)", p):
            if name in ("OHOS",):
                continue
            out.add(name)
    return out


def parse_snapshot():
    methods = []
    for line in open(SNAPSHOT, encoding="utf-8"):
        if line.startswith("#") or "\t" not in line:
            continue
        mangled, demangled = line.rstrip("\n").split("\t", 1)
        m = re.match(r"^[\w:]+::VmManagerClientWrapper::(\w+)\((.*)\)$", demangled)
        if not m:
            continue
        name, args = m.group(1), m.group(2)
        if name in SKIP:
            continue
        params = [norm_type(p) for p in split_top(args)] if args.strip() else []
        methods.append({"name": name, "params": params, "mangled": mangled})
    methods.sort(key=lambda x: x["name"])
    return methods


def gen_header(methods) -> str:
    lines = [
        "// 本文件由 scripts/gen-wrapper-api.py 生成（符号来自设备快照 + 编译器核对）。",
        "// 请勿手工编辑；要增删接口请改脚本里的数据表后重新生成。",
        "//",
        "// 用法：这是华为私有的客户端 SDK（系统内置、无公开头文件）。",
        "//   * 设备侧构建：直接 #include 本头文件并用 VmManagerClientWrapper 的声明；",
        "//   * dlopen 场景：用生成的符号表 abi::FindSym(\"方法名\") 取 mangled 名再 dlsym。",
        "//",
        "// 返回类型：Itanium 改编不含返回类型。带出参的方法一律 ErrCode(int32_t)（实测）；",
        "// 少数按值返回的见下方逐条注释。",
        "#ifndef OHOS_VM_MANAGER_SERVICE_VM_MANAGER_KITS_H",
        "#define OHOS_VM_MANAGER_SERVICE_VM_MANAGER_KITS_H",
        "",
        "#include <cstdint>",
        "#include <functional>",
        "#include <map>",
        "#include <memory>",
        "#include <string>",
        "#include <vector>",
        "",
        '#include "ohos/vm_manager_service/vm_manager_errcode.h"',
        "",
        "namespace OHOS {",
        "",
        "//: 单指针智能指针（对象布局就是一个指针；本仓库按引用传递即可）",
        "template <typename T> class sptr;",
        "",
        "namespace VmManagerService {",
        "",
        "// ---- 被引用类型的声明 ----",
    ]
    fwd = referenced_types([p for m in methods for p in m["params"]])
    for t in sorted(fwd):
        if t in ENUMS:
            lines.append(f"enum {t} : int;")
        else:
            lines.append(f"class {t};")
    lines += [
        "",
        "//: 系统能力 id 与接口描述符（IPC 用）",
        "constexpr int kSystemAbilityId = 65621;",
        'constexpr char kInterfaceDescriptor[] = "OHOS.VmManagerService.IVmManager";',
        'constexpr char kCallbackDescriptor[] = "OHOS.VmManager.IVmManagerCallback";',
        "",
        "class VmManagerClientWrapper {",
        "  public:",
    ]
    for m in methods:
        ret, static, why = RET_OVERRIDES.get(m["name"], ("int32_t", "", ""))
        decl = "    "
        if static:
            decl += "static "
        decl += f"{ret} {m['name']}("
        decl += ", ".join(m["params"]) if m["params"] else ""
        decl += ");"
        if why:
            decl += f"  // {why}"
        lines.append(decl)
    lines += [
        "};",
        "",
        "}  // namespace VmManagerService",
        "}  // namespace OHOS",
        "",
        "#include \"ohos/vm_manager_service/vm_manager_kits.syms.h\"",
        "",
        "#endif  // OHOS_VM_MANAGER_SERVICE_VM_MANAGER_KITS_H",
        "",
    ]
    return "\n".join(lines)


def gen_probe(methods) -> str:
    """生成探测 TU：只取成员函数地址，让编译器产出符号名。

    取地址不需要参数类型完整（前置声明即可），因此不必为那些只声明未定义的
    OHOS 类型编造定义。用 -O0 保证地址确实被物化。
    """
    L = [
        "// 由 scripts/gen-wrapper-api.py 生成：仅用于让编译器产出符号名，不参与链接。",
        '#include "ohos/vm_manager_service/vm_manager_kits.h"',
        "",
        "namespace {",
        "using W = OHOS::VmManagerService::VmManagerClientWrapper;",
        "}  // namespace",
        "",
        'extern "C" void hvmAbiProbe(W *w) {',
        "    (void)w;",
    ]
    counts = {}
    for m in methods:
        counts[m["name"]] = counts.get(m["name"], 0) + 1
    for i, m in enumerate(methods):
        if counts[m["name"]] > 1:
            # 重载：必须给出显式签名才能取地址
            ret = RET_OVERRIDES.get(m["name"], ("int32_t",))[0]
            params = ", ".join(m["params"])
            L.append(f"    using T{i} = {ret} (W::*)({params});")
            L.append(f"    auto p{i} = static_cast<T{i}>(&W::{m['name']}); (void)p{i};")
        else:
            L.append(f"    auto p{i} = &W::{m['name']}; (void)p{i};")
    L += ["}", ""]
    return "\n".join(L)


def main() -> int:
    check_only = "--check" in sys.argv
    methods = parse_snapshot()
    if not methods:
        print("快照为空？", file=sys.stderr)
        return 2

    os.makedirs(BUILD, exist_ok=True)
    if not check_only:
        with open(HDR, "w", encoding="utf-8") as f:
            f.write(gen_header(methods))
        with open(PROBE, "w", encoding="utf-8") as f:
            f.write(gen_probe(methods))

    # 头文件会 include syms 头；探测编译前先放一个空壳，校验通过后再写真正内容
    if not os.path.exists(SYMS_HDR):
        with open(SYMS_HDR, "w", encoding="utf-8") as f:
            f.write("// 占位（生成器随后覆盖）\n")

    # 用 __h shim 编译探测 TU，取编译器产出的 mangled 名
    cxx = os.environ.get("CXX", "clang++")
    cmd = [cxx, "-c", "-O0", "-std=c++17", "-I", SHIM, "-I", os.path.join(ROOT, "include"),
           "-o", PROBE_O, PROBE]
    if subprocess.run(cmd).returncode != 0:
        print("探测 TU 编译失败", file=sys.stderr)
        return 3

    nm = os.path.join(os.path.dirname(cxx), "llvm-nm")
    nm = nm if os.path.exists(nm) else "llvm-nm"
    out = subprocess.run([nm, "--undefined-only", PROBE_O], capture_output=True, text=True).stdout
    produced = {}
    for line in out.splitlines():
        sym = line.split()[-1]
        if "VmManagerClientWrapper" in sym:
            produced[sym] = True

    snapshot = {m["mangled"] for m in methods}
    missing = sorted(snapshot - set(produced))
    extra = sorted(set(produced) - snapshot)
    print(f"方法数 {len(methods)}；编译器产出 {len(produced)} 个符号")
    if missing:
        print(f"!! 编译器未能产出 {len(missing)} 个符号（声明可能不完整）：")
        for s in missing[:5]:
            print("   ", s)
    if extra:
        print(f"!! 多出 {len(extra)} 个符号：")
        for s in extra[:5]:
            print("   ", s)
    ok = not missing and not extra
    print("核对结果:", "全部一致 ✓" if ok else "不一致 ✗")
    if not ok:
        return 1

    if not check_only:
        L = [
            "// 本文件由 scripts/gen-wrapper-api.py 生成：mangled 名由**编译器**从",
            "// vm_manager_kits.h 的声明产出，并与设备符号快照逐一核对通过。",
            "// 请勿手工编辑。",
            "#ifndef OHOS_VM_MANAGER_SERVICE_VM_MANAGER_KITS_SYMS_H",
            "#define OHOS_VM_MANAGER_SERVICE_VM_MANAGER_KITS_SYMS_H",
            "",
            "namespace OHOS {",
            "namespace VmManagerService {",
            "namespace abi {",
            "",
            "struct SymEntry {",
            "    const char *name;",
            "    const char *mangled;",
            "};",
            "",
            "//: 由方法名查 mangled 名（dlopen + dlsym 用）",
            "inline constexpr SymEntry kWrapSyms[] = {",
        ]
        for m in methods:
            L.append(f'    {{"{m["name"]}", "{m["mangled"]}"}},')
        L += [
            "};",
            "constexpr std::size_t kWrapSymCount = sizeof(kWrapSyms) / sizeof(kWrapSyms[0]);",
            "",
            "inline const char *FindSym(const char *name) {",
            "    for (std::size_t i = 0; i < kWrapSymCount; ++i) {",
            "        const char *a = kWrapSyms[i].name;",
            "        const char *b = name;",
            "        while (*a != 0 && *a == *b) { ++a; ++b; }",
            "        if (*a == 0 && *b == 0) return kWrapSyms[i].mangled;",
            "    }",
            "    return nullptr;",
            "}",
            "",
            "}  // namespace abi",
            "}  // namespace VmManagerService",
            "}  // namespace OHOS",
            "",
            "#endif  // OHOS_VM_MANAGER_SERVICE_VM_MANAGER_KITS_SYMS_H",
            "",
        ]
        with open(SYMS_HDR, "w", encoding="utf-8") as f:
            f.write("\n".join(L))
        print("已写出:", os.path.relpath(HDR, ROOT), "与", os.path.relpath(SYMS_HDR, ROOT))
    return 0


if __name__ == "__main__":
    sys.exit(main())
