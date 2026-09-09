# M7 — запуск вимірювань

Результати конкретного ПК і межі перевірки наведено в [M7_REPORT.md](M7_REPORT.md).
Правила порівняння — у [M7_CONTRACT.md](M7_CONTRACT.md).

## Збірка й звичайна робота

Для збереженої збірки M7 відкрийте `build/m7-release/bin/ProtoEditor.exe`.
Кореневий `Launch-Editor.cmd` тепер віддає перевагу M8. Створення проєкту, імпорт,
Play/Stop, експорт та графічні налаштування працюють як у
[посібнику M6](M6_GUIDE.md).

```powershell
./scripts/Build.ps1 -Configuration m7-release -Test
./scripts/Build.ps1 -Configuration m7-debug -Test
```

Використовується наявний C++ toolchain. Окремий Python не потрібний.
Debug потрібен для перевірок; швидкодію порівнюйте на Release без validation.

## Повна матриця

З кореня репозиторію:

```powershell
./scripts/Benchmark-M7.ps1 `
  -Executable build/m7-release/bin/ProtoBenchmark.exe `
  -OutputRoot test-results/my-m7-run
./scripts/Summarize-M7.ps1 -InputRoot test-results/my-m7-run
```

Це 14 сценаріїв, три повтори, 2 с прогрівання та 5 с вибірки. Вікно показу
має 320×180, сцена рендериться у 1280×720. VSync вимкнено. Показаний throughput
стосується цього вимірювача й містить його витрати на збирання статистики.
Це не обіцянка FPS повноекранного Player або довільної сцени.

Для порівняння передавайте окремий незмінний executable:

```powershell
./scripts/Benchmark-M7.ps1 `
  -BaselineExecutable .cache/m7-instrumented-baseline/bin/ProtoBenchmark.exe `
  -Executable build/m7-release/bin/ProtoBenchmark.exe `
  -OutputRoot test-results/my-m7-comparison
./scripts/Summarize-M7.ps1 -InputRoot test-results/my-m7-comparison
```

Порядок before/after чергується між повторами. Скрипт запускає програми
послідовно й записує стан процесів та живлення. Не запускайте паралельно
компіляцію, інший Player/Editor чи GPU-тести. Налаштування Windows і драйвера
скрипт не змінює. Перервану матрицю можна продовжити через `-Resume` з тими
самими параметрами й бінарними файлами; збережені результати перевіряються.

## Одна сцена та контрольний кадр

```powershell
build/m7-release/bin/ProtoBenchmark.exe `
  --scenario instances-10000-moving --warmup 2 --seconds 5 `
  --output test-results/moving.json `
  --capture test-results/moving.png --capture-frame 60
```

Контрольний кадр будується окремо після часової вибірки; його фаза задається
номером кадру при 60 Гц. Рух у самій вибірці безперервний, чотирисекундний,
починається з однакової фази після прогрівання.

Сценарії: `empty`, `small`, `instances-1000`, `instances-10000-visible`,
`instances-10000-culled`, `instances-10000-moving`, `lights-1`, `lights-8`,
`lights-32`, `lights-128`, `moving-lights-32`, `moving-casters`,
`pool-overflow`, `pool-resident`.

`--view-only` вимірює лише побудову runtime view на CPU. `--frames N`
придатний для короткої перевірки правильності, але не замінює часову серію.
Для GPU validation використовуйте `--validation-dir` із каталогом локальних
validation layers, який підготував `Setup-Validation.ps1`.

## Editor, ресурси й VSync

```powershell
./scripts/Benchmark-M7Supplement.ps1 `
  -BinDirectory build/m7-release/bin `
  -OutputRoot test-results/my-m7-supplement
./scripts/Benchmark-M7.ps1 `
  -Executable build/m7-release/bin/ProtoBenchmark.exe `
  -Scenarios empty,small -VSync `
  -OutputRoot test-results/my-m7-vsync
```

Supplement створює тимчасову порожню сцену редактора й виконує idle та
програмний рух камери. Записує фактичні розміри docked viewport і вікна,
окремо загальну та виміряну кількість кадрів. Зміна розміру або неповна
вибірка робить запуск непридатним. Користувацькі проєкти й layout не змінюються.
`-SkipEditor` та `-SkipAssetBenchmark` вибирають окрему групу.

AssetBenchmark використовує власні glTF/GLB fixtures, видаляє лише власний
cooked cache, порівнює cold/warm load та виконує 12 циклів move/copy Undo/Redo
на повтор. OS disk cache Windows не очищується. Перевірка GPU-власників
і стабілізації VMA пам'яті входить у `m7_gpu_asset_cycle`.

## Як читати дані

- `.json`: метадані, часові вибірки CPU, незалежні GPU-вибірки з serial,
  median/p95/p99, counts, uploads та пам'ять.
- `.manifest.json`: точна геометрія, параметри світла/тіней і анімація.
- `.csv`: окремі секції кадрів, підсумкових метрик та GPU serial/time.
  Для автоматичного аналізу всіх секцій зручніше читати JSON.
- `.host.json`: executable SHA-256, команда, exit code, живлення й фонові процеси.
- `summary-*.csv`: звичайні таблиці запусків, груп і порівняння.
- Editor `.observations.json`: межі вибірки, реальні розміри й перевірка GPU coverage.

`wall_ms` — час проходження кадру, включно з очікуванням; `frame_wait_ms`
показує очікування fence. Час етапу не дорівнює процесорному часу процесу.
GPU-вибірки не слід з'єднувати з CPU-рядками за позицією в масиві.
Підсумок трьох повторів — медіана їхніх окремих median/p95/p99; діапазон
медіан теж збережено. При кількох десятках кадрів p99 має мало спостережень.

VMA allocation — облік виділених ресурсів, heap budget/usage може включати
спільну пам'ять інтегрованої GPU. Це не вимір фізичної VRAM. Пули й місткості
буферів можуть залишатися після unload для повторного використання; перевірка
витоку вимагає стабільного плато та звільнення власників ресурсів.
