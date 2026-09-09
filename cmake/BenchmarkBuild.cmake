# Measurement code has its own identity; changing it need not change the public SDK.
file(GLOB_RECURSE PROTO_BENCHMARK_ID_INPUTS CONFIGURE_DEPENDS
    "${PROJECT_SOURCE_DIR}/src/benchmark/*.cpp" "${PROJECT_SOURCE_DIR}/src/benchmark/*.hpp"
    "${PROJECT_SOURCE_DIR}/src/editor/*.cpp" "${PROJECT_SOURCE_DIR}/src/editor/*.hpp")
list(APPEND PROTO_BENCHMARK_ID_INPUTS "${PROJECT_SOURCE_DIR}/cmake/BenchmarkBuild.cmake")
list(SORT PROTO_BENCHMARK_ID_INPUTS)
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS ${PROTO_BENCHMARK_ID_INPUTS})
set(PROTO_BENCHMARK_ID_TEXT "proto-benchmark-v1|${CMAKE_BUILD_TYPE}|${CMAKE_CXX_COMPILER}|${CMAKE_CXX_FLAGS}|${CMAKE_CXX_FLAGS_RELEASE}|${CMAKE_CXX_FLAGS_DEBUG}")
foreach(input IN LISTS PROTO_BENCHMARK_ID_INPUTS)
    file(SHA256 "${input}" hash)
    file(RELATIVE_PATH name "${PROJECT_SOURCE_DIR}" "${input}")
    string(APPEND PROTO_BENCHMARK_ID_TEXT "|${name}:${hash}")
endforeach()
string(SHA256 PROTO_BENCHMARK_SOURCE_HASH "${PROTO_BENCHMARK_ID_TEXT}")
foreach(target ProtoBenchmark ProtoAssetBenchmark ProtoEditor)
    target_compile_definitions(${target} PRIVATE PROTO_BENCHMARK_SOURCE_HASH="${PROTO_BENCHMARK_SOURCE_HASH}")
endforeach()
