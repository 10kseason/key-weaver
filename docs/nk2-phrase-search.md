# NK2 구간 탐색과 실제 채보 비교

NK2에서 원본 7K의 **4K/5K는 B 탐색, 8K/10K는 E Source LN**을 기본 채택한다.
CLI의 `--engine nk2`에서 모드·탐색·C/D/E/F 옵션을 명시하지 않았을 때 적용하며,
각 파일의 원본 키를 파싱한 뒤 결정한다. 다른 조합은 기존 CLI native로 처리한다.
전체 기본 엔진 Classic과 코어 `NK2Options::mode = Mode::Native`는 변경하지 않는다.

GUI 기본은 `Auto (7K presets)`이며 CLI에 `--nk2-mode auto`를 전달한다.
이 모드는 같은 7K 기본값을 선택하고 그 밖의 키는 기존 GUI처럼 faithful로 처리한다.
명시한 모드·탐색·C/D/E/F 옵션 또는 stream 변환이 있으면 자동 채택을 하지 않는다.
예를 들어 `--nk2-mode faithful --nk2-search phrase`는 B를 유지하며 E로 바뀌지 않는다.
`--nk2-mode auto --nk2-search off`는 기존 faithful 경로다. 수동 B/C/D/E/F 선택과
기존 비교판은 유지한다.

GUI의 수동 선택지는 `A - Legacy faithful`, `B - Phrase only`,
`C - Phrase + taps`, `D - Dense taps + LN`, `E - Source LN`, `F - Source LN+`다.
A는 기존 faithful 경로이며 `native`, `harder`, `transform`도 별도로 유지한다.
A–F 모두 **1K–18K의 모든 원본/목표 조합에서 상향·하향 변환**을 지원한다.
같은 키 수의 A–F는 기존처럼 no-op이며, 같은 키 재배치는 `transform`을 사용한다.
BMS 계열 출력은 기존에 지원하는 키 레이아웃만 허용한다.

## 사용

```powershell
# 채택 기본값: source가 7K이면 각각 B 5K, E 10K
.\build\KeyWeaver.exe input7k.osu --target 5 --engine nk2 --out adopted5.osu
.\build\KeyWeaver.exe input7k.osu --target 10 --engine nk2 --out adopted10.osu

# 기존 배치 + 독립 검사: 검사 결과를 보고하며 차트 배치는 바꾸지 않는다.
.\build\KeyWeaver.exe input7k.osu --target 4 --engine nk2 --nk2-mode faithful --nk2-audit --out legacy4.osu --report legacy4.json

# 구간 탐색: 독립 검사를 자동 실행하며 실패한 차트는 저장하지 않는다.
.\build\KeyWeaver.exe input7k.osu --target 4 --engine nk2 --nk2-mode faithful --nk2-search phrase --out phrase4.osu --report phrase4.json
```

수동 phrase는 **1K–18K의 모든 원본/목표 조합, faithful, stream off**를 지원한다.
source는 자동 감지하거나 `--source 1`부터 `--source 18`까지 지정할 수 있다. GUI에서 B를 직접 고르려면 `B - Phrase only`를 선택한다.
수동 탐색판에는 `-phrase`가 붙는다. GUI auto의 파일명은 `NK2-auto`로 구분하며
차트 내부 난이도 표시는 실제로 선택된 B/E 옵션을 따른다. 혼합 키 배치의 auto는
파일마다 적용 여부를 결정한다. 수동 phrase도 source를 자동 감지하는 혼합 키 배치를
지원하며, 특정 원본 키만 변환하려면 source 필터를 지정한다.

## C: 상향·하향 탐색 결과에 탭 추가하기

```powershell
.\build\KeyWeaver.exe input7k.osu --target 10 --engine nk2 --nk2-mode faithful --nk2-search phrase --nk2-phrase-support --out phrase-notes10.osu --report phrase-notes10.json
```

`--nk2-phrase-support`는 **1K–18K 상향·하향 phrase, faithful, stream off**에서 동작한다.
GUI의 `C - Phrase + taps`에 해당하며 자동 채택 E에도 포함된다. 출력에는
`-phrase-notes` 표시가 붙는다. 기존 B의 원본 노트 레인·시각·종류·LN 길이를
확정한 뒤 보조 탭만 추가하므로, 이 옵션을 꺼도 B 결과는 변하지 않는다.

- 원본 강박과 LN 시작/끝에 기존 NK2 보조 탭 후보를 만든다. 새 LN을 만들거나
  음원을 분석해 새 리듬을 추측하지 않는다.
