# KeyWeaver v1.2.0

Языки: [English](README.en.md) | [한국어](README.ko.md) | [日本語](README.ja.md) | [简体中文](README.zh-CN.md) | [繁體中文](README.zh-TW.md) | [Русский](README.ru.md)

KeyWeaver - это инструмент на C++20/CMake для конвертации количества клавиш в osu!mania `.osu` и BMS-семействах. Он работает локально и детерминированно, включает CLI, Windows GUI, batch/отчеты и экспериментальный NK2 для повышения, понижения и same-key преобразований до 18K.

## Что представляет этот релиз

`v1.2.0` расширяет экспериментальный NK2 на все пары source/target от 1K до 18K, включая понижение числа клавиш и преобразования с тем же числом клавиш, и поставляется с проверенным Windows-пакетом, полным архивом исходников и отдельным архивом NK2.

Основные возможности:

- Ввод/вывод osu!mania `.osu`.
- Базовый ввод/вывод BMS/BME/BML/PMS; BMS-вход всегда остается BMS-выходом.
- Автоопределение source key и ручной Source override.
- GUI Source поддерживает `auto` и `1`-`18`.
- В batch-режиме числовой Source конвертирует только совпадающие source-key чарты, остальные пропускаются.
- GUI Target поддерживает 4K-18K. NK2 CLI/core поддерживает все пары source/target 1K-18K, включая конвертацию из большего числа клавиш в меньшее.
- Если `--out` / `--out-dir` не указаны, результат пишется рядом с исходным чартом.
- Параллельный CLI batch через `--batch`, `--input-list`, `--jobs`, с прогрессом и итоговой сводкой.
- Classic-конвертация с проверками collision, LN, distance и jack safety.
- Экспериментальные режимы NK2: `native`, `faithful`, `harder`, `transform`; same-key конвертация выполняется только в `transform`, а `report` предназначен только для анализа одного входа.
- GUI 10K по умолчанию использует Full-Field Mirror-Remix.
- JSON/CSV отчеты для качества конвертации, сравнения политик и диагностики.
- Опциональный batch-only ONNX Runtime lane-policy hook для CUDA-сборок.

Не входит: полноценный редактор чартов, воспроизведение аудио, waveform view, DP split conversion, поведение realtime BMS-плеера, обучение моделей, автоматическая публикация или загрузка.

## Release Package

Если нужно только запустить инструмент, используйте Windows release zip:

```text
KeyWeaver-v1.2.0-win64-<timestamp>.zip
```

Внутри пакета:

- `keyconv_gui.exe` - Windows GUI.
- `KeyWeaver.exe` - CLI entrypoint. При двойном клике открывает GUI, если рядом лежит `keyconv_gui.exe`.
- `keyconv.exe` - CLI alias.
- MinGW runtime DLLs.
- `samples/`, `profiles/`, `docs/`, `models/`, `scripts/`.
- `smoke/` verification logs, созданные package script.

## Быстрый старт GUI

1. Распакуйте release zip.
2. Запустите `keyconv_gui.exe`.
3. Если GUI не нашел его автоматически, выберите `KeyWeaver.exe`.
4. Выберите или перетащите osu!mania/BMS-family чарт.
5. Установите Source в `auto` или значение `1`-`18`.
6. Установите Target от `4` до `18`.
7. Выберите Classic или NK2.
8. Нажмите Convert или Batch Folder.

Заметки GUI:

- Пустой Output означает, что каждый результат пишется рядом со своим исходным чартом.
- Batch с числовым Source конвертирует только чарты с совпадающим detected source key.
- GUI Matrix работает только для Classic/NK1.
- NK2 batch работает для режимов конвертации, но NK2 `report` только для одного входа.

## Примеры CLI

Dry run одного чарта:

```powershell
.\KeyWeaver.exe samples\simple_4k.osu --source 4 --target 10 --dry-run
```

Запись результата и отчета:

```powershell
.\KeyWeaver.exe samples\simple_7k_ln.osu --source 7 --target 10 --out dist\simple_7k_10k.osu --report dist\report.json
```

