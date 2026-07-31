# KeyWeaver v1.2.0

語言: [English](README.en.md) | [한국어](README.ko.md) | [日本語](README.ja.md) | [简体中文](README.zh-CN.md) | [繁體中文](README.zh-TW.md) | [Русский](README.ru.md)

KeyWeaver 是一個以 C++20/CMake 製作的鍵數轉換工具，用於 osu!mania `.osu` 譜面與 BMS 系列譜面。它提供本機確定性轉換、CLI、Windows GUI、批次/報告輸出，以及支援到 18K 的升鍵、降鍵與同鍵轉換的實驗性 NK2 引擎。

## 本版本定位

`v1.2.0` 將實驗性 NK2 擴充至 1K-18K 的全部 source/target 組合，包括降鍵轉換與同鍵轉換，並提供已驗證的 Windows 套件、完整原始碼包與 NK2 純原始碼包。

主要能力:

- osu!mania `.osu` 輸入/輸出。
- 基本 BMS/BME/BML/PMS 輸入/輸出；BMS 系列輸入只會寫出 BMS 系列輸出。
- Source key 自動偵測與手動 override。
- GUI Source 支援 `auto` 與 `1`-`18`。
- 批次模式中選擇數字 Source 時，只轉換相符 source-key 的譜面，其餘略過。
- GUI Target 支援 4K-18K。NK2 CLI/core 支援 1K-18K 的全部 source/target 組合，包括從高鍵數轉換到低鍵數。
- 未指定 `--out` / `--out-dir` 時，輸出寫在輸入譜面旁。
- 以 `--batch`、`--input-list`、`--jobs` 進行平行 CLI 批次轉換，並輸出進度與摘要。
- Classic 轉換包含 collision、LN、distance、jack 安全檢查。
- 實驗性 NK2 模式: `native`、`faithful`、`harder`、`transform`；同鍵轉換只在 `transform` 中執行，`report` 僅限單一輸入分析。
- GUI 10K 轉換預設使用 Full-Field Mirror-Remix。
- 用於品質、策略比較、診斷的 JSON/CSV 報告。
- 可選的 CUDA ONNX Runtime 批次 lane-policy hook。

不包含: 完整譜面編輯器、音訊播放、波形檢視、DP 分割轉換、即時 BMS 播放器行為、訓練執行、自動發布/上傳。

## 發布套件

只想執行工具時請使用 Windows 發布 zip:

```text
KeyWeaver-v1.2.0-win64-<timestamp>.zip
```

套件內容:

- `keyconv_gui.exe` - Windows GUI。
- `KeyWeaver.exe` - CLI 入口。若同資料夾有 `keyconv_gui.exe`，雙擊會開啟 GUI。
- `keyconv.exe` - CLI 別名。
- MinGW runtime DLL。
- `samples/`、`profiles/`、`docs/`、`models/`、`scripts/`。
- package script 產生的 `smoke/` 驗證記錄。

## GUI 快速使用

1. 解壓縮發布 zip。
2. 執行 `keyconv_gui.exe`。
3. 若 GUI 未自動偵測，手動選擇 `KeyWeaver.exe`。
4. 選擇或拖入 osu!mania/BMS 系列譜面。
5. Source 選擇 `auto` 或 `1`-`18`。
6. Target 選擇 `4`-`18`。
7. 選擇 Classic 或 NK2。
8. 按 Convert 或 Batch Folder。

GUI 說明:

- Output 為空時，每個輸出會寫在對應輸入譜面旁。
- 批次模式選擇數字 Source 時，只轉換偵測到的 source key 相符的譜面。
- GUI Matrix 僅支援 Classic/NK1。
- NK2 批次可用於轉換模式，但 NK2 `report` 僅支援單一輸入。

## CLI 範例

單譜面 dry run:

```powershell
.\KeyWeaver.exe samples\simple_4k.osu --source 4 --target 10 --dry-run
```

寫出轉換譜面與報告:

```powershell
.\KeyWeaver.exe samples\simple_7k_ln.osu --source 7 --target 10 --out dist\simple_7k_10k.osu --report dist\report.json
```