- 전체 추가 상한은 원본 노트 수의 12%를 올림한 값이다. 2000ms 구간별 상한은
  그 구간 원본 수의 12%를 올림해 1~4개로 제한한다. 한 시각에는 최대 한 탭만 더한다.
- 같은 레인의 앞뒤 500ms 이내 노트가 있거나 LN 몸통·꼬리에 겹치면 제외한다.
  추가 탭에는 원본 같은 레인 반복의 예외를 적용하지 않는다.
- B를 재배치해 추가 노트 자리를 만들지 않는다. 안전한 자리가 부족하면 상한보다
  적게 추가할 수 있으며 최종 독립 검사 실패 시 저장을 차단한다.

`phraseSearch.supportNotes`가 옵션 상태를 표시하고 `placement.addedNotes`와
`independentSafety.addedNotes`가 실제 추가량을 기록한다. C의 addedNotes는
원본 제외량과 상쇄한 순증가량이 아니다. 노트가 늘었다는 사실만으로 음악적
품질 향상을 뜻하지 않으므로 B와 C를 같은 곡에서 비교한다.

## D: 추가량을 높이고 롱놋도 추가하기

```powershell
.\build\KeyWeaver.exe input7k.osu --target 10 --engine nk2 --nk2-mode faithful --nk2-search phrase --nk2-phrase-support --nk2-phrase-dense-ln --out dense-ln10.osu --report dense-ln10.json
```

GUI에서는 `D - Dense taps + LN`을 선택한다. D는 **C를 먼저 그대로
만든 뒤** 탭과 LN을 더한다. C의 추가 탭도 이동하거나 LN으로 바꾸지 않는다.
수동 B와 C 옵션·결과는 유지하며 D 파일명에는 `-phrase-notes-dense-ln`을 붙인다.

- 전체 생성 노트(C 포함) 상한은 원본 노트 수의 35% 올림이다. 2000ms 구간별
  상한은 그 구간 원본 수의 35% 올림을 1~10개로 제한한다. 한 시각에는 C 포함
  최대 두 생성 노트만 두고, 생성 LN은 한 시각에 최대 한 개다.
- 생성 LN은 전체 원본 수의 12% 올림, 2000ms 구간당 최대 세 개로 제한한다.
  구간 예산은 노트의 시작 시각 기준이다. 실제 추가량은 안전한 공간에 따라 줄어든다.
- 먼저 원본의 200~2000ms LN 구간을 빈 레인에 복제한다. 이후 원본 노트 시작
  시각부터 원본의 다른 시작/끝 시각까지 200~800ms 짧은 LN 후보를 만든다.
  후보 끝점은 한 박 길이에 가장 가까운 원본 시각을 선택한다. 새 음원 분석은 하지 않는다.
- 남는 예산으로 기존 강박·LN 끝 악센트와 다른 원본 탭 시작 시각에 탭을 추가한다.
- 생성 노트의 같은 레인 앞뒤 500ms 내 헤드를 금지한다. LN 후보는 시작 시각뿐
  아니라 **몸통과 꼬리 전체**가 이전/미래 노트와 겹치는지 검사한다. 충돌 때문에
  C 노트를 지우거나 이동해서 공간을 만들지 않는다.

`placement.addedTapNotes`와 `addedHoldNotes`는 원본 대비 총 생성량이다.
`support.denseTapAccepted`/`denseHoldAccepted`는 C 이후 D에서 더한 개수다.
전체 증가량과 LN 증가량을 분리해서 비교한다. 짧은 LN 증가가 더 자연스럽거나
쉽게 느껴진다는 보장은 없으며, D의 플레이 감각은 비교 검증 대상이다.

## E: 원본 LN 구간 안에서 길이와 배치를 다양화하기

```powershell
.\build\KeyWeaver.exe input7k.osu --target 10 --engine nk2 --nk2-mode faithful --nk2-search phrase --nk2-phrase-support --nk2-phrase-dense-ln --nk2-phrase-source-ln --out source-ln10.osu --report source-ln10.json
```

GUI에서는 `E - Source LN`을 선택한다. E는 D를 수정한 별도
선택지이며, **동일한 C부터 다시 생성**한다. C의 모든 노트는 그대로 남지만
D에서 추가된 노트의 위치·길이·개수를 보존하는 모드는 아니다. D 옵션만
선택한 결과는 그대로 유지한다. E 파일명에는 `-phrase-notes-dense-ln-source-ln`을 붙인다.

