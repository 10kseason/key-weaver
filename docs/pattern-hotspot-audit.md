# NK2 E Pattern Hotspot Audit

The later opt-in [E+ full-field mode](nk2-e-fullfield.md) is also supported.
With `--nk2-e-fullfield`, this same read-only diagnostic observes the final
redistributed E+ chart and labels its envelope `preset: "E+"`. Without that
flag the E behavior and report bytes below remain unchanged. The following
no-algorithm-change boundary applies to the diagnostic itself.
The optional [E+ tap boost](nk2-e-tap-boost.md) is also observed after its
additions and final safety gate; that sidecar uses `preset: "E+ taps"` and
counts every final head, including the additional chord taps.

Status: included in v1.4.0. Original implementation base: KeyWeaver 1.3.1,
`2c5a035c3f6728f4f94748f90e6d5740fd5351d2`.

## 목적과 구현 범위

전체 레인 비율이 같아도 특정 시간대의 레인 사용이 다른 NK2 **E - Source LN**
8K/10K 결과를 구별하는 선택적 진단이다. 변환 결과를 고치거나 품질 점수에 반영하지 않는다.

질문은 “전체 분포가 놓치는 8초 구간 편중을 표시할 수 있는가?”다. 성공 기준은
같은 전체 분포의 합성 사례를 구별하고, 모든 구간 합계가 최종 E 이벤트 수와
일치하며, 진단 유무에 따른 채보·기존 리포트·안전 검사 결과가 같다는 것이다.
레인/이벤트가 유효하지 않거나 파서 경고로 모집단 완전성을 확인할 수 없으면
정상 분석을 보류한다. 변환 알고리즘 변경이 필요해지면 이 기능의 범위를 넘는다.

초기 독립 실행 파일 설계는 E에 구현하라는 후속 요청으로 대체했다. E의 최종
`Chart`를 CLI에서 직접 관찰하므로 기존 파서를 재사용하고, 저장된 BMS를 다시
읽을 때 생기는 빈 레인 압축과 시간 양자화 차이를 추가로 끌어들이지 않는다.
Classic/NK2 리포트 스키마, 공개 C++ 헤더, 변환 옵션/배치기/안전 코드는 그대로다.

## 확인한 기존 구조

- `include/keyconv/chart.hpp`, `note.hpp`: 정수 ms, 0-based lane, Tap/Hold,
  선택적 LN 종료 시각을 가진 정규화 이벤트 목록이다.
- `src/core/quality.cpp`: 전체 laneDistribution, entropy, coverage, 손 분포와
  2초/1초 간격의 겹치는 활성 레인 창 점수를 계산한다. 이 요약에서 8초 창의
  레인별 빈도를 복원할 수 없다.
- `src/nk2/nk2_report.*`: NK2의 outputNotes/laneDistribution 및 독립 안전 검사를
  별도로 출력한다. E는 C 배치를 보존하고 생성 LN 전체가 원본 LN 하나에
  포함되도록 검사한다. 생성 LN이 0개여도 정상 E 결과일 수 있다.
- `src/cli.cpp`: E를 결정한 뒤 변환하고, 기존 phrase 독립 안전 검사를 통과한
  최종 `nk2Result.chart`로 새 진단을 계산한다. 안전 실패 시 기존처럼 채보를
  저장하지 않으며, hotspot JSON도 만들지 않는다.

## 사용법

소스 빌드의 CLI에서만 제공한다. 기본값은 꺼짐이며 GUI/배치 옵션은 추가하지 않았다.

7K 입력의 기존 자동 E 선택을 사용하는 예:

```text
build/KeyWeaver.exe input7k.osu --engine nk2 --target 10 --dry-run --pattern-hotspot-report hotspot.json
```

E를 명시적으로 선택하는 예:

```text
build/KeyWeaver.exe input.osu --engine nk2 --target 10 --nk2-mode faithful --nk2-search phrase --nk2-phrase-support --nk2-phrase-dense-ln --nk2-phrase-source-ln --out result10k.osu --report conversion.json --pattern-hotspot-report hotspot.json
```

`hotspot.json`은 존재하지 않는 새 파일이어야 한다. `.json` 확장자를 요구하고
입력/채보 출력/기존 리포트와 같은 경로는 거부한다. 생성 시에도 exclusive-create를
사용하여 덮어쓰지 않는다. Windows 별칭을 생성 후 다시 확인하여 후속 리포트
쓰기가 진단 파일을 덮어쓰는 것을 막는다. 이 단계에서 실패하면 새 진단 파일은
남을 수 있지만 채보는 아직 쓰지 않는다. 불완전한 파일 쓰기도 오류로 보고한다.

