# Модуль Scene (`src/scene`)

Високорівневе представлення 3D об'єктів та камери.

## Ключові Компоненти

### `Camera` (`Camera.hpp/cpp`)
- Представляє спостерігача у 3D світі (FPS-стиль).
- **Керування**: Pitch/Yaw обертання через мишу (ПКМ), рух через `WASD`.
- Розраховує матриці:
  - `getViewMatrix()` — матриця виду (lookAt).
  - `getProjectionMatrix()` — матриця проекції (perspective, Vulkan NDC).
- `update(window, dt)` — оновлення позиції та орієнтації на основі введення.
- `setAspectRatio(ratio)` — оновлення при зміні розміру вікна.

## Майбутні розширення
- `SceneManager` — керування списком об'єктів сцени, culling, LOD.
- `Transform` — компонент трансформації (position, rotation, scale).
- `Light` — джерела освітлення (directional, point, spot).
