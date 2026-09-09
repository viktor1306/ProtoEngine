# The installed public package never exports editor/importer/private header paths.
execute_process(COMMAND "${CMAKE_CXX_COMPILER}" --version OUTPUT_VARIABLE PROTO_COMPILER_VERSION
    OUTPUT_STRIP_TRAILING_WHITESPACE COMMAND_ERROR_IS_FATAL ANY)
execute_process(COMMAND "${CMAKE_CXX_COMPILER}" -dumpmachine OUTPUT_VARIABLE PROTO_COMPILER_TARGET
    OUTPUT_STRIP_TRAILING_WHITESPACE COMMAND_ERROR_IS_FATAL ANY)
file(GLOB_RECURSE PROTO_RUNTIME_ID_INPUTS CONFIGURE_DEPENDS
    "${PROJECT_SOURCE_DIR}/include/Proto/*" "${PROJECT_SOURCE_DIR}/src/runtime/*"
    "${PROJECT_SOURCE_DIR}/src/behavior/*" "${PROJECT_SOURCE_DIR}/src/scene/*"
    "${PROJECT_SOURCE_DIR}/src/core/*" "${PROJECT_SOURCE_DIR}/src/renderer/*"
    "${PROJECT_SOURCE_DIR}/shaders/*")
list(APPEND PROTO_RUNTIME_ID_INPUTS "${PROJECT_SOURCE_DIR}/src/assets/AssetTypes.hpp"
    "${PROJECT_SOURCE_DIR}/src/assets/AssetTypes.cpp" "${PROJECT_SOURCE_DIR}/src/assets/MeshPicking.cpp"
    "${PROJECT_SOURCE_DIR}/src/assets/CookedAssets.cpp" "${PROJECT_SOURCE_DIR}/src/assets/AssetIO.cpp"
    "${PROJECT_SOURCE_DIR}/src/assets/PortableAssets.hpp" "${PROJECT_SOURCE_DIR}/src/assets/PortableAssets.cpp"
    "${PROJECT_SOURCE_DIR}/dependencies.lock.json" "${PROJECT_SOURCE_DIR}/cmake/ProtoSDKConfig.cmake.in"
    "${PROJECT_SOURCE_DIR}/cmake/ProtoSDK.cmake" "${PROJECT_SOURCE_DIR}/CMakeLists.txt")
list(SORT PROTO_RUNTIME_ID_INPUTS)
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS ${PROTO_RUNTIME_ID_INPUTS})
set(PROTO_ID_TEXT "proto-sdk-v1|${CMAKE_BUILD_TYPE}|${CMAKE_CXX_COMPILER}|${PROTO_COMPILER_VERSION}|${PROTO_COMPILER_TARGET}|${CMAKE_CXX_FLAGS}|${CMAKE_CXX_FLAGS_DEBUG}|${CMAKE_CXX_FLAGS_RELEASE}")
foreach(input IN LISTS PROTO_RUNTIME_ID_INPUTS)
    file(SHA256 "${input}" hash)
    file(RELATIVE_PATH name "${PROJECT_SOURCE_DIR}" "${input}")
    string(APPEND PROTO_ID_TEXT "|${name}:${hash}")
endforeach()
string(SHA256 PROTO_SDK_BUILD_ID "${PROTO_ID_TEXT}")
file(MAKE_DIRECTORY "${CMAKE_BINARY_DIR}/generated/Proto")
file(WRITE "${CMAKE_BINARY_DIR}/generated/Proto/Build.hpp"
    "#pragma once\nnamespace proto::sdk {\ninline constexpr char buildId[] = \"${PROTO_SDK_BUILD_ID}\";\ninline constexpr char configuration[] = \"${CMAKE_BUILD_TYPE}\";\n}\n")
