# Модуль Scene (`src/scene`)

Высокорівневе представлення камери та просторових об'єктів сцени.

## Ключові Компоненти

### `Camera` (`Camera.hpp/cpp`)
- FPS-камера з Pitch/Yaw обертанням (ПКМ + миша) та `WASD` рухом.
- Обчислює матриці `viewMatrix` та `projectionMatrix` (perspective, Vulkan NDC Y flip).
- `update(window, dt)` — оновлення позиції на кожному кадрі.
- `setAspectRatio(ratio)` — реакція на зміну розміру вікна.
- `getViewProjection()` — готова VP матриця для передачі у push constants.

### `Frustum` (`Frustum.hpp/cpp`)
- View Frustum для відсічення невидимої геометрії (Frustum Culling).
- `buildFromMatrix(VP)` — витягує 6 площин з View-Projection матриці.
- `isVisible(AABB aabb)` — перевірка чи AABB перетинає frustum.
- Використовується двічі:
  1. **Streaming**: `ChunkManager::updateCamera` — визначає які далекі чанки завантажити/зберегти (Tier 2/3 архітектури).
  2. **Rendering**: `ChunkRenderer::render` — відсікає невидимі чанки перед `vkCmdDrawIndexed`.

### `AABB`
- Axis-Aligned Bounding Box: `{min: Vec3, max: Vec3}`.
- Використовується Frustum для просторових перевірок чанків.
