# 回归基线说明

> 本目录记录**改造前原始算法的真实行为**，作为后续修复的对照基准。
> 基线不是"正确结果"，而是"现状快照"。

## 背景

原始版本（`Answer sheet/Answer sheet.cpp`）只有一张样本 `slant_card.png`，
且依赖 `imshow` 肉眼判断，无法自动判定对错。为建立可重复的基线：

1. 把原算法原样搬到 CLI（`Answer sheet.cli.cpp`），算法一行未改
2. 用 `tools/gen_samples.cpp` 合成 5 张场景各异的答题卡
3. 记录每张图的实际输出，写进 `*.expected.json`

## 样本清单

| 样本 | 场景 | 期望 | 基线实测 |
|---|---|---|---|
| `slant_card.png` | 原项目自带，倾斜 + 木纹背景 | 正确识别 | ✅ `BADBCC` 3/6 |
| `card_01_upright.png` | 正对镜头，无旋转 | 正确识别 | ❌ `not_found` |
| `card_02_rotated.png` | 旋转 7° | 正确识别 | ✅ `ABCDAB` |
| `card_03_dark.png` | 旋转 -5°，深色填涂 | 正确识别 | ✅ `DABCAD` |
| `card_04_small.png` | 缩小到 55% | 正确识别 | ✅ `CCBBAA` |
| `card_05_blank_row.png` | 含空白题 | 见下 | ⚠️ `ACCAAD` |
| `no_card.png` | 无答题卡（熊猫图） | `not_found` | ✅ `not_found` |

## 基线暴露的两个真实缺陷

### 缺陷 1：`Canny(50,200)` 断点导致卡片定位失败

`card_01_upright` 明明是最简单的正视图，却完全找不到卡片。

根因（实测）：

```
raw canny      : maxArea=6552  n=95     ← 边框环在四角断开，不闭合
dilate 3x3     : maxArea=688992 n=4     ← 补 1px 断点后立刻闭合
close 5x5      : maxArea=685652 n=4
otsu binary    : maxArea=683145 n=1
```

`Answer sheet.cpp` L355 要求 `approx.size()==4 && contourArea>20000`，
而断开的边框环面积只有 6552，直接被过滤。

**为什么 `slant_card` 没暴露**：卡片略有倾斜，四角没有落在对齐的像素格上，
Canny 边缘恰好闭合（610938）。这是**运气**，不是设计。

修复方向：Canny 结果做形态学闭运算，或改用 Otsu 分割纸面。

### 缺陷 2：判分取"最大填充"会把空白题误判成 A

`card_05_blank_row` 期望 `[A, null, C, null, A, D]`，实测 `ACCAAD`：

- 第 2 题空白 → 判成 `C`
- 第 4 题空白 → 判成 `A`

`Answer sheet.cpp` L502-506 只按 `countNonZero` 最大值取索引，
**没有绝对阈值、没有置信度、也没有"多涂"检测**。空白题四个选项的
填充量都在噪声水平，谁大就选谁，结果完全随机。

修复方向：加填涂占比阈值 + 与次大值的 margin，低于阈值输出 `null`。

### 附带问题：第 6 题误读

`slant_card` 的二值图显示第 6 题 D 是实心填涂，但程序读成 `BADBCC`
（第 6 题判成 C）。同样是"取最大填充"在边界情况下的误判。

## 使用方式

```powershell
.\scripts\run_regression.ps1
```

脚本逐张跑 CLI，比对 `.expected.json`，输出 PASS/FAIL 与失败明细。

## 重要约定

> **基线记录的是现状，不是目标。**
> 修复缺陷后，需同步更新 `.expected.json` 里的基线值，
> 并把"期望"从当前的实际输出调整为目标输出。
> 届时 `expect` 字段从 `xfail`/`fail` 改为 `pass`。