set(PROTO_SDK_JSON "{\"format\":\"proto.sdk\",\"formatVersion\":1,\"behaviorApiVersion\":1}")
foreach(pair sdkBuildId configuration compiler cmake ninja compilerVersion compilerTarget cxxFlags cxxFlagsDebug cxxFlagsRelease exeLinkerFlags)
    if(pair STREQUAL "sdkBuildId")
        set(value "${PROTO_SDK_BUILD_ID}")
    elseif(pair STREQUAL "configuration")
        set(value "${CMAKE_BUILD_TYPE}")
    elseif(pair STREQUAL "compiler")
        set(value "${CMAKE_CXX_COMPILER}")
    elseif(pair STREQUAL "cmake")
        set(value "${CMAKE_COMMAND}")
    elseif(pair STREQUAL "ninja")
        set(value "${CMAKE_MAKE_PROGRAM}")
    elseif(pair STREQUAL "compilerVersion")
        set(value "${PROTO_COMPILER_VERSION}")
    elseif(pair STREQUAL "compilerTarget")
        set(value "${PROTO_COMPILER_TARGET}")
    elseif(pair STREQUAL "cxxFlags")
        set(value "${CMAKE_CXX_FLAGS}")
    elseif(pair STREQUAL "cxxFlagsDebug")
        set(value "${CMAKE_CXX_FLAGS_DEBUG}")
    elseif(pair STREQUAL "cxxFlagsRelease")
        set(value "${CMAKE_CXX_FLAGS_RELEASE}")
    elseif(pair STREQUAL "exeLinkerFlags")
        set(value "${CMAKE_EXE_LINKER_FLAGS}")
    endif()
    string(REPLACE "\\" "\\\\" escaped "${value}")
    string(REPLACE "\"" "\\\"" escaped "${escaped}")
    string(REPLACE "\n" "\\n" escaped "${escaped}")
    string(REPLACE "\r" "\\r" escaped "${escaped}")
    string(JSON PROTO_SDK_JSON SET "${PROTO_SDK_JSON}" "${pair}" "\"${escaped}\"")
endforeach()
file(WRITE "${CMAKE_BINARY_DIR}/generated/sdk.json" "${PROTO_SDK_JSON}\n")
configure_file("${PROJECT_SOURCE_DIR}/cmake/ProtoSDKConfig.cmake.in"
    "${CMAKE_BINARY_DIR}/generated/ProtoSDKConfig.cmake" @ONLY)
include(CMakePackageConfigHelpers)
write_basic_package_version_file("${CMAKE_BINARY_DIR}/generated/ProtoSDKConfigVersion.cmake"
    VERSION "${PROJECT_VERSION}" COMPATIBILITY ExactVersion)
install(TARGETS proto_runtime proto_renderer proto_lighting_math proto_cooked proto_scene
    proto_assets_runtime proto_core proto_json proto_vma proto_volk glfw ARCHIVE DESTINATION lib)
install(DIRECTORY "${PROJECT_SOURCE_DIR}/include/Proto" DESTINATION include)
install(FILES "${CMAKE_BINARY_DIR}/generated/Proto/Build.hpp" DESTINATION include/Proto)
install(FILES "${PROJECT_SOURCE_DIR}/src/runtime/PlayerMain.cpp" DESTINATION src)
install(FILES "${CMAKE_BINARY_DIR}/generated/ProtoSDKConfig.cmake"
    "${CMAKE_BINARY_DIR}/generated/ProtoSDKConfigVersion.cmake" DESTINATION lib/cmake/ProtoSDK)
install(FILES "${CMAKE_BINARY_DIR}/generated/sdk.json" DESTINATION .)
install(DIRECTORY "${CMAKE_BINARY_DIR}/bin/shaders" DESTINATION .)
install(DIRECTORY "${CMAKE_BINARY_DIR}/bin/licenses" DESTINATION .)
install(FILES "${CMAKE_BINARY_DIR}/bin/THIRD_PARTY_NOTICES.md" DESTINATION .)
if(CMAKE_BUILD_TYPE STREQUAL "Debug")
    install(DIRECTORY "${PROTO_VALIDATION_DIR}/" DESTINATION validation)
endif()
add_custom_target(ProtoSDK ALL
    COMMAND "${CMAKE_COMMAND}" --install "${CMAKE_BINARY_DIR}" --prefix "${CMAKE_BINARY_DIR}/sdk"
    DEPENDS proto_runtime proto_shaders VERBATIM)
add_dependencies(ProtoEditor ProtoSDK)