- 새 LN의 시작부터 끝까지가 **원본의 유효한 LN 하나의 시작~끝 안에 전부
  포함**되어야 한다. 인접하거나 겹친 원본 LN 여러 개를 이어서 더 넓은 구간으로
  취급하지 않는다. 원본 LN이 없는 시간에는 새 LN을 만들지 않는다.
- 원본 노트 시작 시각에서 후보를 만들고, 200~2000ms 범위 안의 원본 시작/끝
  시각을 꼬리로 사용한다. 짧은 길이, 한 박, 두 박, 가능한 긴 길이를 번갈아
  선호하고 이미 자주 쓴 길이 범주·직전 범주에 감점을 준다. 실제 길이는 원본
  끝점, 남은 LN 구간, 안전한 공간에 따라 달라진다.
- 생성 LN만의 레인 사용량을 따로 세고, 직전 생성 LN과 같은 레인·같은 손
  쪽에 감점을 주어 배치를 분산한다. 난수는 사용하지 않으며 같은 입력과 옵션은
  같은 결과를 낸다. 안전한 레인이 하나뿐이면 그 레인을 선택할 수 있다.
- 전체/LN/구간/동시타 예산, 500ms 헤드 간격과 LN 전체 구간 안전 검사는 D와
  같다. LN을 만든 뒤 남는 공간에는 탭을 더한다. 생성 LN이 0개인 결과도 정상이며,
  원본 LN이 적은 곡에서는 D보다 LN 수가 크게 줄 수 있다.

`phraseSearch.sourceLn`은 E 옵션 상태다. 독립 검사의
`sourceLnCoverageRequired`가 true이면 `generatedHoldsOutsideSourceLn`도 0이어야
통과한다. D에서는 범위 밖 LN 수를 비교용으로 집계하되 이 조건으로 실패시키지
않는다. 원본에서 유지된 LN은 새 길이 제한이나 범위 조건으로 변경하지 않는다.
길이·레인 선호를 다양화해도 모든 곡에서 길이 종류가 더 많아지거나 더 좋은
플레이 감각을 보장하는 것은 아니다.

## F: E를 유지하면서 LN만 더 추가하기

```powershell
.\build\KeyWeaver.exe input7k.osu --target 10 --engine nk2 --nk2-mode faithful --nk2-search phrase --nk2-phrase-support --nk2-phrase-dense-ln --nk2-phrase-source-ln --nk2-phrase-source-ln-boost --out source-ln-boost10.osu --report source-ln-boost10.json
```

GUI에서는 `F - Source LN+`를 선택한다. F는 **E를 먼저 그대로
완성한 뒤 LN만 더한다**. E의 탭을 LN으로 바꾸거나 노트를 이동·삭제하지 않는다.
따라서 10K D/E는 기존 결과를 유지하고, F에서도 모든 E 노트를 그대로 비교할 수 있다.
파일명은 E 접미사 뒤에 `-boost`를 붙인다. 모든 1K–18K 상향·하향 조합에서
같은 옵션으로 사용할 수 있다.

- 전체 생성 노트(C/E 포함) 상한을 원본 수의 50% 올림, 생성 LN 상한을 22% 올림으로
  높인다. 2000ms 구간에서는 유지된 원본 수의 50% 올림을 1~14개로 제한하며,
  생성 LN은 최대 다섯 개다. 한 시각에는 생성 노트 세 개, 생성 LN 두 개까지 허용한다.
- 새 LN은 E와 같은 200~2000ms 길이·원본 시작/끝 시각 후보를 사용하며, 전체가
  원본 LN 한 개 안에 있어야 한다. 원본 LN이 없는 구간에는 F도 새 LN이 없다.
- E의 생성 LN 분포를 포함해 길이와 레인을 선택한다. 500ms 헤드 간격과 LN
  몸통/꼬리 충돌 검사를 완화하지 않으므로 안전한 빈자리가 없으면 추가량은 0이다.
- `phraseSearch.sourceLnBoost`는 F 옵션 상태,
  `support.sourceLnBoostHoldAccepted`는 E 대비 더한 LN 개수다.
  `placement.addedHoldNotes`와 `support.denseHoldAccepted`는 E를 포함한 총 생성 LN이다.

