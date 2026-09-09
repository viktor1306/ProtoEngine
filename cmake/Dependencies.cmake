include(FetchContent)
file(READ "${PROJECT_SOURCE_DIR}/dependencies.lock.json" PROTO_LOCK)
set(PROTO_CACHE "${PROJECT_SOURCE_DIR}/.cache" CACHE PATH "Local dependency/tool cache")

function(proto_source name)
    string(JSON repo GET "${PROTO_LOCK}" sources ${name} repository)
    string(JSON revision GET "${PROTO_LOCK}" sources ${name} revision)
    string(JSON hash GET "${PROTO_LOCK}" sources ${name} sha256)
    set(source "${PROTO_CACHE}/src/${name}-${revision}")
    set(ready "${source}/.proto-${hash}.ready")
    # FetchContent's download stamps belong to each binary directory. Without
    # this shared marker, a new preset can delete/re-extract headers while an
    # existing preset is compiling against them. Publish each revision once.
    file(MAKE_DIRECTORY "${PROTO_CACHE}/locks")
    file(LOCK "${PROTO_CACHE}/locks/${name}-${revision}.lock" GUARD FUNCTION TIMEOUT 60)
    if(EXISTS "${ready}")
        set(${name}_SOURCE_DIR "${source}" PARENT_SCOPE)
        return()
    endif()
    set(archive "${PROTO_CACHE}/downloads/${name}.zip")
    if(EXISTS "${archive}")
        set(url "${archive}")
    else()
        set(url "https://codeload.github.com/${repo}/zip/${revision}")
    endif()
    FetchContent_Declare(${name}
        URL "${url}" URL_HASH "SHA256=${hash}"
        SOURCE_DIR "${source}"
        DOWNLOAD_EXTRACT_TIMESTAMP TRUE
        # Dependencies are configured explicitly below, only when needed.
        SOURCE_SUBDIR __proto_do_not_add_upstream_targets)
    FetchContent_MakeAvailable(${name})
    file(WRITE "${ready}" "${hash}\n")
    set(${name}_SOURCE_DIR "${${name}_SOURCE_DIR}" PARENT_SCOPE)
endfunction()

foreach(dep glfw imgui vulkan_headers volk vma glm yyjson cgltf stb mikktspace)
    proto_source(${dep})
endforeach()

add_library(proto_import_deps INTERFACE)
target_include_directories(proto_import_deps SYSTEM INTERFACE "${cgltf_SOURCE_DIR}" "${stb_SOURCE_DIR}" "${mikktspace_SOURCE_DIR}")
add_library(proto_mikktspace STATIC "${mikktspace_SOURCE_DIR}/mikktspace.c")
target_link_libraries(proto_import_deps INTERFACE proto_mikktspace)
configure_file("${cgltf_SOURCE_DIR}/LICENSE" "${CMAKE_BINARY_DIR}/bin/licenses/cgltf.txt" COPYONLY)
configure_file("${stb_SOURCE_DIR}/LICENSE" "${CMAKE_BINARY_DIR}/bin/licenses/stb.txt" COPYONLY)
configure_file("${mikktspace_SOURCE_DIR}/mikktspace.h" "${CMAKE_BINARY_DIR}/bin/licenses/mikktspace-notice.h" COPYONLY)

add_library(proto_math INTERFACE)
target_include_directories(proto_math SYSTEM INTERFACE "${glm_SOURCE_DIR}")
target_compile_definitions(proto_math INTERFACE GLM_FORCE_DEPTH_ZERO_TO_ONE GLM_ENABLE_EXPERIMENTAL)
add_library(proto_json STATIC "${yyjson_SOURCE_DIR}/src/yyjson.c")
target_include_directories(proto_json SYSTEM PUBLIC "${yyjson_SOURCE_DIR}/src")
configure_file("${glm_SOURCE_DIR}/copying.txt" "${CMAKE_BINARY_DIR}/bin/licenses/glm.txt" COPYONLY)
configure_file("${yyjson_SOURCE_DIR}/LICENSE" "${CMAKE_BINARY_DIR}/bin/licenses/yyjson.txt" COPYONLY)

file(COPY "${PROJECT_SOURCE_DIR}/third_party/licenses/" DESTINATION "${CMAKE_BINARY_DIR}/bin/licenses")
configure_file("${PROJECT_SOURCE_DIR}/third_party/NOTICES.md" "${CMAKE_BINARY_DIR}/bin/THIRD_PARTY_NOTICES.md" COPYONLY)

set(GLFW_BUILD_DOCS OFF CACHE BOOL "" FORCE)
set(GLFW_BUILD_TESTS OFF CACHE BOOL "" FORCE)
set(GLFW_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
set(GLFW_INSTALL OFF CACHE BOOL "" FORCE)
set(BUILD_SHARED_LIBS OFF CACHE BOOL "" FORCE)
add_subdirectory("${glfw_SOURCE_DIR}" "${CMAKE_BINARY_DIR}/deps/glfw" EXCLUDE_FROM_ALL)

add_library(proto_volk STATIC "${volk_SOURCE_DIR}/volk.c")
target_include_directories(proto_volk SYSTEM PUBLIC "${volk_SOURCE_DIR}" "${vulkan_headers_SOURCE_DIR}/include")
target_compile_definitions(proto_volk PUBLIC VK_NO_PROTOTYPES VK_USE_PLATFORM_WIN32_KHR WIN32_LEAN_AND_MEAN NOMINMAX)

add_library(proto_vma STATIC "${PROJECT_SOURCE_DIR}/src/renderer/Vma.cpp")
target_include_directories(proto_vma SYSTEM PUBLIC "${vma_SOURCE_DIR}/include")
target_compile_definitions(proto_vma PUBLIC VMA_STATIC_VULKAN_FUNCTIONS=0 VMA_DYNAMIC_VULKAN_FUNCTIONS=1 VMA_VULKAN_VERSION=1003000)
target_link_libraries(proto_vma PUBLIC proto_volk)

add_library(proto_imgui STATIC
    "${imgui_SOURCE_DIR}/imgui.cpp" "${imgui_SOURCE_DIR}/imgui_draw.cpp"
    "${imgui_SOURCE_DIR}/imgui_tables.cpp" "${imgui_SOURCE_DIR}/imgui_widgets.cpp" "${imgui_SOURCE_DIR}/misc/cpp/imgui_stdlib.cpp"
    "${imgui_SOURCE_DIR}/backends/imgui_impl_glfw.cpp" "${imgui_SOURCE_DIR}/backends/imgui_impl_vulkan.cpp")
target_include_directories(proto_imgui SYSTEM PUBLIC "${imgui_SOURCE_DIR}" "${imgui_SOURCE_DIR}/backends")
target_compile_definitions(proto_imgui PUBLIC IMGUI_IMPL_VULKAN_USE_VOLK GLFW_INCLUDE_NONE)
target_link_libraries(proto_imgui PUBLIC proto_volk glfw)

# Keep upstream notices alongside the local executable; fonts stay on Windows.
file(MAKE_DIRECTORY "${CMAKE_BINARY_DIR}/bin/licenses")
foreach(spec "glfw|LICENSE.md" "imgui|LICENSE.txt" "volk|LICENSE.md" "vma|LICENSE.txt" "vulkan_headers|LICENSE.md")
    string(REPLACE "|" ";" parts "${spec}")
    list(GET parts 0 dep)
    list(GET parts 1 notice)
    configure_file("${${dep}_SOURCE_DIR}/${notice}" "${CMAKE_BINARY_DIR}/bin/licenses/${dep}.txt" COPYONLY)
endforeach()
