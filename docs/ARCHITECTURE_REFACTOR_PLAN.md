# Architecture Refactor Plan

## Goal

Create a clear order of work so the project becomes easier to fix without damaging the renderer or losing performance visibility.

## Module Breakdown

### 1. Platform and Runtime Boot

Files:

- `src/main.cpp`
- `src/core/Window.*`
- `src/core/InputManager.*`
- `src/core/Timer.*`
- `src/core/ShaderHotReloader.*`

Responsibility:

- Process startup
- Working directory setup
- Main loop
- Metrics logging
- Hot reload wiring

Current issue:

- Too much policy lives directly in `main.cpp`.
- Runtime path handling, logging, world setup, UI setup, and debug toggles are all mixed together.

### 2. Vulkan Foundation

Files:

- `src/gfx/core/VulkanContext.*`
- `src/gfx/core/Swapchain.*`
- `src/gfx/sync/*`

Responsibility:

- Device and allocator creation
- Swapchain lifecycle
- Command submission
- CPU/GPU synchronization

Current issue:

- Upload helpers still use coarse synchronization.
- Resource lifetime and frame submission are reasonably separated, but transfer behaviour is not yet mature.

### 3. GPU Resource Layer

Files:

- `src/gfx/resources/Buffer.*`
- `src/gfx/resources/Texture.*`
- `src/gfx/resources/Mesh.*`
- `src/gfx/resources/GeometryManager.*`

Responsibility:

- Buffer/image ownership
- Geometry pool allocation
- Upload batching
- Delayed frees

Current issue:

- This layer is strong, but it is doing double duty as both allocator and implicit upload system.
- It needs a cleaner contract with chunk meshing and render scheduling.

### 4. Render Execution Layer

Files:

- `src/gfx/rendering/Renderer.*`
- `src/gfx/rendering/Pipeline.*`
- `src/gfx/rendering/RenderPassProvider.*`
- `src/gfx/rendering/BindlessSystem.*`
- `src/gfx/rendering/DebugRenderer.*`

Responsibility:

- Frame orchestration
- Pipeline creation
- Render pass sequencing
- Descriptor systems

Current issue:

- Render flow is fairly clean, but shader/path policy leaks in from startup and feature code.
- Visibility metrics and render execution are still somewhat coupled through chunk rendering state.

### 5. World Data and Generation

Files:

- `src/world/Chunk.*`
- `src/world/ChunkStorage.*`
- `src/world/BlockType.hpp`
- `src/world/VoxelData.hpp`

Responsibility:

- Terrain generation
- Voxel storage
- Chunk persistence
- Block definitions

Current issue:

- This is where behaviour changed the most recently.
- Generation policy, sparse/full allocation, cache restore, and streaming expectations need a sharper boundary.

### 6. World Streaming and Meshing

Files:

- `src/world/ChunkManager.*`
- `src/world/ChunkRenderer.*`
- `src/world/LODController.*`
- `src/world/MeshWorker.hpp`

Responsibility:

- Chunk lifecycle decisions
- Async generation and meshing
- LOD assignment
- GPU mesh upload and culling

Current issue:

- This is the most valuable subsystem, but also the most fragile one.
- Storage state, render state, eviction state, and worker queue state all interact here.

### 7. UI and Diagnostics

Files:

- `src/ui/*`

Responsibility:

- ImGui controls
- Text rendering
- Debug visibility

Current issue:

- UI is doing real engine control, not just display.
- That makes it powerful, but it also means tuning code and core world config are tightly coupled.

## Refactor Order

### First: Runtime Path and Config Layer

Why first:

- It is low-risk.
- It stabilizes startup and tooling.
- It removes a whole class of false negatives during debugging.

Tasks:

1. Centralize asset root, log root, and shader binary resolution.
2. Move runtime/logging setup out of `main.cpp` into a small bootstrap helper.
3. Document required environment variables for build and launch.

### Second: Chunk Lifecycle Documentation and Cleanup

Why second:

- The project's hardest bugs will live here.
- Right now the biggest risk is not raw performance, but misunderstandings of state transitions.

Tasks:

1. Write down the chunk states and transitions in one place.
2. Define exactly what `UNGENERATED`, `GENERATING`, `READY`, `LOD_UNASSIGNED`, and `LOD_EVICTED` mean.
3. Align comments in `ChunkStorage`, `ChunkManager`, and `ChunkRenderer` with real behaviour.

### Third: Separate Storage Ownership from Render Ownership

Why third:

- It will make future fixes much safer.
- Right now storage and render code know too much about each other's internal timing.

Tasks:

1. Keep storage responsible for voxel/chunk lifetime only.
2. Keep renderer responsible for mesh lifetime only.
3. Introduce a narrow handoff object or event path for “chunk became ready”, “chunk mesh invalid”, and “chunk evicted”.

Status:

- Started and materially advanced.
- `ChunkStorage` now owns chunk lifetime through a registry plus lightweight active list.
- Active-list removal is O(1) via coordinate-to-index bookkeeping.
- `ChunkRenderer` now keeps a compact renderer-owned mesh snapshot for culling, indirect prep, and renderer-side LOD stats instead of iterating storage-owned active chunks.

### Fourth: Replace Queue-Wide Upload Waits

Why fourth:

- This is a focused performance and correctness improvement.
- It should happen after lifecycle cleanup so the submission model is easier to reason about.

Tasks:

1. Replace one-shot upload `vkQueueWaitIdle()` usage with fences.
2. Add upload timing counters before and after the change.
3. Confirm no hidden resource lifetime bug was being masked by queue-idle serialization.

Status:

- The queue-wide wait has already been replaced with per-submit fence waiting.
- The path is still synchronous on the calling thread, so a fuller transfer/upload context remains future work.

### Fifth: Decouple Metrics Collection from Hot Render Paths

Why fifth:

- Your profiling is already useful, so the next step is making it more correct and less invasive.

Tasks:

1. Keep frame counters and visibility summaries separate from indirect command storage.
2. Write per-run logs into dedicated runtime files only.
3. Define a compact performance report format for regression checks.

Status:

- Started.
- Runtime metrics already write to a dedicated per-run log file.
- Visibility metrics now come from the renderer-owned mesh snapshot and a CPU-side visibility pass rather than CPU scanning of the indirect command buffer.
- The remaining larger step is moving draw-indirect storage itself toward a cleaner long-term memory placement.

## What Not To Refactor First

1. Do not start by rewriting the greedy mesher. It is too core and too easy to regress.
2. Do not start by replacing bindless or MDI. Those are advanced parts, but they are not the current source of confusion.
3. Do not start by redesigning the UI. It is noisy, but not the biggest structural risk.

## First Fixing Sprint

If the goal is to begin fixing immediately, this is the best first sprint:

1. Finish runtime cleanup and path centralization.
2. Rewrite chunk lifecycle comments and add one design note.
3. Measure generation time, RAM, and frame time after a clean rebuild.
4. Only then touch streaming logic or upload synchronization.