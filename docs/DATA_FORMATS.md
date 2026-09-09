# Proto Engine v0.1 — формати та контракти даних

Дата: 2026-09-08. Загальні приклади нижче описують повний v0.1.
Підмножини M1–M3 реалізовані; M1 описано в розділі 4, доповнення M2 — у розділі 11,
сценове освітлення M3 — у розділі 12, проєкти й файлові операції M4 — у розділі 13.
Контракт SDK/Player належить наступним етапам.
Робочий приклад: [Starter.scene.json](../examples/Starter.scene.json).
Зв'язок із системами описано в [ARCHITECTURE.md](ARCHITECTURE.md).

## 1. Загальні правила

- JSON — UTF-8; числові значення скінченні, без NaN/Infinity.
- Кожен документ має `format` і цілий `formatVersion`.
- UUID записуються канонічним рядком у нижньому регістрі.
- Шляхи проєкту відносні до його кореня, із `/`; абсолютні шляхи не пакуються.
- На Windows шлях зіставляється з урахуванням регістронезалежної файлової системи,
  але відображення зберігає авторське написання. UTF-8 ↔ UTF-16 перетворюється
  на межі платформного файлового API.
- Невідома більша версія формату не перезаписується. Завантажувач показує
  помилку сумісності; міграція попередньої версії працює на копії.
- Перевіряються типи, діапазони, унікальність ID, цілісність посилань і відсутність
  циклів до публікації нового стану.

## 2. Папка проєкту

```text
LightingLab/
  project.proto.json
  Assets/
    Car.glb
    Car.glb.meta
    Paint.material.json
    Paint.material.json.meta
  Scenes/
    Main.scene.json
    Main.scene.json.meta
  Code/
    Behaviors.cpp
    Spin.cpp
    Spin.hpp
  Config/
    graphics.json
  .proto/
    registry.json
    cache/<hash>/...
    builds/<sdk-toolchain-config>/...
    snapshots/<run-id>/...
    transactions/<transaction-id>/...
    editor.json
  Build/
    LightingLab/
```

Авторські дані — manifest, Assets, Scenes, Code і Config. `.proto` — внутрішні
індекси, кеші та робочі дані. Незавершені транзакції спочатку відновлюються;
після цього cache/builds можна відтворити з авторських даних. `.meta` з UUID
належить до авторських даних і переноситься разом із відповідним файлом.

Файлова панель показує звичайні авторські файли й окремо може показувати
підресурси моделей. Проєктний manifest та службова `.proto` не є цілями
звичайного drag-and-drop. Структуру початкових папок створює шаблон проєкту.

## 3. Manifest проєкту

```json
{
  "format": "proto.project",
  "formatVersion": 1,
  "projectId": "11111111-1111-4111-8111-111111111111",
  "name": "LightingLab",
  "engine": { "sdkVersion": "0.1", "behaviorApiVersion": 1 },
  "startupScene": "22222222-2222-4222-8222-222222222222",
  "contentRoots": ["Assets", "Scenes"],
  "code": {
    "sourceRoot": "Code",
    "registrationFile": "Code/Behaviors.cpp"
  },
  "graphicsSettings": "Config/graphics.json",
  "build": {
    "target": "windows-x64",
    "name": "LightingLab",
    "additionalAssets": []
  }
}
```

`startupScene` — AssetId сцени, а не шлях. Індекс знаходить її поточний файл.
Проєкт може зберігати кілька сцен; у v0.1 відкривається й виконується одна
активна сцена. Перехід між сценами під час виконання не є окремою вимогою v0.1.
`additionalAssets` додає залежності, які C++-код завантажує динамічно.

## 4. Сцена

UUID сцени однаковий у документі та її `.meta`. EntityId унікальні всередині
сцени й зберігаються при save/load; вони не є короткими runtime handles.

