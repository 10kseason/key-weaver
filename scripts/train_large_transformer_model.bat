@echo off
setlocal

set "ROOT=%~dp0.."
pushd "%ROOT%" || exit /b 1

set "OUT_DIR=dist\transformer-large-u_e-circusgalop-632"
set "CHECKPOINT=%OUT_DIR%\large_transformer_lane_policy.pt"
set "REPORT=%OUT_DIR%\train_report.json"
set "ONNX=models\u_e_circusgalop_chart_dataset_lane_policy.onnx"
if not defined KEYWEAVER_SONGS_ROOT set "KEYWEAVER_SONGS_ROOT=%OSU_SONGS_ROOT%"
if not defined KEYWEAVER_SONGS_ROOT (
  echo ERROR: Set KEYWEAVER_SONGS_ROOT or OSU_SONGS_ROOT to your local osu! Songs folder.
  popd
  exit /b 1
)

python scripts\train_transformer_lane_policy.py ^
  --songs-root "%KEYWEAVER_SONGS_ROOT%" ^
  --author u_e ^
  --author CircusGalop ^
  --max-charts 632 ^
  --source-keys 7 ^
  --target-keys 10 ^
  --max-notes-per-chart 1200 ^
  --max-seq 384 ^
  --epochs 12 ^
  --learning-rate 0.0005 ^
  --non-direct-loss-weight 3.0 ^
  --transformer-weight 0.75 ^
  --direct-score-weight 0.85 ^
  --coverage-weight 0.75 ^
  --inserted-lane-bonus 0.0 ^
  --d-model 128 ^
  --nhead 8 ^
  --dim-feedforward 512 ^
  --num-layers 4 ^
  --dropout 0.05 ^
  --device auto ^
  --model-basename large_transformer_lane_policy.pt ^
  --output-dir "%OUT_DIR%"
if errorlevel 1 goto :fail

python scripts\export_transformer_lane_policy_onnx.py ^
  --checkpoint "%CHECKPOINT%" ^
  --train-report "%REPORT%" ^
  --output "%ONNX%" ^
  --verify
if errorlevel 1 goto :fail

echo OK: large Transformer ONNX model written to %ONNX%
popd
exit /b 0

:fail
echo ERROR: large Transformer model training/export failed.
popd
exit /b 1
