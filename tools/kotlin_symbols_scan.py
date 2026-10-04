#!/usr/bin/env python3
"""检查关键 Kotlin 符号是否还在。

## 为什么做这个（而不是通用的"引用完整性"检查）

上一轮用脚本按字符串索引切片改文件，**静默删掉了 readHead 与
hexToBytes 两个 private 函数**，编译才报 Unresolved reference。

而"数花括号"那种结构检查**测不出来**：删掉的是一整块平衡的括号，
开头少一个、结尾也少一个，计数仍然是 0。

先试过做一个通用的"调用但未定义"检查器，很快就放弃了——
Compose 大量用通配导入（androidx.compose.material3.*），
无法把 Text / Column 这类名字解析回去，结果 200 多条全是噪声。
**没有编译器就做不出可靠的通用检查**，做了反而是虚假的安全感。

所以退一步：不做通用检查，只守**关键符号的存在性**。
表是手写的，但它精确、零噪声，而且正好覆盖"被脚本误删"这个真实故障。

## 维护

往 EXPECT 里加条目即可：文件里新增了不容丢失的关键函数就补上。

用法：
  python3 tools/kotlin_symbols_scan.py <root>
退出码：0 = 齐全，1 = 有丢失
"""
import os
import sys

EXPECT = {
    "core/RamdiskProbe.kt": [
        "fun classify", "fun kindOf", "fun isBlockDevice", "fun blockSize",
        "fun fileSize", "fun enumerateBlockDevices", "fun gzipHeadIsCpio",
        "fun readHead", "fun hexToBytes", "fun find", "fun findAll",
        "fun searchedPaths", "enum class Kind", "data class Found",
    ],
    "core/RootShell.kt": [
        "fun detect", "fun exec", "fun shSync", "fun headHexSync",
        "fun sizeOfSync", "fun openRead", "fun rawExec", "fun rawExecSync",
        "data class Result",
    ],
    "core/BossIpc.kt": ["fun exec", "fun ping", "data class Result", "object Flag"],
    "data/LocalInstallRepository.kt": [
        "fun findPartition", "fun partitionSize", "fun sizeOf", "fun freeBytes",
        "fun dump", "fun patch", "fun preparePayload", "fun preflight",
        "fun verifyLocal", "fun flash", "fun restore",
        "data class Step", "data class Gate", "data class PartitionInfo",
    ],
    "data/PatchRepository.kt": [
        "fun probe", "fun analyze", "fun inject", "fun verify", "fun version",
        "serial",
    ],
    "data/BossRepository.kt": [
        "fun status", "fun systemless", "fun verify", "fun exposures",
        "fun modules", "fun policy", "fun auditLog",
    ],
    "ui/BossViewModel.kt": [
        "fun refresh", "fun saveBaseline", "val verify", "fun denyAdd",
        "fun answer", "fun startPromptPolling", "fun stopPromptPolling",
    ],
    "ui/PatchViewModel.kt": [
        "fun pickImage", "fun pickPayload", "fun analyze", "fun patch",
        "fun clearAll", "fun setRole",
    ],
    "ui/LocalInstallViewModel.kt": [
        "fun probe", "fun pickPayload", "fun dump", "fun patch", "fun flash",
        "fun restore",
    ],
    "ui/screens/HomeScreen.kt": ["fun HomeScreen"],
    "ui/screens/PatchScreen.kt": ["fun PatchScreen"],
    "ui/screens/LocalInstallScreen.kt": ["fun LocalInstallScreen"],
    "ui/screens/SuperuserScreen.kt": ["fun SuperuserScreen"],
    "ui/screens/HideScreen.kt": ["fun HideScreen"],
    "ui/screens/ModuleScreen.kt": ["fun ModuleScreen"],
    "ui/MainActivity.kt": ["fun SectionCard", "fun KV", "class MainActivity"],
    "ui/theme/Theme.kt": ["fun BossTheme"],
}


def main():
    root = sys.argv[1] if len(sys.argv) > 1 else "."
    base = os.path.join(root, "app/src/main/java/com/boss/manager")
    bad = 0
    for rel, syms in sorted(EXPECT.items()):
        p = os.path.join(base, rel)
        if not os.path.exists(p):
            print("  FAIL  %s 文件不存在" % rel)
            bad += 1
            continue
        src = open(p, encoding="utf-8", errors="replace").read()
        for s in syms:
            if s not in src:
                print("  FAIL  %s 缺少 %r" % (rel, s))
                bad += 1
    if bad == 0:
        print("  ok    %d 个文件的关键符号齐全" % len(EXPECT))
    return 1 if bad else 0


if __name__ == "__main__":
    sys.exit(main())