```json
{
  "format": "proto.scene",
  "formatVersion": 1,
  "sceneId": "22222222-2222-4222-8222-222222222222",
  "name": "Main",
  "activeCamera": "44444444-4444-4444-8444-444444444444",
  "entities": [
    {
      "id": "33333333-3333-4333-8333-333333333333",
      "name": "Car",
      "parent": null,
      "enabled": true,
      "transform": {
        "position": [0, 0, 0],
        "rotation": [0, 0, 0, 1],
        "scale": [1, 1, 1]
      },
      "components": {
        "meshRenderer": {
          "mesh": "bbbbbbbb-bbbb-4bbb-8bbb-bbbbbbbbbbbb",
          "materials": ["cccccccc-cccc-4ccc-8ccc-cccccccccccc"],
          "castShadows": true,
          "receiveShadows": true
        }
      },
      "behaviors": [
        {
          "id": "88888888-8888-4888-8888-888888888888",
          "type": "77777777-7777-4777-8777-777777777777",
          "enabled": true,
          "properties": { "speedDegreesPerSecond": 30 }
        }
      ]
    },
    {
      "id": "55555555-5555-4555-8555-555555555555",
      "name": "Headlight",
      "parent": "33333333-3333-4333-8333-333333333333",
      "enabled": true,
      "transform": {
        "position": [0.7, 0.8, -1.5],
        "rotation": [0, 0, 0, 1],
        "scale": [1, 1, 1]
      },
      "components": {
        "pointLight": {
          "color": [1, 0.9, 0.75],
          "intensity": 20,
          "range": 10,
          "castShadows": true
        }
      },
      "behaviors": []
    },
    {
      "id": "44444444-4444-4444-8444-444444444444",
      "name": "Camera",
      "parent": null,
      "enabled": true,
      "transform": {
        "position": [0, 2, 6],
        "rotation": [0, 0, 0, 1],
        "scale": [1, 1, 1]
      },
      "components": {
        "camera": {
          "projection": "perspective",
          "verticalFovDegrees": 60,
          "near": 0.05,
          "far": 300,
          "exposure": 1
        }
      },
      "behaviors": []
    }
  ]
}
```

Quaternion має порядок `x,y,z,w` і нормалізується після перевірки ненульової
довжини. Нульовий scale не спричиняє ділення на нуль: вироджена геометрія
пропускається рендерером; відповідні операції переприв'язування відхиляються.
Одиниці intensity у v0.1 відносні й явно названі в інтерфейсі; це не обіцянка
фотометрично каліброваних lux/lumen.

`DirectionalLight` використовує орієнтацію об'єкта, color, intensity та
castShadows. Одночасно активне одне таке джерело. Scene validation пояснює
конфлікт додаткового активного сонця, замість довільного вибору одного.

Редакторська камера, selection, розташування панелей і відкриті вкладки
зберігаються окремо в `.proto/editor.json`; це не runtime-стан сцени.

### Реалізована підмножина M1

M1 читає/пише `proto.scene`, `formatVersion: 1` із Transform, MeshRenderer та
perspective Camera. `activeCamera` може бути `null`. Behavior-масив та
`meshRenderer.materials` поки порожні. Непідтримувані компоненти, поля,
зовнішні AssetId та нові версії відхиляються до зміни відкритої сцени.

Вбудовані Mesh AssetId:

- Куб: `00000000-0000-4000-8000-000000000001`.
- Площина: `00000000-0000-4000-8000-000000000002`.

`meshRenderer.debugColor` — необов’язковий RGBA-вектор 0..1, alpha = 1,
для Unlit-відображення M1. Це не native PBR material. `castShadows`,
`receiveShadows` та camera `exposure` зберігаються як дані; освітлення,
тіні та експозиція ще не виконуються цим renderer.

M1 працює з окремим файлом сцени без проєктного manifest/registry/`.meta`.
Scene AssetId міститься у `sceneId`; EntityId сталі при save/load і Undo.
При першому збереженні й збереженні того самого файла Scene AssetId сталий.
Save As в інший файл уже збереженої сцени видає новий Scene AssetId, залишає
EntityId та початковий файл. Нові ID/шлях публікуються лише після успіху запису.

Поточні межі читання: 32 MiB на документ, до 100000 об’єктів, до 256 рівнів
ієрархії, назва об’єкта до 1024 UTF-8 bytes. Перевіряються дублікати JSON-полів,
UUID, посилання, цикли, скінченність чисел, ненульовий quaternion і параметри камери.
Це межі захисту завантажувача, а не обіцянка продуктивності такого розміру сцени.

## 5. Метадані джерела та підресурси

```json
{
  "format": "proto.assetmeta",
  "formatVersion": 1,
  "assetId": "aaaaaaaa-aaaa-4aaa-8aaa-aaaaaaaaaaaa",
  "kind": "ModelSource",
  "importer": { "name": "gltf", "version": 1 },
  "settings": { "generateTangents": true },
  "dependencies": [],
  "subAssets": [
    {
      "id": "bbbbbbbb-bbbb-4bbb-8bbb-bbbbbbbbbbbb",
      "kind": "Mesh",
      "locator": "/meshes/0",
      "nameHint": "Body"
    },
    {
      "id": "cccccccc-cccc-4ccc-8ccc-cccccccccccc",
      "kind": "Material",
      "locator": "/materials/0",
      "nameHint": "Paint"
    }
  ],
  "materialOverrides": {}
}
```

