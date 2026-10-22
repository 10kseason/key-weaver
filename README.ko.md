# KeyWeaver v1.3.0

언어: [English](README.en.md) | [한국어](README.ko.md) | [日本語](README.ja.md) | [简体中文](README.zh-CN.md) | [繁體中文](README.zh-TW.md) | [Русский](README.ru.md)

KeyWeaver는 osu!mania `.osu` 차트와 BMS 계열 차트를 키 수 기준으로 변환하는 C++20/CMake 도구다. 로컬 결정론적 변환, Windows GUI, 배치/리포트 출력, 18K까지 상향·하향·동일 키 변환을 처리하는 실험적 NK2 엔진을 포함한다.

## 이 릴리즈의 기준

`v1.3.0`은 GUI의 A–F 선택지를 정리하고, 7K→4K/5K에는 B 탐색, 7K→8K/10K에는 E Source LN을 기본 채택하며 독립 안전성 검사를 강화한다. 4K→5K/7K 수동 실험은 선택형으로 유지한다. Windows 패키지, 전체 프로젝트 소스, 10개 파일로 구성된 NK2 전용 소스 압축 파일을 제공한다.

주요 기능:

- osu!mania `.osu` 입력/출력.
- 기본 BMS/BME/BML/PMS 입력/출력. BMS 계열 입력은 BMS 계열 출력으로만 저장한다.
- Source key 자동 감지와 수동 Source override.
- GUI Source 선택은 `auto`, `1`-`18`을 지원.
- 배치에서 숫자 Source를 선택하면 해당 source-key 차트만 변환하고 나머지는 skip한다.
- GUI Target은 4K-18K. NK2 CLI/core는 1K-18K의 모든 source/target 조합과 높은 키에서 낮은 키로의 변환을 지원한다.
- `--out` / `--out-dir`가 없으면 입력 차트 옆에 출력한다.
- `--batch`, `--input-list`, `--jobs` 기반 병렬 CLI 배치와 진행률/요약 출력.
- Classic 변환의 collision, LN, distance, jack 안전 검사.
- 실험적 NK2 모드: `native`, `faithful`, `harder`, `transform`. 동일 키 변환은 `transform`에서만 실행하며, `report`는 단일 입력 분석 전용이다.
- `--engine nk2`에서 모드·탐색·노트 추가 옵션을 명시하지 않으면 원본 7K의 4/5K 변환은 B 탐색, 8/10K 변환은 E Source LN을 기본 채택한다. 다른 조합은 CLI의 기존 native 기본을 유지한다. GUI 기본은 `Auto (7K presets)`이며 `--nk2-mode auto`로 전달하고, 그 밖의 키 조합은 기존 GUI처럼 faithful로 처리한다. 명시한 모드·탐색·추가 옵션과 stream 변환은 자동 채택보다 우선한다. 전체 기본 엔진 Classic과 코어 `Mode` 기본값은 그대로다.
- `--nk2-search phrase`는 faithful 7K→4K/5K/8K/10K와 선택형 4K→5K/7K에서 두 박 구간의 배치 후보를 비교한다. `--nk2-mode faithful --nk2-search off`로 기존 faithful 경로를 명시하고 `--nk2-audit`로 독립 검사할 수 있다. [구간 탐색·OSZ 비교 안내](docs/nk2-phrase-search.md).
- `--nk2-phrase-support`는 7K→8K/10K 또는 선택형 4K→5K/7K 탐색 결과의 원본 배치를 유지하면서 보조 탭을 추가한다. 패키저의 `--comparison phrase-notes`로 B Phrase와 C Phrase Notes의 10K를 비교할 수 있다.
- 여기에 `--nk2-phrase-dense-ln`을 켜면 C를 유지한 채 탭과 롱놋을 더 추가한다. `--comparison phrase-dense-ln` 패키지는 원본 7K와 B/C/D 10K를 담는다.
- D 옵션과 함께 `--nk2-phrase-source-ln`을 켜면 E를 만든다. 생성 LN 전체가 원본 LN 하나의 구간 안에 있도록 제한하면서 길이·레인 선호를 다양화한다. 기존 D는 유지하며 `--comparison phrase-source-ln`으로 원본 7K와 C/D/E 10K를 비교한다.
- E에 `--nk2-phrase-source-ln-boost`를 더하면 E의 모든 노트를 유지하면서 같은 원본 LN 제한 아래 LN만 더하는 F를 만든다. `--comparison phrase-source-ln-boost`는 원본 7K+D/E/F 10K, `--comparison phrase-key-tests`는 원본 7K+A/B 5K+D/E/F 8K를 담는다. 기존 D/E 10K 결과는 유지한다.
- 4K→5K/7K는 B/C/D/E 수동 실험이며 B에는 기존 4K→5K fill을 적용하지 않아 추가 노트가 0개다. F는 7K→8K/10K 전용이다. `--comparison phrase-four-key-tests`는 원본 4K와 B/E 5K·7K 총 5난이도, `--comparison adopted-repair`는 원본 7K와 A Fixed 5K, B Adopted 4K·5K, E Adopted 8K·10K 총 6난이도를 담는다. 기존 비교판도 유지한다.
- 기존 배치의 원본 연타 예외가 모든 레인의 연타 금지 마스크를 해제하던 문제를 수정했다. 이제 실제 같은 원본 레인의 직전 배치 레인에만 예외를 허용한다.
- Classic GUI의 10K 변환은 기본으로 Full-Field Mirror-Remix를 사용한다.
- 변환 품질, 정책 비교, 진단용 JSON/CSV 리포트.
- 선택적 CUDA ONNX Runtime 배치 lane-policy 훅.

