# Chunk Lifecycle

## Current Model

ProtoEngine currently uses a split lifecycle:

- CPU voxel storage lifecycle is owned by `ChunkStorage` and tracked by `Chunk::m_state`.
- GPU mesh lifecycle is owned by `ChunkRenderer` and tracked by `Chunk::m_currentLOD` plus renderer-side mesh data.

This means a chunk can be `READY` on the CPU side while having no GPU mesh yet, or while its mesh is intentionally evicted.

## CPU Storage States

### `UNGENERATED`

- A chunk object exists, but voxel payload is not ready.
- This state is mainly used for chunks recreated after stream-out, not for the initial world bootstrap.
- `ChunkRenderer` must not mesh this chunk.

### `GENERATING`

- A worker owns `fillTerrain()` for this chunk.
- Neighbour queries should still treat this chunk as non-ready.
- Meshing must stay blocked until the worker flips the state to `READY`.

### `READY`

- Voxel payload is valid.
- The chunk can participate in neighbour visibility checks and greedy meshing.
- This says nothing about whether a GPU mesh currently exists.

## GPU / Render States

### `LOD_UNASSIGNED` (`-1`)

- CPU voxel data exists, but `ChunkManager` has not assigned a render LOD yet.
- Common right after `generateWorld()` for distant chunks, or right after a chunk is restored from cache.

### `LOD_EVICTED` (`-2`)

- CPU voxel data is intentionally kept.
- GPU mesh was freed by Tier-3 stream-out because the chunk is outside the keep-sphere and outside the current frustum.
- When the chunk becomes relevant again, `ChunkManager` treats it like an unassigned mesh target and rebuilds render data.

### `0`, `1`, `2`, ...

- A render LOD has been assigned.
- A mesh may already exist or may be queued for rebuild, depending on dirty state.

## Ownership Boundaries

### `ChunkStorage`

- Owns `m_chunkGrid` and `m_activeChunks`.
- Decides whether a chunk object exists in RAM.
- Captures modified chunks in `m_dirtyCache` before Tier-4 removal.

### `ChunkRenderer`

- Owns GPU mesh objects and mesh build queues.
- Owns a compact render snapshot used for culling, indirect command generation, and renderer-side LOD statistics.
- Can drop mesh residency without deleting CPU voxel data.
- Must never assume `READY` implies a mesh already exists.

### `ChunkManager`

- Decides when chunks enter or leave the active streaming set.
- Bridges CPU storage state to render state.
- Assigns LOD, triggers mesh rebuilds, and prioritizes placeholder rehydration near the camera.

## Initial World Boot

`ChunkStorage::generateWorld()` currently preallocates and fills all occupied Y-slices in each `(cx, cz)` column within conservative terrain bounds.

Implications:

- The initial world is not surface-only sparse storage.
- “Underground generation” during `updateCamera()` is now mostly a rehydration path for streamed-out chunks.
- Comments or tools that still assume only surface chunks exist after bootstrap are outdated.

## Stream-Out / Restore Rules

### Tier 3

- Chunk stays in storage.
- GPU mesh is unloaded.
- `m_currentLOD` becomes `LOD_EVICTED`.
- The renderer-owned render snapshot entry is removed together with the GPU mesh residency.

### Tier 4

- Chunk object is removed from storage.
- If `m_isModified == true`, the chunk is moved into `m_dirtyCache` first.
- Re-entry later recreates either:
  - the cached modified chunk, or
  - a fresh `UNGENERATED` placeholder that will run `fillTerrain()` asynchronously.

## Practical Rule

When debugging chunk behaviour, always ask two separate questions:

1. Does CPU voxel data exist and is it `READY`?
2. Does the renderer currently keep a GPU mesh for it, and at what LOD?

Most confusion in the current code comes from mixing those two answers into one mental model.

## Current Renderer Metrics Path

- Frustum/visibility statistics now come from the renderer-owned mesh snapshot and a CPU-side visibility pass, not from scanning the indirect command buffer on the CPU.
- Indirect commands are still the execution path for drawing, but metrics collection is now a separate concern from indirect-buffer readback.
- This makes future migration to device-local indirect buffers much safer because frame stats are no longer tied to indirect-buffer readback.