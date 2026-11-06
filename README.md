# 答题卡识别 —— 回归基线

原始版本是一个 553 行的单文件 OpenCV 练习项目，靠 `imshow` 弹窗肉眼判断结果。
本目录为它加上**可重复、可断言**的测试外壳。

> 算法本身一行未改。这一步只解决"怎么知道改动是变好了还是弄坏了"。

## 快速开始

```powershell
# 1. 编译 CLI
.\scripts\build.ps1

# 2. 跑回归
.\scripts\run_regression.ps1
```

## 目录结构

```
Answer sheet/
├── Answer sheet.cpp          # 原始版本（保留，未改动）
├── Answer sheet.cli.cpp      # 新增：CLI 版本，算法与上面一致
├── tools/
│   ├── gen_samples.cpp       # 合成样本生成器
│   └── diag*.cpp             # 诊断工具（定位失败原因用）
├── data/
│   ├── BASELINE.md           # 基线说明 + 已暴露的缺陷
│   ├── samples/              # 样本 + 同名 .expected.json
│   └── debug/                # 中间产物（gitignore）
└── scripts/
    ├── build.ps1
    └── run_regression.ps1
```

## CLI 用法

```powershell
answersheet_cli.exe --image <path> [--out result.json] [--debug-dir dir] [--gui]
```

| 参数 | 说明 |
|---|---|
| `--image` | 输入图片（必填） |
| `--out` | 结果 JSON 输出路径 |
| `--debug-dir` | 输出校正图、二值图等中间结果 |
| `--gui` | 显示窗口（默认关闭，可无头批处理） |

退出码：`0` 成功识别，`2` 参数错误，`3` 图片读不了，`4` 未找到答题卡。

## 回归模式

```powershell
.\scripts\run_regression.ps1            # 期望模式：对照"应该是什么"
.\scripts\run_regression.ps1 -Baseline  # 基线模式：确认"行为没变"
```

**两种模式的区别很重要：**

- **期望模式**告诉你离目标还差多远（当前 4/7，3 个已知缺陷）
- **基线模式**防止你在重构时不知不觉改变行为（当前 7/7）

重构期间用 `-Baseline`，修 bug 后用期望模式验收。

## 已知缺陷

见 [data/BASELINE.md](data/BASELINE.md)。摘要：

1. **Canny 断点导致定位失败** —— `card_01_upright` 完全找不到卡，
   边框环四角有 1px 断点，补膨胀即恢复（6552 → 688992）
2. **空白题误判成 A/C** —— 只取最大填充，无绝对阈值、无置信度
3. **第 6 题误读** —— 同上，边界情况判错

这些都是**原算法就有的问题**，不是改造引入的。

## 硬件/依赖

- Visual Studio 2022 (v143)
- OpenCV 5.0.0，`E:\Opencv\opencv\build`（`scripts/build.ps1` 顶部可改）
