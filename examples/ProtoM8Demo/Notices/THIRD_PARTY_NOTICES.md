# Third-party notices for the local M2 build

The application links GLFW (zlib), Dear ImGui (MIT), volk (MIT) and Vulkan
Memory Allocator (MIT). It uses Vulkan-Headers (MIT / Apache-2.0). Their
original notices are copied from the pinned source archives into `licenses/`.
M1 also uses GLM 1.0.3 (MIT option) and yyjson 0.12.0 (MIT), with their notices.
M2 adds cgltf 1.15 (MIT), stb_image 2.30 (MIT option) and MikkTSpace (zlib-style
notice). Their original notices are in cgltf.txt, stb.txt and mikktspace-notice.h.
stb_image_write, from the same pinned stb source, is used only by the test fixture generator.
The system Vulkan loader and GPU driver are provided by Windows/the GPU vendor.
Segoe UI is read from the Windows Fonts directory and is not bundled.

GCC runtime libraries use GPLv3 with the GCC Runtime Library Exception.
MinGW-w64 runtime notices, GPLv3 and the exception are included in `licenses/`.
The installed GCC toolchain is not redistributed by this project.

Development-only tools:

- glslang 16.5.0: upstream multi-component license in `glslang.txt`.
- Khronos Validation Layer from LunarG SDK 1.4.357.0: `validation.txt`,
  the `validation-*` license files and the retained `LunarG-NOTICE.txt`.
- The validation build's dependency notices include SPIRV-Tools,
  SPIRV-Headers, Vulkan-Utility-Libraries and mimalloc.

Compiler/tool binaries are kept in the ignored local cache. Only Debug output
contains the validation DLL. Release output contains neither the validation
layer nor a shader compiler. Source URLs, revisions and artifact hashes are in
`dependencies.lock.json` and `docs/DEPENDENCIES.md` in the source project.

These notices describe dependencies; they do not assign a license to Proto
Engine's own source code. This M2 editor is not the future Player/SDK package.
