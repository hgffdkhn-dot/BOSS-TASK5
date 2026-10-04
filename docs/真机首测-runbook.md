# BOSS · 真机首测 Runbook（第一次上机怎么走）

> 适用：**还没刷过修补镜像**，或刷了但 `/proc/1/exe` 不指向 BOSS。
> 目标只有一个：让 `ls -l /proc/1/exe` 指向 boss。
> 其余（bossd、协议、模块、挂载）都是它的下游，会一起亮。

---

## 0. 先判断：你现在是哪一种？

| 现象 | 说明 |
|---|---|
| App 能打开，但"init 接管未生效"、协议 v0、bossd 未运行 | **大概率是还没刷镜像**。这个状态是**预期的**，不是 bug |
| 刷过了，`/proc/1/exe` 仍不指向 boss | 劫持没生效，需要诊断 |

**App 能打开 ≠ BOSS 已安装。** App 是普通 APK，装了就能开；
它只是连不上 daemon（没人拉起 daemon），于是所有项都是空的。

要确认的话，先跑一次诊断（不需要 root）：

```bash
bash tools/device_diag.sh
```

看第 2 节：如果 `/boss`、`/init.boss.rc` 都"不存在"，就是**还没刷**。

---

## 1. 备份（**先做这个，别省**）

变砖不是开玩笑。只要原厂镜像还在，任何情况都能救回来。

```bash
# 从手机抽出当前在用的 init_boot / boot
adb pull /dev/block/by-name/init_boot_a ./orig_init_boot.img
adb pull /dev/block/by-name/boot_a      ./orig_boot.img

# 或者：进 fastboot 后从 recovery/线包里取（各机型不同）
```

⚠️ 抽出来的是**整个分区**（带尾部填充，可能 100MB+），这正是原厂备份最完整的形式。
配合 App 的 `--keep-trailing`，修补产物大小会接近它。

⚠️ **确认能进 fastboot**：
```bash
adb reboot bootloader && fastboot devices
```
如果这一步没输出，先解决驱动/解锁问题再往下走。

---

## 2. 准备 payload

payload 不是一个现成目录，必须组装（仓库里的 `payload/` 缺 `boss` 二进制）：

```bash
MAKE_PAYLOAD=1 bash build/build-ndk.sh
# → build/payload/ 与 dist/boss-payload.zip
```

⚠️ **别用 `make` 那份**：原生 make 出来的是 x86_64，注入 arm64 镜像后
`payload-check` 多半不报错，刷进手机直接 `Exec format error`——
**检查全过、开机才发现没有 BOSS**，是最坑的一种。

或者直接用 CI/release 里的 `boss-payload.zip`（里面已是 arm64）。

---

## 3. 修补

两条路，任选：

**A. App 修补页**（手机上）
1. 把 `boss-payload.zip` 传到手机并**解压成文件夹**
2. 修补页 → 选镜像（第 1 步的那个原厂 img）→ 选解压后的 payload 文件夹
3. 分区类型默认 `auto`，识别不出来再手选 `init_boot`
4. 点「修补」→「导出」到电脑

**B. PC 端 veritpath**
```bash
veritpath inject --init-boot orig_init_boot.img \
    -p build/payload -o out/ --keep-trailing
veritpath verify out/init_boot.veritpath.img -p build/payload
```

---

## 4. 刷入

```bash
adb reboot bootloader

# 关键：刷修补后的镜像
fastboot flash init_boot out/init_boot.veritpath.img

# dm-verity：修补后的镜像不再有 OEM 签名，多数设备需要同时关校验
#   —— 只 flash vbmeta，不要 flash vbmeta --disable-verity 之外的锁操作
fastboot --disable-verity --disable-verification flash vbmeta vbmeta.img

fastboot reboot
```

⚠️ **两个判断点**：
- 如果你的设备 ramdisk 在 `boot` 而不是 `init_boot`（老机型 / 非 2SI），
  刷 `boot` 分区：`fastboot flash boot out/boot.veritpath.img`
- `vbmeta.img` 从原厂线包里取；**没有就先只刷 init_boot 试一次**，
  很多设备只是开不了机提示"系统损坏"，按电源键仍能继续启动

---

## 5. 开机后：看这一个指标

```bash
adb shell ls -l /proc/1/exe
```

- **指向 boss** → 接管成功。装 App，后面都会亮
- **指向原厂 init** → 没生效，**不要反复重试**，先诊断：

```bash
bash tools/device_diag.sh
# 或手机 Root 终端里：
/boss init hijack-prep --dry      # 只打印打算做什么，不动系统
dmesg | grep -i boss
```

把输出贴回来定位断在哪一环。

---

## 6. 装 App（**必须放在第 5 步之后**）

```bash
adb install boss-app-debug.apk
```

⚠️ 顺序不能颠倒：manager 抢注的前提是 daemon 已跑起来，而 daemon 靠
init 接管拉起。`/proc/1/exe` 没指向 boss 时打开 App，表现是
"所有功能被拒绝"——真因是没人拉它起来。

---

## 7. 回退

```bash
adb reboot bootloader
fastboot flash init_boot orig_init_boot.img      # 第 1 步备份的原厂镜像
fastboot reboot
```

**所以第 1 步的备份不能省。** 没有它，一旦 dm-verity 或劫持出问题，
只能靠线包重刷整机（会丢数据）。

---

## 8. 每一步的"通过"标准

| 步骤 | 通过标准 | 不过怎么办 |
|---|---|---|
| 备份 | 两个 img 落到电脑，且 `fastboot devices` 有输出 | 先解决 fastboot |
| payload | `veritpath payload-check build/payload` 通过 | 多半是 boss 架构不对（用了 x86_64） |
| 修补 | 产出 img 且 `verify` 通过 | 看输出里的具体原因 |
| 刷入 | `fastboot flash` 报 OK | 分区名不对（boot vs init_boot） |
| 开机 | `ls -l /proc/1/exe` 指向 boss | **停在这里诊断**，别反复重试 |
| 装 App | 首页 bossd"运行中"、协议非 v0 | 看诊断第 6 节 |

⚠️ **一条纪律**：开机不成功时**不要连着反复刷**。
每次刷之前先想清楚这次改动了什么，否则几个变量叠在一起，
最后连"哪一次引入的问题"都不知道。