`locator` допомагає знайти дані в конкретній версії джерела; це не формула
генерації ID. Структурна зміна glTF перевіряється перед повторним зіставленням.
Зовнішня залежність містить `assetId`, поточний source URI та JSON pointer
до поля URI для транзакційного оновлення шляху.

Відновлюваний registry має generation, AssetId → path/kind/owner та reverse
dependency map. Якщо два різні файли принесено з однаковим UUID, індексатор
показує конфлікт. Він не надає новий UUID довільному файлу без відома користувача.

## 6. Матеріал

```json
{
  "format": "proto.material",
  "formatVersion": 1,
  "assetId": "cccccccc-cccc-4ccc-8ccc-cccccccccccc",
  "name": "Paint",
  "model": "pbrMetallicRoughness",
  "baseColor": [0.8, 0.12, 0.08, 1],
  "metallic": 0.3,
  "roughness": 0.45,
  "normalScale": 1,
  "baseColorTexture": null,
  "metallicRoughnessTexture": null,
  "normalTexture": null,
  "alphaMode": "OPAQUE",
  "alphaCutoff": 0.5,
  "doubleSided": false
}
```

Приклад показує логічний матеріал; він зберігається або як підресурс джерела
з overrides, або як окремий native asset, а не у двох незалежних файлах із тим
самим UUID. Створення незалежної копії матеріалу видає новий ID.

Кольорові текстури декодуються як sRGB; normal і metallic/roughness — як linear
data. Ключ підготовленої текстури включає її інтерпретацію. `MASK` використовує
однакові cutoff/UV/sampler у видимому й тіньовому проходах. При підготовці mip-рівнів
перевіряється збереження покриття маски, щоб листя не зникало непередбачувано.

## 7. Опис C++-поведінки

Player у режимі `--describe-behaviors` повертає контракт, а не виконує сцену:

```json
{
  "format": "proto.behaviors",
  "formatVersion": 1,
  "behaviorApiVersion": 1,
  "sdkBuildId": "example-sdk-build-id",
  "types": [
    {
      "id": "77777777-7777-4777-8777-777777777777",
      "name": "Spin",
      "properties": [
        {
          "name": "speedDegreesPerSecond",
          "type": "float",
          "default": 30,
          "min": -360,
          "max": 360
        }
      ]
    }
  ]
}
```

Помилка/несумісність опису не стирає збережені properties. Невідомий тип
показується як missing behavior; Play/Build із ним потребує виправлення.
Поля з AssetRef/EntityRef відрізняються від звичайних рядків і входять
до перевірки залежностей.

## 8. Налаштування графіки

Нижче наведено початкові значення для реалізації й вимірювань, а не погоджений
мінімум заліза або гарантію FPS. Користувацькі зміни записуються як Custom.

```json
{
  "format": "proto.graphics",
  "formatVersion": 1,
  "profile": "Balanced",
  "renderScale": 1,
  "vSync": true,
  "viewDistance": 300,
  "sunShadows": {
    "enabled": true,
    "cascades": 3,
    "resolution": 2048,
    "distance": 80,
    "filter": "pcf3x3"
  },
  "pointShadows": {
    "enabled": true,
    "resolution": 512,
    "filterTaps": 9,
    "poolBudgetMiB": 96,
    "overflow": "multiPass"
  }
}
```

У цільовому `graphics.json` pool для point shadows не включає CSM і framebuffer.
Поточний M3 використовує спільний pool обох типів світла (див. розділ 12). Одна depth cube map
із 512×512, шістьма faces та 32 бітами на texel потребує 6 MiB лише для
depth-даних; фактичне виділення/бюджет перевіряється через allocator.
Коли pool менший за потребу кадру, використовується більше lighting-пакетів
із тією самою якістю. Це не автоматичне відключення тіней у частини ламп.

Профілі Low/Balanced/High визначають явні набори resolution/filter/distance.
Їх остаточні значення фіксуються після M7 разом із результатами вимірювань.

## 9. Підготовлені ресурси та runtime manifest

Mesh/texture blobs мають явний заголовок: magic, formatVersion, type,
headerBytes, payloadBytes і таблиці діапазонів. Поля серіалізуються окремо
з визначеним порядком байтів; сирі C++-структури з padding у файл не копіюються.
Перевіряються межі, множення count×stride та відповідність очікуваному типу.

