# KeyWeaver v1.2.0

语言: [English](README.en.md) | [한국어](README.ko.md) | [日本語](README.ja.md) | [简体中文](README.zh-CN.md) | [繁體中文](README.zh-TW.md) | [Русский](README.ru.md)

KeyWeaver 是一个基于 C++20/CMake 的键数转换工具，用于 osu!mania `.osu` 谱面和 BMS 系列谱面。它提供本地确定性转换、CLI、Windows GUI、批量/报告输出，以及支持到 18K 的升键、降键和同键转换的实验性 NK2 引擎。

## 本版本定位

`v1.2.0` 将实验性 NK2 扩展到 1K-18K 的全部 source/target 组合，包括降键转换和同键转换，并提供已验证的 Windows 包、完整源码包和 NK2 纯源码包。

主要能力:

- osu!mania `.osu` 输入/输出。
- 基础 BMS/BME/BML/PMS 输入/输出；BMS 系列输入只写出 BMS 系列输出。
- Source key 自动检测和手动 override。
- GUI Source 支持 `auto` 与 `1`-`18`。
- 批量模式中选择数字 Source 时，只转换匹配 source-key 的谱面，其余跳过。
- GUI Target 支持 4K-18K。NK2 CLI/core 支持 1K-18K 的全部 source/target 组合，包括从高键数转换到低键数。
- 未指定 `--out` / `--out-dir` 时，输出写在输入谱面旁边。
- 基于 `--batch`、`--input-list`、`--jobs` 的并行 CLI 批量转换，带进度和汇总输出。
- Classic 转换包含 collision、LN、distance、jack 安全检查。
- 实验性 NK2 模式: `native`、`faithful`、`harder`、`transform`；同键转换只在 `transform` 中执行，`report` 仅限单输入分析。
- GUI 10K 转换默认使用 Full-Field Mirror-Remix。
- 用于质量、策略比较、诊断的 JSON/CSV 报告。
- 可选的 CUDA ONNX Runtime 批量 lane-policy 钩子。

不包含: 完整谱面编辑器、音频播放、波形显示、DP 拆分转换、实时 BMS 播放器行为、训练运行、自动发布/上传。

## 发布包

只想运行工具时使用 Windows 发布 zip:

```text
KeyWeaver-v1.2.0-win64-<timestamp>.zip
```

包内包含:

- `keyconv_gui.exe` - Windows GUI。
- `KeyWeaver.exe` - CLI 入口。若同目录存在 `keyconv_gui.exe`，双击会打开 GUI。
- `keyconv.exe` - CLI 别名。
- MinGW 运行时 DLL。
- `samples/`、`profiles/`、`docs/`、`models/`、`scripts/`。
- package script 生成的 `smoke/` 验证日志。

## GUI 快速使用

1. 解压发布 zip。
2. 运行 `keyconv_gui.exe`。
3. 如果 GUI 没有自动检测到，手动选择 `KeyWeaver.exe`。
4. 选择或拖入 osu!mania/BMS 系列谱面。
5. Source 选择 `auto` 或 `1`-`18`。
6. Target 选择 `4`-`18`。
7. 选择 Classic 或 NK2。
8. 点击 Convert 或 Batch Folder。

GUI 说明:

- Output 为空时，每个输出写在对应输入谱面旁边。
- 批量模式选择数字 Source 时，只转换检测到的 source key 匹配的谱面。
- GUI Matrix 仅支持 Classic/NK1。
- NK2 批量可用于转换模式，但 NK2 `report` 仅支持单输入。

## CLI 示例

单谱面 dry run:

```powershell
.\KeyWeaver.exe samples\simple_4k.osu --source 4 --target 10 --dry-run
```

写出转换谱面和报告:

```powershell
.\KeyWeaver.exe samples\simple_7k_ln.osu --source 7 --target 10 --out dist\simple_7k_10k.osu --report dist\report.json
```

input-list 快速批量:

```powershell
.\KeyWeaver.exe --batch --input-list charts.txt --source 7 --target 10 --engine nk2 --nk2-mode native --batch-quiet
```

混合 source 批量规则:

```text
--source 7 只转换检测为 7K 的谱面，其他谱面计为 skipped。
```

BMS 规则:

```text
BMS 系列输入只允许写出 .bms、.bme、.bml、.pms 系列输出。
```

完整选项请运行 `KeyWeaver.exe --help`。

## 从源码构建

默认构建:

```powershell
cmake -S . -B build -G Ninja
cmake --build build
ctest --test-dir build --output-on-failure
```

常用目标构建:

```powershell
cmake --build build --target KeyWeaver keyconv keyconv_gui keyconv_tests keyconv_public_header_smoke
```

发布包:

```powershell
.\scripts\package_release.ps1 -Version 1.2.0
```

package script 会执行 Release 构建、unit test、public header smoke、GUI smoke、CLI dry-run smoke、样本转换、reconversion guard、BMS guard，并在 `dist/release/` 写出 zip 和 `.sha256`。

## 可选 ONNX CUDA 批量路径

默认构建不需要 ONNX Runtime。ONNX 是可选功能，并且仅用于批量模式。

手动配置:

```powershell
cmake -S . -B build-onnx -G Ninja -DKEYWEAVER_WITH_ONNXRUNTIME=ON -DONNXRUNTIME_ROOT=<onnxruntime-package-root>
cmake --build build-onnx --target KeyWeaver
```

Windows helper:

```powershell
.\scripts\build_onnx_cuda.bat
```

ONNX 说明:

- 当前源码路径只支持 CUDA provider。
- 模型输出只是 advisory；collision、LN、distance、no-created-jack 安全检查仍决定最终 lane。
- Target-10 批量可自动加载捆绑的 `models/lane_policy_student_mlp_u_e_circusgalop.onnx`。
- 使用 `--no-auto-onnx-policy` 可强制确定性批量行为。
- 验证运行时配置时使用 `--onnx-policy-strict`。

## 源码包

源码包名称格式:

```text
KeyWeaver-v1.2.0-source-<timestamp>.zip
```

包含项目源码、文档、模型、脚本和发布 diff 产物。故意排除 `.git`、本地 agent 指令、构建目录和旧发布 zip。

发布页还提供 KeyWeaver-NK2-source-1K-18K-v1.2.0-<timestamp>.zip，其中只包含 src/nk2/ 下的 8 个文件。

## 重要文档

- `docs/algorithm-lock-v0.6.0.md` - normal mode 转换契约。
- `docs/algorithm-lock-v0.6.1.md` - 10K staged planner 契约。
- `docs/design-10k-fullfield-remix.md` - GUI 10K Full-Field Mirror-Remix 设计。
- `docs/nk2-algorithm.md` - 当前 NK2 算法说明。
- `docs/nk2-design.md` - 第二代 NK2 设计笔记。
- `docs/lane-policy-student.md` - ONNX lane-policy student 笔记。
- `docs/code-architecture.md` - 代码架构概览。

## 限制和注意

- BMS 支持仍是 MVP。安全输出布局为 1K-10K、12K、14K、16K、18K；其他 BMS target 会报错，而不会静默丢弃轨道。
- 9K 与 18K 的 BMS 系列输出默认使用 `.pms`；强制按 18K 解析时会保留全部 18 个通道位置。
- 未实现 DP 转换。
- Beam search 是保留选项，目前 fallback 到 greedy。
- 强力键数压缩可能按策略 drop 或 roll overflow notes。
- 已转换谱面会带 marker，防止误重复转换。
- NK2 和 ONNX 路径仍是实验性功能；比较行为时请查看随包 smoke 输出和 report。

## 许可证

见 `LICENSE`。