포함하지 않는 것: 전체 차트 에디터, 오디오 재생, waveform 표시, DP 분리 변환, 실시간 BMS 플레이어 동작, 학습 실행, 배포/업로드 자동화.

## 릴리즈 패키지

실행만 하려면 Windows 릴리즈 zip을 사용한다.

```text
KeyWeaver-v1.3.0-win64-<timestamp>.zip
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
5. Source를 `auto` 또는 `1`-`18` 중 하나로 선택한다.
6. Target을 `4`-`18`로 선택한다.
7. Classic 또는 NK2를 선택한다.
8. Convert 또는 Batch Folder를 누른다.

GUI 참고:

- Output이 비어 있으면 각 입력 차트 옆에 출력한다.
- 배치에서 숫자 Source를 선택하면 감지된 source key가 일치하는 차트만 변환한다.
- GUI Matrix는 Classic/NK1 전용이다.
- NK2 배치는 변환 모드에서 동작하지만 NK2 `report`는 단일 입력 전용이다.

NK2 GUI 선택지:

| 선택지 | 동작 |
| --- | --- |
| `Auto (7K presets)` | 원본 7K의 4K/5K는 B, 8K/10K는 E. 나머지는 faithful. |
| `A - Legacy faithful` | 기존 faithful 변환. |
| `B - Phrase only` | 노트 추가 없는 구간 탐색. |
| `C - Phrase + taps` | B를 보존하고 안전한 보조 탭 추가. |
| `D - Dense taps + LN` | C를 보존하고 탭과 LN을 더 추가. |
| `E - Source LN` | C를 보존하며 생성 LN 전체를 원본 LN 안으로 제한. |
| `F - Source LN+` | E를 보존하고 원본 LN 안에 LN만 더 추가. |

`native`, `harder`, `transform`도 유지한다. Auto는 혼합 키 배치에서도 입력별로
결정한다. 4K→5K/7K 수동 실험은 B/C/D/E를 지원하고 F는 7K→8K/10K 전용이다.
잘못된 수동 조합은 오류로 거부한다.

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
.\scripts\package_release.ps1 -Version 1.3.0
```

package script는 Release 빌드, 등록된 전체 CTest(NK2 안전성·구간 탐색·CLI 회귀 포함), public header smoke, GUI smoke, CLI dry-run smoke, 샘플 변환, reconversion guard, BMS guard를 실행하고 `dist/release/`에 압축 파일과 `.sha256`을 만든다. 자동 검사는 구조적 안전성과 결정성을 확인하며, 실제 플레이 감각은 직접 비교해야 한다.

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
KeyWeaver-v1.3.0-source-<timestamp>.zip
```

명시적으로 선택한 추적 프로젝트 파일과 최신 NK2 소스·테스트·문서를 포함한다. 개인 음원·학습 실험, 로컬 MP3·채보·OSZ, `AGENTS.md` 등 로컬 agent 지침, `.git`, 빌드 폴더, 기존 릴리즈 압축 파일은 제외한다. 작업 폴더의 diff 산출물도 포함하지 않는다.

릴리즈에는 독립 안전성 검사기를 포함한 `src/nk2/`의 10개 파일을 담은 `KeyWeaver-NK2-source-1K-18K-v1.3.0-<timestamp>.zip`도 제공한다. 검토·통합용 소스이며 애플리케이션 빌드에는 전체 소스 압축 파일을 사용한다.

## 주요 문서

- `docs/algorithm-lock-v0.6.0.md` - normal mode 변환 계약.
- `docs/algorithm-lock-v0.6.1.md` - 10K staged planner 계약.
- `docs/design-10k-fullfield-remix.md` - GUI 10K Full-Field Mirror-Remix 설계.
- `docs/nk2-algorithm.md` - 현재 NK2 알고리즘 설명.
- `docs/nk2-phrase-search.md` - 기본·수동 구간 탐색 선택지, 독립 안전성 검사, OSZ 비교 패키지.
- `docs/nk2-design.md` - 2세대 NK2 설계 노트.
- `docs/lane-policy-student.md` - ONNX lane-policy student 노트.
- `docs/code-architecture.md` - 코드 구조 개요.

## 제한과 주의

- BMS 지원은 MVP이며 모든 BMS 확장을 구현하지 않는다. 안전한 출력 레이아웃은 1K-10K, 12K, 14K, 16K, 18K이며, 그 외 BMS 타깃은 레인을 조용히 버리지 않고 오류로 거부한다.
- 9K와 18K BMS 계열 출력은 기본 확장자로 `.pms`를 사용하며, 18K 강제 파싱은 18개 채널 위치를 모두 보존한다.
- 18개 채널을 모두 쓰는 18K 차트는 자동 감지에서도 레인 위치를 보존한다. 빈 레인이 있는 차트는 `--source 18`을 사용한다.
- NK2 병렬 배치는 공통 `--out-dir`에 같은 이름의 입력을 저장할 때 고유 접미사를 붙여 결과를 모두 보존한다.
- NK2 초과 노트의 시간 이동은 앞뒤에 배치된 노트와의 새 연타를 모두 검사한다. 안전한 자리가 없는 탭은 faithful 모드에서도 제외될 수 있다.
- DP 변환은 구현되어 있지 않다.
- Beam search는 예약 옵션이며 현재는 greedy로 fallback한다.
- 강한 key-count 압축은 정책에 따라 overflow note를 drop 또는 roll할 수 있다.
- 변환된 차트는 marker로 보호되어 실수로 재변환되지 않게 한다.
- NK2와 ONNX 경로는 실험적이다. 비교할 때는 포함된 smoke 출력과 report를 확인한다.

## 라이선스

`LICENSE`를 참고한다.