하향 변환에서도 C/D/E/F를 선택할 수 있다. 먼저 B가 원본 시각·남겨진 LN 길이를
보존하며 배치 불가능한 원본 노트를 제외하고 보고한다. 그 결과에 C의 탭, D/E의
탭/LN, F의 LN을 안전한 공간에만 추가한다. 자리를 만들려고 B/C/E를 이동하거나
삭제하지 않으며, 500ms 간격·LN 전체 구간 검사와 기존 예산을 그대로 적용한다.
따라서 목표 키 수가 적거나 밀도가 높으면 추가 노트가 0개일 수 있다.
`independentSafety.droppedSourceNotes`와 `independentSafety.addedNotes`를 별도로 보고 추가량과 원본 손실을 비교한다.

```powershell
# 5K → 12K E 상향
.\build\KeyWeaver.exe input5k.osu --target 12 --engine nk2 --nk2-mode faithful --nk2-search phrase --nk2-phrase-support --nk2-phrase-dense-ln --nk2-phrase-source-ln --out five-e12.osu
# 8K → 6K F 하향
.\build\KeyWeaver.exe input8k.osu --target 6 --engine nk2 --nk2-mode faithful --nk2-search phrase --nk2-phrase-support --nk2-phrase-dense-ln --nk2-phrase-source-ln --nk2-phrase-source-ln-boost --out eight-f6.osu
```

## 기존 4K→5K/7K 비교와 호환

4K 입력에는 자동 B/E 채택을 적용하지 않는다. CLI의 암묵적 기본은 native,
GUI/CLI `auto`는 faithful로 남는다. 다음처럼 B 또는 E를 직접 선택한다.

```powershell
.\build\KeyWeaver.exe input4k.osu --target 5 --engine nk2 --nk2-mode faithful --nk2-search phrase --out four-b5.osu
.\build\KeyWeaver.exe input4k.osu --target 7 --engine nk2 --nk2-mode faithful --nk2-search phrase --nk2-phrase-support --nk2-phrase-dense-ln --nk2-phrase-source-ln --out four-e7.osu
```

B는 기존 4K→5K fill 예외를 끄므로 탭/LN을 추가하지 않는다. C/D/E/F도 수동 지원하며
E는 C를 보존하고 생성 LN 전체가 원본 LN 하나에 포함되어야 한다. F는 E를 보존한다.
원본 LN 또는 안전한 공간이 없으면 생성 LN 0개도 정상 결과다. 기존 B/E 비교 OSZ
묶음의 구성은 유지하며, CLI/GUI의 일반 변환 지원 범위와는 별개다.

## 탐색 방식과 한계

- 입력을 시간순으로 읽고 현재 구간의 uninherited BPM에 해당하는 두 박을
  탐색한다. 시간 범위는 200~2000ms, 노트 수는 최대 48개이며 동시타 묶음을
  중간에서 나누지 않는다. 한 묶음이 48개를 초과하면 오류를 반환한다.
- 각 노트에서 안전한 레인 후보와 제외 후보를 만든다. 각 단계에서 최대
  24개 상태를 유지하며, 동률은 고정된 생성 순서로 결정한다. 시간 제한에
  따른 무작위 중단이나 난수 탐색은 없다.
- 제외 노트 수가 적은 후보를 우선한다. 제외 수가 같으면 LN을 더 보존하고,
  그다음 기존 레이아웃·모티프 점수와 짧은 원본 레인 연결성을 비교한다.
- 구간 끝에서 한 배치를 확정하며 다음 구간으로 레인 사용량·최근 배치·LN
  점유 상태를 이어간다. 과거 모든 노트를 후보마다 복사하지 않는다.
- 원본 시각과 살아남은 LN 길이를 바꾸지 않는다. 탐색 단계 자체는 탭/LN 추가,
  LN의 탭 변환, 고정 ms roll을 하지 않는다. 압축이 불가능하면 일부 원본 노트를 제외한다.
  상향·하향 변환에서 C/D/E/F를 선택하면 탐색 후 C의 탭, D/E의 탭/LN,
  F의 추가 LN을 만든다. 7K→8K/10K의 자동 E에는 이 단계가 포함된다.
- 레인별 마지막 노트와 500ms 이내에 다른 원본 레인의 노트를 이어 붙이지
  않는다. 원본에 존재하는 같은 레인 반복은 허용한다. LN 꼬리 시각도 점유로 본다.

전체 곡 최적화나 최소 삭제량을 보장하지 않는다. 24개 후보 밖의 해는
버릴 수 있고, 구간 경계를 가로질러 이전 결정을 되돌리지 않는다.
기존판보다 항상 노트를 더 보존하거나 더 자연스럽다는 주장은 하지 않는다.
빠른 패턴의 과도한 제외나 특정 레인에 대한 고착은 실제 채보 비교 대상이다.