Mesh містить buffers, bounds, primitives і default material slots. Texture —
формат, dimensions, mip table та дані. Початково підтримуються потрібні
некомпресовані формати; контейнер резервує явний format enum для майбутньої
компресії. Рішення про додатковий encoder приймається за виміряним вузьким місцем.

Runtime manifest відображає AssetId → type, contentHash, blob path, dependencies.
Для готової програми використовується такий layout:

```text
LightingLab.exe
Data/
  runtime.json
  scenes/<scene-id>.scene.json
  blobs/<content-hash>...
  shaders/*.spv
Config/graphics.json
Licenses/...
THIRD_PARTY_NOTICES.txt
```

У runtime manifest не потрапляють абсолютні авторські шляхи, editor layout,
компілятори або кеші, прив'язані до драйвера іншого ПК. Перевірка пакета
проходить до публікації папки збірки.

## 10. Файловий журнал

Транзакція описує generation реєстру, source/target paths, початкові hashes,
issued UUID map, staged data та стан кожного кроку. Стани:
`Prepared → Applying → Committed`; перерване Applying має відновлення/компенсацію.
Після commit Undo є новою перевіреною операцією, а не сліпим виконанням старого
списку шляхів. Зовнішній конфлікт лишає файли недоторканими й формує діагностику.

Приклади JSON у цьому документі перевіряються на синтаксис і внутрішні
посилання окремо від майбутніх runtime-валідаторів.

## 11. Реалізований контракт M2

M2 зберігає `proto.scene`, `formatVersion: 1`. Сцена з імпортованими ресурсами
додатково містить `assetRoot` (відносний шлях від сцени до папки ресурсів) і
`modelSources` (масив UUID джерел). `MeshRenderer.mesh` посилається на UUID mesh
підресурсу; `materials: []` використовує матеріали mesh за замовчуванням.
Непорожній `materials` містить по UUID матеріалу на кожен primitive/slot.
Невідомі UUID й невідповідна кількість слотів є помилкою.

Перший імпорт копіює glTF/GLB та зовнішні залежності в
`Assets/<назва>-<uuid8>/`. Поруч із джерелом записується `<файл>.gltf.meta` або
`<файл>.glb.meta`: `proto.assetmeta`, `formatVersion: 1`, `kind: ModelSource`,
source `assetId`, importer `gltf` version 2, `settings.generateTangents: true`,
структурна `signature`, `contentHash`, `cacheHash`, dependencies URI/SHA-256,
`subAssets` із locator → UUID та `materialOverrides`.

Locators: `/meshes/N`, `/materials/N`, `/materials/N/texture/S`. Переміщення
всього source-пакета разом із `.meta` зберігає UUID. Зміна геометричних даних
при тій самій структурі зберігає UUID; зміна імен/порядку/кількості mesh,
матеріалів, primitive-слотів, зв'язків вузлів або додавання нового texture locator
потребує нового імпорту. Reload не видає нові незбережені UUID.
Існуючі entity-трансформації зберігають редакторські значення після reimport.
Повторний імпорт однакового вмісту перевикористовує наявне джерело; UUID
перевіряється за актуальними файлами, а не тільки старою metadata.
Файловий Copy із видачею нових UUID реалізовано в M4.

`.proto/registry.json` (`proto.registry`, version 1) відновлюється зі source
metadata. `.proto/cache/*.bin` має magic `ProtoCook4-win64`; ключ включає
SHA-256 джерела, залежностей, алгоритму `proto-cook-4` і source UUID.
Це локальний кеш поточного C++ layout Windows x64, не формат Player чи обміну.
Він містить mesh/index/slot arrays, BVH, спільні RGBA8 mip chains, samplers,
матеріали й ієрархію. M4 перевіряє `.bin.hash` у цьому ж кеші; для старого
M2-кешу підтримує hash із source metadata. Відсутній,
пошкоджений або старий кеш відбудовується з джерел. Кеш і registry можна
видаляти, `.meta` потрібно переносити разом із ресурсами.

Інспектор зберігає overrides базового RGB, metallic, roughness і normalScale
в source `.meta`, з перевіркою конфлікту й атомарною заміною. Під час drag
оновлюється тільки preview; завершення gesture зберігає файл і одну Undo-команду.
Undo/Redo також зберігають metadata. Вихідний glTF/GLB не переписується.
Alpha mode/cutoff, текстури та інші фактори змінюють через джерело й reimport.

