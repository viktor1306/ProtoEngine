# Модуль Graphics (`src/gfx`)

Це серце рушія, що реалізує сучасний конвеєр рендерингу на базі Vulkan 1.3.

## Структура підмодулів

```
src/gfx/
├── core/           Vulkan ядро (ініціалізація, swapchain)
├── resources/      GPU ресурси (буфери, меші, текстури)
├── rendering/      Рендеринг (renderer, pipeline, bindless, passes)
├── sync/           Синхронізація (команди, семафори, фенси)
└── *.hpp           Forwarding headers для зворотної сумісності
```

---

## `gfx/core/` — Vulkan Ядро

### `VulkanContext`
- Ініціалізує `VkInstance`, `VkDevice`, `VkQueue` та `VmaAllocator`.
- Вмикає необхідні розширення: `VK_KHR_dynamic_rendering`, `VK_KHR_synchronization2`, `VK_KHR_buffer_device_address`.
- Надає утиліти: `beginSingleTimeCommands`, `endSingleTimeCommands`, `createBuffer`, `createImage`.

### `Swapchain`
- Керує `VkSwapchainKHR`, зображеннями для відображення та буферами глибини.
- Обробляє зміну розміру вікна та перестворення ланцюжка.
- Надає формати кольору та глибини для конфігурації пайплайнів.

---

## `gfx/resources/` — GPU Ресурси

### `Buffer`
- RAII обгортка для `VkBuffer` + `VmaAllocation`.
- Підтримує `VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT` (BDA).
- Методи: `upload()`, `getBuffer()`, `getDeviceAddress()`.

### `Mesh`
- Легкий дескриптор геометрії (зміщення в глобальному буфері).
- Зберігає: `indexCount`, `firstIndex`, `vertexOffset`.
- Метод `draw(VkCommandBuffer)` — один виклик `vkCmdDrawIndexed`.

### `Texture`
- RAII обгортка для `VkImage` + `VkImageView` + `VkSampler`.
- Реєструється в `BindlessSystem` та отримує глобальний `textureID`.
- Підтримує: завантаження з файлу, процедурна генерація (checkerboard).

### `GeometryManager`
- **Monolithic Buffers**: Вся геометрія в одному Vertex/Index буфері (VMA, GPU_ONLY).
- Розміри: Vertex 10 МБ, Index 2 МБ.
- `uploadMesh()` — копіює через staging буфер з Sync2 бар'єрами.
- `bind()` — одна прив'язка для всієї геометрії сцени.
- Структура `Vertex`: Position(3) + Normal(3) + Color(3) + UV(2) = **48 байт**.

---

## `gfx/rendering/` — Рендеринг

### `Renderer`
- Керує циклом рендерингу: `beginFrame()` → passes → `endFrame()`.
- Делегує синхронізацію до `SyncManager`, команди до `CommandManager`, проходи до `RenderPassProvider`.
- `reloadShaders()` — безпечне перестворення пайплайнів через `vkDeviceWaitIdle`.
- Надає: `getDescriptorSetLayout()`, `getDescriptorSet()`, `getBindlessSystem()`, `getSwapchain()`.

### `Pipeline`
- Обгортка для `VkPipeline` + `VkPipelineLayout`.
- Конфігурується через `PipelineConfig`: шляхи шейдерів, cull mode, depth test, blend, формати.
- Підтримує Dynamic Rendering (без `VkRenderPass`).
- `readFile()` — завантаження SPIR-V відносно поточної робочої директорії.

### `BindlessSystem`
- Керує глобальними наборами дескрипторів.
- **Set 0**: Shadow map sampler (для main pass).
- **Set 1**: Bindless textures (`sampler2D textures[]`) + Object SSBO (`objects[]`).
- `registerTexture()` / `unregisterTexture()` — динамічна реєстрація.
- `updateObject()` — оновлення Model Matrix та Texture ID per-frame.
- Double buffering: окремі SSBO для кожного frame-in-flight.

### `RenderPassProvider`
- Інкапсулює `vkCmdBeginRendering` / `vkCmdEndRendering`.
- **Shadow Pass**: Depth-only, 2048×2048, `VK_FORMAT_D32_SFLOAT`, VMA allocation.
- **Main Pass**: Color + Depth, viewport = swapchain extent.
- Надає shadow image view для реєстрації в дескрипторах.

### `ImageUtils` (header-only)
- `transitionImageLayout()` — inline утиліта для переходів layout через Sync2 (`VkImageMemoryBarrier2`).

---

## `gfx/sync/` — Синхронізація

### `CommandManager`
- RAII керування `VkCommandPool` та `VkCommandBuffer` per-frame.
- `begin(frameIndex)` — скидає та починає запис команд.
- `get(frameIndex)` — повертає активний command buffer.

### `SyncManager`
- Керує семафорами (`imageAvailable`, `renderFinished`) та фенсами (`inFlight`) per-frame.
- `submit()` — `vkQueueSubmit2` з Sync2 структурами.
- `present()` — `vkQueuePresentKHR`.
- `waitForFence()` / `resetFence()` — синхронізація CPU-GPU.

---

## Forwarding Headers (`gfx/*.hpp`)

Для зворотної сумісності з кодом, що включає старі шляхи:
```cpp
// gfx/Renderer.hpp
#pragma once
#include "gfx/rendering/Renderer.hpp"
```
Аналогічно для: `Buffer`, `Mesh`, `Texture`, `Pipeline`, `BindlessSystem`,
`GeometryManager`, `Swapchain`, `VulkanContext`, `CommandManager`, `SyncManager`,
`RenderPassProvider`, `ImageUtils`.
