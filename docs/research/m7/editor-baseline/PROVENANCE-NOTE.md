# Excluded Editor reference

Do not use this directory for before/after performance claims.

The attempt to restore M6 algorithm source files preserved their older file
timestamps. The incremental build log (`.cache/m7-editor-baseline-build.log`)
shows that it rebuilt PlayerApp and Editor objects but did not rebuild
AssetRenderer, LightingMath, or SceneViewBuilder. Those objects retained the
previous optimized implementation. The saved source snapshot/SDK identity
therefore does not establish the binary's actual algorithm composition.

The measurements themselves are retained as historical data, not a valid M6
Editor reference. A replacement reference must use a new source/build directory
and compile every dependent object. This issue does not affect the original
runtime benchmark baseline, which was built in the new M7 build directory
before any optimization, or the final native benchmark's V2 source state.