진단 옵션은 단일 입력, target 8 또는 10, 실제로 선택된 E에서만 허용한다. E+와
E+ taps도 최종 출력 키수에 맞춰 분석한다. F boost,
Classic, 다른 타깃, 배치는 오류다. 이 옵션 자체가 프리셋을 E로 바꾸지는 않는다.
이미 변환된 파일을 입력해 재변환하는 기능도 아니다. 기존 재변환 방지 규칙을 유지한다.

`--dry-run`은 채보 저장을 생략하지만 명시적으로 요청한 진단 JSON은 쓴다.
진단은 **저장 전 최종 Chart**를 나타내므로, 뒤따르는 export/write의 성공 증명이
아니다. 실제 변환의 성공은 CLI 종료 결과와 채보 출력도 확인해야 한다.

## 집계 및 결정론 계약

- 0ms 기준 고정 구간 `[8000*k, 8000*(k+1))`. 시작 포함, 끝 제외이며 epsilon이나
  반올림을 적용하지 않는다. 음수 시각도 수학적 floor로 배정하고 int64로 계산한다.
- Tap과 Hold를 시작 시각에서 각각 1개 센다. LN 끝/유지 시간은 추가 이벤트가
  아니다. 동시치기는 각 노트, 생성 노트도 각 노트로 집계한다.
- 집계의 모집단은 **최종 E 결과**다. 변환 전 원본의 노트 수와 같아야 한다는 뜻이
  아니다. 기존 추가/삭제 정책과 회계는 그대로 둔다.
- 구간에 20개 미만이면 판정만 생략한다. 해당 노트는 전체 레인 분포와 분모,
  구간 합계에 모두 남는다.
- 노트 순서를 변경하거나 정렬/삭제/병합/재배치하지 않는다. 읽기 전용 함수와
  창 번호순 map으로 입력 순서와 무관하게 같은 카운트/JSON을 만든다.
- JSON은 locale과 필드/창/레인 순서를 고정한다. 시각 측정, 파일 경로, 곡 이름과
  원문 이벤트를 포함하지 않는다. 표시용 소수는 12자리 유효숫자이며 판정에는 쓰지 않는다.

```text
N = finalChart.notes.size()
G[l] = 전체 레인 l 노트 수
n[k] = 구간 k의 노트 수
C[k,l] = 구간 k의 레인 l 노트 수
deltaPp[k,l] = 100 * (C[k,l]/n[k] - G[l]/N)
hotspot[k] = n[k] >= 20 AND any_lane(abs(deltaPp[k,l]) >= 15)

sum(window.noteCount) = N = sum(globalLaneCounts)
모든 l: sum(window.laneCounts[l]) = G[l]
```

15% 상대 증감이 아닌 **15퍼센트포인트**이며 증가·감소 모두 포함한다. 전체 비율은
창별 비율의 단순 평균이 아니다. 정확히 15%p인 경계는 다음 checked uint64 비교로
판정한다. 잘못된 카운트 또는 오버플로를 정상적인 “편중 없음”으로 바꾸지 않는다.

```text
20 * abs(C[k,l]*N - G[l]*n[k]) >= 3*n[k]*N
```

## 입력과 상태

기존 `.osu` 및 BMS-family 파서가 정규화한 최종 E 결과를 분석한다. 새 CLI의 레인 수는
타깃인 10이다. 내부 모듈은 명시적 1–18 레인을 처리하며 `max(lane)+1`로 추측하지 않는다.
사용되지 않은 레인도 배열에 보존한다. BMS의 분석 시각은 export 이전 ms이므로
192분할로 저장된 파일을 다시 읽은 시각과는 다를 수 있다. 원래 BMS 파서의 지원 범위와
희소 원본 레인 추론 한계를 확장하거나 수정하지 않았다.

| 상태 | 의미 |
| --- | --- |
| `ok` | 20개 이상 창이 하나 이상 존재. hotspot이 0개일 수도 있음 |
| `no_eligible_windows` | 노트는 있지만 모든 창이 20개 미만 |
| `no_data` | 최종 노트가 0개. 전역 비율/분석 범위는 null |
| `invalid_input` | K/레인/타입/LN 오류, parser_warnings 또는 count_overflow |

