# M6 — portable Windows projects and Player graphics

M6 implements the agreed standalone-folder build and usable Player graphics
settings. M5 source is retained in .cache/m6-baseline and its Debug/Release
binaries remain in build/m5-*. All M6 builds use build/m6-*.

## Export

Export compiles saved project Code and packages the saved startup scene. The
editor requires saving current scene/code changes before export; Play retains
its separate unsaved-snapshot workflow. The package includes the startup
dependency closure and explicit build.additionalAssets, never assets inferred
from C++ text or every file in the editor's loaded catalog.

The folder contains the project's executable, Data/runtime.json,
Data/scenes/<uuid>.scene.json, content-addressed model/data blobs, Data/shaders,
Config/graphics.json, licenses/notices and any required non-system DLLs from
the known SDK. Windows and Vulkan driver components remain system prerequisites.
PE imports are inspected. No compiler, importer, editor interface or authored
paths are required to execute the folder.

Portable model blobs have explicit versioned headers, section ranges and
little-endian fields. Native C++ struct padding and the M5 compiler-specific
cooked cache are not an export format. Resource records retain UUID, type,
content hash, blob path and dependencies. Decode validates sizes, ranges,
references and content before publication to the runtime scene.

Additional regular data files are immutable resident bytes. C++ receives a
read-only view via BehaviorContext::GetAssetBytes(AssetId); there is no per-frame
file reading in that API. Model/subasset dependencies are prepared for both Play
and exported Player. Missing references block the build with a diagnostic.

A new export is staged beside its output, fully checked and then published.
Failure/cancellation preserves the previous usable output. Replacing an existing
directory requires an identified package of this project; unrelated directories
are not overwritten. The previous successful build is retained for recovery.

## Runtime and graphics

With no snapshot argument, Player finds Data/runtime.json relative to its own
executable, independently of its working directory. --validate-package performs
package/schema checks without a window, GPU or behavior callbacks.

The lightweight native F1 settings panel has Apply/Cancel. Settings include
Low/Balanced/High/Custom profiles, render scale, VSync, view distance, directional
and point shadow quality, independent shadow resolutions/budgets and top texture
mip removal. Values are applied to rendering, not merely stored. Point-shadow
pool overflow continues through additional lighting batches; quality is never
silently reduced. Existing scene lighting remains intact.

Config/graphics.json contains packaged defaults. User changes use the separate
Config/graphics.user.json, outside the immutable-file inventory. Invalid or
unwritable user settings produce a diagnostic while preserving a valid running
configuration. No global Windows settings or AppData changes are necessary.

## Completion evidence

Test portable model roundtrip and malformed payloads; missing/inconsistent
resources, explicit additional files/models, compile failure/cancellation and
failed output publication; SDK/schema mismatch; immutable package integrity.
Run the exported executable from another working directory and a copied folder
with compiler/editor paths absent from its environment. Check actual graphics
changes, persistence, dynamic shadows and Vulkan validation. Repeat relevant
M0-M5 regressions and obtain an independent Astra/max review.

A clean second Windows host is a separate compatibility check. If unavailable,
the report must limit its conclusion to local relocation/environment tests.
Broad scaling and performance optimization remain M7.
