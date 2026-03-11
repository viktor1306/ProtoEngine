# Модуль World (`src/world`)

Модуль відповідає за генерацію, підтримання та оптимізований рендеринг воксельного світу.

## Ключові Компоненти

### `Chunk` (`Chunk.hpp/cpp`)
- Базова одиниця світу розміром `32×32×32` вокселів.
- **Генерація**: Процедурне заповнення на основі OpenSimplex2 шуму (FastNoiseLite). Оптимізовано за допомогою **білінійної інтерполяції 2D карти висот** (рендер 81 семплів замість 1024 на чанк), що прискорює генерацію в понад 12 разів.
- **Greedy Meshing**: Алгоритм стиснення 3D сітки — об'єднує суміжні однакові грані в один прямокутник. Десятки раз зменшує кількість вершин.
- **Closed Chunk Meshes & Skirts**: Кожен чанк формує "закриту коробку" — між-чанковий culling оптимізовано, а для суміжних LOD-різниць додано "спідниці" (skirts), що витягують геометрію вниз, закриваючи щілини.
- **Ambient Occlusion**: 4 AO-значення на вершину (аналіз 27 сусідів через `volumeCache`).

### `ChunkStorage` (`ChunkStorage.hpp/cpp`)
- Зберігає воксельні дані для **всіх** чанків світу у плоскому масиві (пам'ять виділяється паралельно багатопотоково для пришвидшення Zero-Page Faults в ОС).
- Поточна модель: `generateWorld()` одразу виділяє та заповнює **всі зайняті Y-slices** у межах кожної `(cx, cz)` колонки. Це не surface-only sparse storage.
- Після Tier-4 eviction чанки можуть бути відновлені як `UNGENERATED` placeholders і догенеровуватись асинхронно під час повторного входу в зону стрімінгу.
- `generateWorld(radiusX, radiusZ, seed)` — заповнює фіксовану сітку `m_chunkGrid`.
- `createChunkIfMissing(cx, cy, cz, seed, renderer)` — re-creates повністю видалені чанки або відновлює modified чанки з RAM cache.
- `getSurfaceBounds(cx, cz)` / `getSurfaceMidY(cx, cz)` — історична назва; фактично це межі **зайнятого chunk-column span**, а не лише поверхні.
- Надає геттери меж світу: `getMinX/MaxX/MinZ/MaxZ`.

### `ChunkManager` (`ChunkManager.hpp/cpp`)
- Координує streaming, LOD, Progressive Generation та рендеринг.
- Координує два окремі життєві цикли: CPU voxel storage (`ChunkState`) та GPU mesh residency (`m_currentLOD`, `LOD_EVICTED`).
- **Demand-Driven Rehydration**: Слідкує за chunk placeholders, які повертаються після стрімінгу, і пріоритизує їх генерацію біля камери або під гравцем.
- **Два незалежні параметри (ImGui слайдери):**
  - **Sphere Radius** (`m_unloadRadius`) — сфера навколо камери, де чанки завжди завантажені.
  - **Camera View Dist** (`m_frustumRadius`) — дальність завантаження чанків у напрямку погляду камери.
- **Streaming In** — два підходи:
  - *Zone 1 (сфера)*: grid loop в межах `m_unloadRadius`, `createChunkIfMissing` для нових колонок.
  - *Zone 2 (frustum)*: ітерує **grid bounds світу** (не лише existing chunks), `createChunkIfMissing` для всіх колонок у frustum між `m_unloadRadius` і `m_frustumRadius`. Це дозволяє re-load чанки видалені Tier 4.
- **Streaming Out + LOD — тришарова архітектура:**

  | Tier | Умова | Дія |
  |------|-------|-----|
  | 1 | `dist ≤ sphereRadius` | вокселі + GPU меш |
  | 2 | `dist ≤ frustumRadius` AND у frustum | вокселі + GPU меш |
  | 3 | `dist ≤ frustumRadius` NOT у frustum | вокселі в RAM, GPU меш звільнено |
  | 4 | `dist > frustumRadius` | вокселі + GPU меш звільнено |

- Tier 3 звільняє лише GPU mesh і ставить `LOD_EVICTED`.
- Tier 4 видаляє chunk object зі storage; modified chunks перед цим перехоплюються в `m_dirtyCache`.

### `ChunkRenderer` (`ChunkRenderer.hpp/cpp`)
- Асинхронна побудова GPU мешів через `MeshWorker` (N потоків).
- Тримає власний компактний `render snapshot` для mesh-resident чанків; culling, indirect draw prep і renderer-side LOD stats більше не ітерують storage-owned `m_activeChunks`.
- `markDirty(cx, cy, cz)` → `flushDirty()` → `rebuildDirtyChunks()` — pipeline побудови.
- `removeChunk(key)` — звільняє лише GPU меш (GeometryManager free-list), не торкається ChunkStorage.
- `cull(...)` — CPU-driven frustum filtering і підготовка indirect draw команд з renderer-owned snapshot.
- `renderCamera(...)` / `renderShadow(...)` — виконують MDI draw calls для camera/shadow pass.
- Видимість для metrics тепер рахується з renderer-owned snapshot, а не через CPU readback indirect command buffer.

### `LODController` (`LODController.hpp/cpp`)
- Обчислює LOD `0/1/2` для кожного чанку за Евклідовою дистанцією до камери.
- Гістерезис запобігає миготінню між рівнями на межі зон.
- Параметри: `m_lodDist0`, `m_lodDist1` — налаштовуються в реальному часі через ImGui.

### `MeshWorker` (`MeshWorker.hpp`)
- **Priority-Based Async Generation**: Використовує два паралельні Lock-Free Ring Buffers:
  - `m_ringHigh`: Для поверхневих чанків високого пріоритету та підземного фечінгу під час падіння/копання.
  - `m_ringLow`: Для фонової генерації віддалених чанків.
- Підтримує два типи завдань: `GENERATE` (для математики вокселів) та `MESH` (для Greedy Meshing).
- Кожен потік має власний інстанс `FastNoiseLite`, що зводить накладні витрати на ініціалізацію шуму до абсолютної норми 0%.

---

## Формат `VoxelVertex`
Упаковано у **8 байт** на вершину (vs стандартних 48):
- `x, y, z` — uint8×3
- `normalDir + faceID` — uint8 (упаковано)
- `paletteIndex` — uint8 (індекс кольору)
- `ao0..ao3` — uint8×4 (AO-фактори кожного кута)