input-list 快速批次:

```powershell
.\KeyWeaver.exe --batch --input-list charts.txt --source 7 --target 10 --engine nk2 --nk2-mode native --batch-quiet
```

混合 source 批次規則:

```text
--source 7 只轉換偵測為 7K 的譜面，其他譜面會計為 skipped。
```

BMS 規則:

```text
BMS 系列輸入只允許寫出 .bms、.bme、.bml、.pms 系列輸出。
```

完整選項請執行 `KeyWeaver.exe --help`。

## 從原始碼建置

預設建置:

```powershell
cmake -S . -B build -G Ninja
cmake --build build
ctest --test-dir build --output-on-failure
```

常用目標建置:

```powershell
cmake --build build --target KeyWeaver keyconv keyconv_gui keyconv_tests keyconv_public_header_smoke
```

發布套件:

```powershell
.\scripts\package_release.ps1 -Version 1.2.0
```

package script 會執行 Release 建置、unit test、public header smoke、GUI smoke、CLI dry-run smoke、樣本轉換、reconversion guard、BMS guard，並在 `dist/release/` 寫出 zip 與 `.sha256`。

## 可選 ONNX CUDA 批次路徑

預設建置不需要 ONNX Runtime。ONNX 是可選功能，且僅用於批次模式。

手動設定:

```powershell
cmake -S . -B build-onnx -G Ninja -DKEYWEAVER_WITH_ONNXRUNTIME=ON -DONNXRUNTIME_ROOT=<onnxruntime-package-root>
cmake --build build-onnx --target KeyWeaver
```

Windows helper:

```powershell
.\scripts\build_onnx_cuda.bat
```

ONNX 說明:

- 目前原始碼路徑只支援 CUDA provider。
- 模型輸出只是 advisory；collision、LN、distance、no-created-jack 安全檢查仍決定最終 lane。
- Target-10 批次可自動載入隨附的 `models/lane_policy_student_mlp_u_e_circusgalop.onnx`。
- 使用 `--no-auto-onnx-policy` 可強制確定性批次行為。
- 驗證 runtime 設定時使用 `--onnx-policy-strict`。

## 原始碼套件

原始碼套件名稱格式:

```text
KeyWeaver-v1.2.0-source-<timestamp>.zip
```

包含專案原始碼、文件、模型、腳本與發布 diff 產物。刻意排除 `.git`、本機 agent 指示、建置目錄與舊發布 zip。

發佈頁另提供 KeyWeaver-NK2-source-1K-18K-v1.2.0-<timestamp>.zip，其中只包含 src/nk2/ 下的 8 個檔案。

## 重要文件

- `docs/algorithm-lock-v0.6.0.md` - normal mode 轉換契約。
- `docs/algorithm-lock-v0.6.1.md` - 10K staged planner 契約。
- `docs/design-10k-fullfield-remix.md` - GUI 10K Full-Field Mirror-Remix 設計。
- `docs/nk2-algorithm.md` - 目前 NK2 演算法說明。
- `docs/nk2-design.md` - 第二代 NK2 設計筆記。
- `docs/lane-policy-student.md` - ONNX lane-policy student 筆記。
- `docs/code-architecture.md` - 程式碼架構概要。

## 限制與注意

- BMS 支援仍是 MVP。安全輸出配置為 1K-10K、12K、14K、16K、18K；其他 BMS target 會回報錯誤，而不會靜默捨棄軌道。
- 9K 與 18K 的 BMS 系列輸出預設使用 `.pms`；強制以 18K 解析時會保留全部 18 個通道位置。
- 未實作 DP 轉換。
- Beam search 是保留選項，目前 fallback 到 greedy。
- 強力鍵數壓縮可能依策略 drop 或 roll overflow notes。
- 已轉換譜面會帶 marker，以避免誤重複轉換。
- NK2 與 ONNX 路徑仍是實驗性功能；比較行為時請查看套件內 smoke 輸出與 report。

## 授權

見 `LICENSE`。
