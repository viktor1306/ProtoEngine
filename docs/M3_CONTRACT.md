# M3 integration contract — 2026-09-08

Implemented and verified in Debug/Release; see `M3_REPORT.md`. M0–M2 baseline is preserved in `.cache/m3-baseline`;
build outputs use `m3-debug` and `m3-release`. M2 binaries remain usable.

## Ownership

- Astra parent: renderer/shaders, `Lighting.hpp`, RenderView, integration in existing Editor files,
  CMake, GPU validation and final artifacts.
- Luna scene worker: Scene.hpp/.cpp, SceneIO.cpp, SceneDocument.hpp/.cpp, M3SceneTests.cpp.
- Luna UI worker: new EditorLighting.cpp only; existing Editor files are integrated by parent.
- Astra reviewer: independent read-only review after implementation and meaningful test evidence.

## Scene-facing API

Types in `scene/Lighting.hpp`: DirectionalLight, PointLight, LightingSettings.
EntityRecord gains optional `directionalLight` and `pointLight`; allow mesh/camera plus light,
but reject both light types on the same entity. Scene stores corresponding dense pools and exposes
`directionalLight(handle)`, `pointLight(handle)`, `directionalLights()`, `pointLights()`.
Sun rays follow world local -Z. Point radius is in world metres and is not multiplied by Transform scale.
Both kinds honor entity/parent enable state; degenerate transforms are skipped as in M2.
Multiple directional and point lights are supported; no silent truncation.

Scene and SceneSnapshot gain `LightingSettings lighting`.
SceneDocument provides `void lighting(const LightingSettings&)` with one Undo command for each applied setting change.
Light properties use existing EntityRecord edit and gesture history. Reparent, subtree delete/restore,
Save As and atomic failed-load behavior apply to lights identically to meshes.

Scene JSON remains formatVersion 1, with optional `lighting` object and optional
components `directionalLight` / `pointLight`; missing fields in old scenes use type defaults.
When lighting equals defaults it may be omitted to preserve old canonical output.
Light component JSON keys: color[3], intensity, shadows; PointLight additionally radius.
Settings keys exactly match C++ fields. Validate strict types, duplicate/unknown keys and finite values.

Validation limits: color channels 0..1; intensity 0..100000; point radius 0.1..10000;
resolution one of 256/512/1024/2048; cascades 1..4; pool MiB 1..512;
shadowDistance 1..10000; depthBias 0..0.05; normalBias 0..1; ambient 0..1.
Pool budget must fit at least one 6-layer D32 slot: resolution^2 * 4 * 6 bytes.
Invalid setting edits preserve the prior scene/history.

## Renderer commitments

- Scene lighting path: MASK-aware depth prepass, 16x16 compute tile lists, HDR accumulation,
  one final tone map. Studio preview remains an explicit compatibility/test mode for M2.
- Depth cube-array pool uses six D32 layers per shadow slot; a directional light uses up to four
  stabilized cascade layers in one slot. Point lights render six faces with radial depth.
- Unchanged maps are reused. Relevant dirty maps update before sampling in the same frame.
  Shadow keys include world transform, actual mesh revision, cast state and alpha geometry;
  light color/intensity changes do not invalidate depth maps.
- Pool capacity bounds shadow memory, not total contributing lights. More shadowed lights are
  rendered in batches into the same HDR target. Ambient/emissive/unlit terms are added once.
- A tile stores 64 indices. Overflow falls back to all lights in its batch and is counted visibly.
- Caster packets include off-camera objects; light/cascade culling is separate from camera culling.
- Shared shadow storage uses explicit fragment-read/depth-write barriers on the one queue;
  old allocations and per-frame descriptors/buffers remain alive until frame fences complete.
- HDR uses RGBA16F for a single batch and promotes to RGBA32F for multiple batches,
  preventing repeated half-float rounding from dropping dim contributions. Promotion is
  retained at the current viewport size to avoid allocation churn.
- The configured budget bounds the current shadow-pool allocation including driver alignment;
  framebuffer/HDR/asset buffers are additional. A replaced pool remains alive until its
  in-flight users complete, so a quality change can temporarily hold both generations.
- Camera depth and PBR use invariant positions; read-only depth attachments use STORE_NONE.
  Shadow sampling uses explicit LOD 0; CSM receiver-plane correction accounts for the
  nearest sample center and filtered taps. Vertex/material derivatives precede MASK termination.

## UI contract

New Editor private methods: `addLight(bool point)`, `lightInspector(EntityRecord&)`,
`lightingSettings()`. Existing fields/methods document_, renderer_, propertyGesture(), showError()
are available. Parent calls addLight from Add menu, lightInspector inside Inspector with item width
and entity ID scope already pushed; lightingSettings in its own window when `showLightingSettings_`.
Only the new EditorLighting.cpp is owned by the UI worker.
Settings are staged locally in Editor fields `lightingDraft_`, `lightingDraftScene_`; Apply invokes
document_.lighting(), Reset uses LightingSettings{}. No file writes each frame. Graphics diagnostics
will be provided by parent renderer integration.