`assetRoot` може бути `..` після Save As на тому самому диску; абсолютні шляхи
не записуються. Новий зовнішній імпорт glTF обмежений папкою вибраної моделі.
M4 reload допускає сусідні папки всередині `Assets`, щоб переносити джерела
та залежності окремо. UTF-8, percent encoding та base64 data URI підтримуються;
вихід за відповідну межу, абсолютні/network URI та drive/ADS відхиляються.

`proto.project`, watch service і журнал файлових транзакцій реалізовані в M4.
Runtime manifest реалізовано в M5–M6 (розділ 14). Поточний native Open scene читає
сцену синхронно; новий імпорт і reimport виконуються у worker.

## 12. Реалізоване освітлення M3

Файл сцени зберігає `formatVersion: 1`. Нові необов'язкові компоненти:

```json
"directionalLight": { "color": [1, 0.95, 0.85], "intensity": 3, "shadows": true }
```

```json
"pointLight": { "color": [1, 1, 1], "intensity": 30, "radius": 8, "shadows": true }
```

Один об'єкт має щонайбільше один тип світла. Орієнтація сонця задає напрям
променів уздовж світової локальної осі `-Z`. Радіус точкової лампи — у світових
метрах; масштаб об'єкта його не змінює. Вимкнений або вироджений батьківський
об'єкт приховує дочірнє світло так само, як геометрію.

Поточні налаштування знаходяться в необов'язковому кореневому `lighting`:

```json
"lighting": {
  "shadowResolution": 512,
  "shadowCascades": 3,
  "shadowPoolMiB": 64,
  "shadowDistance": 40,
  "depthBias": 0.001,
  "normalBias": 0.015,
  "ambient": 0.025,
  "shadows": true,
  "filteredShadows": true,
  "referenceLighting": false,
  "forceShadowRefresh": false
}
```

Допустимі значення й точний C++ контракт: [M3_CONTRACT.md](M3_CONTRACT.md).
`depthBias` задає відступ у нормалізованій глибині карти, `normalBias` — у метрах.
Для дуже великого радіуса лампи відповідний світовий відступ depth bias зростає.
Відсутні поля використовують типові значення; стандартні налаштування можуть
бути опущені при записі. M3 відкриває старі сцени; старі редактори не зобов'язані
розуміти нові поля. Файли прикладів M1/M2 залишаються окремими.

M3 об'єднує сонце й точкові лампи у спільному pool D32. Один слот має шість
шарів: точкове світло використовує всі шість, сонце — 1–4. Ліміт перевіряється
за фактичним розміром VMA allocation, включно з вирівнюванням драйвера.
Framebuffer/HDR, ресурси моделей і буфери обліковуються окремо. При зміні
якості попередній pool живе до завершення кадрів, які ним користувалися;
це може тимчасово збільшити загальну пам'ять понад бюджет нового pool.

Зміни світла й застосування графічних параметрів мають Undo/Redo. Прапорці
`castShadows` / `receiveShadows` геометрії працюють для вбудованих та імпортованих
моделей. Нові сцени редактора містять сонце; старі сцени автоматично не переписуються.

## 13. Реалізовані проєкти й файлові операції M4

`project.proto.json` використовує описаний вище `proto.project`, version 1.
`startupScene` — UUID сцени в `Scenes`. Сцена має sidecar `<name>.scene.json.meta`
з `format: proto.assetmeta`, `formatVersion: 1`, `kind: Scene` та `assetId`,
що дорівнює `sceneId`. EntityId при копіюванні сцени зберігаються; SceneId змінюється.
Поля SDK і registrationFile використовуються у M5, build — у M6. Налаштування
світла редактор читає зі сцени; `Config/graphics.json` задає початкову графіку Player.

Зовнішні glTF-залежності отримують `RawDependency` sidecars зі своїм `assetId`,
`sourcePath` та необов'язковим `owner` як походженням ресурсу. У source metadata
залежності мають `uri`, `sha256`, `assetId` і JSON `pointer`. Авторитетні зв'язки —
URI та типізовані UUID-посилання; походження `owner` не означає, що нова копія
залежності вже використовується моделлю.

Move/rename переносить metadata та зберігає UUID. Copy видає UUID один раз
під час планування, перенаправляє внутрішні посилання вибраної групи і зберігає
зовнішні. Redo використовує підготовлені байти з тими самими UUID. URI glTF та
JSON-частина GLB переписуються відносно нового місця; binary chunk зберігається.
Source-моделі та їхні URI-залежності залишаються в `Assets`.

