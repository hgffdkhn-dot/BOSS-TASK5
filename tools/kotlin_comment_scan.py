#!/usr/bin/env python3
"""扫描 Kotlin / Java 源文件的块注释是否平衡。

为什么需要这个：
  Kotlin（和 Java）的块注释**支持嵌套**。注释正文里一旦出现开启注释的
  那两个字符（例如写路径 /dev/block/platform/*/by-name），就会再开一层，
  一直吞到文件尾。报错是 "Missing '}'" + "Unclosed comment"，
  且位置指向**文件末尾**——离真因很远；还会引发几十条 Unresolved reference
  连锁报错，看着像"整个模块坏了"，实际只错了一行注释。

用法：
  python3 tools/kotlin_comment_scan.py <目录或文件>...
退出码：0 = 全部平衡，1 = 有未闭合
"""
import os
import sys

OPEN = "/*"
CLOSE = "*/"


def scan(path):
    """返回第一个未闭合块注释的起始行；平衡则返回 None。

    ⚠️ 关键：进入块注释后**只**看 /* 与 */，绝不能再解析引号。
       第一版就是在这里出错的——注释正文里的撇号（如 "command's output"）
       被当成字符字面量起点，一路吞掉了后面的 */，
       于是一个完全正常的上游文件被报成"未闭合"（假阳性）。
    """
    src = open(path, encoding="utf-8", errors="replace").read()
    i = 0
    n = len(src)
    line = 1
    depth = 0
    opens = []
    while i < n:
        c = src[i]

        if depth > 0:
            # 注释内部：只关心嵌套与闭合
            if src.startswith(OPEN, i):
                depth += 1
                opens.append(line)
                i += 2
                continue
            if src.startswith(CLOSE, i):
                depth -= 1
                opens.pop()
                i += 2
                continue
            if c == "\n":
                line += 1
            i += 1
            continue

        # ---- 代码区 ----
        if c == "\n":
            line += 1
            i += 1
            continue
        if src.startswith("//", i):
            j = src.find("\n", i)
            i = n if j < 0 else j
            continue
        if src.startswith(OPEN, i):
            depth += 1
            opens.append(line)
            i += 2
            continue
        if c == '"':
            j = i + 1
            while j < n:
                if src[j] == "\\":
                    j += 2
                    continue
                if src[j] == '"' or src[j] == "\n":
                    break
                j += 1
            i = j + 1
            continue
        if c == "'":
            j = i + 1
            while j < n:
                if src[j] == "\\":
                    j += 2
                    continue
                if src[j] == "'" or src[j] == "\n":
                    break
                j += 1
            i = j + 1
            continue
        i += 1
    return opens[0] if depth else None


def main():
    paths = sys.argv[1:] or ["app/src/main/java"]
    files = []
    for p in paths:
        if os.path.isfile(p):
            files.append(p)
        else:
            for r, _, fs in os.walk(p):
                files += [os.path.join(r, f) for f in fs
                          if f.endswith((".kt", ".java"))]
    bad = 0
    for f in sorted(files):
        ln = scan(f)
        if ln:
            print("  FAIL  %s 第 %d 行起的块注释没闭合（注释正文里出现了 %s ？）"
                  % (os.path.basename(f), ln, OPEN))
            bad += 1
    if bad == 0:
        print("  ok    %d 个文件的块注释都是平衡的" % len(files))
    return 1 if bad else 0


if __name__ == "__main__":
    sys.exit(main())
