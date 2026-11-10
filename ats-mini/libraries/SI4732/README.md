# SI4732 / SI4735 收音机库（加固分支）

面向 Silicon Labs **SI4732-A10** 与 **SI4735-D60** 的 Arduino 驱动库，AM / FM / SW / SSB。
接口与 PU2CLR SI4735 2.1.8 **完全一致**，本分支只修 bug，不加新功能，不改 API。

## 来源与许可

| 层级 | 仓库 | 说明 |
|---|---|---|
| 原始上游 | https://github.com/pu2clr/SI4735 | Ricardo Lima Caratti（PU2CLR）官方库，本分支基于 **2.1.8** |
| 中间来源 | **https://github.com/zhang-chong/SI4735** | 上游 2.1.8 的完整镜像，额外加 1 笔提交（搜索循环修复） |
| 本仓库 | https://github.com/openwrt-1983/SI4732 | 在中间来源基础上补齐 I²C 超时与搜索流程修复 |

- 版权归原作者 Ricardo Lima Caratti 所有，许可证见 [license.txt](license.txt)（GPL）。
- 上游英文文档保留在 [README-upstream.md](README-upstream.md)。
- 本仓库**未保留中间来源的 git 历史**，只同步了源码树；历史请到上面两个仓库查。
- 本仓库**不声明任何新的版权**，仅做代码修正。

## 与上游的差别

改动全部集中在 `src/SI4735.cpp` 与 `src/SI4735.h`，逐条说明见 **[FORK-NOTES.md](FORK-NOTES.md)**。摘要：

1. `waitToSend()` 加超时，并且不再把 `Wire.read()` 的 `-1` 当作 CTS 就绪
2. `getStatus()` 重试有上限，失败保留上次有效状态，不再死循环
3. `getFrequency()` 读取状态时不再带 `CANCEL=1`（原来会把正在跑的搜索掐断）
4. `seekNextStation()` / `seekPreviousStation()` 等搜索结束再读频率
5. `seekStationProgress()` 超时后主动中止搜索；总线失效时立刻返回
6. `getFirmware()` / `getAutomaticGainControl()` / `getRdsStatus()` / `getSsbAgcStatus()` / `queryLibraryId()` 同样改为有界重试
7. `getRdsDateTime()` 的 `mjd` 未初始化（会导致日期乱码）

## 安装

### Arduino IDE / arduino-cli（推荐：zip 安装）

Download ZIP 后，IDE 菜单 **草图 → 包含库 → 添加 .ZIP 库**。

### 手动放置

把整个目录放到 `~/Arduino/libraries/SI4732`（Windows：`文档\Arduino\libraries\SI4732`）后重启 IDE。

### 注意：与官方库冲突

本库的主头文件仍叫 `SI4735.h`，和 `PU2CLR SI4735` **不能同时安装**，否则 IDE 只会挑其中一个，编译行为不可预期。二选一。

## 验证

已用库自带示例 `examples/SI47XX_01_SERIAL_MONITOR/SI4735_01_POC` 在 `esp32:esp32:esp32s3`（arduino-esp32 3.3.12）上编译通过：

```
Sketch uses 332932 bytes (25%) of program storage space.
Global variables use 24464 bytes (7%) of dynamic memory.
```

编译无警告。**未做实机长时间测试**，只是在 SI4732-A10 设备上验证过基本收发与搜索。

## 使用提示

- 只适用于 I²C 接线的 SI4732 / SI4735；SSB 需要先 `loadPatch()` 下载补丁（补丁只存在芯片 RAM，断电即失效）。
- 需要判断"读不到芯片"时，用新增的 `rx.busTimedOut()`（fork 新增，官方库没有）。
- 若你的代码里 `getFrequency()` 依赖原来 `CANCEL=1` 的副作用（几乎不可能），请注意这一处语义变化。
