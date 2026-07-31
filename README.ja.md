# KeyWeaver v1.2.0

言語: [English](README.en.md) | [한국어](README.ko.md) | [日本語](README.ja.md) | [简体中文](README.zh-CN.md) | [繁體中文](README.zh-TW.md) | [Русский](README.ru.md)

KeyWeaver は、osu!mania `.osu` チャートと BMS 系チャートのキー数を変換する C++20/CMake 製ツールです。ローカルで決定論的に動作し、CLI、Windows GUI、バッチ/レポート出力、18K までの上方向・下方向・同一キー変換に対応する実験的 NK2 エンジンを含みます。

## このリリース

`v1.2.0` は、実験的な NK2 を 1K-18K の全 source/target 組み合わせ、降格変換、同一キー変換まで拡張し、検証済み Windows パッケージと全ソース・NK2 専用ソースの各アーカイブを提供します。

主な機能:

- osu!mania `.osu` の入力/出力。
- 基本的な BMS/BME/BML/PMS の入力/出力。BMS 系入力は BMS 系出力のまま保存します。
- Source key の自動検出と手動 override。
- GUI Source は `auto` と `1`-`18` に対応。
- バッチで数値 Source を選ぶと、一致する source-key のチャートだけを変換し、それ以外は skip します。
- GUI Target は 4K-18K。NK2 CLI/core は 1K-18K の全 source/target 組み合わせと高キーから低キーへの変換に対応します。
- `--out` / `--out-dir` を省略すると入力チャートの横に出力します。
- `--batch`、`--input-list`、`--jobs` による並列 CLI バッチ、進捗、サマリー出力。
- Classic 変換の collision、LN、distance、jack 安全チェック。
- 実験的 NK2 モード: `native`、`faithful`、`harder`、`transform`。同一キー変換は `transform` のみで実行され、`report` は単一入力の解析専用です。
- GUI の 10K 変換は既定で Full-Field Mirror-Remix を使います。
- 変換品質、ポリシー比較、診断用 JSON/CSV レポート。
- 任意の CUDA ONNX Runtime バッチ lane-policy フック。

含まれないもの: フルチャートエディタ、音声再生、波形表示、DP 分割変換、リアルタイム BMS プレイヤー動作、学習実行、配布/アップロード自動化。

## リリースパッケージ

実行だけなら Windows リリース zip を使います。

```text
KeyWeaver-v1.2.0-win64-<timestamp>.zip
```

中身:

- `keyconv_gui.exe` - Windows GUI。
- `KeyWeaver.exe` - CLI エントリ。`keyconv_gui.exe` が同じフォルダにある場合、ダブルクリックで GUI を開きます。
- `keyconv.exe` - CLI エイリアス。
- MinGW ランタイム DLL。
- `samples/`、`profiles/`、`docs/`、`models/`、`scripts/`。
- package script が生成した `smoke/` 検証ログ。

## GUI クイックスタート

1. リリース zip を展開します。
2. `keyconv_gui.exe` を起動します。
3. 自動検出されない場合は `KeyWeaver.exe` を選択します。
4. osu!mania/BMS 系チャートを選択またはドロップします。
5. Source を `auto` または `1`-`18` から選びます。
6. Target を `4`-`18` から選びます。
7. Classic または NK2 を選びます。
8. Convert または Batch Folder を押します。

GUI メモ:

- Output が空なら各入力チャートの横に出力します。
- バッチで数値 Source を選ぶと、検出された source key が一致するチャートだけを変換します。
- GUI Matrix は Classic/NK1 専用です。
- NK2 バッチは変換モードで動作しますが、NK2 `report` は単一入力専用です。

## CLI 例

単一チャート dry run:

```powershell
.\KeyWeaver.exe samples\simple_4k.osu --source 4 --target 10 --dry-run
```

変換結果とレポートを書き出す:

```powershell
.\KeyWeaver.exe samples\simple_7k_ln.osu --source 7 --target 10 --out dist\simple_7k_10k.osu --report dist\report.json
```

input-list を使った高速バッチ:

```powershell
.\KeyWeaver.exe --batch --input-list charts.txt --source 7 --target 10 --engine nk2 --nk2-mode native --batch-quiet
```

混在 source バッチ:

```text
--source 7 は、7K と検出されたチャートだけを変換し、それ以外を skipped にします。
```

BMS ルール:

```text
BMS 系入力は .bms、.bme、.bml、.pms 系出力だけを許可します。
```

全オプションは `KeyWeaver.exe --help` を確認してください。

## ソースからビルド

基本ビルド:

```powershell
cmake -S . -B build -G Ninja
cmake --build build
ctest --test-dir build --output-on-failure
```

対象を絞ったビルド:

```powershell
cmake --build build --target KeyWeaver keyconv keyconv_gui keyconv_tests keyconv_public_header_smoke
```

リリースパッケージ:

```powershell
.\scripts\package_release.ps1 -Version 1.2.0
```

package script は Release ビルド、unit test、public header smoke、GUI smoke、CLI dry-run smoke、サンプル変換、再変換ガード、BMS ガードを実行し、`dist/release/` に zip と `.sha256` を作成します。

## 任意の ONNX CUDA バッチ経路

既定ビルドは ONNX Runtime を必要としません。ONNX は任意で、バッチ専用です。

手動構成:

```powershell
cmake -S . -B build-onnx -G Ninja -DKEYWEAVER_WITH_ONNXRUNTIME=ON -DONNXRUNTIME_ROOT=<onnxruntime-package-root>
cmake --build build-onnx --target KeyWeaver
```

Windows helper:

```powershell
.\scripts\build_onnx_cuda.bat
```

ONNX メモ:

- 現在のソース経路は CUDA provider のみ対応します。
- モデル出力は advisory です。collision、LN、distance、no-created-jack の安全チェックが最終 lane を決定します。
- Target-10 バッチは、同梱された `models/lane_policy_student_mlp_u_e_circusgalop.onnx` を自動ロードできます。
- 決定論的バッチを強制するには `--no-auto-onnx-policy` を使います。
- ランタイム構成を検証する場合は `--onnx-policy-strict` を使います。

## ソースパッケージ

ソースパッケージ名:

```text
KeyWeaver-v1.2.0-source-<timestamp>.zip
```

プロジェクトソース、文書、モデル、スクリプト、リリース diff 成果物を含みます。`.git`、ローカル agent 指示、ビルドフォルダ、古いリリース zip は除外します。

リリースには、src/nk2/ 配下の 8 ファイルだけを収録した KeyWeaver-NK2-source-1K-18K-v1.2.0-<timestamp>.zip も含まれます。

## 重要文書

- `docs/algorithm-lock-v0.6.0.md` - normal mode の変換契約。
- `docs/algorithm-lock-v0.6.1.md` - 10K staged planner 契約。
- `docs/design-10k-fullfield-remix.md` - GUI 10K Full-Field Mirror-Remix 設計。
- `docs/nk2-algorithm.md` - 現在の NK2 アルゴリズム説明。
- `docs/nk2-design.md` - 第二世代 NK2 設計ノート。
- `docs/lane-policy-student.md` - ONNX lane-policy student ノート。
- `docs/code-architecture.md` - コード構造の概要。

## 制限と注意

- BMS 対応は MVP で、すべての BMS 拡張を実装しているわけではありません。安全な出力レイアウトは 1K-10K、12K、14K、16K、18K で、それ以外はレーンを黙って破棄せずエラーにします。
- 9K と 18K の BMS 系出力は既定で `.pms` を使い、18K 強制解析では 18 個のチャンネル位置をすべて保持します。
- DP 変換は未実装です。
- Beam search は予約オプションで、現在は greedy に fallback します。
- 強いキー数圧縮では、選択した compression policy により overflow note を drop または roll することがあります。
- 変換済みチャートは marker で保護され、誤って再変換されにくくなっています。
- NK2 と ONNX 経路は実験的です。比較時は同梱の smoke 出力と report を確認してください。

## ライセンス

`LICENSE` を参照してください。
