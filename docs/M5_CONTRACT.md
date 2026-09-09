# M5 implementation contract

M5 adds a public C++20 SDK, explicit behavior registration, project-specific static Player builds, schema discovery, behavior editing and isolated Play/Stop. M4 source and binaries are retained in `.cache/m5-baseline` and `build/m4-*`.

## Public SDK and scene data

The public interface is `include/Proto/Behavior.hpp` and `Id.hpp`, namespace `proto::sdk`. It contains only standard-library and SDK types. RegisterProjectBehaviors is an explicit global function. Registry descriptors use stable UUID type IDs; each scene attachment has its own stable UUID. Lifecycle callbacks receive a context exposing copies of transform/light data, properties and keyboard state. No lifecycle runs in the editor or schema discovery.

Scene v1 stores `{id,type,enabled,properties}`. Values are bool, signed integer, finite number, UTF-8 string, Vec3 `[x,y,z]`, Color `[r,g,b,a]`, EntityRef `{"entityRef": UUID-or-null}`, AssetRef `{"assetRef": UUID-or-null}`. Unknown types and property names retain these values verbatim in meaning. Numeric schema normalization accepts a JSON integer for float. Missing fields use descriptor defaults; incompatible or unknown fields block Play with a diagnostic, never silently reset. Duplicate binding IDs are invalid throughout a scene. Missing referenced entities/assets may be edited/saved but block Play. Ref wrappers allow safe dependency validation/remapping without guessing string contents.

Describe output is `proto.behaviors` formatVersion 1, behaviorApiVersion 1, sdkBuildId, types. It is produced by the same executable that will run, without creating a window/GPU or invoking lifecycle. Stable fields have name/type/default and optional min/max/label. Registration/discovery failures and incompatible build identity block launch.

## Builds and processes

Builds use a relocatable installed CMake package and the exact packaged compiler/build identity. Project Code source/header contents, SDK identity and configuration determine a content key under `.proto/builds`. An internally generated CMake project compiles Code sources plus SDK PlayerMain; user CMake files are not executed. No stale executable is a substitute for a failed build. Compiler diagnostics preserve file/line/column. Build/discovery run asynchronously with cancellation and bounded logs.

Windows child processes run in an owned kill-on-close job with redirected output. Player receives a unique named stop event. Stop signals it cooperatively, then kills only its owned process tree after a bounded grace period. Crashes/hangs return the editor to Editing with a diagnostic. Runtime and editor CPU/memory/render metrics remain separate. Process isolation is not a sandbox for arbitrary user code.

## Play and resources

Editing -> Preparing -> Running -> Stopping -> Editing. Snapshot the current scene in memory without saving/reloading/replacing the authored document; preserve dirty state, selection and Undo/Redo. Lock project editing while preparing/running/stopping; log and Stop remain available. Pause editor scene rendering while Player is running while retaining its viewport texture; throttle UI updates.

Player loads scene JSON and a validated immutable resource manifest from `.proto/snapshots/<run-id>`. Content-addressed cooked model blobs under `.proto/play-cache` include current material overrides and are shared between runs. Hold file handles denying mutation/deletion until the Player exits. Never import glTF or read mutable authored files in the Player. Typed AssetRef closure participates in snapshot dependency resolution. M5 snapshot paths are internal; portable packaging remains M6.

## Evidence required

All eight property types roundtrip, missing schemas/refs preserve data and block Play, duplicate IDs fail before mutation, Undo/SaveAs preserve bindings. Actual external project compilation/discovery, Spin and MoveLight lifecycle, keyboard changes and real runtime shadow movement are tested. Test compile/describe/runtime failures, SDK mismatch, a hung child + Stop, restart and stale-build prevention. Verify editor render count remains paused and unsaved edits remain exact after Stop. Run Debug/Release regressions and independent Astra/max review before reporting completion.