## 독립 안전성 검사

`safety_audit.cpp`는 배치기의 안전 마스크나 기존 보고 카운터를 재사용하지
않는다. 완성된 출력을 레인별로 다시 정렬하고 원본 노트 ID로 출처를 확인한다.

검사 실패 조건은 범위 밖 레인, 잘못된 LN, 같은 시각 충돌, LN 몸통/꼬리
겹침, 새 레인 반복, LN 길이/종류 변경, 출처 불명/중복 원본 노트다.
다른 레인의 노트가 중간에 있어도 같은 레인의 인접 반복을 검사한다.
두 노트가 원본에서도 같은 레인이며 원본 시각 차이가 0보다 크고 500ms
이내일 때만 원본 반복으로 인정한다. 생성 노트는 원본 반복으로 간주하지 않는다.

`independentSafety`에는 통과 여부와 위 카운터, 원본 보존·제외·추가·시간 이동
수를 기록한다. 제외/시간 이동 자체는 검사 실패가 아니므로 수치를 따로 읽어야
한다. 특히 기존 하향 변환은 합법적인 roll을 포함할 수 있다.
기존 `createdJacks`는 전체 노트열의 연속성 지표이며, 새 `createdLaneRepeats`는
레인별 인접성 지표이므로 값이 다를 수 있다. 기존 지표 의미는 바꾸지 않는다.

`--nk2-mode faithful --nk2-search off --nk2-audit`처럼 기존 경로를 명시하면
검사가 실패해도 보고하고 원래 출력을 유지한다. `--nk2-audit` 자체는 자동 채택을 끄지 않는다.
phrase 경로는 검사를 자동 실행하며 실패하면 CLI가 보고서를 남기고 차트 저장
전에 오류로 종료한다. 라이브러리 사용자는 `report.independentSafety->passed()`를
확인한 뒤 내보내야 한다. 검사 통과는 플레이 감각·음악적 품질 검증과 별개다.

## 비교용 OSZ 만들기

```powershell
python scripts\package_nk2_comparison.py --converter build\KeyWeaver.exe --output dist\my-nk2-comparison --chart '<원본 곡 폴더>\native7k.osu'

# B 탐색과 C 탐색+노트의 10K 비교 (원본 7K 포함 총 세 난이도)
python scripts\package_nk2_comparison.py --converter build\KeyWeaver.exe --comparison phrase-notes --output dist\my-nk2-notes-comparison --chart '<원본 곡 폴더>\native7k.osu'

# B/C/D 10K 비교 (원본 7K 포함 총 네 난이도)
python scripts\package_nk2_comparison.py --converter build\KeyWeaver.exe --comparison phrase-dense-ln --output dist\my-nk2-dense-ln-comparison --chart '<원본 곡 폴더>\native7k.osu'

# C/D/E 10K 비교 (원본 7K 포함 총 네 난이도)
python scripts\package_nk2_comparison.py --converter build\KeyWeaver.exe --comparison phrase-source-ln --output dist\my-nk2-source-ln-comparison --chart '<원본 곡 폴더>\native7k.osu'

# D/E/F 10K 비교 (원본 7K 포함 총 네 난이도)
python scripts\package_nk2_comparison.py --converter build\KeyWeaver.exe --comparison phrase-source-ln-boost --output dist\my-nk2-ln-boost-comparison --chart '<원본 곡 폴더>\native7k.osu'

# 7K→5K A/B와 7K→8K D/E/F 테스트 (원본 7K 포함 총 여섯 난이도)
python scripts\package_nk2_comparison.py --converter build\KeyWeaver.exe --comparison phrase-key-tests --output dist\my-nk2-5k-8k-comparison --chart '<원본 곡 폴더>\native7k.osu'

# 채택 기본판과 수정된 기존 5K 비교 (원본 7K 포함 총 여섯 난이도)
python scripts\package_nk2_comparison.py --converter build\KeyWeaver.exe --comparison adopted-repair --output dist\my-nk2-adopted-comparison --chart '<원본 곡 폴더>\native7k.osu'

# 4K→5K/7K B/E 수동 실험 (원본 4K 포함 총 다섯 난이도)
python scripts\package_nk2_comparison.py --converter build\KeyWeaver.exe --comparison phrase-four-key-tests --output dist\my-nk2-four-key-comparison --chart '<원본 곡 폴더>\native4k.osu'
```