invalid_input에서는 inputNotes만 입력량을 설명하고 totalNotes=0, 카운트·창 배열은
비어 있다. 파서가 생략/정규화한 이벤트가 있을 수 있으므로 최종 Chart에 경고가
하나라도 있으면 보수적으로 보류한다. 무해한 경고도 보류할 수 있으며, 경고 없이
정규화된 원문까지 완전 검증하는 기능은 아니다.

분석 불가능 상태는 별도 JSON과 콘솔에서 드러내되 **기존 변환의 판정/출력은 바꾸지
않는다**. 반면 잘못된 진단 옵션/경로 또는 진단 파일 쓰기 실패는 CLI 오류다.

별도 JSON 최상위는 `engine:"nk2"`, `preset:"E"`,
`analysisStage:"final_chart_before_export"`, `patternHotspots`다. 안쪽 객체는
schemaVersion=1, 고정 설정, 상태, inputNotes/totalNotes, 전역 카운트/비율,
analysisRangeMs, windows, emptyWindowRanges를 가진다.

각 창은 index/startMs/endMsExclusive, noteCount/laneCounts, status,
hotspot, laneRatios/deltaPp, flaggedLanes를 가진다. 레인 번호는 0-based다.
20개 미만 창은 `insufficient_notes`, hotspot/비율/편차=null이다.
빈 창은 `[beginIndex,endIndexExclusive)` 범위를 압축한 emptyWindowRanges에 기록한다.
범위는 0 또는 최초 창 중 이른 시작부터 마지막 노트 시작 창의 끝까지이며,
오디오 길이나 마지막 LN 끝으로 빈 꼬리를 만들지 않는다.

## 구현 위치와 유지할 경계

| 파일 | 책임 |
| --- | --- |
| `src/diagnostics/pattern_hotspot.hpp/.cpp` | private 결과 타입, 순수 집계와 독립 JSON |
| `src/cli.cpp` | 선택 옵션 검증, E 안전 검사 이후 연결, 새 파일 쓰기 |
| `tests/test_pattern_hotspot.cpp` | 수식·경계·결정론·입력 불변·E 생성 LN 검증 |
| `tests/test_pattern_hotspot_cli.cmake` | 실제 CLI·JSON·출력/경로·BMS·dry-run 회귀 |
| `CMakeLists.txt` | 기존 라이브러리에 진단 모듈 및 테스트 등록 |

기본 실행에서는 진단 함수를 호출하지 않는다. 새 의존성/공개 API는 없다.
N개 노트, B개 비어 있지 않은 창, K개 레인에 대해 시간 O(N log B + BK),
메모리 O(BK + K)이며 긴 무음의 빈 창 전체를 할당하지 않는다.

추가하지 않음: 자동 교정, 변환 점수 변경, Jack Guard/no-created-jack/LN 규칙 변경,
생성 노트만의 별도 점수, LN 유지시간 점유율, GUI, 배치, ONNX, 독립 채보 재분석 도구,
새 입력 형식, 업로드·게시.

## 검증과 알려진 한계

검증 상세는 [검증 기록](pattern-hotspot-audit-validation.md)을 참조한다.
전체 레인당 8개(10%)로 같은 두 80-note 합성 사례에서 균등 배치는 표시 0창,
집중 배치는 표시 4창이다. 구현의 집계/회귀 검증이며 품질 개선 실측은 아니다.

15%p 기준상 전체 10% 레인이 구간에서 0%가 되어도 -10%p라 표시되지 않는다.
한쪽 손의 5개 레인에 각 20%씩 몰려도 전체 10%와의 차이는 +10%p다. 채보 전체가
일관되게 편중되어도 구간과 전체 비율이 같으면 잡히지 않는다. 기존 전체 지표와
함께 해석해야 한다. 짧은 버스트, 창 경계에 나뉜 편중, 박자/잭/손 이동/LN 점유는
측정하지 않으며 20노트 조건은 통계적 유의성을 보장하지 않는다.

후속 플레이테스트는 같은 E 결과에서 표시/미표시 구간을 밀도·LN 비중이 비슷하게
골라 앞뒤 약 4초를 포함해 플레이한다. 처음에는 표시 여부를 가리고 레인/손 편중,
불편함, 의도된 반복 여부를 기록한 뒤 진단과 대조한다. 원본 잭/트릴/편손 LN과
생성 LN이 없는 구간도 포함해 오탐·미탐을 기록한다. 실제 플레이 품질이나 탐지율은
아직 측정하지 않았다.
