# KeyWeaver v1.1.1 End Stable

언어: [English](README.en.md) | [한국어](README.ko.md) | [日本語](README.ja.md) | [简体中文](README.zh-CN.md) | [繁體中文](README.zh-TW.md) | [Русский](README.ru.md)

KeyWeaver는 osu!mania `.osu` 차트와 BMS 계열 차트를 키 수 기준으로 변환하는 C++20/CMake 도구다. 4K-10K SP 계열 로컬 변환을 목표로 하며, CLI, Windows GUI, 배치 변환, 리포트 출력, 실험적 NK2 엔진을 포함한다.

## 이 릴리즈의 기준

`v1.1.1 End Stable`은 현재 프로젝트 상태를 마감하는 로컬 안정 패키지 라인이다. v1.1 기반 GUI를 유지하고, 현재 Source 선택/필터 동작과 CLI/core 변경을 반영한다. GitHub 업로드나 배포 자동화는 포함하지 않는다.

주요 기능:

- osu!mania `.osu` 입력/출력.
- 기본 BMS/BME/BML/PMS 입력/출력. BMS 계열 입력은 BMS 계열 출력으로만 저장한다.
- Source key 자동 감지와 수동 Source override.
- GUI Source 선택은 `auto`, `1`-`10`으로 고정.
- 배치에서 숫자 Source를 선택하면 해당 source-key 차트만 변환하고 나머지는 skip한다.
- GUI Target은 4K-10K.
- `--out` / `--out-dir`가 없으면 입력 차트 옆에 출력한다.
- `--batch`, `--input-list`, `--jobs` 기반 병렬 CLI 배치와 진행률/요약 출력.
- Classic 변환의 collision, LN, distance, jack 안전 검사.
- 실험적 NK2 모드: `native`, `faithful`, `harder`, `transform`. `report`는 단일 입력 분석 전용.
- GUI 10K 변환은 기본으로 Full-Field Mirror-Remix를 사용한다.
- 변환 품질, 정책 비교, 진단용 JSON/CSV 리포트.
- 선택적 CUDA ONNX Runtime 배치 lane-policy 훅.

포함하지 않는 것: 전체 차트 에디터, 오디오 재생, waveform 표시, DP 분리 변환, 실시간 BMS 플레이어 동작, 학습 실행, 배포/업로드 자동화.

## 릴리즈 패키지

실행만 하려면 Windows 릴리즈 zip을 사용한다.

```text
KeyWeaver-v1.1.1-win64-<timestamp>.zip
```

패키지 구성:

- `keyconv_gui.exe` - Windows GUI.
- `KeyWeaver.exe` - CLI 진입점. 같은 폴더에 `keyconv_gui.exe`가 있으면 더블클릭 시 GUI를 연다.
- `keyconv.exe` - CLI 별칭.
- MinGW 런타임 DLL.
- `samples/`, `profiles/`, `docs/`, `models/`, `scripts/`.
- package script가 생성한 `smoke/` 검증 로그.

## GUI 빠른 사용

1. 릴리즈 zip을 푼다.
2. `keyconv_gui.exe`를 실행한다.
3. GUI가 자동 감지하지 못하면 `KeyWeaver.exe`를 선택한다.
4. osu!mania/BMS 계열 차트를 선택하거나 드롭한다.
5. Source를 `auto` 또는 `1`-`10` 중 하나로 선택한다.
6. Target을 `4`-`10`으로 선택한다.
7. Classic 또는 NK2를 선택한다.
8. Convert 또는 Batch Folder를 누른다.

GUI 참고:

- Output이 비어 있으면 각 입력 차트 옆에 출력한다.
- 배치에서 숫자 Source를 선택하면 감지된 source key가 일치하는 차트만 변환한다.
- GUI Matrix는 Classic/NK1 전용이다.
- NK2 배치는 변환 모드에서 동작하지만 NK2 `report`는 단일 입력 전용이다.

## CLI 예시

단일 차트 dry run:

```powershell
.\KeyWeaver.exe samples\simple_4k.osu --source 4 --target 10 --dry-run
```

차트와 리포트 쓰기:

```powershell
.\KeyWeaver.exe samples\simple_7k_ln.osu --source 7 --target 10 --out dist\simple_7k_10k.osu --report dist\report.json
```

input-list 기반 빠른 배치:

```powershell
.\KeyWeaver.exe --batch --input-list charts.txt --source 7 --target 10 --engine nk2 --nk2-mode native --batch-quiet
```

혼합 source 배치 규칙:

```text
--source 7이면 7K로 감지된 차트만 변환하고 나머지는 skipped로 처리한다.
```

BMS 규칙:

```text
BMS 계열 입력은 .bms, .bme, .bml, .pms 계열 출력만 허용한다.
```

전체 옵션은 `KeyWeaver.exe --help`로 확인한다.

## 소스 빌드

기본 빌드:

```powershell
cmake -S . -B build -G Ninja
cmake --build build
ctest --test-dir build --output-on-failure
```

타깃 빌드:

```powershell
cmake --build build --target KeyWeaver keyconv keyconv_gui keyconv_tests keyconv_public_header_smoke
```

릴리즈 패키지:

```powershell
.\scripts\package_release.ps1 -Version 1.1.1
```

package script는 Release 빌드, unit test, public header smoke, GUI smoke, CLI dry-run smoke, 샘플 변환, reconversion guard, BMS guard를 실행하고 `dist/release/`에 zip과 `.sha256`을 만든다.

## 선택적 ONNX CUDA 배치 경로

기본 빌드는 ONNX Runtime을 요구하지 않는다. ONNX는 선택적이며 배치 전용이다.

수동 구성:

```powershell
cmake -S . -B build-onnx -G Ninja -DKEYWEAVER_WITH_ONNXRUNTIME=ON -DONNXRUNTIME_ROOT=<onnxruntime-package-root>
cmake --build build-onnx --target KeyWeaver
```

Windows helper:

```powershell
.\scripts\build_onnx_cuda.bat
```

ONNX 참고:

- 현재 소스 경로는 CUDA provider만 지원한다.
- 모델 출력은 advisory일 뿐이며 collision, LN, distance, no-created-jack 안전 검사가 최종 lane 수락을 결정한다.
- Target-10 배치는 번들된 `models/lane_policy_student_mlp_u_e_circusgalop.onnx`를 자동 로드할 수 있다.
- 결정론적 배치를 강제하려면 `--no-auto-onnx-policy`를 사용한다.
- 런타임 구성을 검증할 때는 `--onnx-policy-strict`를 사용한다.

## 소스 패키지

소스 패키지 이름은 다음 형식이다.

```text
KeyWeaver-v1.1.1-end-stable-source-<timestamp>.zip
```

프로젝트 소스, 문서, 모델, 스크립트, 릴리즈 diff 산출물을 포함한다. `.git`, 로컬 agent 지침, 빌드 폴더, 기존 릴리즈 zip은 제외한다.

## 주요 문서

- `docs/algorithm-lock-v0.6.0.md` - normal mode 변환 계약.
- `docs/algorithm-lock-v0.6.1.md` - 10K staged planner 계약.
- `docs/design-10k-fullfield-remix.md` - GUI 10K Full-Field Mirror-Remix 설계.
- `docs/nk2-algorithm.md` - 현재 NK2 알고리즘 설명.
- `docs/nk2-design.md` - 2세대 NK2 설계 노트.
- `docs/lane-policy-student.md` - ONNX lane-policy student 노트.
- `docs/code-architecture.md` - 코드 구조 개요.

## 제한과 주의

- BMS 지원은 MVP이며 모든 BMS 확장을 구현하지 않는다.
- DP 변환은 구현되어 있지 않다.
- Beam search는 예약 옵션이며 현재는 greedy로 fallback한다.
- 강한 key-count 압축은 정책에 따라 overflow note를 drop 또는 roll할 수 있다.
- 변환된 차트는 marker로 보호되어 실수로 재변환되지 않게 한다.
- NK2와 ONNX 경로는 실험적이다. 비교할 때는 포함된 smoke 출력과 report를 확인한다.

## 라이선스

`LICENSE`를 참고한다.
