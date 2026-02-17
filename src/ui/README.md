# Модуль UI (`src/ui`)

Системи користувацького інтерфейсу та рендерингу тексту.

## Ключові Компоненти

### `TextRenderer` (`TextRenderer.hpp/cpp`)
- Рендерить 2D текст поверх сцени (UI overlay).
- Використовує **Signed Distance Fields (SDF)** для чіткого тексту при будь-якому масштабуванні.
- **Persistent Vertex Buffers**: Per-frame host-visible буфери з постійним маппінгом (zero-copy upload).
- Інтегрований з `BindlessSystem` для семплювання атласу шрифту (Set 0).
- **Multi-path font loading**: Автоматично шукає шрифт у `bin/fonts/`, `fonts/`, `../bin/fonts/`.
- API: `beginFrame(frameIndex)`, `renderText(cmd, text, x, y, scale, color)`.

### `FontSDF` (`FontSDF.hpp/cpp`)
- Генерує SDF атлас текстур з TrueType шрифтів (`.ttf`).
- Використовує `stb_truetype` (`stbtt_GetGlyphSDF`) для растеризації.
- Атлас: 512×512, `VK_FORMAT_R8_UNORM`, linear sampler.
- Кешує метрики гліфів (`GlyphInfo`): UV координати, bearing, advance.
- Реєструється в `BindlessSystem` та отримує глобальний `textureID`.
- Завантаження через staging buffer → `vkCmdCopyBufferToImage`.

### `BitmapFont` (`BitmapFont.hpp`)
- Header-only утиліта для растрових шрифтів (альтернатива SDF для debug UI).

## Примітки щодо координат
- Вершини тексту генеруються в clip space `[-1, 1]`.
- `x = -0.95, y = -0.90` — лівий нижній кут екрану.
- `scale` множиться на `0.05f` для отримання розміру в clip space.

## Майбутні розширення
- `GlyphAtlas` — виділення генерації атласу в окремий клас.
- `TextVertexBuilder` — виділення побудови вершин тексту.
- Screen-space координати замість clip space.
