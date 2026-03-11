# Technical Review

## Scope

This review focuses on bug risk, performance risk, maintenance risk, and places where the code's current behaviour diverges from its stated design.

## High-Priority Findings

1. World generation model has drifted away from comments and older assumptions.

`ChunkStorage::generateWorld()` now allocates full Y-ranges per column, while comments and some surrounding logic still describe surface-only sparse generation. The code in `src/world/ChunkStorage.cpp` and comments in `src/world/ChunkManager.cpp` do not agree anymore. This is dangerous because later changes will be made against the wrong mental model.

Why this matters:

- Memory expectations and streaming costs are now different from the documented design.
- Future fixes in streaming and underground generation can easily target the wrong lifecycle state.
- Performance regressions will be hard to explain if comments keep describing a different pipeline.

What has been done:

- Comments and docs were aligned toward the current full-column preallocation model.
- A dedicated chunk lifecycle note now documents CPU storage state vs GPU mesh residency.

What to do next:

- Decide whether to keep full-column preallocation as the long-term model or move back toward true sparse generation.
- Add lightweight runtime counters for placeholder chunks, cached-modified chunks, ready chunks, and mesh-evicted chunks.
- Keep future streaming changes aligned with the lifecycle note so the model does not drift again.

2. Upload helpers were relying on a queue-wide synchronization stall.

`VulkanContext::endSingleTimeCommands()` previously used `vkQueueWaitIdle()`. That has now been reduced to fence-based completion for the submitted one-shot command buffer, but the path is still synchronous on the calling thread.

Why this matters:

- It hides synchronization problems during development.
- It scales badly once more uploads, texture streaming, or async resource preparation are added.
- It creates timing noise in profiling because unrelated work is forced to serialize.

What to do next:

- Keep a small upload context or per-frame transfer submission path.
- Move texture and geometry uploads away from immediate submit-and-wait where practical.
- Re-measure geometry upload and frame pacing after that change.

3. Runtime asset loading still depends on implicit process state.

The engine relies on current working directory setup to find compiled shaders and fonts. This is partially mitigated by startup code and path probing, but it is still a fragile global assumption.

Why this matters:

- Launching from a different working directory or packaging layout can break startup.
- Shader hot reload, text rendering, and compute setup do not all use the same resolution strategy.
- Startup failures become environment-specific instead of deterministic.

What to do first:

- Centralize asset path resolution behind one helper.
- Resolve from executable location or explicit asset root, not only from cwd.
- Keep one consistent policy for shaders, fonts, and logs.

4. Host-visible indirect buffers are a future scaling risk.

`ChunkRenderer` still uses CPU-visible storage/indirect buffers today, but visibility metrics are no longer derived by reading back the indirect command buffer. They now come from the renderer-owned mesh snapshot and a CPU-side frustum visibility pass, which removes the direct coupling between frame statistics and indirect-buffer readback.

Why this matters:

- It is fine at the current scale, but indirect draw data is still not in its ideal long-term memory placement for heavier scenes.
- The worst indirect-readback coupling is gone, but the draw execution path is still host-visible.
- There is still room to move toward device-local indirect buffers plus explicit summary/readback paths.

What to do first:

- Keep the renderer-owned visibility path separate from indirect-buffer readback during future culling changes.
- Consider moving draw-indirect buffers to device-local memory and keep only tiny summary/readback buffers host-visible.

5. Active chunk storage is simple but churn-heavy.

`ChunkStorage` originally kept `m_activeChunks` as a vector erased via `remove_if`. The ownership split is now in place, and active-list removal was reduced to O(1) swap-pop through a coordinate-to-index map, but the storage and render iteration layers are still only partially separated.

Why this matters:

- Tier-3 and Tier-4 streaming repeatedly touch these containers.
- The active-list churn is lower now, but render-facing iteration is still coupled to storage-maintained bookkeeping.
- Cache interception for modified chunks adds another lifecycle branch that is easy to break accidentally.

What to do first:

- Keep the ownership model around `m_chunkGrid`, `m_activeChunks`, and `m_dirtyCache` documented and in sync with the code.
- Continue keeping `ChunkRenderer` on its own compact render snapshot so culling and draw prep stay off storage-owned iteration state.

## Medium-Priority Findings

1. Logging used to append indefinitely and pollute the repo.

This has been partially cleaned up by moving metric logging into a dedicated runtime directory. The remaining step is process discipline: keep runtime logs out of source control and avoid using root-level text dumps as normal workflow artifacts.

2. Build portability was overly machine-specific.

The project used hardcoded SDK and font locations. The Makefile cleanup reduces that, but the next step is documenting the required environment variables and supported fallbacks clearly.

3. Old log files and comments still describe older behaviour.

Several tracked output files refer to previous generation strategies. They are useful as historical notes but not as current truth.

## Suggested Validation Passes

1. Run a clean build from an environment where `VULKAN_SDK` is set and one where only `glslc` is on `PATH`.
2. Launch the engine from both project root and `bin/` to verify asset resolution.
3. Regenerate a large world radius and compare RAM, generation time, and frame pacing before and after future streaming refactors.
4. Add one debug overlay line that prints current chunk lifecycle counts: allocated, generating, ready, meshed, evicted, cached-modified.