Файловий журнал — `.proto/transactions/<uuid>/journal.json`,
`proto.transaction`, version 1; `before-*` / `after-*` містять дискові знімки.
Перед записом перевіряються SHA-256, типи файлів, склад папок, джерела та
посилання. Це стосується початкової операції, Undo й повторного Redo.
Перерваний redo повертається до стану перед ним; перерваний undo — до останнього
підтвердженого стану. Напрям відновлення зберігається при повторному перериванні.
Несподівана зовнішня версія зупиняє відновлення й залишає журнал для розв'язання
конфлікту замість її перезаписування.

Відновлення виконується при відкритті під єдиним writer-lock
`.proto/editor.lock`, до побудови каталогу. Звичайний refresh не видаляє журнали
живої історії. `.proto` відтворюється, якщо кеш не перенесли разом із проєктом.
Назви, шляхи та metadata перевіряються до завантаження ресурсів поза поточним
документом; reparse points відхиляються. При файловій операції спочатку
готуються кандидати стану сцени, проєкту й браузера, потім вони публікуються разом.

Історія файлів і спільних матеріалів зберігається до закриття проєкту, включно
з переходом між сценами; команди об'єктів очищаються при зміні активної сцени.
Матеріальна команда зберігає точні байти `.meta` до й після зміни. Під час
редагування властивості watcher не замінює незавершений preview. Браузер отримує
події `ReadDirectoryChangesW`, об'єднує їх і оновлює кеш після 200 ms тиші;
перелік папок не читається на кожному кадрі. У таблиці малюються видимі рядки.

## 14. Реалізовані Player і пакування M5–M6

M5 Play використовує приватний знімок сцени та pinned-файли у `.proto/play-*`.
Його model cache `ProtoCook4-win64` прив'язаний до реалізації SDK. У M6 знімок
також переносить `rawAssets`: UUID, відносний шлях, SHA-256 та кількість байтів;
однакові payloads після читання ділять незмінний буфер.

Самостійна папка M6 має `Data/runtime.json`, `proto.runtime`, version 1,
`target: windows-x64`. Маніфест містить `projectId`, `name`, `sdkBuildId`,
`executable`, `startupScene`, `scene`, `graphics`, `shaders`, `models`, `rawAssets`,
`additionalAssets`, `resources` і `files`. Ідентичність SDK має збігатися з
скомпільованим Player. Кожний immutable-файл має шлях, SHA-256 і довжину;
ресурс — UUID, тип, шлях, contentHash та список залежностей. Типи: Model,
Mesh, Material, Texture, RawData, Scene. Таблиця звіряється з payloads і сценою.

Model blob M6 — окремий portable-формат version 1: заголовок 136 байтів,
6 секцій, little-endian поля без C++ padding. Він містить геометрію, BVH,
вузли, матеріали, sampler/UV налаштування й mip-рівні RGBA. Читач перевіряє
діапазони, ID, власників матеріалів, дерево BVH і залежності. Hash текстури
в runtime виводиться з фактичних pixel/mip даних. Авторські шляхи не пакуються.

Зафіксовані межі v1: 512 MiB на immutable-файл, 64 MiB на raw-файл,
65 536 записів і 8 GiB на папку пакета. Це захист від некоректного вводу,
а не результат тесту продуктивності на таких обсягах.

Фактичний склад папки має відповідати інвентарю. Окремо дозволені змінні
`Player.log`, `Config/graphics.user.json`, його `.bak` та службові атомарні
тимчасові файли. `Data/runtime.json` перевіряється як маніфест і не хешує себе.
Поки пакет відкритий, immutable-файли й корінь утримуються від заміни.

`build.additionalAssets` приймає UUID та відносні шляхи авторських ресурсів.
Plain raw-файл отримує сталий `.meta` з `kind: RawDependency`; посилання
в маніфесті проєкту переводиться в UUID. Збережена стартова сцена, її залежності,
схема C++ і додаткові корені перевіряються до публікації нового пакета.

`proto.graphics`, version 1, читається Player. M6 додає необов'язкове поле
`textureTopMipDrop` (тип uint32, початково 0). Масштаб, VSync, camera distance,
сонячні та точкові тіні змінюють runtime RenderView без переписування сцени.
Користувацький файл записується після підготовки обох кадрових слотів і завершення
потрібних uploads. Параметри профілів наведено у [посібнику M6](M6_GUIDE.md).
