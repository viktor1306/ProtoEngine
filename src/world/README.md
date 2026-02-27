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
- **Progressive Generation**: Ініціалізація світу виділяє лише "поверхневі" чанки (Surface Chunks). Підземні чанки генеруються на вимогу.
- `generateWorld(radiusX, radiusZ, seed)` — заповнює фіксовану сітку `m_chunkGrid`.
- `createChunkIfMissing(cx, cy, cz, seed, renderer)` — re-creates чанки видалені стрімінгом.
- `getSurfaceBounds(cx, cz)` / `getSurfaceMidY(cx, cz)` — обчислюють Y-межі стовпця чанків (Heightmap Pre-pass).
- Надає геттери меж світу: `getMinX/MaxX/MinZ/MaxZ`.

### `ChunkManager` (`ChunkManager.hpp/cpp`)
- Координує streaming, LOD, Progressive Generation та рендеринг.
- **Demand-Driven Fetching**: Слідкує за Y-координатою гравця під час копання. Якщо гравець наближається до дна поточного чанка, створюється Lock-Free Boost запит до пулу потоків для генерації підземних чанків.
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

### `ChunkRenderer` (`ChunkRenderer.hpp/cpp`)
- Асинхронна побудова GPU мешів через `MeshWorker` (N потоків).
- `markDirty(cx, cy, cz)` → `flushDirty()` → `rebuildDirtyChunks()` — pipeline побудови.
- `setLOD` / `getLOD` — управління LOD-рівнем per chunk.
- `removeChunk(key)` — звільняє лише GPU меш (GeometryManager free-list), не торкається ChunkStorage.
- `render(cmd, layout, frustum)` — AABB frustum culling під час рендерингу (окрема логіка від стрімінгу!).

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