Быстрый batch через input-list:

```powershell
.\KeyWeaver.exe --batch --input-list charts.txt --source 7 --target 10 --engine nk2 --nk2-mode native --batch-quiet
```

Поведение mixed-source batch:

```text
--source 7 конвертирует только чарты, определенные как 7K, остальные получает статус skipped.
```

Правило BMS:

```text
BMS-family input может писать только BMS-family output: .bms, .bme, .bml или .pms.
```

Полный список опций: `KeyWeaver.exe --help`.

## Сборка из исходников

Обычная сборка:

```powershell
cmake -S . -B build -G Ninja
cmake --build build
ctest --test-dir build --output-on-failure
```

Целевая сборка:

```powershell
cmake --build build --target KeyWeaver keyconv keyconv_gui keyconv_tests keyconv_public_header_smoke
```

Release package:

```powershell
.\scripts\package_release.ps1 -Version 1.2.0
```

Package script выполняет Release CMake build, unit tests, public-header smoke, GUI smoke, CLI dry-run smokes, sample conversions, reconversion guard, BMS guard и пишет zip плюс `.sha256` в `dist/release/`.

## Опциональный ONNX CUDA batch path

Обычная сборка не требует ONNX Runtime. ONNX опционален и используется только для batch.

Ручная конфигурация:

```powershell
cmake -S . -B build-onnx -G Ninja -DKEYWEAVER_WITH_ONNXRUNTIME=ON -DONNXRUNTIME_ROOT=<onnxruntime-package-root>
cmake --build build-onnx --target KeyWeaver
```

Windows helper:

```powershell
.\scripts\build_onnx_cuda.bat
```

ONNX notes:

- Текущий source path поддерживает только CUDA provider.
- Модель дает advisory output. Collision, LN, distance и no-created-jack safety checks все равно решают, будет ли lane принят.
- Target-10 batch может auto-load `models/lane_policy_student_mlp_u_e_circusgalop.onnx`, если модель включена в пакет.
- Используйте `--no-auto-onnx-policy`, чтобы принудительно выбрать deterministic batch.
- Используйте `--onnx-policy-strict` для проверки runtime setup.

## Source Package

Source package имеет имя:

```text
KeyWeaver-v1.2.0-source-<timestamp>.zip
```

Он содержит source, docs, models, scripts и release diff artifacts. Он намеренно исключает `.git`, локальные agent instructions, build folders и старые release zips.

В релиз также входит KeyWeaver-NK2-source-1K-18K-v1.2.0-<timestamp>.zip, содержащий только восемь файлов из src/nk2/.

## Важные документы

- `docs/algorithm-lock-v0.6.0.md` - контракт normal-mode conversion.
- `docs/algorithm-lock-v0.6.1.md` - контракт 10K staged planner.
- `docs/design-10k-fullfield-remix.md` - дизайн GUI 10K Full-Field Mirror-Remix.
- `docs/nk2-algorithm.md` - текущий NK2 walkthrough.
- `docs/nk2-design.md` - заметки по дизайну NK2 второго поколения.
- `docs/lane-policy-student.md` - заметки ONNX lane-policy student.
- `docs/code-architecture.md` - обзор архитектуры кода.

## Ограничения и риски

- BMS support остается MVP. Безопасные выходные раскладки: 1K-10K, 12K, 14K, 16K и 18K; остальные BMS target отклоняются с ошибкой вместо тихой потери дорожек.
- Для BMS-family 9K и 18K расширение по умолчанию — `.pms`; принудительный разбор 18K сохраняет все 18 позиций каналов.
- DP conversion не реализован.
- Beam search зарезервирован и сейчас fallback to greedy.
- Сильное key-count compression может drop или roll overflow notes в зависимости от policy.
- Конвертированные чарты маркируются и защищены от случайной повторной конвертации.
- NK2 и ONNX paths экспериментальны; при сравнении поведения используйте включенные smoke outputs и reports.

## License

См. `LICENSE`.