`--chart`를 반복하면 여러 곡을 처리한다. 원본 채보는 음원·배경과 함께 있는
곡 폴더에서 지정한다. 출력 폴더가 이미 있으면 중단하며 원본과 기존 압축 파일은
덮어쓰지 않는다. 새 라이브러리나 네트워크 요청은 필요하지 않다.

곡별 OSZ에 원본 7K, A Legacy 4K/10K, B Phrase 4K/10K 총 다섯 난이도와
참조 음원·배경을 담는다. 원작자는 보존하고 제목/난이도에 비교 표시를 추가하며
맵 ID를 초기화한다. 원본 7K의 노트는 복사하고 A/B는 동일한 faithful 조건이다.

`--comparison phrase-notes`에서는 원본 7K, B Phrase 10K, C Phrase Notes 10K를
담는다. B/C 모두 독립 검사 통과가 필수이며 B의 모든 HitObjects 행이 C에도
그대로 존재하는지, 차이가 보고된 개수의 추가 탭뿐인지 확인한다.

`--comparison phrase-dense-ln`은 원본 7K와 B/C/D 10K를 담는다. D에 C의 모든
노트가 그대로 포함되어야 하며, 실제 노트 수와 LN 수 모두 늘었는지 검사한다.
생성 공간이 없거나 검사가 실패하면 보고서에 원인을 남기고 OSZ를 완성하지 않는다.

`--comparison phrase-source-ln`은 원본 7K와 C/D/E 10K를 담는다. B도 C 보존 검증을
위해 실행하지만 OSZ에는 넣지 않는다. C의 노트 보존과 E 생성 LN 전체의 원본 LN
포함 여부를 내보낸 HitObjects에서 다시 확인한다. 이 모드의 E는 LN 추가가 없어도
허용하며 `generatedHoldStatus`에 `noSourceLongNotes` 또는 `noSafeRoom`을 기록한다.
D/E 모두 `sourceLnCoverageViolations`, `generatedHoldDurationKinds`,
`generatedHoldDurationMs`, `generatedHoldLaneCounts`, `generatedHoldMaxLaneShare`로
구간 위반 수, 길이 종류/목록, 레인별 개수와 가장 많은 레인의 비율을 비교할 수 있다.
E의 구간 위반 수는 반드시 0이어야 한다.

`--comparison phrase-source-ln-boost`는 원본 7K와 D/E/F 10K를 담고 B/C를 내부
검증용으로만 실행한다. `--comparison phrase-key-tests`는 원본 7K, A Legacy 5K,
B Phrase 5K, D/E/F 8K를 담고 B/C 8K를 내부 검증용으로 실행한다. F는 E의
HitObjects가 모두 남고 차집합이 LN뿐인지, 생성 LN 전체가 원본 구간 안인지 검사한다.
F의 `extraHoldNotesVsBase`는 E 대비 증가량이며 `generatedHold*`는 C 대비 총 생성
LN 지표다. `baseVariant`/`holdMetricsBaseVariant`로 비교 기준을 표시한다.
안전한 공간이 없어 F 추가량이 0이어도 기존 E를 훼손하지 않고 그대로 보고한다.

`--comparison adopted-repair`는 원본 7K, A Fixed 5K, B Adopted 4K/5K,
E Adopted 8K/10K 총 여섯 난이도다. A Fixed는 원본 연타가 이어진다는 이유로
모든 레인의 연타 금지 마스크를 풀던 문제를 수정한 기존 배치다. 예외는 실제로
같은 원본 레인의 직전 배치 레인에만 적용한다. 기존 A/B/D/E/F 비교 묶음은 유지한다.

`--comparison phrase-four-key-tests`는 원본 4K와 B/E 5K, B/E 7K 총 다섯 난이도다.
C는 E 보존 검증용으로만 실행한다. B의 추가 노트는 0개이며, E의 생성 LN이 없어도
원본 LN 제약과 안전 검사를 통과하면 비교용으로 포함할 수 있다.

`summary.csv`, `comparison.json`, 개별 보고서와 `SHA256SUMS.txt`를 함께 생성한다.
기존 A의 검사 실패도 숨기지 않으며 B가 실패하면 OSZ를 완성하지 않는다.
압축 후 CRC·파일 목록·모드에 맞는 3개/4개/5개/6개 난이도의 키 수·음원 참조·채보/미디어 바이트를 다시 검사한다.
로컬 경로는 결과 메타데이터에 넣지 않는다. 실제 게임에서 import 후 동기화와
패턴 감각을 확인해야 한다.
