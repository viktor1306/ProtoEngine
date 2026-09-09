# M4 implementation contract

M4 work uses `m4-debug` / `m4-release`. M3 binaries and example source projects
are preserved. The source baseline is `.cache/m4-baseline`.

## Ownership and modules

- Parent Astra: FileTransactions, Editor integration, CMake, native verification
  and final report.
- Luna asset worker: AssetFilePlanner, project-scoped glTF URI resolution,
  associated resource-graph tests; no direct filesystem mutations in planner.
- Luna gizmo/document worker: GizmoMath, EditorGizmo, SceneDocument history
  and project-bound loading, plus targeted tests.
- Luna project worker: ProjectSession, native directory watcher and tests.
- Astra reviewer: independent actual-code and test review before completion.

## Project layout

`project.proto.json`, `Assets/`, `Scenes/`, `Code/`, `Config/`, `.proto/`.
The project manifest follows DATA_FORMATS.md's documented v1 shape; startupScene
is a Scene UUID. M4 does not implement SDK, Player, build packaging or behaviors.
Scenes receive Scene sidecars; source models retain M2 ModelSource metadata.
Scene creation/opening and asset imports are constrained to this project.
Standalone M1–M3 scene diagnostics remain supported.

## Filesystem plan API

`project/FileTransactions.hpp` defines FileKind, FileStamp, FileEdit, FileGuard,
PathMove and FilePlan. `FileTransaction::prepare` stages before/after versions;
`redo` initially applies it, `undo` restores originals, another redo reuses the
same staged payloads/UUIDs. File bytes stay on disk, with bounded history RAM.

Planner returns every affected authored path exactly once in `edits`. A File
edit has either `text` bytes or a project-relative `source` copied to staging.
Missing means removal; Directory means an explicitly present directory.
Directory removals never recursively delete unplanned contents. All parsed JSON
and topology assumptions have guards. `moves` describes live editor path updates.

Paths are UTF-8 relative paths; Windows case aliases, reserved/device names,
ADS, absolute paths, escapes and reparse points are rejected. `.proto`, sidecars,
backup/lock files and project control roots are not direct browser-operation targets.
Source models and their URI dependencies stay inside `Assets`.

Moving preserves source, scene and subasset UUIDs. Copying assigns resource UUIDs
once, remaps typed references inside the selected group, and retains references
outside the group. Redo never issues another UUID. Metadata travels with its source.
glTF URI paths are resolved from the model's directory and rewritten relative to
the new model location. External import keeps its existing strict source-folder
boundary; in-project reload may resolve siblings within the project's Assets root.

Each journal is `.proto/transactions/<uuid>/` with flushed staged bytes, expected
hashes, intent/progress and commit state. Interrupted redo rolls back; interrupted
undo rolls forward to the last committed state. Recovery refuses to overwrite
unexpected externally changed data. Before Undo/Redo, all affected paths and
directory membership are checked before any mutation.

## UI / history

The bottom browser shows cached directory entries, breadcrumbs, selection and
copy/cut/paste/rename/move/new-folder commands with visible name conflicts.
Scene/file commands share Undo order. File and shared-material commands survive
active scene switches inside the same project; entity/property commands belong
to the active scene and are cleared on switching it. History ends when the project
session closes. A file operation must update the
active document path without turning a move into Save As or losing unsaved edits.
Filesystem change notifications trigger refresh; there is no directory scan per frame.
Recovery is performed only while opening a project, before live history exists.
Editor, browser and session candidates are validated before any live state is
published. Project scene loading validates the exact scene/sidecar bytes and
project boundary before reading a model catalog.

Model reload writes derived cooked bytes and their integrity hashes only to
`.proto/cache`. It must not rewrite authored metadata as a side effect of file
Undo reconciliation. Material commands retain exact before/after metadata bytes.
Directory operations reject hidden backup/lock/reparse entries during preflight;
they do not silently discard or partially move those unplanned entries.

Translation uses world axes and inverse-parent conversion. Rotation and scaling
use local axes to preserve TRS under nonuniform parents. One drag is one command;
Escape cancels. Gizmos cannot simultaneously trigger scene picking/camera drag.

## Acceptance

Create/open/reopen project; file UI; move/rename/copy plus Undo/Redo/save/reopen;
copy group with both internal/external asset dependencies; stable Redo UUIDs;
glTF and external-reference GLB URI rewrites; conflicts after external edits;
subprocess interruption between journal steps; UTF-8/spaces/case-only rename;
traversal/reparse rejection; native gizmo gesture/cancel and parent transforms;
M0–M3 regressions and Vulkan validation in Debug/Release.
